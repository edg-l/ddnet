#include "automation.h"

#if defined(CONF_AUTOMATION)

#include <base/log.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>

#include <generated/protocol.h>

#include <game/version.h>

#include <algorithm>
#include <cstddef>
#include <utility>

// ---------------------------------------------------------------------------------------------
// CAutomationConnection
// ---------------------------------------------------------------------------------------------

bool CAutomationConnection::Open(int Port)
{
	NETADDR BindAddr;
	if(net_addr_from_str(&BindAddr, "127.0.0.1") != 0)
	{
		return false;
	}
	BindAddr.port = (unsigned short)Port;

	m_ListenSocket = net_tcp_create(BindAddr);
	if(!m_ListenSocket)
	{
		return false;
	}
	if(net_tcp_listen(m_ListenSocket, 1) != 0 || net_set_non_blocking(m_ListenSocket) != 0)
	{
		net_tcp_close(m_ListenSocket);
		m_ListenSocket = nullptr;
		return false;
	}
	return true;
}

void CAutomationConnection::CloseSocket()
{
	if(m_ClientSocket)
	{
		net_tcp_close(m_ClientSocket);
		m_ClientSocket = nullptr;
	}
	m_ClientConnected = false;
	m_RecvBuffer.clear();
	m_SendBuffer.clear();
}

void CAutomationConnection::Close()
{
	CloseSocket();
	if(m_ListenSocket)
	{
		net_tcp_close(m_ListenSocket);
		m_ListenSocket = nullptr;
	}
}

void CAutomationConnection::Receive()
{
	if(!m_ListenSocket)
	{
		return;
	}

	NETSOCKET NewSocket;
	NETADDR Addr;
	if(net_tcp_accept(m_ListenSocket, &NewSocket, &Addr) > 0)
	{
		if(m_ClientConnected)
		{
			static const char *const pREJECT_MSG = "{\"id\":-1,\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"automation client already connected\"}}\n";
			net_tcp_send(NewSocket, pREJECT_MSG, str_length(pREJECT_MSG));
			net_tcp_close(NewSocket);
		}
		else
		{
			net_set_non_blocking(NewSocket);
			m_ClientSocket = NewSocket;
			m_ClientConnected = true;
			m_RecvBuffer.clear();
			m_SendBuffer.clear();
		}
	}

	if(!m_ClientConnected)
	{
		return;
	}

	char aBuf[4096];
	while(true)
	{
		int Bytes = net_tcp_recv(m_ClientSocket, aBuf, sizeof(aBuf));
		if(Bytes > 0)
		{
			m_RecvBuffer.append(aBuf, (size_t)Bytes);
			// Only the unterminated tail counts towards the limit. Measuring the whole buffer
			// would let any single complete line already sitting in it disable the check,
			// because complete lines are not drained until OnFrameBegin runs.
			const size_t LastNewline = m_RecvBuffer.rfind('\n');
			const size_t TailLength = LastNewline == std::string::npos ? m_RecvBuffer.size() : m_RecvBuffer.size() - LastNewline - 1;
			if(TailLength > MAX_LINE_LENGTH)
			{
				DropClient("{\"id\":-1,\"ok\":false,\"error\":{\"code\":\"parse_error\",\"message\":\"line too long\"}}");
				return;
			}
		}
		else if(Bytes < 0)
		{
			if(!net_would_block())
			{
				CloseSocket();
			}
			break;
		}
		else
		{
			// Bytes == 0: remote end closed the connection.
			CloseSocket();
			break;
		}
	}
}

bool CAutomationConnection::NextLine(std::string *pLine)
{
	const size_t Pos = m_RecvBuffer.find('\n');
	if(Pos == std::string::npos)
	{
		return false;
	}
	std::string Line = m_RecvBuffer.substr(0, Pos);
	m_RecvBuffer.erase(0, Pos + 1);
	if(!Line.empty() && Line.back() == '\r')
	{
		Line.pop_back();
	}
	*pLine = std::move(Line);
	return true;
}

void CAutomationConnection::Send(const char *pLine)
{
	m_SendBuffer += pLine;
	m_SendBuffer += '\n';
}

void CAutomationConnection::Flush()
{
	if(!m_ClientConnected || m_SendBuffer.empty())
	{
		return;
	}
	int Bytes = net_tcp_send(m_ClientSocket, m_SendBuffer.data(), (int)m_SendBuffer.size());
	if(Bytes < 0)
	{
		if(!net_would_block())
		{
			CloseSocket();
		}
		return;
	}
	if(Bytes > 0)
	{
		m_SendBuffer.erase(0, (size_t)Bytes);
	}
}

void CAutomationConnection::FlushBlocking(std::chrono::nanoseconds Timeout)
{
	if(!m_ClientConnected)
	{
		return;
	}
	// Real clock, not time_get_nanoseconds(): this deadline is the only guaranteed exit when
	// the peer stops reading, and it runs after CClient::Run's loop (and with it, the only
	// caller of time_advance_fixed_step) has already exited, so a virtual clock would never
	// advance here and the loop would spin forever.
	const int64_t Deadline = time_get_real_nanoseconds().count() + Timeout.count();
	while(m_ClientConnected && !m_SendBuffer.empty() && time_get_real_nanoseconds().count() < Deadline)
	{
		Flush();
	}
}

void CAutomationConnection::DropClient(const char *pReason)
{
	if(!m_ClientConnected)
	{
		return;
	}
	if(pReason && pReason[0])
	{
		Send(pReason);
		Flush();
	}
	CloseSocket();
}

// ---------------------------------------------------------------------------------------------
// Protocol helpers
// ---------------------------------------------------------------------------------------------

// CJsonWriter always pretty-prints (jsonwriter.cpp writes '\n' and '\t' unconditionally between
// elements). WriteInternalEscaped escapes both characters inside string values before this
// point, so every raw '\n'/'\t' left in the finished string is indentation and can be stripped
// unconditionally to turn the writer's output into a single line, as required by the
// line-delimited-JSON wire format.
static std::string CompactJson(std::string &&Json)
{
	Json.erase(std::remove(Json.begin(), Json.end(), '\n'), Json.end());
	Json.erase(std::remove(Json.begin(), Json.end(), '\t'), Json.end());
	return std::move(Json);
}

/**
 * Writes a string that did not come from the request line, and so carries no guarantee of being
 * valid UTF-8: config string values come from settings files and the console, map names come from
 * the filesystem, where non-UTF-8 bytes are legal. CJsonWriter::WriteInternalEscaped asserts on
 * invalid UTF-8, and dbg_assert is active in release builds, so writing such a string directly
 * would take the whole client down. Invalid input is replaced rather than reported, because these
 * are observational fields and a garbled name is more useful than a failed request.
 */
