#include "map_dummies.h"

#include <base/log.h>

#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <game/server/gamecontext.h>
#include <game/teamscore.h>

IServer *CMapDummies::Server() const { return m_pGameServer->Server(); }

void CMapDummies::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_vTiles.clear();
}

void CMapDummies::AddTile(vec2 Pos, bool Hammer, bool FacingLeft)
{
	m_vTiles.push_back({Pos, Hammer, FacingLeft});
}

void CMapDummies::OnMapLoaded()
{
	// the server outlives the map, so a map without tiles has to reset the count
	if(!HasTiles() || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
	{
		if(HasTiles())
			log_warn("map_dummies", "%d tiles ignored: sv_team is forced solo", (int)m_vTiles.size());
		Server()->SetGameIdCount(MAX_CLIENTS);
		return;
	}

	Server()->SetGameIdCount(MAX_GAME_IDS);
	log_info("map_dummies", "%d tiles, every client uses an id map", (int)m_vTiles.size());
}
