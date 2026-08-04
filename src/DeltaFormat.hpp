#ifndef DELTAFORMAT_HPP
#define DELTAFORMAT_HPP

#include <cstddef>
#include <cstdint>

// Delta file header: 4-byte magic + 4-byte BE version.
inline constexpr uint8_t DELTA_MAGIC[4] = { 'R', 'H', 'D', 0x00 };
inline constexpr uint32_t DELTA_FORMAT_VERSION = 3;  // v3: + whole-file hash trailer
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

// Per-entry type tag. Serialized as 8 bytes big-endian in the delta stream.
enum class EntryType : uint8_t {
    ORIGINAL_CHUNK = 0,
    ADDED_CHUNK = 1,
    MODIFIED_CHUNK = 2,
    REMOVED_CHUNK = 3,
};

#endif
