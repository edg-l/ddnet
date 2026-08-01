#ifndef ENGINE_CLIENT_AUTOMATION_H
#define ENGINE_CLIENT_AUTOMATION_H

#if defined(CONF_AUTOMATION)

#include <base/types.h>

#include <engine/automation.h>
#include <engine/client.h>
#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/json.h>

#include <chrono>
#include <deque>
#include <string>
#include <vector>

class IEngineInput;

/**
 * Non-blocking loopback TCP transport for the automation protocol: one listen socket, one
 * client slot, line-delimited framing. Structurally modelled on CNetConsole, but without its
 * 1021-byte send truncation, str_sanitize_cc pass or one-client-per-IP limit, none of which fit
 * a JSON control channel that must carry multi-KB replies.
 */
class CAutomationConnection
{
	NETSOCKET m_ListenSocket = nullptr;
	NETSOCKET m_ClientSocket = nullptr;
	bool m_ClientConnected = false;
	std::string m_RecvBuffer;
	std::string m_SendBuffer;

	void CloseSocket();

public:
	static constexpr size_t MAX_LINE_LENGTH = 64 * 1024;

	bool Open(int Port); // binds 127.0.0.1:Port, listens, sets non-blocking
	void Close();
	bool IsOpen() const { return m_ListenSocket != nullptr; }
	bool IsConnected() const { return m_ClientConnected; }
	/** Accept a pending connection, then read all available bytes into the receive buffer. */
	void Receive();
	/** Pop one complete line, stripped of \r\n. False when none is buffered. */
	bool NextLine(std::string *pLine);
	/** Append one line plus \n to the send buffer. */
	void Send(const char *pLine);
	/** Push as much of the send buffer as the socket accepts. */
	void Flush();
	/** Flush with a deadline, used on shutdown so the reply to `quit` gets out. */
	void FlushBlocking(std::chrono::nanoseconds Timeout);
	void DropClient(const char *pReason);
};

/** Outcome of a command handler. */
enum class EDispatch
{
	REPLY_NOW,
	DEFER,
	ERROR,
};

/** Closed set of `wait_for` predicates implemented so far, plus `wait_ticks`. */
enum class EDeferKind
{
	WAIT_TICKS,
	STATE,
	GAME_TICK_AT_LEAST,
	PREDICTED_TICK_AT_LEAST,
	MAP_LOADED,
	HAS_LOCAL_CHARACTER,
};

/** A `wait_ticks`/`wait_for` request whose completion is evaluated once per frame. */
class CPendingRequest
{
public:
	int m_Id = 0;
	EDeferKind m_Kind = EDeferKind::WAIT_TICKS;
	int m_TimeoutFrame = 0;
	// WAIT_TICKS, GAME_TICK_AT_LEAST, PREDICTED_TICK_AT_LEAST
	int m_TargetTick = 0;
	// STATE
	IClient::EClientState m_TargetState = IClient::STATE_OFFLINE;
	// MAP_LOADED
	std::string m_TargetMapName;
};

/** Error half of a reply envelope. m_pCode must point at one of the closed set of code strings. */
class CError
{
public:
	const char *m_pCode = "";
	char m_aMessage[256] = "";
};

/**
 * A reply whose result or error is already built, but whose envelope is not. Envelopes are
 * stamped in OnFrameEnd so that every reply reports the tick state of the same point in the
 * frame, after Update() has run. Building them where the command is dispatched instead would
 * report the previous frame's ticks against the current frame number.
 */
class CQueuedReply
{
public:
	int m_Id = 0;
	bool m_Ok = false;
	std::string m_ResultJson;
	CError m_Error;
};

/** Scripted input for one dummy slot, applied by CControls::SnapInput while m_Active. */
class CScriptedInput
{
public:
	bool m_Active = false;
	CAutomationInput m_Input;
};

