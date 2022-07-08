#include <limits.h>
#include "gtest/gtest.h"

#include "RK_finger.hpp"

TEST(RKfinger, initialize_correct)
{
	RKFinger rk;

	std::vector<uint8_t> init(48, 0xBE);
	EXPECT_EQ(rk.initialize(init), true);
}

TEST(RKfinger, initialize_incorrect)
{
	RKFinger rk;

	std::vector<uint8_t> init(42, 0xBE);					// Less than window size
	EXPECT_EQ(rk.initialize(init), false);
}

TEST(RKfinger, compute_next)
{
	RKFinger rk;

	std::vector<uint8_t> init(48, 0xBE);

	rk.initialize(init);
	EXPECT_EQ(rk.compute_next(10), 758716516);
	EXPECT_EQ(rk.compute_next(10), 957899876);
	EXPECT_EQ(rk.compute_next(255), 409232753);
	EXPECT_EQ(rk.compute_next(99), 1684369811);
}

TEST(RKfinger, get_alphabet_size)
{
	RKFinger rk(12, 30, 123009);
	
	EXPECT_EQ(rk.get_alphabet_size(), 12);
}

TEST(RKfinger, get_window_size)
{
	RKFinger rk(12, 30, 123009);
	
	EXPECT_EQ(rk.get_window_size(), 30);
}

TEST(RKfinger, get_modulus)
{
	RKFinger rk(12, 30, 123009);
	
	EXPECT_EQ(rk.get_modulus(), 123009);
}

TEST(RKfinger, default_params)
{
	RKFinger rk;

	EXPECT_EQ(rk.get_alphabet_size(), ALPHABET_DEF_SIZE);
	EXPECT_EQ(rk.get_window_size(), WINDOW_DEF_SIZE);
	EXPECT_EQ(rk.get_modulus(), INT_MAX);
}

TEST(RKfinger, get_current_fingerprint)
{
	RKFinger rk;

	std::vector<uint8_t> init(48, 0xBE);

	rk.initialize(init);
	EXPECT_EQ(rk.compute_next(10), 758716516);

	EXPECT_EQ(rk.get_current_fingerprint(), 758716516);
}