static void WriteUntrustedStrValue(CJsonWriter &Writer, const char *pStr)
{
	if(str_utf8_check(pStr))
	{
		Writer.WriteStrValue(pStr);
	}
	else
	{
		Writer.WriteStrValue("<invalid utf-8>");
	}
}

static const char *ClientStateName(int State)
{
	switch(State)
	{
	case IClient::STATE_OFFLINE: return "offline";
	case IClient::STATE_CONNECTING: return "connecting";
	case IClient::STATE_LOADING: return "loading";
	case IClient::STATE_ONLINE: return "online";
	case IClient::STATE_DEMOPLAYBACK: return "demoplayback";
	case IClient::STATE_QUITTING: return "quitting";
	case IClient::STATE_RESTARTING: return "restarting";
	}
	return "unknown";
}

// Returns -1 for an unrecognized name.
static int ClientStateFromName(const char *pName)
{
	static const struct
	{
		const char *m_pName;
		IClient::EClientState m_State;
	} s_aStates[] = {
		{"offline", IClient::STATE_OFFLINE},
		{"connecting", IClient::STATE_CONNECTING},
		{"loading", IClient::STATE_LOADING},
		{"online", IClient::STATE_ONLINE},
		{"demoplayback", IClient::STATE_DEMOPLAYBACK},
		{"quitting", IClient::STATE_QUITTING},
		{"restarting", IClient::STATE_RESTARTING},
	};
	for(const auto &Entry : s_aStates)
	{
		if(str_comp(Entry.m_pName, pName) == 0)
		{
			return Entry.m_State;
		}
	}
	return -1;
}

// Generic cl_* config lookup, built from the same X-macro that CConfig itself uses
// (src/engine/shared/config.cpp:308).
enum class EConfigType
{
	INT,
	COLOR,
	STRING,
};

class CConfigEntry
{
public:
	const char *m_pName;
	EConfigType m_Type;
	size_t m_Offset;
};

static const CConfigEntry s_aConfigEntries[] = {
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) {#ScriptName, EConfigType::INT, offsetof(CConfig, m_##Name)},
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc) {#ScriptName, EConfigType::COLOR, offsetof(CConfig, m_##Name)},
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc) {#ScriptName, EConfigType::STRING, offsetof(CConfig, m_##Name)},
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR
};

// ---------------------------------------------------------------------------------------------
// CAutomation: lifecycle
// ---------------------------------------------------------------------------------------------

void CAutomation::Init()
{
	const int Port = g_Config.m_ClAutomationPort;
	if(Port == 0)
	{
		return;
	}

	m_pClient = Kernel()->RequestInterface<IClient>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pGameClient = Kernel()->RequestInterface<IGameClient>();
	m_pInput = Kernel()->RequestInterface<IEngineInput>();

	time_set_fixed_step((int64_t)g_Config.m_ClAutomationFixedStep * 1000);
	m_pConsole->Chain("cl_automation_fixed_step", ConchainFixedStep, this);

	if(!m_Connection.Open(Port))
	{
		log_error("automation", "failed to open automation socket on 127.0.0.1:%d", Port);
		return;
	}
	log_info("automation", "listening on 127.0.0.1:%d", Port);
}

void CAutomation::Shutdown()
{
	// Restore the real clock first. Everything that runs after this point is teardown, past the
	// only caller of time_advance_fixed_step, so a virtual clock would be frozen for the rest of
	// the process and any elapsed-time logic in teardown would never make progress.
	time_set_fixed_step(0);
	m_Connection.FlushBlocking(std::chrono::milliseconds(200));
	m_Connection.Close();
}

bool CAutomation::Enabled() const
{
	return m_Connection.IsOpen();
}

void CAutomation::ConchainFixedStep(IConsole::IResult *pResult, void *, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() == 1)
	{
		time_set_fixed_step((int64_t)g_Config.m_ClAutomationFixedStep * 1000);
	}
}

// ---------------------------------------------------------------------------------------------
// CAutomation: frame pump
// ---------------------------------------------------------------------------------------------

void CAutomation::OnFrameBegin()
{
	m_Frame++;
	if(!m_Connection.IsOpen())
	{
		return;
	}

	m_Connection.Receive();
	if(!m_Connection.IsConnected())
	{
		m_vPending.clear();
		m_vOutbox.clear();
		return;
	}

	std::string Line;
	while(m_Connection.NextLine(&Line))
	{
		if(!str_utf8_check(Line.c_str()))
		{
			m_Connection.DropClient("{\"id\":-1,\"ok\":false,\"error\":{\"code\":\"parse_error\",\"message\":\"invalid utf-8\"}}");
			m_vPending.clear();
			m_vOutbox.clear();
			return;
		}

		HandleLine(Line);

		if(!m_Connection.IsConnected())
		{
			m_vPending.clear();
			m_vOutbox.clear();
			return;
		}
	}
}

void CAutomation::OnFrameEnd()
{
	if(!m_Connection.IsOpen())
	{
		m_vOutbox.clear();
		return;
	}

	for(auto It = m_vPending.begin(); It != m_vPending.end();)
	{
		// Initialized only to satisfy -Wmaybe-uninitialized: the switch covers every EDeferKind,
		// and leaving it without a default arm means adding one is a -Wswitch build failure.
		bool Done = false;
		switch(It->m_Kind)
		{
		case EDeferKind::WAIT_TICKS:
		case EDeferKind::PREDICTED_TICK_AT_LEAST:
			Done = m_pClient->PredGameTick(g_Config.m_ClDummy) >= It->m_TargetTick;
			break;
		case EDeferKind::GAME_TICK_AT_LEAST:
			Done = m_pClient->GameTick(g_Config.m_ClDummy) >= It->m_TargetTick;
			break;
		case EDeferKind::STATE:
			Done = m_pClient->State() == It->m_TargetState;
			break;
		case EDeferKind::MAP_LOADED:
			Done = m_pGameClient->Map()->IsLoaded() && str_comp(m_pGameClient->Map()->BaseName(), It->m_TargetMapName.c_str()) == 0;
			break;
		case EDeferKind::HAS_LOCAL_CHARACTER:
			Done = m_HasLocalCharacter;
			break;
		}

		if(Done)
		{
			EmitReply(It->m_Id, true, "{}", CError());
			It = m_vPending.erase(It);
		}
		else if(It->m_TimeoutFrame <= m_Frame)
		{
			CError Error;
			Error.m_pCode = "timeout";
			str_copy(Error.m_aMessage, "request timed out");
			EmitReply(It->m_Id, false, "{}", Error);
			It = m_vPending.erase(It);
		}
		else
		{
			++It;
		}
	}

	FlushReplies();

	m_Connection.Flush();
	if(!m_Connection.IsConnected())
	{
		m_vPending.clear();
		m_vOutbox.clear();
	}
}

