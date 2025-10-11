#ifndef DELTA_HPP
#define DELTA_HPP

#include "Signature.hpp"
#include "FileIO.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

/**
* Entry type for each delta record.
* Each record represent original, added, modified or removed chunk.
*/
enum class EntryType : uint8_t {
    ORIGINAL_CHUNK = 0,
    ADDED_CHUNK = 1,
    MODIFIED_CHUNK = 2,
    REMOVED_CHUNK = 3
};

/**
* Structure representing single delta record.
*/
template <class T>
struct DeltaEntry {
    EntryType type;									/*!< Entry type (original, added etc) */
    SignedChunk<T> chunk_data;						/*!< Signed chunk structure connected to the delta */
    std::vector<uint8_t> chunk_data_raw;			/*!< Raw chunk data (only for added and modified) */
};

/**
* Optimized class for generating deltas between two files using their signatures.
*
* Key improvements:
* - O(n) complexity using hash maps instead of O(n²) nested loops
* - Run-length encoding for efficient diff storage
* - Proper error handling with Result structure
* - Modular design with clear phases
*/
template<class T, class U>
class Delta {
public:
    static_assert(std::is_base_of<IRollingHash<typename T::RollingHashType>, T>::value,
                  "T must derive from IRollingHash");
    static_assert(std::is_base_of<IHash, U>::value, "U must derive from IHash");

    /**
    * Result of delta generation with error handling
    */
    struct Result {
        bool success;
        std::string error_message;
        size_t chunks_processed;
        size_t bytes_written;
    };

    /**
    * Generates delta between two files with optimized algorithm.
    * @param[in] original original file signatures
    * @param[in] newfile new file signatures
    * @param[in] oldfile old file path
    * @param[in] file_to_check new file path
    * @param[in] delta_file delta file path
    * @return Result structure with success status, error message, and statistics
    */
    Result generate_delta(const Signature<T, U>& original,
                         const Signature<T, U>& newfile,
                         const std::string& oldfile,
                         const std::string& file_to_check,
                         const std::string& delta_file)
    {
        Result result{false, "", 0, 0};

        // Open files with error checking
        FileIO old, file, delta;
        if (!openFiles(old, file, delta, oldfile, file_to_check, delta_file, result)) {
            return result;
        }

        const auto& original_chunks = original.get_chunks();
        const auto& new_chunks = newfile.get_chunks();

        // Build hash map for O(1) chunk lookups
        auto chunk_map = buildChunkMap(original_chunks);

        // Process chunks with optimized algorithm
        if (original_chunks.size() == 1 && new_chunks.size() == 1) {
            processSingleChunkFiles(original_chunks[0], new_chunks[0], old, file, delta, result);
        } else {
            processMultipleChunks(original_chunks, new_chunks, chunk_map, old, file, delta, result);
        }

        old.close();
        file.close();
        delta.close();

        result.success = true;
        return result;
    }

private:
    // Hash function for chunk lookup
    struct ChunkHash {
        size_t operator()(const SignedChunk<typename T::RollingHashType>& chunk) const {
            // Combine signature and first few hash bytes for unique key
            size_t h1 = std::hash<typename T::RollingHashType>{}(chunk.signature);
            size_t h2 = chunk.hash.size() >= 8 ?
                       *reinterpret_cast<const size_t*>(chunk.hash.data()) : 0;
            return h1 ^ (h2 << 1);
        }
    };

    struct ChunkEqual {
        bool operator()(const SignedChunk<typename T::RollingHashType>& a,
                       const SignedChunk<typename T::RollingHashType>& b) const {
            return a == b;
        }
    };

    using ChunkMap = std::unordered_map<SignedChunk<typename T::RollingHashType>,
                                        size_t, ChunkHash, ChunkEqual>;

    /**
    * Build hash map of chunks for O(1) lookups
    */
    ChunkMap buildChunkMap(const std::vector<SignedChunk<typename T::RollingHashType>>& chunks) {
        ChunkMap map;
        map.reserve(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            map[chunks[i]] = i;
        }
        return map;
    }

