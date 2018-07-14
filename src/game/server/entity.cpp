/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "entity.h"
#include "gamecontext.h"

//////////////////////////////////////////////////
// Entity
//////////////////////////////////////////////////
CEntity::CEntity(CGameWorld *pGameWorld, int ObjType)
{
	m_pGameWorld = pGameWorld;

	m_ObjType = ObjType;
	m_Pos = vec2(0,0);
	m_ProximityRadius = 0;

	m_MarkedForDestroy = false;
	m_ID = Server()->SnapNewID();

	m_pPrevTypeEntity = 0;
	m_pNextTypeEntity = 0;
}

CEntity::~CEntity()
{
	GameWorld()->RemoveEntity(this);
	Server()->SnapFreeID(m_ID);
}

int CEntity::NetworkClipped(int SnappingClient)
{
	return NetworkClipped(SnappingClient, m_Pos);
}

int CEntity::NetworkClipped(int SnappingClient, vec2 CheckPos)
{
	if(SnappingClient == -1)
		return 0;

	float dx = GameServer()->m_apPlayers[SnappingClient]->m_ViewPos.x-CheckPos.x;
	float dy = GameServer()->m_apPlayers[SnappingClient]->m_ViewPos.y-CheckPos.y;

	if(absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
		return 1;

	if(distance(GameServer()->m_apPlayers[SnappingClient]->m_ViewPos, CheckPos) > 4000.0f)
		return 1;
	return 0;
}

bool CEntity::GameLayerClipped(vec2 CheckPos)
{
	return round_to_int(CheckPos.x)/32 < -200 || round_to_int(CheckPos.x)/32 > GameServer()->Collision()->GetWidth()+200 ||
			round_to_int(CheckPos.y)/32 < -200 || round_to_int(CheckPos.y)/32 > GameServer()->Collision()->GetHeight()+200 ? true : false;
}

bool CEntity::GetNearestAirPos(vec2 Pos, vec2 ColPos, vec2* pOutPos)
{
	int iter = 0;
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "ColPos: %f, %f", ColPos.x, ColPos.y);
	GameServer()->SendChat(-1, 0, aBuf);
	while (GameServer()->Collision()->CheckPoint(Pos)) {
		str_format(aBuf, sizeof(aBuf), "Pos (iter: %d): %f, %f", iter, Pos.x, Pos.y);
		GameServer()->SendChat(-1, 0, aBuf);
		vec2 normalized = normalize(ColPos - Pos);
		Pos += normalize(ColPos - Pos);
		str_format(aBuf, sizeof(aBuf), "Normalize (iter: %d): %f", iter, normalized.x, normalized.y);
		GameServer()->SendChat(-1, 0, aBuf);
		vec2 minus = ColPos - Pos;
		str_format(aBuf, sizeof(aBuf), "Minus (iter: %d): %f", iter, minus.x, minus.y);
		GameServer()->SendChat(-1, 0, aBuf);
		iter++;
	}
	
	str_format(aBuf, sizeof(aBuf), "Pos after loop: %f, %f", Pos.x, Pos.y);
	GameServer()->SendChat(-1, 0, aBuf);

	vec2 PosInBlock = vec2(round_to_int(Pos.x) % 32, round_to_int(Pos.y) % 32);
	vec2 BlockCenter = vec2(round_to_int(Pos.x), round_to_int(Pos.y)) - PosInBlock + vec2(16.0f, 16.0f);
	GameServer()->CreateExplosion(BlockCenter, 0, WEAPON_GRENADE, true, -1, -1LL);

	float Size = 31.0f; // A bit less than the tile size

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x < 16 ? -Size : Size), Pos.y);
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x < 16 ? -Size : Size), BlockCenter.y);
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(Pos.x, BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x, BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x  < 16 ? -Size : Size),
		BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	return false;
}

bool CEntity::GetNearestAirPosLaser(vec2 Pos, vec2* pOutPos)
{
	vec2 PosInBlock = vec2(round_to_int(Pos.x) % 32, round_to_int(Pos.y) % 32);
	vec2 BlockCenter = vec2(round_to_int(Pos.x), round_to_int(Pos.y)) - PosInBlock + vec2(16.0f, 16.0f);
	GameServer()->CreateExplosion(BlockCenter, 0, WEAPON_GRENADE, true, -1, -1LL);

	float Size = 31.0f; // A bit less than the tile size

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x < 16 ? -Size : Size), Pos.y);
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x < 16 ? -Size : Size), BlockCenter.y);
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(Pos.x, BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x, BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	*pOutPos = vec2(BlockCenter.x + (PosInBlock.x  < 16 ? -Size : Size),
		BlockCenter.y + (PosInBlock.y < 16 ? -Size : Size));
	if (!GameServer()->Collision()->TestBox(*pOutPos, vec2(28.0f, 28.0f)))
		return true;

	return false;
}

bool CEntity::GetNearestAirPosPlayer(vec2 PlayerPos, vec2* OutPos)
{
	int dist = 5;
	for (; dist >= -1; dist--)
	{
		*OutPos = vec2(PlayerPos.x, PlayerPos.y - dist);
		if (!GameServer()->Collision()->TestBox(*OutPos, vec2(28.0f, 28.0f)))
		{
			return true;
		}
	}
	return false;
}
