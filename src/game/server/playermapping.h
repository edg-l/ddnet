#ifndef GAME_SERVER_PLAYERMAPPING_H
#define GAME_SERVER_PLAYERMAPPING_H

#include <engine/shared/protocol.h>

#include <generated/protocol7.h>

#include <game/teamscore.h>

#include <optional>

class CGameContext;
class CPlayer;

// The teeworlds 0.7 client and older ddnet clients
// only support up to 64 clients. And also only up to 64 client ids (0-63).
// The CPlayerMapping class manages a mapping of real server internal
// client ids in the range of 0-127 and the fake client ids send to clients
// in the range of 0-63 (or vanilla 0.6: ids 0-15)
//
// If a client does not support 128 slots the server will only show the 63 (64-1 for empty client for chat)
// closest players (that closest selection currently tries to minimize changes by only changing when really required)
// to the client. Because these 64 closest players are not
// guaranteed to have client ids in the range of 0-63 we need to maintain a
// fake client id list for every old client.
//
// Additionally there is the "See Others" feature which lets you cycle through a list of
// all players manually in pause or spectator mode by holding right shift (+spectate) or call vote menu.

class CPlayerMapping
{
public:
	class CSixupCfg
	{
	public:
		CSixupCfg() :
			m_SkipTimeoutedId(false),
			m_ClearSlots(false)
		{
		}
		bool m_SkipTimeoutedId;
		bool m_ClearSlots;
	};

private:
	class CGameContext *m_pGameServer;
	class CConfig *m_pConfig;
	class IServer *m_pServer;

	// Number of players per page for see others feature in +spectate
	// (128 - (64 - 2)) / 2 + 1 reserved slots for max 2 pages. We want to show some players still on screen, as we have enough slots with 64 slots
	static constexpr int ms_MaxNumSeeOthers = 34;
	// 16 - 2 - 1 is max for 16p clients, keep local char. MaxNumSeeOthers() will take care if reserved players take up more space.
	static constexpr int ms_MaxNumSeeOthersVanilla = 13;
	// Teams are messy. Dont highlight teams bigger than 10 tees in playermapping so that big teams wont break anything
	static constexpr int ms_MaxTeamSizePlayerMap = 10;
	// Dont reserve more than half of the 62 slots for players in teams, the rest is needed for the closest tees
	static constexpr int ms_MaxTotalTeamSizePlayerMap = 30;

	int m_aTeamSizes[NUM_DDRACE_TEAMS];
	bool m_ReserveAnyTeamSlots;
	char m_aSeeOthersName[MAX_NAME_LENGTH];

	class CPlayerMap
	{
	public:
		void Init(int ClientId, CPlayerMapping *pPlayerMapping);
		void InitPlayer(CSixupCfg SixupCfg);
		CPlayerMapping *m_pPlayerMapping;
		CPlayer *Player() const;
		// A modern client's own id space covers every player id, so it maps every
		// player to its own id instead of the first free slot.
		bool IdentityMode() const;
		int m_ClientId;
		int m_NumReserved;
		bool m_UpdateTeamsState;
		bool m_aReserved[MAX_GAME_IDS];
		// own-team map dummies currently in this client's view; never evicted for overflow
		bool m_aPriority[MAX_GAME_IDS];
		bool m_ResortReserved;
		int *m_pMap;
		int *m_pReverseMap;
		// the tick a visible slot was emptied, so it is not reused until the next
		// mapping update (the client still interpolates the old occupant by id)
		int m_aFreedTick[MAX_CLIENTS];
		void Update();
		void Add(int MapId, int ClientId);
		int Remove(int MapId);
		void InsertNextEmptyOrReplace(int ClientId);
		int MapSize() const;
		// See others
		int m_SeeOthersPage;
		int m_TotalOverhang;
		int m_NumPages;
		int m_NumSeeOthers;
		bool m_aWasSeeOthers[MAX_GAME_IDS];
		int m_LastSeeOthersVoteTick;
		bool m_DoSeeOthersByVote;
		void DoSeeOthers();
		void CycleSeeOthers();
		void UpdateSeeOthers() const;
		void ResetSeeOthers();
		int MaxNumSeeOthers();
	} m_aMap[MAX_CLIENTS];
	void UpdatePlayerMap(int ClientId);
	void UpdateDemoMap();
	// quarantine for the demo id map (SERVER_DEMO_CLIENT has no CPlayerMap)
	int m_aDemoFreedTick[MAX_CLIENTS];

public:
	class CGameContext *GameServer() { return m_pGameServer; }
	class CConfig *Config() { return m_pConfig; }
	class IServer *Server() { return m_pServer; }

	// Pure helpers with no server access, shared by every client's mapping update.
	//
	// The slot for GameId, preferring its identity slot when Identity is set and
	// free, otherwise the first free, unquarantined slot below MapSize - NumSeeOthers.
	static std::optional<int> ChooseSlot(const int *pMap, int MapSize, int NumSeeOthers, bool Identity, int GameId, const int *pFreedTick, int Tick);
	// The farthest mapped id below MapSize - NumSeeOthers that is neither reserved
	// nor priority, for overflow eviction.
	static std::optional<int> ChooseEviction(const int *pMap, int MapSize, int NumSeeOthers, const bool *pReserved, const bool *pPriority, const float *pDistSq);

	void Init(CGameContext *pGameServer);
	void Tick();

	void InitPlayerMap(int ClientId, CSixupCfg SixupCfg = CSixupCfg()) { m_aMap[ClientId].InitPlayer(SixupCfg); }
	void UpdateTeamsState(int ClientId) { m_aMap[ClientId].m_UpdateTeamsState = true; }
	void ForceInsertPlayer(int Insert, int ClientId) { m_aMap[ClientId].InsertNextEmptyOrReplace(Insert); }

	enum class ESeeOthersInd
	{
		NONE = -1,
		PLAYER = 0,
		BUTTON = 1,
	};
	int SeeOthersId(int ClientId) const;
	bool DoSeeOthers(int ClientId, int SelectedId, bool DoByVote = false);
	void ResetSeeOthers(int ClientId);
	int TotalOverhang(int ClientId) const;
	ESeeOthersInd SeeOthersInd(int ClientId, int MapId) const;
	const char *SeeOthersName(int ClientId);
	bool ReserveTeamSlots(int DDTeam, int ClientId) const;
};

#endif