// ---------------------------------------------------------------------------------------------
// CAutomation: dispatch and envelope
// ---------------------------------------------------------------------------------------------

void CAutomation::EmitReply(int Id, bool Ok, const char *pResultJson, const CError &Error)
{
	CQueuedReply Reply;
	Reply.m_Id = Id;
	Reply.m_Ok = Ok;
	Reply.m_ResultJson = pResultJson;
	Reply.m_Error = Error;
	m_vOutbox.push_back(std::move(Reply));
}

void CAutomation::FlushReplies()
{
	const int GameTick = m_pClient->State() == IClient::STATE_ONLINE ? m_pClient->GameTick(g_Config.m_ClDummy) : -1;
	const int PredictedTick = m_pClient->State() == IClient::STATE_ONLINE ? m_pClient->PredGameTick(g_Config.m_ClDummy) : -1;

	for(const CQueuedReply &Reply : m_vOutbox)
	{
		BuildEnvelope(Reply, GameTick, PredictedTick);
	}
	m_vOutbox.clear();
}

void CAutomation::BuildEnvelope(const CQueuedReply &Reply, int GameTick, int PredictedTick)
{
	const int Id = Reply.m_Id;
	const bool Ok = Reply.m_Ok;
	const char *pResultJson = Reply.m_ResultJson.c_str();
	const CError &Error = Reply.m_Error;

	char aNum[32];
	std::string Envelope = "{\"id\":";
	str_format(aNum, sizeof(aNum), "%d", Id);
	Envelope += aNum;
	Envelope += ",\"ok\":";
	Envelope += Ok ? "true" : "false";
	Envelope += ",\"frame\":";
	str_format(aNum, sizeof(aNum), "%d", m_Frame);
	Envelope += aNum;
	Envelope += ",\"game_tick\":";
	str_format(aNum, sizeof(aNum), "%d", GameTick);
	Envelope += aNum;
	Envelope += ",\"predicted_tick\":";
	str_format(aNum, sizeof(aNum), "%d", PredictedTick);
	Envelope += aNum;

	if(Ok)
	{
		Envelope += ",\"result\":";
		Envelope += pResultJson;
	}
	else
	{
		CJsonStringWriter MessageWriter;
		MessageWriter.WriteStrValue(Error.m_aMessage);
		Envelope += ",\"error\":{\"code\":\"";
		Envelope += Error.m_pCode;
		Envelope += "\",\"message\":";
		Envelope += CompactJson(MessageWriter.GetOutputString());
		Envelope += "}";
	}
	Envelope += "}";

	m_Connection.Send(Envelope.c_str());
}

void CAutomation::HandleLine(const std::string &Line)
{
	json_value *pRoot = JsonParse(Line.c_str(), Line.size());
	if(!pRoot || pRoot->type != json_object)
	{
		json_value_free(pRoot);
		CError Error;
		Error.m_pCode = "parse_error";
		str_copy(Error.m_aMessage, "invalid JSON request");
		EmitReply(-1, false, "{}", Error);
		return;
	}

	const json_value *pIdValue = json_object_get(pRoot, "id");
	if(pIdValue->type != json_integer)
	{
		json_value_free(pRoot);
		CError Error;
		Error.m_pCode = "parse_error";
		str_copy(Error.m_aMessage, "missing or invalid 'id'");
		EmitReply(-1, false, "{}", Error);
		return;
	}
	const int Id = json_int_get(pIdValue);

	const json_value *pCmdValue = json_object_get(pRoot, "cmd");
	if(pCmdValue->type != json_string)
	{
		json_value_free(pRoot);
		CError Error;
		Error.m_pCode = "bad_request";
		str_copy(Error.m_aMessage, "missing or invalid 'cmd'");
		EmitReply(Id, false, "{}", Error);
		return;
	}
	const char *pCmd = json_string_get(pCmdValue);

	static const struct
	{
		const char *m_pName;
		FCommandHandler m_pfnHandler;
	} s_aCommands[] = {
		{"ping", &CAutomation::CmdPing},
		{"console", &CAutomation::CmdConsole},
		{"get_config", &CAutomation::CmdGetConfig},
		{"get_status", &CAutomation::CmdGetStatus},
		{"wait_ticks", &CAutomation::CmdWaitTicks},
		{"wait_for", &CAutomation::CmdWaitFor},
		{"key_down", &CAutomation::CmdKeyDown},
		{"key_up", &CAutomation::CmdKeyUp},
		{"key_press", &CAutomation::CmdKeyPress},
		{"text", &CAutomation::CmdText},
		{"mouse_move", &CAutomation::CmdMouseMove},
		{"set_input", &CAutomation::CmdSetInput},
		{"clear_input", &CAutomation::CmdClearInput},
		{"get_state", &CAutomation::CmdGetState},
		{"get_parity_history", &CAutomation::CmdGetParityHistory},
	};

	FCommandHandler pfnHandler = nullptr;
	for(const auto &Command : s_aCommands)
	{
		if(str_comp(Command.m_pName, pCmd) == 0)
		{
			pfnHandler = Command.m_pfnHandler;
			break;
		}
	}

	if(!pfnHandler)
	{
		CError Error;
		Error.m_pCode = "unknown_command";
		str_format(Error.m_aMessage, sizeof(Error.m_aMessage), "unknown command '%s'", pCmd);
		json_value_free(pRoot);
		EmitReply(Id, false, "{}", Error);
		return;
	}

	const json_value *pArgs = json_object_get(pRoot, "args");

	std::string ResultJson = "{}";
	CPendingRequest Pending;
	CError Error;
	const EDispatch Result = (this->*pfnHandler)(pArgs, Id, &ResultJson, &Pending, &Error);
	json_value_free(pRoot);

	switch(Result)
	{
	case EDispatch::REPLY_NOW:
		EmitReply(Id, true, ResultJson.c_str(), CError());
		break;
	case EDispatch::ERROR:
		EmitReply(Id, false, "{}", Error);
		break;
	case EDispatch::DEFER:
		Pending.m_Id = Id;
		m_vPending.push_back(std::move(Pending));
		break;
	}
}

