#include <game/server/playermapping.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <vector>

TEST(PlayerMapping, IdentitySlotPreferred)
{
	int aMap[8];
	std::fill(std::begin(aMap), std::end(aMap), -1);
	int aFreedTick[8];
	std::fill(std::begin(aFreedTick), std::end(aFreedTick), -1);

	// Every slot is free, so a plain first-free scan would pick 0, but the
	// candidate's own id must win.
	std::optional<int> Slot = CPlayerMapping::ChooseSlot(aMap, 8, 0, true, 3, aFreedTick, 100);
	ASSERT_TRUE(Slot.has_value());
	EXPECT_EQ(*Slot, 3);
}

TEST(PlayerMapping, LegacyTakesFirstFree)
{
	std::mt19937 Rng(7);
	std::uniform_int_distribution<int> SizeDist(1, 64);
	std::uniform_int_distribution<int> SeeOthersDist(0, 4);
	std::uniform_int_distribution<int> OccupantDist(0, 1);

	for(int Trial = 0; Trial < 10000; Trial++)
	{
		const int MapSize = SizeDist(Rng);
		const int NumSeeOthers = std::min(SeeOthersDist(Rng), MapSize - 1);

		std::vector<int> vMap(MapSize);
		for(int &Occupant : vMap)
			Occupant = OccupantDist(Rng) ? 0 : -1;
		std::vector<int> vFreedTick(MapSize, -1);

		// Verbatim copy of the reference loop this replaces.
		int Reference = -1;
		for(int j = 0; j < MapSize - NumSeeOthers; j++)
			if(vMap[j] == -1)
			{
				Reference = j;
				break;
			}

		std::optional<int> Slot = CPlayerMapping::ChooseSlot(vMap.data(), MapSize, NumSeeOthers, false, -1, vFreedTick.data(), 0);
		if(Reference == -1)
			EXPECT_FALSE(Slot.has_value());
		else
			EXPECT_EQ(Slot, Reference);
	}
}

TEST(PlayerMapping, QuarantinedSlotSkipped)
{
	int aMap[4] = {-1, -1, -1, -1};
	int aFreedTick[4] = {100, -1, -1, -1};

	// Slot 0 was just freed this tick, so both identity and plain scans skip it.
	std::optional<int> Identity = CPlayerMapping::ChooseSlot(aMap, 4, 0, true, 0, aFreedTick, 100);
	ASSERT_TRUE(Identity.has_value());
	EXPECT_EQ(*Identity, 1);

	std::optional<int> Plain = CPlayerMapping::ChooseSlot(aMap, 4, 0, false, -1, aFreedTick, 100);
	ASSERT_TRUE(Plain.has_value());
	EXPECT_EQ(*Plain, 1);

	// A tick later the quarantine is over.
	std::optional<int> NextUpdate = CPlayerMapping::ChooseSlot(aMap, 4, 0, false, -1, aFreedTick, 101);
	ASSERT_TRUE(NextUpdate.has_value());
	EXPECT_EQ(*NextUpdate, 0);
}

TEST(PlayerMapping, EvictsFarthestUnreserved)
{
	// Slots 0..2 hold clients 0..2, distances 10, 30, 20.
	int aMap[3] = {0, 1, 2};
	bool aReserved[3] = {false, false, false};
	bool aPriority[3] = {false, false, false};
	float aDistSq[3] = {10.0f, 30.0f, 20.0f};

	std::optional<int> Eviction = CPlayerMapping::ChooseEviction(aMap, 3, 0, aReserved, aPriority, aDistSq);
	ASSERT_TRUE(Eviction.has_value());
	EXPECT_EQ(*Eviction, 1); // slot 1 holds client 1, the farthest
}

TEST(PlayerMapping, NeverEvictsPriority)
{
	// Slot 1 (client 1) is farthest but a priority map dummy; slot 2 (client 2) follows.
	int aMap[3] = {0, 1, 2};
	bool aReserved[3] = {false, false, false};
	bool aPriority[3] = {false, true, false};
	float aDistSq[3] = {10.0f, 30.0f, 20.0f};

	std::optional<int> Eviction = CPlayerMapping::ChooseEviction(aMap, 3, 0, aReserved, aPriority, aDistSq);
	ASSERT_TRUE(Eviction.has_value());
	EXPECT_EQ(*Eviction, 2); // slot 2 holds client 2, the farthest non-priority
}
