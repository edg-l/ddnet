#ifndef ENGINE_AUTOMATION_H
#define ENGINE_AUTOMATION_H

#include "kernel.h"

#if defined(CONF_AUTOMATION)

/**
 * Snapshot of one character's state, in the quantized integer domain that the client and
 * server are required to agree on (see CCharacterCore::Write).
 */
class CAutomationCharacter
{
public:
	bool m_Valid = false;
	int m_Tick = -1;
	// CNetObj_CharacterCore fields, excluding its m_Tick (which is a dead-reckoning tick,
	// not a game tick, see CCharacter::Snap).
	int m_X = 0;
	int m_Y = 0;
	int m_VelX = 0;
	int m_VelY = 0;
	int m_Angle = 0;
	int m_Direction = 0;
	int m_Jumped = 0;
	int m_HookedPlayer = -1;
	int m_HookState = 0;
	int m_HookTick = 0;
	int m_HookX = 0;
	int m_HookY = 0;
	int m_HookDx = 0;
	int m_HookDy = 0;
	// Only meaningful for the authoritative snapshot character.
	int m_PlayerFlags = 0;
	int m_Health = 0;
	int m_Armor = 0;
	int m_AmmoCount = 0;
	int m_Weapon = -1;
	int m_Emote = 0;
	int m_AttackTick = 0;
	// Only meaningful for the predicted character.
	int m_ActiveWeapon = 0;
	int m_Jumps = 0;
	int m_JumpedTotal = 0;
	int m_FreezeStart = 0;
	int m_FreezeEnd = 0;
	bool m_Solo = false;
	bool m_Jetpack = false;
	bool m_CollisionDisabled = false;
	bool m_EndlessHook = false;
	bool m_EndlessJump = false;
	bool m_HammerHitDisabled = false;
	bool m_GrenadeHitDisabled = false;
	bool m_LaserHitDisabled = false;
	bool m_ShotgunHitDisabled = false;
	bool m_HookHitDisabled = false;
	bool m_Super = false;
	bool m_Invincible = false;
	bool m_DeepFrozen = false;
	bool m_LiveFrozen = false;
};

/** Mirrors CNetObj_PlayerInput without pulling generated/protocol.h into the engine. */
class CAutomationInput
{
public:
	int m_Direction = 0;
	int m_TargetX = 0;
	int m_TargetY = 1;
	int m_Jump = 0;
	int m_Fire = 0;
	int m_Hook = 0;
	int m_PlayerFlags = 0;
	int m_WantedWeapon = 0;
	int m_NextWeapon = 0;
	int m_PrevWeapon = 0;
};

class IAutomation : public IInterface
{
	MACRO_INTERFACE("automation")
public:
	/** Open the listen socket if cl_automation_port is set. */
	virtual void Init() = 0;
	void Shutdown() override = 0;
	/** True when the listen socket is open. */
	virtual bool Enabled() const = 0;

	/** Read pending requests and apply them. Called once per frame from CClient::Run. */
	virtual void OnFrameBegin() = 0;
	/** Resolve pending replies and flush them. Called once per frame from CClient::Run. */
	virtual void OnFrameEnd() = 0;

	// --- filled in phase 3 ---
	/** True when scripted input is active for Dummy; then pInput has been overwritten. */
	virtual bool OverrideInput(int Dummy, CAutomationInput *pInput) const = 0;
	/** Per-frame push of the observable game state from the game client. */
	virtual void SetObservedState(const CAutomationCharacter &Predicted,
		const CAutomationCharacter &Snapshot, const CAutomationInput &LastInput,
		int LocalClientId) = 0;
};

IAutomation *CreateAutomation();

#endif // CONF_AUTOMATION
#endif // ENGINE_AUTOMATION_H