// ---------------------------------------------------------------------------------------------
// CAutomation: transport and console commands
// ---------------------------------------------------------------------------------------------

EDispatch CAutomation::CmdPing(const json_value *, int, std::string *pResultJson, CPendingRequest *, CError *)
{
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("protocol");
	Writer.WriteIntValue(1);
	Writer.WriteAttribute("version");
	Writer.WriteStrValue(GAME_RELEASE_VERSION);
	Writer.EndObject();
	*pResultJson = CompactJson(Writer.GetOutputString());
	return EDispatch::REPLY_NOW;
}

// Console has no return value, so this reports dispatch, not success; the harness reads the
// client's stdout for console output.
EDispatch CAutomation::CmdConsole(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	const json_value *pLineValue = json_object_get(pArgs, "line");
	if(pLineValue->type != json_string)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'line' must be a string");
		return EDispatch::ERROR;
	}
	m_pConsole->ExecuteLineFlag(json_string_get(pLineValue), CFGFLAG_CLIENT, IConsole::CLIENT_ID_UNSPECIFIED, true);
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdGetConfig(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	const json_value *pNameValue = json_object_get(pArgs, "name");
	if(pNameValue->type != json_string)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'name' must be a string");
		return EDispatch::ERROR;
	}
	const char *pName = json_string_get(pNameValue);

	for(const auto &Entry : s_aConfigEntries)
	{
		if(str_comp(Entry.m_pName, pName) != 0)
		{
			continue;
		}

		const char *pField = reinterpret_cast<const char *>(&g_Config) + Entry.m_Offset;

		CJsonStringWriter Writer;
		Writer.BeginObject();
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(pName);
		Writer.WriteAttribute("type");
		switch(Entry.m_Type)
		{
		case EConfigType::INT:
			Writer.WriteStrValue("int");
			Writer.WriteAttribute("value");
			Writer.WriteIntValue(*reinterpret_cast<const int *>(pField));
			break;
		case EConfigType::COLOR:
			Writer.WriteStrValue("color");
			Writer.WriteAttribute("value");
			Writer.WriteIntValue((int)*reinterpret_cast<const unsigned *>(pField));
			break;
		case EConfigType::STRING:
			Writer.WriteStrValue("string");
			Writer.WriteAttribute("value");
			WriteUntrustedStrValue(Writer, reinterpret_cast<const char *>(pField));
			break;
		}
		Writer.EndObject();
		*pResultJson = CompactJson(Writer.GetOutputString());
		return EDispatch::REPLY_NOW;
	}

	pError->m_pCode = "bad_args";
	str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "unknown config variable '%s'", pName);
	return EDispatch::ERROR;
}

EDispatch CAutomation::CmdGetStatus(const json_value *, int, std::string *pResultJson, CPendingRequest *, CError *)
{
	char aAddr[NETADDR_MAXSTRSIZE] = "";
	if(m_pClient->ServerAddress().type != NETTYPE_INVALID)
	{
		net_addr_str(&m_pClient->ServerAddress(), aAddr, sizeof(aAddr), true);
	}

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("state");
	Writer.WriteStrValue(ClientStateName(m_pClient->State()));
	Writer.WriteAttribute("prev_game_tick");
	Writer.WriteIntValue(m_pClient->PrevGameTick(g_Config.m_ClDummy));
	Writer.WriteAttribute("tick_speed");
	Writer.WriteIntValue(m_pClient->GameTickSpeed());
	// Milliseconds, not microseconds: LocalTime() is a float holding cumulative uptime, so a
	// microsecond scaling exceeds float's exact-integer range after ~17 seconds and overflows int
	// after ~36 minutes. Milliseconds stay exact for over four hours, which keeps the "advances by
	// exactly one fixed step per frame" property usable for a whole test run.
	Writer.WriteAttribute("local_time_ms");
	Writer.WriteIntValue((int)(m_pClient->LocalTime() * 1000.0f));
	Writer.WriteAttribute("frame_time_us");
	Writer.WriteIntValue((int)(m_pClient->RenderFrameTime() * 1000000.0f));
	Writer.WriteAttribute("server_address");
	Writer.WriteStrValue(aAddr);
	Writer.WriteAttribute("map_name");
	WriteUntrustedStrValue(Writer, m_pGameClient->Map()->IsLoaded() ? m_pGameClient->Map()->BaseName() : "");
	// local_client_id/menus_active come from the last per-frame state push, so they are
	// meaningless until the first SnapInput of an online session has run.
	Writer.WriteAttribute("local_client_id");
	Writer.WriteIntValue(m_LastLocalClientId);
	Writer.WriteAttribute("dummy");
	Writer.WriteIntValue(g_Config.m_ClDummy);
	Writer.WriteAttribute("dummy_connected");
	Writer.WriteBoolValue(m_pClient->DummyConnected());
	Writer.WriteAttribute("menus_active");
	Writer.WriteBoolValue((m_LastInput.m_PlayerFlags & PLAYERFLAG_IN_MENU) != 0);
	Writer.WriteAttribute("editor_active");
	Writer.WriteBoolValue(g_Config.m_ClEditor != 0);
	Writer.WriteAttribute("connection_problems");
	Writer.WriteBoolValue(m_pClient->ConnectionProblems());
	Writer.WriteAttribute("fixed_step_us");
	Writer.WriteIntValue(g_Config.m_ClAutomationFixedStep);
	Writer.EndObject();
	*pResultJson = CompactJson(Writer.GetOutputString());
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdWaitTicks(const json_value *pArgs, int, std::string *, CPendingRequest *pPending, CError *pError)
{
	const json_value *pTicksValue = json_object_get(pArgs, "ticks");
	if(pTicksValue->type != json_integer || json_int_get(pTicksValue) <= 0)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'ticks' must be a positive integer");
		return EDispatch::ERROR;
	}
	const int Ticks = json_int_get(pTicksValue);

	int TimeoutFrames = 3000;
	const json_value *pTimeoutValue = json_object_get(pArgs, "timeout_frames");
	if(pTimeoutValue->type == json_integer)
	{
		TimeoutFrames = json_int_get(pTimeoutValue);
	}

	if(m_pClient->State() != IClient::STATE_ONLINE)
	{
		pError->m_pCode = "not_online";
		str_copy(pError->m_aMessage, "client is not online");
		return EDispatch::ERROR;
	}

	pPending->m_Kind = EDeferKind::WAIT_TICKS;
	pPending->m_TargetTick = m_pClient->PredGameTick(g_Config.m_ClDummy) + Ticks;
	pPending->m_TimeoutFrame = m_Frame + TimeoutFrames;
	return EDispatch::DEFER;
}

