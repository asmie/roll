#ifndef CHUNKINDEX_HPP
#define CHUNKINDEX_HPP

#include "Signature.hpp"

#include <cstddef>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

/**
* Content-addressed index over a signature's chunks, mapping chunk content to
* the position it occupies.
*
* Delta and Apply both need this, and both carried their own verbatim copy of
* the hash and equality functors. The copies drifted: `h1 ^ (h2 << 1)` was
* found to collide too easily on adjacent signatures sharing a hash prefix and
* was replaced with a hash_combine-style mix in one copy, while the other kept
* the weak version. A single definition is what stops that from recurring.
*/
template <class T>
struct ChunkHash {
	size_t operator()(const SignedChunk<T>& chunk) const {
		size_t h1 = std::hash<T>{}(chunk.signature);
		size_t h2 = 0;
		if (chunk.hash.size() >= sizeof(size_t))
			std::memcpy(&h2, chunk.hash.data(), sizeof(size_t));
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
	}
};

/**
* Equality for the index. SignedChunk::operator== deliberately ignores
* start_offset, so equal content at different offsets compares equal — which is
* what makes moved-chunk detection work.
*/
template <class T>
struct ChunkEqual {
	bool operator()(const SignedChunk<T>& a, const SignedChunk<T>& b) const {
		return a == b;
	}
};

template <class T>
using ChunkMap = std::unordered_map<SignedChunk<T>, size_t, ChunkHash<T>, ChunkEqual<T>>;

/**
* Index `chunks` by content. When the same content occurs more than once, the
* map retains the last occurrence; callers that must consume each occurrence
* separately should pair this with find_unused_match().
*/
template <class T>
[[nodiscard]] ChunkMap<T> build_chunk_map(const std::vector<SignedChunk<T>>& chunks) {
	ChunkMap<T> map;
	map.reserve(chunks.size());
	for (size_t i = 0; i < chunks.size(); ++i)
		map[chunks[i]] = i;
	return map;
}

/**
* Locate a not-yet-consumed chunk equal to `probe`.
*
* Prefers the position the map records. Falls back to a linear scan when that
* position is already consumed, which is exactly the duplicate-content case the
* map cannot represent on its own.
*
* @param[in] chunks chunk list the index was built over
* @param[in] used per-chunk consumed flags, parallel to `chunks`
* @param[in] map index produced by build_chunk_map()
* @param[in] probe content to look for
* @param[out] out_index position of the match, set only on success
* @return True if an unused match was found.
*/
template <class T>
[[nodiscard]] bool find_unused_match(const std::vector<SignedChunk<T>>& chunks,
                       const std::vector<bool>& used,
                       const ChunkMap<T>& map,
                       const SignedChunk<T>& probe,
                       size_t& out_index) {
	auto it = map.find(probe);

	// A miss means no chunk has this content at all, so the scan below could not
	// succeed either. Returning here keeps a lookup that finds nothing at O(1):
	// scanning on every miss would make a caller that probes each of n chunks
	// against a non-matching index cost O(n^2).
	if (it == map.end())
		return false;

	if (!used[it->second]) {
		out_index = it->second;
		return true;
	}

	// The indexed position is taken. The map holds one position per distinct
	// content, so another copy may still be free — scan for it.
	for (size_t i = 0; i < chunks.size(); ++i) {
		if (!used[i] && chunks[i] == probe) {
			out_index = i;
			return true;
		}
	}
	return false;
}

#endif // CHUNKINDEX_HPP