    /**
    * Open all required files with error handling
    */
    bool openFiles(FileIO& old, FileIO& file, FileIO& delta,
                   const std::string& oldfile, const std::string& file_to_check,
                   const std::string& delta_file, Result& result) {
        if (!old.open(oldfile, FileMode::IN)) {
            result.error_message = "Failed to open old file: " + oldfile;
            return false;
        }
        if (!file.open(file_to_check, FileMode::IN)) {
            result.error_message = "Failed to open new file: " + file_to_check;
            return false;
        }
        if (!delta.open(delta_file, FileMode::OUT)) {
            result.error_message = "Failed to create delta file: " + delta_file;
            return false;
        }
        return true;
    }

    /**
    * Process single chunk files
    */
    void processSingleChunkFiles(const SignedChunk<typename T::RollingHashType>& old_chunk,
                                 const SignedChunk<typename T::RollingHashType>& new_chunk,
                                 FileIO& old, FileIO& file, FileIO& delta, Result& result) {
        DeltaEntry<typename T::RollingHashType> entry;

        if (old_chunk == new_chunk) {
            entry.type = EntryType::ORIGINAL_CHUNK;
            entry.chunk_data = old_chunk;
        } else {
            entry.type = EntryType::MODIFIED_CHUNK;
            entry.chunk_data = new_chunk;

            auto old_data = old.read_chunk(old_chunk.chunk_size, old_chunk.start_offset);
            auto new_data = file.read_chunk(new_chunk.chunk_size, new_chunk.start_offset);

            if (old_data && new_data) {
                entry.chunk_data_raw = createOptimizedDiff(*old_data, *new_data);
            }
        }

        writeDeltaEntry(delta, entry, result);
        result.chunks_processed = 1;
    }

    /**
    * Process multiple chunks with optimized algorithm
    *
    * Uses 4-phase approach:
    * 1. Match identical chunks at same positions (O(n))
    * 2. Find moved chunks using hash map (O(n))
    * 3. Handle modifications and additions (O(n))
    * 4. Process removed chunks (O(n))
    */
    void processMultipleChunks(const std::vector<SignedChunk<typename T::RollingHashType>>& original_chunks,
                               const std::vector<SignedChunk<typename T::RollingHashType>>& new_chunks,
                               const ChunkMap& chunk_map,
                               FileIO& old, FileIO& file, FileIO& delta, Result& result) {
        std::vector<bool> original_used(original_chunks.size(), false);
        std::vector<bool> new_processed(new_chunks.size(), false);

        // Phase 1: Match identical chunks at same positions
        size_t min_size = std::min(original_chunks.size(), new_chunks.size());
        for (size_t i = 0; i < min_size; ++i) {
            if (original_chunks[i] == new_chunks[i]) {
                DeltaEntry<typename T::RollingHashType> entry;
                entry.type = EntryType::ORIGINAL_CHUNK;
                entry.chunk_data = original_chunks[i];
                writeDeltaEntry(delta, entry, result);

                original_used[i] = true;
                new_processed[i] = true;
                result.chunks_processed++;
            }
        }

        // Phase 2: Find moved chunks using hash map (O(1) lookup)
        for (size_t i = 0; i < new_chunks.size(); ++i) {
            if (new_processed[i]) continue;

            auto it = chunk_map.find(new_chunks[i]);
            if (it != chunk_map.end() && !original_used[it->second]) {
                // Chunk moved from different position
                DeltaEntry<typename T::RollingHashType> entry;
                entry.type = EntryType::ORIGINAL_CHUNK;
                entry.chunk_data = new_chunks[i];
                writeDeltaEntry(delta, entry, result);

                original_used[it->second] = true;
                new_processed[i] = true;
                result.chunks_processed++;
            }
        }

        // Phase 3: Handle modifications and additions
        for (size_t i = 0; i < new_chunks.size(); ++i) {
            if (new_processed[i]) continue;

            DeltaEntry<typename T::RollingHashType> entry;

            // Check if this is a modification of nearby chunk
            bool is_modification = false;
            if (i < original_chunks.size() && !original_used[i]) {
                // Likely a modification of the chunk at same position
                entry.type = EntryType::MODIFIED_CHUNK;
                entry.chunk_data = new_chunks[i];

                auto old_data = old.read_chunk(original_chunks[i].chunk_size,
                                              original_chunks[i].start_offset);
                auto new_data = file.read_chunk(new_chunks[i].chunk_size,
                                               new_chunks[i].start_offset);

                if (old_data && new_data) {
                    entry.chunk_data_raw = createOptimizedDiff(*old_data, *new_data);
                    is_modification = true;
                    original_used[i] = true;
                }
            }

            if (!is_modification) {
                // New chunk added
                entry.type = EntryType::ADDED_CHUNK;
                entry.chunk_data = new_chunks[i];

                auto chunk_data = file.read_chunk(entry.chunk_data.chunk_size,
                                                 entry.chunk_data.start_offset);
                if (chunk_data) {
                    entry.chunk_data_raw = std::move(*chunk_data);
                }
            }

            writeDeltaEntry(delta, entry, result);
            result.chunks_processed++;
        }

        // Phase 4: Handle removed chunks
        for (size_t i = 0; i < original_chunks.size(); ++i) {
            if (!original_used[i]) {
                DeltaEntry<typename T::RollingHashType> entry;
                entry.type = EntryType::REMOVED_CHUNK;
                entry.chunk_data = original_chunks[i];
                writeDeltaEntry(delta, entry, result);
                result.chunks_processed++;
            }
        }
    }