EDispatch CAutomation::CmdWaitFor(const json_value *pArgs, int, std::string *, CPendingRequest *pPending, CError *pError)
{
	const json_value *pPredicateValue = json_object_get(pArgs, "predicate");
	if(pPredicateValue->type != json_string)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'predicate' must be a string");
		return EDispatch::ERROR;
	}
	const char *pPredicate = json_string_get(pPredicateValue);

	int TimeoutFrames = 3000;
	const json_value *pTimeoutValue = json_object_get(pArgs, "timeout_frames");
	if(pTimeoutValue->type == json_integer)
	{
		TimeoutFrames = json_int_get(pTimeoutValue);
	}
	pPending->m_TimeoutFrame = m_Frame + TimeoutFrames;

	if(str_comp(pPredicate, "state") == 0)
	{
		const json_value *pValueValue = json_object_get(pArgs, "value");
		if(pValueValue->type != json_string)
		{
			pError->m_pCode = "bad_args";
			str_copy(pError->m_aMessage, "'state' predicate requires a string 'value'");
			return EDispatch::ERROR;
		}
		const int TargetState = ClientStateFromName(json_string_get(pValueValue));
		if(TargetState < 0)
		{
			pError->m_pCode = "bad_args";
			str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "unknown state '%s'", json_string_get(pValueValue));
			return EDispatch::ERROR;
		}
		pPending->m_Kind = EDeferKind::STATE;
		pPending->m_TargetState = (IClient::EClientState)TargetState;
		return EDispatch::DEFER;
	}
	else if(str_comp(pPredicate, "game_tick_at_least") == 0 || str_comp(pPredicate, "predicted_tick_at_least") == 0)
	{
		const json_value *pValueValue = json_object_get(pArgs, "value");
		if(pValueValue->type != json_integer)
		{
			pError->m_pCode = "bad_args";
			str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'%s' predicate requires an integer 'value'", pPredicate);
			return EDispatch::ERROR;
		}
		pPending->m_Kind = str_comp(pPredicate, "game_tick_at_least") == 0 ? EDeferKind::GAME_TICK_AT_LEAST : EDeferKind::PREDICTED_TICK_AT_LEAST;
		pPending->m_TargetTick = json_int_get(pValueValue);
		return EDispatch::DEFER;
	}
	else if(str_comp(pPredicate, "has_local_character") == 0)
	{
		pPending->m_Kind = EDeferKind::HAS_LOCAL_CHARACTER;
		return EDispatch::DEFER;
	}
	else if(str_comp(pPredicate, "map_loaded") == 0)
	{
		const json_value *pNameValue = json_object_get(pArgs, "name");
		if(pNameValue->type != json_string)
		{
			pError->m_pCode = "bad_args";
			str_copy(pError->m_aMessage, "'map_loaded' predicate requires a string 'name'");
			return EDispatch::ERROR;
		}
		pPending->m_Kind = EDeferKind::MAP_LOADED;
		pPending->m_TargetMapName = json_string_get(pNameValue);
		return EDispatch::DEFER;
	}

	pError->m_pCode = "bad_args";
	str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "unknown predicate '%s'", pPredicate);
	return EDispatch::ERROR;
}

// ---------------------------------------------------------------------------------------------
// CAutomation: raw input injection
// ---------------------------------------------------------------------------------------------

// Resolves the 'key'/'code' argument pair shared by key_down/key_up/key_press.
static bool ResolveKeyArg(const json_value *pArgs, IInput *pInput, int *pKey, CError *pError)
{
	const json_value *pKeyValue = json_object_get(pArgs, "key");
	if(pKeyValue->type == json_string)
	{
		*pKey = pInput->FindKeyByName(json_string_get(pKeyValue));
		if(*pKey == KEY_UNKNOWN)
		{
			pError->m_pCode = "bad_args";
			str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "unknown key '%s'", json_string_get(pKeyValue));
			return false;
		}
		return true;
	}

	const json_value *pCodeValue = json_object_get(pArgs, "code");
	if(pCodeValue->type == json_integer)
	{
		const int Code = json_int_get(pCodeValue);
		if(Code < KEY_FIRST || Code >= KEY_LAST)
		{
			pError->m_pCode = "bad_args";
			str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'code' out of range: %d", Code);
			return false;
		}
		*pKey = Code;
		return true;
	}

	pError->m_pCode = "bad_args";
	str_copy(pError->m_aMessage, "either 'key' or 'code' is required");
	return false;
}

