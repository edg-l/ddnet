#include "automation.h"

#if defined(CONF_AUTOMATION)

#include <base/log.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/input.h>
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
// CAutomation: phase-1 commands
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
	Writer.WriteAttribute("local_time_us");
	Writer.WriteIntValue((int)(m_pClient->LocalTime() * 1000000.0f));
	Writer.WriteAttribute("frame_time_us");
	Writer.WriteIntValue((int)(m_pClient->RenderFrameTime() * 1000000.0f));
	Writer.WriteAttribute("server_address");
	Writer.WriteStrValue(aAddr);
	Writer.WriteAttribute("map_name");
	WriteUntrustedStrValue(Writer, m_pGameClient->Map()->IsLoaded() ? m_pGameClient->Map()->BaseName() : "");
	// local_client_id/menus_active reflect the last state pushed via SetObservedState(), which
	// is only wired up starting phase 3; until then they report their construction defaults.
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
		// Only meaningful once the phase-3 state push reports a valid snapshot character.
		pError->m_pCode = "internal";
		str_copy(pError->m_aMessage, "has_local_character is not implemented until phase 3");
		return EDispatch::ERROR;
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
// CAutomation: phase-3 seams (no-ops in phase 1)
// ---------------------------------------------------------------------------------------------

bool CAutomation::OverrideInput(int, CAutomationInput *) const
{
	return false;
}

void CAutomation::SetObservedState(const CAutomationCharacter &Predicted, const CAutomationCharacter &Snapshot,
	const CAutomationInput &LastInput, int LocalClientId)
{
	m_LastPredicted = Predicted;
	m_LastSnapshot = Snapshot;
	m_LastInput = LastInput;
	m_LastLocalClientId = LocalClientId;
}

IAutomation *CreateAutomation()
{
	return new CAutomation();
}

#endif // CONF_AUTOMATION
