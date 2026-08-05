#include "gtest/gtest.h"

#include "ChunkIndex.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using Chunk = SignedChunk<uint64_t>;

Chunk make_chunk(uint64_t signature, uint8_t hash_seed, size_t size, size_t offset)
{
	Chunk c;
	c.signature = signature;
	c.hash.assign(64, hash_seed);
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
	const auto map = build_chunk_map(chunks);
	std::vector<bool> used(chunks.size(), false);

	size_t index = 999;
	ASSERT_TRUE(find_unused_match(chunks, used, map, chunks[1], index));
	EXPECT_EQ(index, 1u);
}

// SignedChunk::operator== ignores start_offset, so identical content at a
// different offset must still match. Moved-chunk detection depends on this.
TEST(ChunkIndex, matches_identical_content_at_a_different_offset)
{
	std::vector<Chunk> chunks{ make_chunk(11, 0xA1, 512, 0) };
	const auto map = build_chunk_map(chunks);
	std::vector<bool> used(chunks.size(), false);

	const auto probe = make_chunk(11, 0xA1, 512, 4096);  // same content, elsewhere
	size_t index = 999;
	ASSERT_TRUE(find_unused_match(chunks, used, map, probe, index));
	EXPECT_EQ(index, 0u);
}

TEST(ChunkIndex, rejects_content_differing_in_hash_or_size)
{
	std::vector<Chunk> chunks{ make_chunk(11, 0xA1, 512, 0) };
	const auto map = build_chunk_map(chunks);
	std::vector<bool> used(chunks.size(), false);
	size_t index = 999;

	EXPECT_FALSE(find_unused_match(chunks, used, map,
	                               make_chunk(11, 0xFF, 512, 0), index))
		<< "differing strong hash must not match";
	EXPECT_FALSE(find_unused_match(chunks, used, map,
	                               make_chunk(11, 0xA1, 256, 0), index))
		<< "differing chunk size must not match";
	EXPECT_FALSE(find_unused_match(chunks, used, map,
	                               make_chunk(99, 0xA1, 512, 0), index))
		<< "differing rolling signature must not match";
}

// The map keeps only one position per distinct content, so consuming duplicates
// depends on the fallback scan. This is what lets a delta reuse every copy of a
// repeated chunk instead of only the last one.
TEST(ChunkIndex, falls_back_to_scan_when_the_indexed_position_is_consumed)
{
	std::vector<Chunk> chunks{
		make_chunk(11, 0xA1, 512, 0),
		make_chunk(11, 0xA1, 512, 512),   // duplicate content
		make_chunk(11, 0xA1, 512, 1024),  // duplicate content
	};
	const auto map = build_chunk_map(chunks);
	std::vector<bool> used(chunks.size(), false);

	// All three must be consumable one at a time, each yielding a distinct index.
	std::vector<size_t> found;
	for (int i = 0; i < 3; ++i) {
		size_t index = 999;
		ASSERT_TRUE(find_unused_match(chunks, used, map, chunks[0], index))
			<< "duplicate copy " << i << " was not reachable";
		ASSERT_LT(index, chunks.size());
		EXPECT_FALSE(used[index]) << "returned an already-consumed position";
		used[index] = true;
		found.push_back(index);
	}

	std::sort(found.begin(), found.end());
	EXPECT_EQ(found, std::vector<size_t>({0u, 1u, 2u}));

	// A fourth request has nothing left to consume.
	size_t index = 999;
	EXPECT_FALSE(find_unused_match(chunks, used, map, chunks[0], index));
}

TEST(ChunkIndex, empty_index_matches_nothing)
{
	const std::vector<Chunk> chunks;
	const auto map = build_chunk_map(chunks);
	const std::vector<bool> used;

	size_t index = 999;
	EXPECT_FALSE(find_unused_match(chunks, used, map,
	                               make_chunk(11, 0xA1, 512, 0), index));
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
