#include "gtest/gtest.h"

#include "RK_finger.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace {

// Independent reference: the polynomial window hash computed straight from the
// definition with a plain remainder. reduce()'s Mersenne fast path must agree
// with this exactly, or chunk boundaries (and every delta) would shift.
uint64_t naive_window_hash(std::span<const uint8_t> window, uint64_t alphabet,
                           uint64_t modulus)
{
	uint64_t h = 0;
	for (const uint8_t b : window)
		h = (alphabet * h + b) % modulus;
	return h;
}

std::vector<uint8_t> patterned_data(size_t n, uint32_t seed)
{
	std::vector<uint8_t> data(n);
	uint32_t x = seed;
	for (auto& b : data) {
		x = x * 1103515245u + 12345u;
		b = static_cast<uint8_t>(x >> 16);
	}
	return data;
}

} // namespace

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
	EXPECT_EQ(rk.get_modulus(), MODULUS_DEF_SIZE);
}

TEST(RKfinger, get_current_fingerprint_tracks_compute_next)
{
	RKFinger rk;
	std::vector<uint8_t> init(WINDOW_DEF_SIZE, 0xBE);

	ASSERT_TRUE(rk.initialize(init));
	const uint64_t after_roll = rk.compute_next(10);
	EXPECT_EQ(rk.get_current_fingerprint(), after_roll);
}
// reduce() takes a Mersenne fast path on the default M31 modulus. Every rolled
// fingerprint must match the naive definition at every position, otherwise the
// optimisation silently changes chunking.
TEST(RKfinger, mersenne_reduction_matches_naive_at_every_position)
{
	const auto data = patterned_data(4096, 0xA5A5u);

	RKFinger rk;
	ASSERT_TRUE(rk.initialize(std::span<const uint8_t>{data.data(), WINDOW_DEF_SIZE}));
	EXPECT_EQ(rk.get_current_fingerprint(),
	          naive_window_hash(std::span<const uint8_t>{data.data(), WINDOW_DEF_SIZE},
	                            ALPHABET_DEF_SIZE, MODULUS_DEF_SIZE));

	for (size_t i = WINDOW_DEF_SIZE; i < data.size(); ++i) {
		const uint64_t rolled = rk.compute_next(data[i]);
		const auto window = std::span<const uint8_t>{
			data.data() + (i - WINDOW_DEF_SIZE + 1), WINDOW_DEF_SIZE};
		ASSERT_EQ(rolled, naive_window_hash(window, ALPHABET_DEF_SIZE, MODULUS_DEF_SIZE))
			<< "diverged from the naive reference at byte " << i;
	}
}

// A non-Mersenne modulus must fall back to the plain remainder and stay
// correct. 123010 is not a power of two, so no fast path applies here.
TEST(RKfinger, non_mersenne_modulus_matches_naive)
{
	constexpr uint64_t MOD = 123009;
	constexpr unsigned ALPHA = 251;
	constexpr unsigned WIN = 30;
	const auto data = patterned_data(2048, 0x5A5Au);

	RKFinger rk(ALPHA, WIN, MOD);
	ASSERT_TRUE(rk.initialize(std::span<const uint8_t>{data.data(), WIN}));
	EXPECT_EQ(rk.get_current_fingerprint(),
	          naive_window_hash(std::span<const uint8_t>{data.data(), WIN}, ALPHA, MOD));

	for (size_t i = WIN; i < data.size(); ++i) {
		const uint64_t rolled = rk.compute_next(data[i]);
		const auto window = std::span<const uint8_t>{data.data() + (i - WIN + 1), WIN};
		ASSERT_EQ(rolled, naive_window_hash(window, ALPHA, MOD))
			<< "diverged from the naive reference at byte " << i;
	}
}

// A Mersenne modulus small enough that a single fold would *not* land in range
// must also stay correct — configure_fast_reduction has to reject it rather
// than enable an unsafe fold. 2^5-1 = 31 with alphabet 256 is such a case.
TEST(RKfinger, small_mersenne_modulus_matches_naive)
{
	constexpr uint64_t MOD = 31;  // 2^5 - 1
	constexpr unsigned WIN = 16;
	const auto data = patterned_data(1024, 0x1234u);

	RKFinger rk(ALPHABET_DEF_SIZE, WIN, MOD);
	ASSERT_TRUE(rk.initialize(std::span<const uint8_t>{data.data(), WIN}));
	EXPECT_EQ(rk.get_current_fingerprint(),
	          naive_window_hash(std::span<const uint8_t>{data.data(), WIN},
	                            ALPHABET_DEF_SIZE, MOD));

	for (size_t i = WIN; i < data.size(); ++i) {
		const uint64_t rolled = rk.compute_next(data[i]);
		const auto window = std::span<const uint8_t>{data.data() + (i - WIN + 1), WIN};
		ASSERT_EQ(rolled, naive_window_hash(window, ALPHABET_DEF_SIZE, MOD))
			<< "diverged from the naive reference at byte " << i;
	}
}

TEST(RKfinger, rejects_overflowing_custom_parameters)
{
	EXPECT_THROW(RKFinger(256, 48, uint64_t{18446744073709551557ULL}), std::invalid_argument);
	EXPECT_THROW(RKFinger(256, 48, uint64_t{1} << 63), std::invalid_argument);
	EXPECT_THROW(RKFinger(0, 48, 123009), std::invalid_argument);
}

TEST(RKfinger, small_bases_handle_the_full_byte_range)
{
	const auto data = patterned_data(2048, 0x1234u);
	for (unsigned base : {1u, 2u, 17u}) {
		RKFinger rk(base, 16, 1023);
		ASSERT_TRUE(rk.initialize(std::span<const uint8_t>(data).first(16)));
		for (size_t i = 16; i < data.size(); ++i)
			EXPECT_EQ(rk.compute_next(data[i]), naive_window_hash(std::span<const uint8_t>(data).subspan(i - 15, 16), base, 1023));
	}
}
