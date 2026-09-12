#include "test.h"

#include <base/detect.h>
#include <base/logger.h>
#include <base/mem.h>
#include <base/types.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/register.h>
#include <engine/server/server.h>
#include <engine/server/server_logger.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/gamecore.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/version.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <thread>

bool IsInterrupted()
{
	return false;
}

#if defined(CONF_PLATFORM_ANDROID)
std::vector<std::string> FetchAndroidServerCommandQueue()
{
	return {};
}
#endif

class GameWorld : public ::testing::Test // NOLINT(readability-identifier-naming)
{
public:
	IGameServer *m_pGameServer = nullptr;
	CServer *m_pServer = nullptr;
	std::unique_ptr<IKernel> m_pKernel;
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;

	CGameContext *GameServer() // NOLINT(readability-make-member-function-const)
	{
		return (CGameContext *)m_pGameServer;
	}

	GameWorld()
	{
		CServer *pServer = CreateServer();
		m_pServer = pServer;

		m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
		m_pKernel->RegisterInterface(m_pServer);

		IEngine *pEngine = CreateTestEngine(GAME_NAME);
		m_pKernel->RegisterInterface(pEngine);

		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		EXPECT_NE(m_pStorage, nullptr);
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
		m_pKernel->RegisterInterface(pConsole);

		IConfigManager *pConfigManager = CreateConfigManager();
		m_pKernel->RegisterInterface(pConfigManager);

		IEngineHttp *pEngineHttp = CreateEngineHttp();
		m_pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
		m_pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);

		IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
		m_pKernel->RegisterInterface(pEngineAntibot);
		m_pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		m_pGameServer = CreateGameServer();
		m_pKernel->RegisterInterface(m_pGameServer);

		pEngine->Init();
		pConsole->Init();
		pConfigManager->Init();

		m_pServer->RegisterCommands();

		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		m_pServer->m_RunServer = CServer::RUNNING;

		m_pServer->m_AuthManager.Init();

		{
			int Size = GameServer()->PersistentClientDataSize();
			for(auto &Client : m_pServer->m_aClients)
			{
				Client.m_HasPersistentData = false;
				Client.m_pPersistentData = malloc(Size);
			}
		}
		m_pServer->m_pPersistentData = malloc(GameServer()->PersistentDataSize());
		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		EXPECT_TRUE(pEngineHttp->Init(std::chrono::seconds{2})) << "Failed to initialize the HTTP client";

		pServer->m_NetServer.SetCallbacks(
			CServer::NewClientCallback,
			CServer::NewClientNoAuthCallback,
			CServer::ClientRejoinCallback,
			CServer::DelClientCallback, pServer);

		pServer->m_Econ.Init(pServer->Config(), pServer->Console(), &pServer->m_ServerBan);

		pServer->m_Fifo.Init(pServer->Console(), pServer->Config()->m_SvInputFifo, CFGFLAG_SERVER);
		m_pServer->Antibot()->Init();
		GameServer()->OnInit(nullptr);
		pServer->ReadAnnouncementsFile();
		pServer->InitMaplist();
	}

	~GameWorld() override
	{
		m_pServer->m_Econ.Shutdown();
		m_pServer->m_Fifo.Shutdown();
		m_pGameServer->OnShutdown(nullptr);
		m_pServer->DbPool()->OnShutdown();
	}
};

TEST_F(GameWorld, ClosestCharacter)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChr1 = new(0) CCharacter(&GameServer()->m_World, Input);
	pChr1->m_Pos = vec2(0, 0);
	GameServer()->m_World.InsertEntity(pChr1);

	CCharacter *pChr2 = new(1) CCharacter(&GameServer()->m_World, Input);
	pChr2->m_Pos = vec2(10, 10);
	GameServer()->m_World.InsertEntity(pChr2);

	CCharacter *pClosest = GameServer()->m_World.ClosestCharacter(vec2(1, 1), 20, nullptr);
	EXPECT_EQ(pClosest, pChr1);
}

