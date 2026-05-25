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

TEST(RKfinger, compute_next_matches_fresh_initialize)
{
	// Rolling forward N bytes must yield the same fingerprint as initializing
	// fresh on the shifted window. This is the defining property of a rolling
	// hash and is independent of any specific modulus/alphabet choice.
	std::vector<uint8_t> data(WINDOW_DEF_SIZE * 2);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>(i * 37u + 13u);

	RKFinger rolled;
	ASSERT_TRUE(rolled.initialize(std::span<const uint8_t>{data.data(), WINDOW_DEF_SIZE}));
	for (size_t i = WINDOW_DEF_SIZE; i < data.size(); ++i)
		rolled.compute_next(data[i]);

	RKFinger fresh;
	ASSERT_TRUE(fresh.initialize(std::span<const uint8_t>{
		data.data() + (data.size() - WINDOW_DEF_SIZE), WINDOW_DEF_SIZE}));

	EXPECT_EQ(rolled.get_current_fingerprint(), fresh.get_current_fingerprint());
}

TEST(RKfinger, compute_next_distinguishes_content)
{
	std::vector<uint8_t> a(WINDOW_DEF_SIZE, 0xBE);
	std::vector<uint8_t> b(WINDOW_DEF_SIZE, 0xBE);
	b[WINDOW_DEF_SIZE / 2] ^= 0x01;

	RKFinger ra, rb;
	ASSERT_TRUE(ra.initialize(a));
	ASSERT_TRUE(rb.initialize(b));
	EXPECT_NE(ra.get_current_fingerprint(), rb.get_current_fingerprint());
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

TEST(RKfinger, ctor_rejects_zero_window)
{
	EXPECT_THROW(RKFinger(256, 0, 123009), std::invalid_argument);
}

TEST(RKfinger, ctor_rejects_zero_modulus)
{
	EXPECT_THROW(RKFinger(256, 48, 0), std::invalid_argument);
}

TEST(RKfinger, default_params)
{
	RKFinger rk;

	EXPECT_EQ(rk.get_alphabet_size(), ALPHABET_DEF_SIZE);
	EXPECT_EQ(rk.get_window_size(), WINDOW_DEF_SIZE);
	EXPECT_EQ(rk.get_modulus(), INT_MAX);
}

TEST(RKfinger, get_current_fingerprint_tracks_compute_next)
{
	RKFinger rk;
	std::vector<uint8_t> init(WINDOW_DEF_SIZE, 0xBE);

	ASSERT_TRUE(rk.initialize(init));
	const uint64_t after_roll = rk.compute_next(10);
	EXPECT_EQ(rk.get_current_fingerprint(), after_roll);
}