EDispatch CAutomation::CmdKeyDown(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	int Key;
	if(!ResolveKeyArg(pArgs, m_pInput, &Key, pError))
	{
		return EDispatch::ERROR;
	}
	m_pInput->InjectKeyEvent(Key, IInput::FLAG_PRESS);
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdKeyUp(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	int Key;
	if(!ResolveKeyArg(pArgs, m_pInput, &Key, pError))
	{
		return EDispatch::ERROR;
	}
	m_pInput->InjectKeyEvent(Key, IInput::FLAG_RELEASE);
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdKeyPress(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	int Key;
	if(!ResolveKeyArg(pArgs, m_pInput, &Key, pError))
	{
		return EDispatch::ERROR;
	}
	m_pInput->InjectKeyEvent(Key, IInput::FLAG_PRESS);
	m_pInput->InjectKeyEvent(Key, IInput::FLAG_RELEASE);
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdText(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	const json_value *pTextValue = json_object_get(pArgs, "text");
	if(pTextValue->type != json_string)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'text' must be a string");
		return EDispatch::ERROR;
	}
	const char *pText = json_string_get(pTextValue);
	if(!str_utf8_check(pText))
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'text' must be valid utf-8");
		return EDispatch::ERROR;
	}
	m_pInput->InjectTextEvent(pText);
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdMouseMove(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	const json_value *pDxValue = json_object_get(pArgs, "dx");
	const json_value *pDyValue = json_object_get(pArgs, "dy");
	if(pDxValue->type != json_integer || pDyValue->type != json_integer)
	{
		pError->m_pCode = "bad_args";
		str_copy(pError->m_aMessage, "'dx' and 'dy' must be integers");
		return EDispatch::ERROR;
	}
	m_pInput->InjectMouseRelative(vec2((float)json_int_get(pDxValue), (float)json_int_get(pDyValue)));
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

// ---------------------------------------------------------------------------------------------
// CAutomation: scripted gameplay input
// ---------------------------------------------------------------------------------------------

// Reads an optional integer argument. Returns false (leaving *pError untouched) when the
// argument is absent, so callers can distinguish "not given" from "given but wrong type".
static bool ReadOptionalIntArg(const json_value *pArgs, const char *pName, int *pOut, bool *pPresent, CError *pError)
{
	const json_value *pValue = json_object_get(pArgs, pName);
	if(pValue->type == json_none)
	{
		*pPresent = false;
		return true;
	}
	if(pValue->type != json_integer)
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'%s' must be an integer", pName);
		return false;
	}
	*pOut = json_int_get(pValue);
	*pPresent = true;
	return true;
}

EDispatch CAutomation::CmdSetInput(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	int Dummy = g_Config.m_ClDummy;
	bool DummyPresent;
	if(!ReadOptionalIntArg(pArgs, "dummy", &Dummy, &DummyPresent, pError))
	{
		return EDispatch::ERROR;
	}
	if(DummyPresent && (Dummy < 0 || Dummy >= NUM_DUMMIES))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'dummy' out of range: %d", Dummy);
		return EDispatch::ERROR;
	}

	// Validate every argument before applying any of them, so a rejected request never leaves
	// the scripted input partially updated.
	bool DirPresent, JumpPresent, HookPresent, FirePresent, TargetXPresent, TargetYPresent, WeaponPresent, NextWeaponPresent, PrevWeaponPresent;
	int Dir = 0, Jump = 0, Hook = 0, Fire = 0, TargetX = 0, TargetY = 0, Weapon = 0, NextWeapon = 0, PrevWeapon = 0;
	if(!ReadOptionalIntArg(pArgs, "dir", &Dir, &DirPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "jump", &Jump, &JumpPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "hook", &Hook, &HookPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "fire", &Fire, &FirePresent, pError) ||
		!ReadOptionalIntArg(pArgs, "target_x", &TargetX, &TargetXPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "target_y", &TargetY, &TargetYPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "weapon", &Weapon, &WeaponPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "next_weapon", &NextWeapon, &NextWeaponPresent, pError) ||
		!ReadOptionalIntArg(pArgs, "prev_weapon", &PrevWeapon, &PrevWeaponPresent, pError))
	{
		return EDispatch::ERROR;
	}
	if(DirPresent && (Dir < -1 || Dir > 1))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'dir' out of range: %d", Dir);
		return EDispatch::ERROR;
	}
	if(JumpPresent && (Jump != 0 && Jump != 1))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'jump' out of range: %d", Jump);
		return EDispatch::ERROR;
	}
	if(HookPresent && (Hook != 0 && Hook != 1))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'hook' out of range: %d", Hook);
		return EDispatch::ERROR;
	}
	if(FirePresent && (Fire != 0 && Fire != 1))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'fire' out of range: %d", Fire);
		return EDispatch::ERROR;
	}
	if(WeaponPresent && (Weapon < 0 || Weapon > 5))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'weapon' out of range: %d", Weapon);
		return EDispatch::ERROR;
	}

	CScriptedInput &Scripted = m_aScripted[Dummy];
	if(!Scripted.m_Active)
	{
		// Seed the press counters from the live ones before applying anything. m_Fire,
		// m_NextWeapon and m_PrevWeapon are INPUT_STATE_MASK counters that the character reads
		// through CountInput, which counts every parity flip while walking from the previous
		// value to the current one. Handing it a counter tracked independently of the one the
		// character last saw would register presses the caller never asked for on the first
		// scripted tick.
		// Round up to even, i.e. to "not held". A physically held fire button leaves an odd
		// counter, and seeding that verbatim would carry the held state into scripted input the
		// caller never asked to hold.
		Scripted.m_Input.m_Fire = m_LastInput.m_Fire + ((m_LastInput.m_Fire & 1) != 0 ? 1 : 0);
		Scripted.m_Input.m_NextWeapon = m_LastInput.m_NextWeapon;
		Scripted.m_Input.m_PrevWeapon = m_LastInput.m_PrevWeapon;
	}
	if(DirPresent)
	{
		Scripted.m_Input.m_Direction = Dir;
	}
	if(JumpPresent)
	{
		Scripted.m_Input.m_Jump = Jump;
	}
	if(HookPresent)
	{
		Scripted.m_Input.m_Hook = Hook;
	}
	if(FirePresent)
	{
		// m_Fire is a counter in the protocol; odd means held. Adjust the parity instead of
		// setting the value directly, so a held-fire request across several set_input calls
		// does not keep incrementing it every call.
		const bool WantHeld = Fire != 0;
		const bool CurrentlyHeld = (Scripted.m_Input.m_Fire & 1) != 0;
		if(WantHeld != CurrentlyHeld)
		{
			Scripted.m_Input.m_Fire++;
		}
	}
	if(TargetXPresent)
	{
		Scripted.m_Input.m_TargetX = TargetX;
	}
	if(TargetYPresent)
	{
		Scripted.m_Input.m_TargetY = TargetY;
	}
	if(WeaponPresent)
	{
		Scripted.m_Input.m_WantedWeapon = Weapon;
	}
	// Press counts, not raw counter values: the character reads these through CountInput, so
	// assigning the request's integer directly would make the same request twice in a row a
	// silent no-op the second time. Adding the count registers exactly that many presses.
	if(NextWeaponPresent)
	{
		Scripted.m_Input.m_NextWeapon += NextWeapon;
	}
	if(PrevWeaponPresent)
	{
		Scripted.m_Input.m_PrevWeapon += PrevWeapon;
	}

	if(!Scripted.m_Active)
	{
		m_aNeedsInputReset[Dummy] = true;
	}
	Scripted.m_Active = true;

	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdClearInput(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	int Dummy = g_Config.m_ClDummy;
	bool DummyPresent;
	if(!ReadOptionalIntArg(pArgs, "dummy", &Dummy, &DummyPresent, pError))
	{
		return EDispatch::ERROR;
	}
	if(DummyPresent && (Dummy < 0 || Dummy >= NUM_DUMMIES))
	{
		pError->m_pCode = "bad_args";
		str_format(pError->m_aMessage, sizeof(pError->m_aMessage), "'dummy' out of range: %d", Dummy);
		return EDispatch::ERROR;
	}

	// Request a reset on the way out as well as on the way in. Nothing else rewrites the movement
	// fields when the override stops, and a headless client has no real key events to clear them,
	// so without this a tee keeps running and holding jump forever after clear_input.
	if(m_aScripted[Dummy].m_Active)
	{
		m_aNeedsInputReset[Dummy] = true;
	}
	m_aScripted[Dummy].m_Active = false;
	*pResultJson = "{}";
	return EDispatch::REPLY_NOW;
}

