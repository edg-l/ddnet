#ifndef GAME_SERVER_MAP_DUMMIES_H
#define GAME_SERVER_MAP_DUMMIES_H

#include <base/vmath.h>

#include <vector>

class CGameContext;

inline constexpr const char *MAP_DUMMY_NAME = "Dummy";

// Owns the map dummy tiles found while loading the map. Populated by
// IGameController::OnEntity while entities are created.
class CMapDummies
{
public:
	void Init(CGameContext *pGameServer);
	void AddTile(vec2 Pos, bool Hammer, bool FacingLeft);
	void OnMapLoaded();
	bool HasTiles() const { return !m_vTiles.empty(); }

private:
	class CTile
	{
	public:
		vec2 m_Pos;
		bool m_Hammer;
		bool m_FacingLeft;
	};
	std::vector<CTile> m_vTiles;
	CGameContext *m_pGameServer = nullptr;

	CGameContext *GameServer() const { return m_pGameServer; }
	class IServer *Server() const;
};

#endif // GAME_SERVER_MAP_DUMMIES_H
