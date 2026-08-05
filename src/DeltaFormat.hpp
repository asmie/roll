#ifndef DELTAFORMAT_HPP
#define DELTAFORMAT_HPP

#include <cstddef>
#include <cstdint>

// Delta file header: 4-byte magic + 4-byte BE version.
inline constexpr uint8_t DELTA_MAGIC[4] = { 'R', 'H', 'D', 0x00 };
// v3: + whole-file hash trailer
// v4: boundary predicate targets DELTA_BOUNDARY_TARGET instead of zero. The
//     wire layout is unchanged, but chunk boundaries move, and the applier
//     re-chunks the old file with its own chunker — a v3 delta applied by a
//     v4 build would fail with a misleading "references unknown chunk" rather
//     than a version error, so the version must reject it up front.
inline constexpr uint32_t DELTA_FORMAT_VERSION = 4;
inline constexpr size_t DELTA_HEADER_SIZE = 8;

// Trailer follows the last entry. First byte is the tag (chosen so it can
// never collide with the first byte of a big-endian u64 EntryType, which is
// always 0x00); after the tag comes get_hash_size() bytes of the whole-file
// hash of the reconstructed new file.
inline constexpr uint8_t DELTA_TRAILER_TAG = 0xFF;

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
// it must match the build that created the delta (hence the version bump that
// introduced it).
//
// It is deliberately NOT zero, and not arbitrary. Constant content holds the
// rolling fingerprint at a constant value — for a window of byte c it is
// c * S mod M with S = sum(256^i, i<48) — so the boundary predicate either
// fires at every byte or never. With a zero target, all-zero content (fp = 0)
// fired at every byte past the minimum, degenerating zero padding and sparse
// files into maximal chunk counts at minimal chunk size: 17% metadata
// overhead and, formerly, quadratic index behaviour. 0x2AAB was verified
// against all 256 constant-byte fingerprints under both boundary masks: no
// single-byte-constant content can satisfy it, so constant runs always cut at
// the maximum chunk size instead of the minimum. (Periodic multi-byte
// patterns can still be unlucky; only the single-byte case is provable.)
inline constexpr uint64_t DELTA_BOUNDARY_TARGET = 0x2AAB;

// Per-entry type tag. Serialized as 8 bytes big-endian in the delta stream.
enum class EntryType : uint8_t {
    ORIGINAL_CHUNK = 0,
    ADDED_CHUNK = 1,
    MODIFIED_CHUNK = 2,
    REMOVED_CHUNK = 3,
};

#endif