// ---------------------------------------------------------------------------------------------
// CAutomation: character state readback
// ---------------------------------------------------------------------------------------------

// The 14 fields CCharacterCore::Write produces, minus m_Tick (a dead-reckoning tick on the
// snapshot side, see CCharacter::Snap, not comparable between predicted and authoritative state).
static void WriteCoreFields(CJsonWriter &Writer, const CAutomationCharacter &Char)
{
	Writer.WriteAttribute("x");
	Writer.WriteIntValue(Char.m_X);
	Writer.WriteAttribute("y");
	Writer.WriteIntValue(Char.m_Y);
	Writer.WriteAttribute("vel_x");
	Writer.WriteIntValue(Char.m_VelX);
	Writer.WriteAttribute("vel_y");
	Writer.WriteIntValue(Char.m_VelY);
	Writer.WriteAttribute("angle");
	Writer.WriteIntValue(Char.m_Angle);
	Writer.WriteAttribute("direction");
	Writer.WriteIntValue(Char.m_Direction);
	Writer.WriteAttribute("jumped");
	Writer.WriteIntValue(Char.m_Jumped);
	Writer.WriteAttribute("hooked_player");
	Writer.WriteIntValue(Char.m_HookedPlayer);
	Writer.WriteAttribute("hook_state");
	Writer.WriteIntValue(Char.m_HookState);
	Writer.WriteAttribute("hook_tick");
	Writer.WriteIntValue(Char.m_HookTick);
	Writer.WriteAttribute("hook_x");
	Writer.WriteIntValue(Char.m_HookX);
	Writer.WriteAttribute("hook_y");
	Writer.WriteIntValue(Char.m_HookY);
	Writer.WriteAttribute("hook_dx");
	Writer.WriteIntValue(Char.m_HookDx);
	Writer.WriteAttribute("hook_dy");
	Writer.WriteIntValue(Char.m_HookDy);
}

static void WritePredictedCharacter(CJsonWriter &Writer, const CAutomationCharacter &Char)
{
	Writer.BeginObject();
	Writer.WriteAttribute("tick");
	Writer.WriteIntValue(Char.m_Tick);
	WriteCoreFields(Writer, Char);
	Writer.WriteAttribute("active_weapon");
	Writer.WriteIntValue(Char.m_ActiveWeapon);
	Writer.WriteAttribute("jumps");
	Writer.WriteIntValue(Char.m_Jumps);
	Writer.WriteAttribute("jumped_total");
	Writer.WriteIntValue(Char.m_JumpedTotal);
	Writer.WriteAttribute("freeze_start");
	Writer.WriteIntValue(Char.m_FreezeStart);
	Writer.WriteAttribute("freeze_end");
	Writer.WriteIntValue(Char.m_FreezeEnd);
	Writer.WriteAttribute("solo");
	Writer.WriteBoolValue(Char.m_Solo);
	Writer.WriteAttribute("jetpack");
	Writer.WriteBoolValue(Char.m_Jetpack);
	Writer.WriteAttribute("collision_disabled");
	Writer.WriteBoolValue(Char.m_CollisionDisabled);
	Writer.WriteAttribute("endless_hook");
	Writer.WriteBoolValue(Char.m_EndlessHook);
	Writer.WriteAttribute("endless_jump");
	Writer.WriteBoolValue(Char.m_EndlessJump);
	Writer.WriteAttribute("hammer_hit_disabled");
	Writer.WriteBoolValue(Char.m_HammerHitDisabled);
	Writer.WriteAttribute("grenade_hit_disabled");
	Writer.WriteBoolValue(Char.m_GrenadeHitDisabled);
	Writer.WriteAttribute("laser_hit_disabled");
	Writer.WriteBoolValue(Char.m_LaserHitDisabled);
	Writer.WriteAttribute("shotgun_hit_disabled");
	Writer.WriteBoolValue(Char.m_ShotgunHitDisabled);
	Writer.WriteAttribute("hook_hit_disabled");
	Writer.WriteBoolValue(Char.m_HookHitDisabled);
	Writer.WriteAttribute("super");
	Writer.WriteBoolValue(Char.m_Super);
	Writer.WriteAttribute("invincible");
	Writer.WriteBoolValue(Char.m_Invincible);
	Writer.WriteAttribute("deep_frozen");
	Writer.WriteBoolValue(Char.m_DeepFrozen);
	Writer.WriteAttribute("live_frozen");
	Writer.WriteBoolValue(Char.m_LiveFrozen);
	Writer.EndObject();
}

static void WriteSnapshotCharacter(CJsonWriter &Writer, const CAutomationCharacter &Char)
{
	Writer.BeginObject();
	Writer.WriteAttribute("tick");
	Writer.WriteIntValue(Char.m_Tick);
	WriteCoreFields(Writer, Char);
	Writer.WriteAttribute("player_flags");
	Writer.WriteIntValue(Char.m_PlayerFlags);
	Writer.WriteAttribute("health");
	Writer.WriteIntValue(Char.m_Health);
	Writer.WriteAttribute("armor");
	Writer.WriteIntValue(Char.m_Armor);
	Writer.WriteAttribute("ammo_count");
	Writer.WriteIntValue(Char.m_AmmoCount);
	Writer.WriteAttribute("weapon");
	Writer.WriteIntValue(Char.m_Weapon);
	Writer.WriteAttribute("emote");
	Writer.WriteIntValue(Char.m_Emote);
	Writer.WriteAttribute("attack_tick");
	Writer.WriteIntValue(Char.m_AttackTick);
	Writer.EndObject();
}