TEST_F(GameWorld, IntersectEntity)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChrLeft = new(0) CCharacter(&GameServer()->m_World, Input);
	pChrLeft->m_Pos = vec2(15, 10);
	GameServer()->m_World.InsertEntity(pChrLeft);

	CCharacter *pChrRight = new(1) CCharacter(&GameServer()->m_World, Input);
	pChrRight->m_Pos = vec2(16, 10);
	GameServer()->m_World.InsertEntity(pChrRight);

	float Radius = 5.0f;
	vec2 IntersectAt;
	CCharacter *pIntersectedChar;

	// both tees are exactly on the line
	// if we go intersect left to right we find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// if we intersect right to left we find the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);

	// but not if we ignore the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrRight, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// or we force find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// pNotThis == pThisOnly => nullptr

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrLeft, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, nullptr);

	// the tee closer to the start of the intersection line
	// will not be matched if it is further than Radius away
	// from the line

	vec2 CloserToFromButTooFarFromLine = vec2(11, 11 + Radius + pChrLeft->GetProximityRadius());
	pChrLeft->SetPosition(CloserToFromButTooFarFromLine);
	pChrLeft->m_Pos = CloserToFromButTooFarFromLine;

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);
}

TEST_F(GameWorld, BasicTick)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	const int StartTeam = GameServer()->m_pController->GetAutoTeam(ClientId);
	GameServer()->CreatePlayer(ClientId, StartTeam, Afk, LastWhisperTo);

	GameServer()->OnTick();
}

TEST_F(GameWorld, CharacterEmote)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, Afk, LastWhisperTo);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pChr = pPlayer->GetCharacter();
	ASSERT_NE(pChr, nullptr);

	// afk
	pPlayer->SetAfk(true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// not afk
	pPlayer->SetAfk(false);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// frozen and paused
	pPlayer->Pause(CPlayer::PAUSE_PAUSED, true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// ninja jetpack
	pPlayer->Pause(CPlayer::PAUSE_NONE, true);
	pChr->Unfreeze();
	pPlayer->m_NinjaJetpack = true;
	pChr->m_NinjaJetpack = true;
	pChr->SetJetpack(true);
	pChr->SetActiveWeapon(WEAPON_GUN);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_HAPPY);

	// /emote angry 3 chat command
	pChr->SetEmote(EMOTE_ANGRY, GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed() * 3);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);

	// /emote angry 3 chat command and frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);
}

TEST(Tunings, OutOfRangeBecomesIntMin)
{
	const float IntMin = std::numeric_limits<int>::min() / 100.0f;
	CTuneParam Param;
	EXPECT_EQ((float)(Param = 555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = -555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = std::numeric_limits<float>::quiet_NaN()), IntMin);
	EXPECT_EQ((float)(Param = 0.5f), 0.5f);
}

// Reference hammer hit expression that HammerHitForce must reproduce bit for bit.
static vec2 OriginalHammerHitForce(vec2 HammerPos, vec2 TargetPos, vec2 TargetVel, int TargetMoveRestrictions)
{
	vec2 Dir;
	if(length(TargetPos - HammerPos) > 0.0f)
		Dir = normalize(TargetPos - HammerPos);
	else
		Dir = vec2(0.f, -1.f);

	vec2 Temp = TargetVel + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f;
	Temp = ClampVel(TargetMoveRestrictions, Temp);
	Temp -= TargetVel;
	return vec2(0.f, -1.0f) + Temp;
}

TEST(HammerHitForce, MatchesInline)
{
	std::mt19937 Rng(0);
	std::uniform_real_distribution<float> PosDist(-50000.0f, 50000.0f);
	std::uniform_real_distribution<float> VelDist(-6000.0f, 6000.0f);

	for(int i = 0; i < 100000; i++)
	{
		const vec2 HammerPos(PosDist(Rng), PosDist(Rng));
		const vec2 TargetPos = (i % 100 == 0) ? HammerPos : vec2(PosDist(Rng), PosDist(Rng));
		const vec2 TargetVel(VelDist(Rng), VelDist(Rng));
		const int TargetMoveRestrictions = i % 16;

		const vec2 Actual = HammerHitForce(HammerPos, TargetPos, TargetVel, TargetMoveRestrictions);
		const vec2 Expected = OriginalHammerHitForce(HammerPos, TargetPos, TargetVel, TargetMoveRestrictions);
		ASSERT_EQ(mem_comp(&Actual, &Expected, sizeof(vec2)), 0) << "sample " << i;
	}
}