/** One tick's worth of predicted and authoritative core state, for get_parity_history. */
class CParityEntry
{
public:
	int m_Tick = -1;
	CAutomationCharacter m_Predicted;
	CAutomationCharacter m_Snapshot;
};

class CAutomation : public IAutomation
{
	CAutomationConnection m_Connection;
	int m_Frame = 0;
	std::vector<CPendingRequest> m_vPending;
	std::vector<CQueuedReply> m_vOutbox;

	IClient *m_pClient = nullptr;
	IConsole *m_pConsole = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IEngineInput *m_pInput = nullptr;

	// Observed state, pushed once per frame by SetObservedState().
	CAutomationCharacter m_LastPredicted;
	CAutomationCharacter m_LastSnapshot;
	CAutomationInput m_LastInput;
	int m_LastLocalClientId = -1;

	// Scripted input, applied by CControls::SnapInput via OverrideInput().
	CScriptedInput m_aScripted[NUM_DUMMIES];
	// Consumed by ConsumeInputReset(): true once per inactive -> active transition.
	bool m_aNeedsInputReset[NUM_DUMMIES] = {};

	// Prediction ring, 4s at 50Hz, matching the CClient::m_aInputs[200] convention.
	CAutomationCharacter m_aPredictionHistory[200];
	int m_LastPredictedTick = -1;

	// Per-tick predicted/authoritative pairs, drained by get_parity_history.
	std::deque<CParityEntry> m_vParityHistory;
	static constexpr size_t MAX_PARITY_ENTRIES = 400;
	bool m_ParityTruncated = false;
	int m_LastParityTick = -1;

	// Feeds the has_local_character wait_for predicate.
	bool m_HasLocalCharacter = false;

	using FCommandHandler = EDispatch (CAutomation::*)(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);

	void HandleLine(const std::string &Line);
	/** Queues a reply. The envelope is built later, by FlushReplies(). */
	void EmitReply(int Id, bool Ok, const char *pResultJson, const CError &Error);
	/** Builds and sends an envelope for every queued reply, stamping the current tick state. */
	void FlushReplies();
	void BuildEnvelope(const CQueuedReply &Reply, int GameTick, int PredictedTick);

	EDispatch CmdPing(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdConsole(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdGetConfig(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdGetStatus(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdWaitTicks(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	/**
	 * `wait_for` predicates: `state` (string `value`), `game_tick_at_least` (integer `value`),
	 * `predicted_tick_at_least` (integer `value`), `map_loaded` (string `name`), and
	 * `has_local_character` (no extra args; true once SetObservedState() has reported a valid
	 * snapshot character).
	 */
	EDispatch CmdWaitFor(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);

	// Raw input injection, applied during OnFrameBegin so events carry the frame's input
	// counter and are consumed this frame (see CInput::ConsumeEvents).
	EDispatch CmdKeyDown(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdKeyUp(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdKeyPress(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdText(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdMouseMove(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);

	// Scripted gameplay input, applied by CControls::SnapInput via OverrideInput().
	EDispatch CmdSetInput(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdClearInput(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);

	// State readback.
	EDispatch CmdGetState(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);
	EDispatch CmdGetParityHistory(const json_value *pArgs, int Id, std::string *pResultJson, CPendingRequest *pPending, CError *pError);

	/** Re-applies cl_automation_fixed_step to the base/time.h virtual clock on every change. */
	static void ConchainFixedStep(IConsole::IResult *pResult, void *, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

public:
	void Init() override;
	void Shutdown() override;
	bool Enabled() const override;

	void OnFrameBegin() override;
	void OnFrameEnd() override;

	bool OverrideInput(int Dummy, CAutomationInput *pInput) const override;
	bool ConsumeInputReset(int Dummy) override;
	void SetObservedState(const CAutomationCharacter &Predicted, const CAutomationCharacter &Snapshot,
		const CAutomationInput &LastInput, int LocalClientId) override;
};

#endif // CONF_AUTOMATION
#endif // ENGINE_CLIENT_AUTOMATION_H
