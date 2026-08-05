#include "gtest/gtest.h"

#include "HashConcepts.hpp"
#include "RK_finger.hpp"
#include "blake2b.h"

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace {

// A conforming rolling hash that inherits from nothing. The previous concepts
// checked std::derived_from against an abstract base, so a type like this was
// rejected however correct it was; constraining the operations instead makes the
// abstraction open to any implementation that provides them.
struct CountingFingerprint {
	using RollingHashType = uint64_t;

	bool initialize(std::span<const uint8_t> window) noexcept
	{
		value_ = 0;
		for (const uint8_t b : window)
			value_ += b;
		return window.size() >= 4;
	}
	uint64_t compute_next(uint8_t byte) noexcept { return value_ += byte; }
	uint64_t get_current_fingerprint() const noexcept { return value_; }
	unsigned int get_window_size() const noexcept { return 4; }
	unsigned int get_alphabet_size() const noexcept { return 256; }

private:
	uint64_t value_ { 0 };
};

// A conforming strong hash that inherits from nothing, using the shared one-shot
// helper that used to be a virtual default on the base class.
struct SumHash {
	static constexpr size_t HASH_SIZE = 4;

	size_t get_hash_size() const noexcept { return HASH_SIZE; }
	void init() { sum_ = 0; }
	void update(std::span<const uint8_t> in)
	{
		for (const uint8_t b : in)
			sum_ += b;
	}
	void finalize(std::span<uint8_t> out)
	{
		if (out.size() < HASH_SIZE) return;
		for (size_t i = 0; i < HASH_SIZE; ++i)
			out[i] = static_cast<uint8_t>(sum_ >> (8 * i));
	}
	void hash(std::span<uint8_t> out, std::span<const uint8_t> in)
	{
		hash_oneshot(*this, out, in);
	}

private:
	uint32_t sum_ { 0 };
};

// Non-conforming types, each missing exactly one requirement.
struct MissingTypedef {
	bool initialize(std::span<const uint8_t>) noexcept { return true; }
	uint64_t compute_next(uint8_t) noexcept { return 0; }
	uint64_t get_current_fingerprint() const noexcept { return 0; }
	unsigned int get_window_size() const noexcept { return 1; }
	unsigned int get_alphabet_size() const noexcept { return 256; }
};

struct WrongComputeNextType {
	using RollingHashType = uint64_t;
	bool initialize(std::span<const uint8_t>) noexcept { return true; }
	uint32_t compute_next(uint8_t) noexcept { return 0; }  // not RollingHashType
	uint64_t get_current_fingerprint() const noexcept { return 0; }
	unsigned int get_window_size() const noexcept { return 1; }
	unsigned int get_alphabet_size() const noexcept { return 256; }
};

struct MissingFinalize {
	size_t get_hash_size() const noexcept { return 4; }
	void init() {}
	void update(std::span<const uint8_t>) {}
	void hash(std::span<uint8_t>, std::span<const uint8_t>) {}
};

} // namespace

// The shipped implementations conform. These also assert at namespace scope in
// their own headers; repeated here so a concept regression names the concept.
static_assert(RollingHashAlgorithm<RKFinger>);
static_assert(StrongHashAlgorithm<BLAKE2b>);

// Conformance no longer requires inheritance.
static_assert(RollingHashAlgorithm<CountingFingerprint>,
              "a duck-typed rolling hash must satisfy the concept");
static_assert(StrongHashAlgorithm<SumHash>,
              "a duck-typed strong hash must satisfy the concept");

// And the concepts still reject types that miss a requirement.
static_assert(!RollingHashAlgorithm<MissingTypedef>);
static_assert(!RollingHashAlgorithm<WrongComputeNextType>);
static_assert(!StrongHashAlgorithm<MissingFinalize>);
static_assert(!StrongHashAlgorithm<int>);
static_assert(!RollingHashAlgorithm<int>);

// The concepts are compile-time claims; this checks the duck-typed pair actually
// works when driven, so conformance is not merely syntactic.
TEST(HashConcepts, duck_typed_strong_hash_streams_and_one_shots_alike)
{
	const std::vector<uint8_t> input{1, 2, 3, 4, 5, 6, 7, 8};

	SumHash one_shot;
	std::vector<uint8_t> expected(SumHash::HASH_SIZE);
	one_shot.hash(expected, input);

	SumHash streamed;
	streamed.init();
	streamed.update(std::span<const uint8_t>{input.data(), 3});
	streamed.update(std::span<const uint8_t>{input.data() + 3, input.size() - 3});
	std::vector<uint8_t> got(SumHash::HASH_SIZE);
	streamed.finalize(got);

	EXPECT_EQ(got, expected) << "hash_oneshot must agree with init/update/finalize";
	EXPECT_EQ(got[0], 36u) << "1+2+...+8 = 36";
}

TEST(HashConcepts, one_shot_helper_refuses_an_undersized_destination)
{
	SumHash hash;
	std::vector<uint8_t> too_small(SumHash::HASH_SIZE - 1, 0xEE);
	const std::vector<uint8_t> input{1, 2, 3};

	hash.hash(too_small, input);

	EXPECT_EQ(too_small, std::vector<uint8_t>(SumHash::HASH_SIZE - 1, 0xEE))
		<< "an undersized destination must be left untouched, not partly written";
}

TEST(HashConcepts, duck_typed_rolling_hash_rolls)
{
	const std::vector<uint8_t> window{10, 20, 30, 40};

	CountingFingerprint fp;
	ASSERT_TRUE(fp.initialize(window));
	EXPECT_EQ(fp.get_current_fingerprint(), 100u);
	EXPECT_EQ(fp.compute_next(5), 105u);
	EXPECT_EQ(fp.get_current_fingerprint(), 105u);

	// A short window reports that there is nothing to roll.
	CountingFingerprint tiny;
	EXPECT_FALSE(tiny.initialize(std::span<const uint8_t>{window.data(), 2}));
}

// Neither implementation should carry a vtable: both are selected as template
// arguments, and nothing in the project holds a base pointer to them.
TEST(HashConcepts, implementations_are_not_polymorphic)
{
	EXPECT_FALSE(std::is_polymorphic_v<RKFinger>);
	EXPECT_FALSE(std::is_polymorphic_v<BLAKE2b>);
	EXPECT_FALSE(std::has_virtual_destructor_v<RKFinger>);
	EXPECT_FALSE(std::has_virtual_destructor_v<BLAKE2b>);
}