EDispatch CAutomation::CmdGetState(const json_value *, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	if(m_pClient->State() != IClient::STATE_ONLINE)
	{
		pError->m_pCode = "not_online";
		str_copy(pError->m_aMessage, "client is not online");
		return EDispatch::ERROR;
	}
	if(!m_LastSnapshot.m_Valid)
	{
		pError->m_pCode = "no_character";
		str_copy(pError->m_aMessage, "no active local character in the last snapshot");
		return EDispatch::ERROR;
	}

	CJsonStringWriter Writer;
	Writer.BeginObject();

	Writer.WriteAttribute("local_client_id");
	Writer.WriteIntValue(m_LastLocalClientId);

	Writer.WriteAttribute("predicted");
	if(m_LastPredicted.m_Valid)
	{
		WritePredictedCharacter(Writer, m_LastPredicted);
	}
	else
	{
		Writer.WriteNullValue();
	}

	Writer.WriteAttribute("snapshot");
	WriteSnapshotCharacter(Writer, m_LastSnapshot);

	Writer.WriteAttribute("prediction_for_snapshot_tick");
	const CAutomationCharacter &PredictionForSnapshotTick = m_aPredictionHistory[m_LastSnapshot.m_Tick % 200];
	if(PredictionForSnapshotTick.m_Valid && PredictionForSnapshotTick.m_Tick == m_LastSnapshot.m_Tick)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("tick");
		Writer.WriteIntValue(PredictionForSnapshotTick.m_Tick);
		WriteCoreFields(Writer, PredictionForSnapshotTick);
		Writer.EndObject();
	}
	else
	{
		Writer.WriteNullValue();
	}

	Writer.WriteAttribute("input");
	Writer.BeginObject();
	Writer.WriteAttribute("direction");
	Writer.WriteIntValue(m_LastInput.m_Direction);
	Writer.WriteAttribute("target_x");
	Writer.WriteIntValue(m_LastInput.m_TargetX);
	Writer.WriteAttribute("target_y");
	Writer.WriteIntValue(m_LastInput.m_TargetY);
	Writer.WriteAttribute("jump");
	Writer.WriteIntValue(m_LastInput.m_Jump);
	Writer.WriteAttribute("fire");
	Writer.WriteIntValue(m_LastInput.m_Fire);
	Writer.WriteAttribute("hook");
	Writer.WriteIntValue(m_LastInput.m_Hook);
	Writer.WriteAttribute("player_flags");
	Writer.WriteIntValue(m_LastInput.m_PlayerFlags);
	Writer.WriteAttribute("wanted_weapon");
	Writer.WriteIntValue(m_LastInput.m_WantedWeapon);
	Writer.WriteAttribute("next_weapon");
	Writer.WriteIntValue(m_LastInput.m_NextWeapon);
	Writer.WriteAttribute("prev_weapon");
	Writer.WriteIntValue(m_LastInput.m_PrevWeapon);
	Writer.EndObject();

	Writer.EndObject();
	*pResultJson = CompactJson(Writer.GetOutputString());
	return EDispatch::REPLY_NOW;
}

EDispatch CAutomation::CmdGetParityHistory(const json_value *pArgs, int, std::string *pResultJson, CPendingRequest *, CError *pError)
{
	if(m_pClient->State() != IClient::STATE_ONLINE)
	{
		pError->m_pCode = "not_online";
		str_copy(pError->m_aMessage, "client is not online");
		return EDispatch::ERROR;
	}

	int Max = (int)MAX_PARITY_ENTRIES;
	const json_value *pMaxValue = json_object_get(pArgs, "max");
	if(pMaxValue->type == json_integer)
	{
		Max = json_int_get(pMaxValue);
	}
	if(Max < 0)
	{
		Max = 0;
	}

	CJsonStringWriter Writer;
	Writer.BeginObject();

	// Reported once, then cleared: there is no per-entry granularity to preserve across a call
	// that may not drain the whole deque.
	Writer.WriteAttribute("truncated");
	Writer.WriteBoolValue(m_ParityTruncated);
	m_ParityTruncated = false;

	Writer.WriteAttribute("entries");
	Writer.BeginArray();
	int Count = 0;
	while(!m_vParityHistory.empty() && Count < Max)
	{
		const CParityEntry &Entry = m_vParityHistory.front();
		Writer.BeginObject();
		Writer.WriteAttribute("tick");
		Writer.WriteIntValue(Entry.m_Tick);
		Writer.WriteAttribute("predicted");
		Writer.BeginObject();
		WriteCoreFields(Writer, Entry.m_Predicted);
		Writer.EndObject();
		Writer.WriteAttribute("snapshot");
		Writer.BeginObject();
		WriteCoreFields(Writer, Entry.m_Snapshot);
		Writer.EndObject();
		Writer.EndObject();
		m_vParityHistory.pop_front();
		Count++;
	}
	Writer.EndArray();

	Writer.EndObject();
	*pResultJson = CompactJson(Writer.GetOutputString());
	return EDispatch::REPLY_NOW;
}

// ---------------------------------------------------------------------------------------------
// CAutomation: IAutomation observation seams
// ---------------------------------------------------------------------------------------------

bool CAutomation::OverrideInput(int Dummy, CAutomationInput *pInput) const
{
	*pInput = m_aScripted[Dummy].m_Input;
	return m_aScripted[Dummy].m_Active;
}

bool CAutomation::ConsumeInputReset(int Dummy)
{
	const bool NeedsReset = m_aNeedsInputReset[Dummy];
	m_aNeedsInputReset[Dummy] = false;
	return NeedsReset;
}

void CAutomation::SetObservedState(const CAutomationCharacter &Predicted, const CAutomationCharacter &Snapshot,
	const CAutomationInput &LastInput, int LocalClientId)
{
	m_LastPredicted = Predicted;
	m_LastSnapshot = Snapshot;
	m_LastInput = LastInput;
	m_LastLocalClientId = LocalClientId;

	if(Predicted.m_Valid && Predicted.m_Tick > m_LastPredictedTick)
	{
		m_aPredictionHistory[Predicted.m_Tick % 200] = Predicted;
		m_LastPredictedTick = Predicted.m_Tick;
	}

	if(Snapshot.m_Valid && Snapshot.m_Tick > m_LastParityTick)
	{
		const CAutomationCharacter &PredictionForTick = m_aPredictionHistory[Snapshot.m_Tick % 200];
		if(PredictionForTick.m_Valid && PredictionForTick.m_Tick == Snapshot.m_Tick)
		{
			CParityEntry Entry;
			Entry.m_Tick = Snapshot.m_Tick;
			Entry.m_Predicted = PredictionForTick;
			Entry.m_Snapshot = Snapshot;
			m_vParityHistory.push_back(std::move(Entry));
			if(m_vParityHistory.size() > MAX_PARITY_ENTRIES)
			{
				m_vParityHistory.pop_front();
				m_ParityTruncated = true;
			}
		}
		m_LastParityTick = Snapshot.m_Tick;
	}

	m_HasLocalCharacter = Snapshot.m_Valid;
}

IAutomation *CreateAutomation()
{
	return new CAutomation();
}

#endif // CONF_AUTOMATION
