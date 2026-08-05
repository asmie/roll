#include "gtest/gtest.h"

#include "ChunkIndex.hpp"
#include "DeltaFormat.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using Chunk = SignedChunk<uint64_t>;
using Index = ChunkIndex<uint64_t>;

Chunk make_chunk(uint64_t signature, uint8_t hash_seed, size_t size, size_t offset)
{
	Chunk c;
	c.signature = signature;
	c.hash.assign(DELTA_DIGEST_BYTES, hash_seed);
	c.chunk_size = size;
	c.start_offset = offset;
	return c;
}

} // namespace

TEST(ChunkIndex, finds_chunk_by_content)
{
	std::vector<Chunk> chunks{
		make_chunk(11, 0xA1, 512, 0),
		make_chunk(22, 0xB2, 512, 512),
		make_chunk(33, 0xC3, 512, 1024),
	};
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	size_t position = 999;
	ASSERT_TRUE(index.find_unused(used, chunks[1], position));
	EXPECT_EQ(position, 1u);
}

// SignedChunk::operator== ignores start_offset, so identical content at a
// different offset must still match. Moved-chunk detection depends on this.
TEST(ChunkIndex, matches_identical_content_at_a_different_offset)
{
	std::vector<Chunk> chunks{ make_chunk(11, 0xA1, 512, 0) };
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	const auto probe = make_chunk(11, 0xA1, 512, 4096);  // same content, elsewhere
	size_t position = 999;
	ASSERT_TRUE(index.find_unused(used, probe, position));
	EXPECT_EQ(position, 0u);
}

TEST(ChunkIndex, rejects_content_differing_in_digest_or_size)
{
	std::vector<Chunk> chunks{ make_chunk(11, 0xA1, 512, 0) };
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);
	size_t position = 999;

	EXPECT_FALSE(index.find_unused(used, make_chunk(11, 0xFF, 512, 0), position))
		<< "differing strong digest must not match";
	EXPECT_FALSE(index.find_unused(used, make_chunk(11, 0xA1, 256, 0), position))
		<< "differing chunk size must not match";
}

// Identity is the strong digest plus the length. The rolling signature exists to
// find boundaries, so two chunks agreeing on digest and size are the same
// content whatever their fingerprints say — which is what lets the wire format
// omit the signature entirely.
TEST(ChunkIndex, identity_ignores_the_rolling_signature)
{
	const Chunk indexed = make_chunk(11, 0xA1, 512, 0);
	Chunk probe = make_chunk(99, 0xA1, 512, 4096);  // same digest and size

	EXPECT_TRUE(indexed == probe);

	std::vector<Chunk> chunks{ indexed };
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	size_t position = 999;
	ASSERT_TRUE(index.find_unused(used, probe, position));
	EXPECT_EQ(position, 0u);
}

// Every copy of a repeated content must be consumable, one at a time, each
// yielding a distinct position. This is what lets a delta reuse every copy of
// a duplicated chunk instead of only the first.
TEST(ChunkIndex, duplicates_are_consumed_one_position_at_a_time)
{
	std::vector<Chunk> chunks{
		make_chunk(11, 0xA1, 512, 0),
		make_chunk(11, 0xA1, 512, 512),   // duplicate content
		make_chunk(11, 0xA1, 512, 1024),  // duplicate content
	};
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	std::vector<size_t> consumed;
	for (int i = 0; i < 3; ++i) {
		size_t position = 999;
		ASSERT_TRUE(index.find_unused(used, chunks[0], position))
			<< "duplicate copy " << i << " was not reachable";
		ASSERT_LT(position, chunks.size());
		EXPECT_FALSE(used[position]) << "returned an already-consumed position";
		used[position] = true;
		consumed.push_back(position);
	}

	std::sort(consumed.begin(), consumed.end());
	EXPECT_EQ(consumed, std::vector<size_t>({0u, 1u, 2u}));

	// A fourth request has nothing left to consume.
	size_t position = 999;
	EXPECT_FALSE(index.find_unused(used, chunks[0], position));
}

