#ifndef DELTAFORMAT_HPP
#define DELTAFORMAT_HPP

#include <cstddef>
#include <cstdint>

// Delta file header: 4-byte magic + 4-byte BE version.
inline constexpr uint8_t DELTA_MAGIC[4] = { 'R', 'H', 'D', 0x00 };

// v3: + whole-file hash trailer
// v4: boundary predicate targets DELTA_BOUNDARY_TARGET instead of zero
// v5: entries reference old chunks by index and carry a truncated digest and
//     varint lengths, shrinking a reused-chunk entry from 88 bytes to about 19.
//     REMOVED entries are gone: they existed only to validate a consumption
//     model that index-based references make unnecessary.
inline constexpr uint32_t DELTA_FORMAT_VERSION = 5;
inline constexpr size_t DELTA_HEADER_SIZE = 8;

// Trailer follows the last entry: the tag, then a digest of the whole
// reconstructed file. The tag cannot be mistaken for an entry because entry
// types are single bytes below DELTA_ENTRY_TYPE_LIMIT.
inline constexpr uint8_t DELTA_TRAILER_TAG = 0xFF;

// Per-entry digest length, truncated from the strong hash.
//
// 128 bits is the identity of a chunk and the verification of a generated
// payload. A random pair collides with probability about 2^-128; generic
// birthday collision search has about 2^64 work. Thousands of
// these appear in a delta, so the 48 bytes saved against a full BLAKE2b-512
// digest dominate the format's overhead. The whole-file trailer is deliberately
// *not* truncated — there is exactly one, so its size is irrelevant and full
// strength is free.
inline constexpr size_t DELTA_DIGEST_BYTES = 16;

// Chunk-size bounds. These are format invariants, not merely chunker tuning:
// no entry may legitimately declare a chunk larger than DELTA_MAX_CHUNK_SIZE,
// so readers must reject anything bigger *before* allocating a buffer for it.
// Skipping that check turns a corrupt or hostile size field into a
// multi-exabyte allocation and an uncaught std::bad_alloc. There is no
// enforceable lower bound: the residual chunk at EOF, and any file shorter
// than one rolling window, are legitimately smaller than the minimum.
inline constexpr size_t DELTA_MIN_CHUNK_SIZE = 512;
inline constexpr size_t DELTA_TARGET_CHUNK_SIZE = 8192;
inline constexpr size_t DELTA_MAX_CHUNK_SIZE = 16384;

// Value the masked rolling fingerprint must equal at a chunk boundary. This is
// a chunking parameter with format weight: builds that disagree on it chunk
// the same file differently, and the applier re-chunks the old file itself, so
// it must match the build that created the delta.
//
// It is deliberately NOT zero, and not arbitrary. Constant content holds the
// rolling fingerprint at a constant value — for a window of byte c it is
// c * S mod M with S = sum(256^i, i<48) — so the boundary predicate either
// fires at every byte or never. With a zero target, all-zero content (fp = 0)
// fired at every byte past the minimum, degenerating zero padding and sparse
// files into maximal chunk counts at minimal chunk size. 0x2AAB was verified
// against all 256 constant-byte fingerprints under both boundary masks: no
// single-byte-constant content can satisfy it, so constant runs always cut at
// the maximum chunk size instead of the minimum. (Periodic multi-byte
// patterns can still be unlucky; only the single-byte case is provable.)
inline constexpr uint64_t DELTA_BOUNDARY_TARGET = 0x2AAB;

/**
* Per-entry type tag, serialized as one byte.
*
* Each entry describes one chunk of the reconstructed file, in order. ORIGINAL
* and MODIFIED name their source chunk in the old file by index, so the same
* old chunk may be referenced any number of times — content repeated in the new
* file costs one small entry per occurrence rather than a full copy after the
* first. That also removes the consumption bookkeeping the previous format
* needed, and with it the REMOVED entry, whose only purpose was to let the
* applier verify that bookkeeping.
*/
enum class EntryType : uint8_t {
    ORIGINAL_CHUNK = 0,  ///< old_index:varint | digest — copy an old chunk verbatim
    ADDED_CHUNK = 1,     ///< out_size:varint | digest | payload — new bytes
    MODIFIED_CHUNK = 2,  ///< old_index:varint | out_size:varint | digest | opcodes
};

/// One past the highest valid entry type; also the bound a reader checks.
inline constexpr uint8_t DELTA_ENTRY_TYPE_LIMIT = 3;

#endif