// Folds one CCharacterCore's Write() output into a running FNV-1a-64 hash.
static uint64_t HashCharacterCore(uint64_t Hash, const CCharacterCore &Core)
{
	CNetObj_CharacterCore Obj = {};
	Core.Write(&Obj);
	const auto *pBytes = reinterpret_cast<const unsigned char *>(&Obj);
	for(size_t i = 0; i < sizeof(Obj); i++)
	{
		Hash ^= pBytes[i];
		Hash *= 1099511628211ULL; // FNV-1a-64 prime
	}
	return Hash;
}

TEST_F(GameWorld, CoreTeeTeeGoldenTrajectory)
{
	CWorldCore World;
	CTeamsCore Teams;

	vec2 SpawnPos;
	GameServer()->m_pController->CanSpawn(TEAM_GAME, &SpawnPos, 0);

	std::array<CCharacterCore, 12> aCores{};
	for(int i = 0; i < (int)aCores.size(); i++)
	{
		aCores[i].Init(&World, GameServer()->Collision(), &Teams);
		aCores[i].m_Id = i;
		aCores[i].m_Pos = SpawnPos + vec2((i % 4) * 40.0f, (i / 4) * -40.0f);
		aCores[i].SetAntiPingInterfereCallback([](int, bool) {});
		World.m_apCharacters[i] = &aCores[i];
	}

	Teams.Team(6, 1);
	Teams.Team(7, 1);
	Teams.Team(8, 1);
	aCores[9].m_Super = true;
	Teams.SetSolo(10, true);

	std::mt19937 Rng(1);
	std::uniform_int_distribution<int> DirDist(-1, 1);
	std::uniform_int_distribution<int> BoolDist(0, 1);
	std::uniform_int_distribution<int> TargetDist(-200, 200);

	uint64_t Hash = 14695981039346656037ULL; // FNV-1a-64 offset basis
	for(int Tick = 0; Tick < 1500; Tick++)
	{
		for(CCharacterCore &Core : aCores)
		{
			CNetObj_PlayerInput Input = {};
			Input.m_Direction = DirDist(Rng);
			Input.m_Jump = BoolDist(Rng);
			Input.m_Hook = BoolDist(Rng);
			Input.m_TargetX = TargetDist(Rng);
			Input.m_TargetY = TargetDist(Rng);
			Core.m_Input = Input;
			Core.Tick(true);
		}
		for(CCharacterCore &Core : aCores)
		{
			Core.Move();
			Core.Quantize();
		}
		for(const CCharacterCore &Core : aCores)
		{
			Hash = HashCharacterCore(Hash, Core);
		}
	}

#if defined(CONF_PLATFORM_LINUX) && defined(CONF_ARCH_AMD64)
	// hash of the tee-tee trajectory; the core loops must not change it
	EXPECT_EQ(Hash, 0xD8647CE98336B1BDULL);
#else
	(void)Hash;
#endif
}