// Delta's same-position fast path consumes chunks without going through the
// index, so the cursor must correctly step over externally-consumed positions —
// and must never skip a position that is still live.
TEST(ChunkIndex, respects_flags_set_outside_the_index)
{
	std::vector<Chunk> chunks{
		make_chunk(11, 0xA1, 512, 0),
		make_chunk(11, 0xA1, 512, 512),
		make_chunk(11, 0xA1, 512, 1024),
	};
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	// Consume the middle copy externally, as the same-position path would.
	used[1] = true;

	size_t position = 999;
	ASSERT_TRUE(index.find_unused(used, chunks[0], position));
	EXPECT_EQ(position, 0u);
	used[0] = true;

	// The next lookup must step over both consumed copies and land on the last.
	ASSERT_TRUE(index.find_unused(used, chunks[0], position));
	EXPECT_EQ(position, 2u);
	used[2] = true;

	EXPECT_FALSE(index.find_unused(used, chunks[0], position));
}

// Until the caller marks the returned position used, repeated lookups must
// keep returning it — the index reports availability, it does not consume.
TEST(ChunkIndex, lookup_without_consumption_is_stable)
{
	std::vector<Chunk> chunks{ make_chunk(11, 0xA1, 512, 0),
	                           make_chunk(11, 0xA1, 512, 512) };
	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	size_t first = 999, second = 999;
	ASSERT_TRUE(index.find_unused(used, chunks[0], first));
	ASSERT_TRUE(index.find_unused(used, chunks[0], second));
	EXPECT_EQ(first, second);
}

TEST(ChunkIndex, empty_index_matches_nothing)
{
	const std::vector<Chunk> chunks;
	Index index(chunks);
	const std::vector<bool> used;

	size_t position = 999;
	EXPECT_FALSE(index.find_unused(used, make_chunk(11, 0xA1, 512, 0), position));
}

// The consumption cursor makes stepping past consumed duplicates O(1)
// amortized. The predecessor rescanned the whole chunk list per lookup, which
// went quadratic on repetitive input — this sizing (50k duplicates) finishes
// in milliseconds amortized but took noticeable seconds quadratically. The
// assertion is on the returned positions; the timing difference is what CI
// timeouts would surface.
TEST(ChunkIndex, consuming_many_duplicates_stays_cheap)
{
	constexpr size_t COPIES = 50000;
	std::vector<Chunk> chunks;
	chunks.reserve(COPIES);
	for (size_t i = 0; i < COPIES; ++i)
		chunks.push_back(make_chunk(11, 0xA1, 512, i * 512));

	Index index(chunks);
	std::vector<bool> used(chunks.size(), false);

	for (size_t i = 0; i < COPIES; ++i) {
		size_t position = 999;
		ASSERT_TRUE(index.find_unused(used, chunks[0], position))
			<< "copy " << i << " unreachable";
		ASSERT_FALSE(used[position]);
		used[position] = true;
	}

	size_t position = 999;
	EXPECT_FALSE(index.find_unused(used, chunks[0], position));
}

// The mix folds the strong hash in by addition rather than `h2 << 1`, which
// discarded the top bit of the first hash word outright. Two distinct chunks
// sharing a rolling signature whose strong hashes differ only in that bit
// therefore collided under the old mix; they must not collide here.
TEST(ChunkIndex, hash_uses_the_top_bit_of_the_strong_hash)
{
	Chunk a = make_chunk(4242, 0x11, 512, 0);
	Chunk b = a;
	b.hash[7] ^= 0x80;  // bit 63 of the first 8-byte word

	ASSERT_FALSE(a == b) << "the two chunks must actually differ";

	ChunkHash<uint64_t> hasher;
	EXPECT_NE(hasher(a), hasher(b))
		<< "distinct chunks collided, so the top hash bit is being dropped";
}

// Distinct content across a run of chunks should spread across distinct hashes.
TEST(ChunkIndex, distinct_chunks_get_distinct_hashes)
{
	ChunkHash<uint64_t> hasher;
	std::vector<size_t> hashes;
	for (uint64_t sig = 1000; sig < 1016; ++sig) {
		Chunk c = make_chunk(sig, 0xD4, 512, 0);
		c.hash[0] = static_cast<uint8_t>(sig & 0xFF);
		hashes.push_back(hasher(c));
	}

	std::sort(hashes.begin(), hashes.end());
	const auto duplicates = std::unique(hashes.begin(), hashes.end());
	EXPECT_EQ(duplicates, hashes.end()) << "hash collided across distinct chunks";
}
