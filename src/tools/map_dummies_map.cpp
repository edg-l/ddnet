#include <base/logger.h>
#include <base/os.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/gamecore.h>
#include <game/mapitems.h>

#include <iterator>
#include <memory>

static const char *TOOL_NAME = "map_dummies_map";

static constexpr int LAYER_WIDTH = 64;
static constexpr int LAYER_HEIGHT = 32;

static int TileIndex(int x, int y)
{
	return y * LAYER_WIDTH + x;
}

static void SetTile(CTile *pTiles, int x, int y, int Index, int Flags = 0)
{
	pTiles[TileIndex(x, y)] = CTile{.m_Index = (unsigned char)Index, .m_Flags = (unsigned char)Flags, .m_Skip = 0, .m_MustBe0 = 0};
}

static void PlaceMapDummy(CTile *pTiles, int x, int y, bool Hammer, bool FacingLeft, const char *pLayerName)
{
	const int Index = ENTITY_OFFSET + (Hammer ? ENTITY_MAP_DUMMY_HAMMER : ENTITY_MAP_DUMMY);
	const int Flags = FacingLeft ? TILEFLAG_XFLIP : 0;
	SetTile(pTiles, x, y, Index, Flags);
	log_info(TOOL_NAME, "%s map dummy at (%d,%d) in the %s layer, facing %s",
		Hammer ? "hammer" : "plain", x, y, pLayerName, FacingLeft ? "left" : "right");
}

