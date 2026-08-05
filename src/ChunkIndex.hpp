#ifndef CHUNKINDEX_HPP
#define CHUNKINDEX_HPP

#include "Signature.hpp"

#include <cstddef>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

/**
* Hash for content-addressed chunk lookup.
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

/**
* Content-addressed index over a signature's chunks, answering "give me a
* not-yet-consumed chunk with this content" in O(1) amortized.
*
* Each distinct content maps to every position it occupies plus a consumption
* cursor. The predecessor design mapped content to a single position and fell
* back to a linear scan over all chunks whenever that position was already
* consumed. On repetitive input — zero padding, VM images — where thousands of
* chunks share one content, the scan ran per lookup and the whole delta
* generation went quadratic: 40 MB of zeros appended to 40 MB of zeros took
* 6.2s to create and 3.2s to apply, roughly quadrupling with each doubling of
* input.
*
* The cursor is safe because consumption is monotone: callers only ever flip
* `used` flags from false to true (some through this index, some through the
* same-position fast path that bypasses it). An entry the cursor has skipped as
* consumed can therefore never become live again, and each bucket entry is
* passed at most once across the index's lifetime.
*/
template <class T>
class ChunkIndex {
public:
	/// Index every position of `chunks` by content. Positions within a bucket
	/// are ascending, so matches are consumed in file order.
	explicit ChunkIndex(const std::vector<SignedChunk<T>>& chunks) {
		buckets_.reserve(chunks.size());
		for (size_t i = 0; i < chunks.size(); ++i)
			buckets_[chunks[i]].positions.push_back(i);
	}

	/**
	* Locate a not-yet-consumed chunk equal to `probe`.
	*
	* @param[in] used per-chunk consumed flags, parallel to the indexed list.
	*            Flags must only ever transition from false to true.
	* @param[in] probe content to look for
	* @param[out] out_index position of the match, set only on success
	* @return True if an unused match was found. The caller is expected to mark
	*         the returned position used; until it does, subsequent calls return
	*         the same position.
	*/
	[[nodiscard]] bool find_unused(const std::vector<bool>& used,
	                               const SignedChunk<T>& probe,
	                               size_t& out_index) {
		auto it = buckets_.find(probe);
		if (it == buckets_.end())
			return false;

		Bucket& bucket = it->second;
		while (bucket.cursor < bucket.positions.size() &&
		       used[bucket.positions[bucket.cursor]])
			++bucket.cursor;

		if (bucket.cursor == bucket.positions.size())
			return false;

		out_index = bucket.positions[bucket.cursor];
		return true;
	}

private:
	struct Bucket {
		std::vector<size_t> positions;  // ascending, by construction
		size_t cursor { 0 };            // positions before this are consumed
	};

	std::unordered_map<SignedChunk<T>, Bucket, ChunkHash<T>, ChunkEqual<T>> buckets_;
};

#endif // CHUNKINDEX_HPP