// Runs the 12-tee scenario from CoreTeeTeeGoldenTrajectory, plus NumExtraCores cores at
// ids MAX_CLIENTS.., in team ExtraTeam and solo state ExtraSolo, with the world's loop
// bound raised to GameIdCount. Returns every player core's per-tick Write() output.
// ExtraNearby places the extra cores on the same grid as the player cluster (so they
// can collide and be hooked); otherwise they sit far outside hook range (380) of the
// player cluster and of each other's worst-case drift, so a m_Super player core (which
// can hook across teams) never finds a target in range either.
static std::vector<std::array<CNetObj_CharacterCore, 12>> RunTeeTeeTrajectory(CGameContext *pGameServer, int GameIdCount, int NumExtraCores, int ExtraTeam, bool ExtraSolo, bool ExtraNearby)
{
	CWorldCore World;
	CTeamsCore Teams;
	World.m_GameIdCount = GameIdCount;

	vec2 SpawnPos;
	pGameServer->m_pController->CanSpawn(TEAM_GAME, &SpawnPos, 0);

	constexpr float ExtraOffset = 100000.0f;

	std::vector<CCharacterCore> vCores(12 + NumExtraCores);
	for(int i = 0; i < (int)vCores.size(); i++)
	{
		const int Id = i < 12 ? i : MAX_CLIENTS + (i - 12);
		vCores[i].Init(&World, pGameServer->Collision(), &Teams);
		vCores[i].m_Id = Id;
		vCores[i].m_Pos = i < 12 || ExtraNearby ?
					  SpawnPos + vec2((i % 4) * 40.0f, (i / 4) * -40.0f) :
					  SpawnPos + vec2(ExtraOffset + (i - 12) % 4 * 40.0f, (i - 12) / 4 * -40.0f);
		vCores[i].SetAntiPingInterfereCallback([](int, bool) {});
		World.m_apCharacters[Id] = &vCores[i];
	}

	Teams.Team(6, 1);
	Teams.Team(7, 1);
	Teams.Team(8, 1);
	vCores[9].m_Super = true;
	Teams.SetSolo(10, true);

	for(int i = 0; i < NumExtraCores; i++)
	{
		const int Id = MAX_CLIENTS + i;
		Teams.Team(Id, ExtraTeam);
		Teams.SetSolo(Id, ExtraSolo);
	}

	// Player and extra cores draw from independent streams, so a player core's input
	// sequence never depends on NumExtraCores.
	std::mt19937 PlayerRng(1);
	std::mt19937 ExtraRng(2);
	std::uniform_int_distribution<int> DirDist(-1, 1);
	std::uniform_int_distribution<int> BoolDist(0, 1);
	std::uniform_int_distribution<int> TargetDist(-200, 200);

	std::vector<std::array<CNetObj_CharacterCore, 12>> vPlayerTicks(1500);
	for(int Tick = 0; Tick < 1500; Tick++)
	{
		for(int i = 0; i < (int)vCores.size(); i++)
		{
			std::mt19937 &Rng = i < 12 ? PlayerRng : ExtraRng;
			CNetObj_PlayerInput Input = {};
			Input.m_Direction = DirDist(Rng);
			Input.m_Jump = BoolDist(Rng);
			Input.m_Hook = BoolDist(Rng);
			Input.m_TargetX = TargetDist(Rng);
			Input.m_TargetY = TargetDist(Rng);
			vCores[i].m_Input = Input;
			vCores[i].Tick(true);
		}
		for(CCharacterCore &Core : vCores)
		{
			Core.Move();
			Core.Quantize();
		}
		for(int i = 0; i < 12; i++)
		{
			vCores[i].Write(&vPlayerTicks[Tick][i]);
		}
	}
	return vPlayerTicks;
}

TEST_F(GameWorld, CoreExtraIdsDoNotAffectPlayers)
{
	const std::vector<std::array<CNetObj_CharacterCore, 12>> vRunA = RunTeeTeeTrajectory(GameServer(), MAX_CLIENTS, 0, 0, false, false);
	// Run B: 20 extra cores on their own team, far from the cluster, so they never collide or hook with players.
	const std::vector<std::array<CNetObj_CharacterCore, 12>> vRunB = RunTeeTeeTrajectory(GameServer(), MAX_GAME_IDS, 20, 7, false, false);

	for(size_t Tick = 0; Tick < vRunA.size(); Tick++)
	{
		for(int i = 0; i < 12; i++)
		{
			ASSERT_EQ(mem_comp(&vRunA[Tick][i], &vRunB[Tick][i], sizeof(CNetObj_CharacterCore)), 0) << "tick " << Tick << " core " << i;
		}
	}

	// Run C: extra cores share team 0 with players 0-5, are not solo, and sit on the
	// player grid, so they can interact.
	const std::vector<std::array<CNetObj_CharacterCore, 12>> vRunC = RunTeeTeeTrajectory(GameServer(), MAX_GAME_IDS, 20, 0, false, true);

	bool AnyDifference = false;
	for(size_t Tick = 0; Tick < vRunA.size() && !AnyDifference; Tick++)
	{
		for(int i = 0; i < 12; i++)
		{
			if(mem_comp(&vRunA[Tick][i], &vRunC[Tick][i], sizeof(CNetObj_CharacterCore)) != 0)
			{
				AnyDifference = true;
				break;
			}
		}
	}
	EXPECT_TRUE(AnyDifference) << "interacting extra cores should perturb player trajectories";
}
