#include <game/mapitems.h>

#include <gtest/gtest-printers.h>
#include <gtest/gtest.h>

namespace testing::internal
{
	template<>
	class UniversalPrinter<CFixedTime>
	{
	public:
		static void Print(const CFixedTime &FixedTime, std::ostream *pOutputStream)
		{
			*pOutputStream << "CFixedTime with internal value " << FixedTime.GetInternal();
		}
	};

}

TEST(Mapitems, FixedTimeRoundtrip)
{
	for(CFixedTime Fixed = CFixedTime(0); Fixed < CFixedTime(1000000); Fixed += CFixedTime(1))
	{
		ASSERT_EQ(Fixed, CFixedTime::FromSeconds(Fixed.AsSeconds()));
	}
}

TEST(Mapitems, MapDummyTiles)
{
	EXPECT_TRUE(IsValidEntity(ENTITY_OFFSET + ENTITY_MAP_DUMMY));
	EXPECT_TRUE(IsValidEntity(ENTITY_OFFSET + ENTITY_MAP_DUMMY_HAMMER));
	EXPECT_TRUE(IsRotatableTile(ENTITY_OFFSET + ENTITY_MAP_DUMMY));
	EXPECT_TRUE(IsRotatableTile(ENTITY_OFFSET + ENTITY_MAP_DUMMY_HAMMER));
	EXPECT_FALSE(IsValidSwitchTile(ENTITY_OFFSET + ENTITY_MAP_DUMMY));
	EXPECT_FALSE(IsValidSwitchTile(ENTITY_OFFSET + ENTITY_MAP_DUMMY_HAMMER));

	EXPECT_TRUE(IsValidSwitchTile(ENTITY_OFFSET + ENTITY_DOOR));

	EXPECT_FALSE(IsValidEntity(ENTITY_OFFSET + 52));
}
