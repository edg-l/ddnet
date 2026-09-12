/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_CHARACTER_OWNER_H
#define GAME_SERVER_CHARACTER_OWNER_H

#include <cstdint>
#include <optional>

class CCharacter;

// what a character needs from whatever controls it: a connected client, or a map dummy
class ICharacterOwner
{
public:
	virtual ~ICharacterOwner() = default;

	// the connected client controlling the character; a map dummy has none
	virtual std::optional<int> ClientId() const = 0;
	// identifies the owner for weapon interactions, unique over the server's lifetime
	virtual uint32_t GetUniqueCid() const = 0;
	virtual CCharacter *GetCharacter() = 0;
	virtual const CCharacter *GetCharacter() const = 0;

	virtual int GetTeam() const = 0;
	virtual int GetDefaultEmote() const = 0;
	// AFK or paused, which shows blinking eyes
	virtual bool IsInactive() const = 0;
	virtual bool NinjaJetpack() const = 0;
	virtual int PlayerFlags() const = 0;

	// death bookkeeping for the respawn delay
	virtual void OnCharacterDeath(int Tick) = 0;
	virtual int DieTick() const = 0;
	virtual void SetDieTick(int Tick) = 0;
};

#endif