static void CreateMap(IStorage *pStorage)
{
	const char *pMapName = "maps/map_dummies.map";

	CDataFileWriter Writer;
	if(!Writer.Open(pStorage, pMapName))
	{
		log_error(TOOL_NAME, "Failed to open map '%s' for writing", pMapName);
		return;
	}

	{
		CMapItemVersion Item;
		Item.m_Version = 1;
		Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Item), &Item);
	}

	CMapItemGroup_v1 Group;
	Group.m_Version = 1;
	Group.m_OffsetX = 0;
	Group.m_OffsetY = 0;
	Group.m_ParallaxX = 100;
	Group.m_ParallaxY = 100;
	Group.m_StartLayer = 0;
	Group.m_NumLayers = 2;
	Writer.AddItem(MAPITEMTYPE_GROUP, 0, sizeof(Group), &Group);

	std::unique_ptr<CTile[]> pGameTiles(new CTile[LAYER_WIDTH * LAYER_HEIGHT]{});
	std::unique_ptr<CTile[]> pFrontTiles(new CTile[LAYER_WIDTH * LAYER_HEIGHT]{});
	CTile *pGame = pGameTiles.get();
	CTile *pFront = pFrontTiles.get();

	for(int x = 0; x < LAYER_WIDTH; x++)
		SetTile(pGame, x, 0, TILE_SOLID);
	for(int y = 0; y < LAYER_HEIGHT; y++)
	{
		SetTile(pGame, 0, y, TILE_SOLID);
		SetTile(pGame, LAYER_WIDTH - 1, y, TILE_SOLID);
	}
	for(int x = 0; x < LAYER_WIDTH; x++)
	{
		if(x != 60)
			SetTile(pGame, x, LAYER_HEIGHT - 1, TILE_SOLID);
	}

	// Floor, with a gap at x=60 for the (60,20) map dummy to fall through.
	for(int x = 1; x <= 62; x++)
	{
		if(x != 60)
			SetTile(pGame, x, 24, TILE_SOLID);
	}

	for(int x = 7; x <= 14; x++)
		SetTile(pGame, x, 20, TILE_SOLID);
	for(int x = 22; x <= 26; x++)
		SetTile(pGame, x, 20, TILE_SOLID);

	// Death tile on top of the first shelf, under the (12,16) map dummy.
	SetTile(pGame, 12, 19, TILE_DEATH);

	SetTile(pGame, 4, 23, ENTITY_OFFSET + ENTITY_SPAWN);
	for(int y = 1; y <= 23; y++)
		SetTile(pGame, 28, y, TILE_START);
	for(int y = 1; y <= 23; y++)
		SetTile(pGame, 56, y, TILE_FINISH);

	// Row-major, the order in which the server reads entities and hands out client ids.
	PlaceMapDummy(pGame, 12, 16, /*Hammer=*/false, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 8, 19, /*Hammer=*/false, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 24, 19, /*Hammer=*/false, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 60, 20, /*Hammer=*/false, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 3, 23, /*Hammer=*/true, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 32, 23, /*Hammer=*/false, /*FacingLeft=*/false, "game");
	PlaceMapDummy(pGame, 36, 23, /*Hammer=*/true, /*FacingLeft=*/true, "game");

	// Front-layer map dummies: (44,23) is valid, (46,24) sits on the solid floor and must be ignored.
	PlaceMapDummy(pFront, 44, 23, /*Hammer=*/false, /*FacingLeft=*/false, "front");
	PlaceMapDummy(pFront, 46, 24, /*Hammer=*/false, /*FacingLeft=*/false, "front");

	CMapItemLayerTilemap_v2 GameLayer;
	GameLayer.m_Layer.m_Version = 0;
	GameLayer.m_Layer.m_Type = LAYERTYPE_TILES;
	GameLayer.m_Layer.m_Flags = 0;
	GameLayer.m_Version = 2;
	GameLayer.m_Width = LAYER_WIDTH;
	GameLayer.m_Height = LAYER_HEIGHT;
	GameLayer.m_Flags = TILESLAYERFLAG_GAME;
	GameLayer.m_Color.r = 255;
	GameLayer.m_Color.g = 255;
	GameLayer.m_Color.b = 255;
	GameLayer.m_Color.a = 255;
	GameLayer.m_ColorEnv = -1;
	GameLayer.m_ColorEnvOffset = 0;
	GameLayer.m_Image = -1;
	GameLayer.m_Data = Writer.AddData(LAYER_WIDTH * LAYER_HEIGHT * sizeof(CTile), pGame);
	Writer.AddItem(MAPITEMTYPE_LAYER, 0, sizeof(GameLayer), &GameLayer);

	CMapItemLayerTilemap FrontLayer;
	FrontLayer.m_Layer.m_Version = 0;
	FrontLayer.m_Layer.m_Type = LAYERTYPE_TILES;
	FrontLayer.m_Layer.m_Flags = 0;
	FrontLayer.m_Version = 3;
	FrontLayer.m_Width = LAYER_WIDTH;
	FrontLayer.m_Height = LAYER_HEIGHT;
	FrontLayer.m_Flags = TILESLAYERFLAG_FRONT;
	FrontLayer.m_Color.r = 255;
	FrontLayer.m_Color.g = 255;
	FrontLayer.m_Color.b = 255;
	FrontLayer.m_Color.a = 255;
	FrontLayer.m_ColorEnv = -1;
	FrontLayer.m_ColorEnvOffset = 0;
	FrontLayer.m_Image = -1;
	StrToInts(FrontLayer.m_aName, std::size(FrontLayer.m_aName), "Front");
	FrontLayer.m_Tele = -1;
	FrontLayer.m_Speedup = -1;
	FrontLayer.m_Switch = -1;
	FrontLayer.m_Tune = -1;
	FrontLayer.m_Front = Writer.AddData(LAYER_WIDTH * LAYER_HEIGHT * sizeof(CTile), pFront);
	std::unique_ptr<CTile[]> pEmptyTiles(new CTile[LAYER_WIDTH * LAYER_HEIGHT]{});
	FrontLayer.m_Data = Writer.AddData(LAYER_WIDTH * LAYER_HEIGHT * sizeof(CTile), pEmptyTiles.get());
	Writer.AddItem(MAPITEMTYPE_LAYER, 1, sizeof(FrontLayer), &FrontLayer);

	Writer.Finish();

	log_info(TOOL_NAME, "Map dummies map written to '%s'", pMapName);
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	std::unique_ptr<IStorage> pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::SERVER, argc, argv));
	if(!pStorage)
	{
		log_error(TOOL_NAME, "Error creating server storage");
		return -1;
	}

	CreateMap(pStorage.get());
	return 0;
}