    /**
    * Create optimized diff using run-length encoding for sequences
    *
    * Format: 'D' <position:4> <count:1> <bytes...>
    * This efficiently encodes up to 255 consecutive changes in 6+n bytes
    * instead of the original 6 bytes per change.
    */
    std::vector<uint8_t> createOptimizedDiff(const std::vector<uint8_t>& old_data,
                                            const std::vector<uint8_t>& new_data) {
        std::vector<uint8_t> diff;
        diff.reserve(std::min(old_data.size(), new_data.size()) / 4); // Estimate

        size_t i = 0, j = 0;

        while (i < old_data.size() && j < new_data.size()) {
            // Find runs of matching bytes
            while (i < old_data.size() && j < new_data.size() &&
                   old_data[i] == new_data[j]) {
                i++;
                j++;
            }

            // Find runs of differences
            size_t diff_start = i;
            std::vector<uint8_t> diff_bytes;
            while (i < old_data.size() && j < new_data.size() &&
                   old_data[i] != new_data[j] && diff_bytes.size() < 255) {
                diff_bytes.push_back(new_data[j]);
                i++;
                j++;
            }

            // Encode differences if any
            if (!diff_bytes.empty()) {
                // Format: 'D' <position:4> <count:1> <bytes...>
                diff.push_back('D');
                pushUint32(diff, diff_start);
                diff.push_back(static_cast<uint8_t>(diff_bytes.size()));
                diff.insert(diff.end(), diff_bytes.begin(), diff_bytes.end());
            }
        }

        // Handle remaining bytes
        if (i < old_data.size()) {
            // Deletions
            diff.push_back('X');
            pushUint32(diff, i);
            pushUint32(diff, old_data.size() - i);
        }

        if (j < new_data.size()) {
            // Additions
            diff.push_back('I');
            pushUint32(diff, j);
            diff.push_back(static_cast<uint8_t>(std::min(size_t(255), new_data.size() - j)));
            for (size_t k = j; k < new_data.size() && k - j < 255; ++k) {
                diff.push_back(new_data[k]);
            }
        }

        return diff;
    }

    /**
    * Helper to push 32-bit value as 4 bytes
    */
    void pushUint32(std::vector<uint8_t>& vec, uint32_t value) {
        vec.push_back(value >> 24);
        vec.push_back(value >> 16);
        vec.push_back(value >> 8);
        vec.push_back(value);
    }

    /**
    * Write delta entry to file
    */
    void writeDeltaEntry(FileIO& delta, const DeltaEntry<typename T::RollingHashType>& entry,
                        Result& result) {
        result.bytes_written += sizeof(uint64_t); // Entry type
        delta.write_chunk(static_cast<uint64_t>(entry.type));

        result.bytes_written += sizeof(entry.chunk_data.signature);
        delta.write_chunk(entry.chunk_data.signature);

        result.bytes_written += entry.chunk_data.hash.size();
        delta.write_chunk(const_cast<uint8_t*>(entry.chunk_data.hash.data()), entry.chunk_data.hash.size());

        result.bytes_written += sizeof(entry.chunk_data.chunk_size);
        delta.write_chunk(entry.chunk_data.chunk_size);

        if (entry.type == EntryType::ADDED_CHUNK || entry.type == EntryType::MODIFIED_CHUNK) {
            result.bytes_written += entry.chunk_data_raw.size();
            delta.write_chunk(const_cast<uint8_t*>(entry.chunk_data_raw.data()), entry.chunk_data_raw.size());
        }
    }
};

#endif // DELTA_HPP
