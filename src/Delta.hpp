#ifndef DELTA_HPP
#define DELTA_HPP

#include "DeltaFormat.hpp"
#include "Signature.hpp"
#include "FileIO.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
* Generates a binary delta between two file signatures by emitting one entry
* per chunk of the new file (followed by REMOVED entries for old chunks not
* referenced from the new file).
*/
template<RollingHashAlgorithm T, StrongHashAlgorithm U>
class Delta {
public:
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
                         const std::filesystem::path& oldfile,
                         const std::filesystem::path& file_to_check,
                         const std::filesystem::path& delta_file)
    {
        Result result{false, "", 0, 0};

        // Open files with error checking
        FileIO old, file, delta;
        if (!openFiles(old, file, delta, oldfile, file_to_check, delta_file, result)) {
            return result;
        }

        if (!writeHeader(delta, result))
            return result;

        const auto& original_chunks = original.get_chunks();
        const auto& new_chunks = newfile.get_chunks();

        auto chunk_map = buildChunkMap(original_chunks);
        bool ok = processMultipleChunks(original_chunks, new_chunks, chunk_map,
                                        old, file, delta, result);

        if (ok)
            ok = writeTrailer(file, delta, result);

        old.close();
        file.close();
        if (!delta.close()) {
            result.error_message = "Failed to flush delta file";
            return result;
        }

        result.success = ok;
        return result;
    }

private:
    // Hash function for chunk lookup
    struct ChunkHash {
        size_t operator()(const SignedChunk<typename T::RollingHashType>& chunk) const {
            size_t h1 = std::hash<typename T::RollingHashType>{}(chunk.signature);
            size_t h2 = 0;
            if (chunk.hash.size() >= sizeof(size_t))
                std::memcpy(&h2, chunk.hash.data(), sizeof(size_t));
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

    // Stream-hash the entire new-file content in fixed-size buffers and emit
    // (DELTA_TRAILER_TAG | hash) so the applier can verify end-to-end
    // reconstruction, not just per-chunk hashes.
    bool writeTrailer(FileIO& file, FileIO& delta, Result& result) {
        U hash_func;
        hash_func.init();

        constexpr size_t STREAM_CHUNK = 64 * 1024;
        auto buf = file.read_chunk(STREAM_CHUNK, 0);
        while (!buf.empty()) {
            hash_func.update(buf);
            buf = file.read_chunk(STREAM_CHUNK);
        }

        std::vector<uint8_t> digest(hash_func.get_hash_size());
        hash_func.finalize(digest);

        const uint8_t tag = DELTA_TRAILER_TAG;
        if (!delta.write_chunk(std::span<const uint8_t>{&tag, 1})) {
            result.error_message = "Failed to write trailer tag";
            return false;
        }
        if (!delta.write_chunk(digest)) {
            result.error_message = "Failed to write trailer hash";
            return false;
        }
        result.bytes_written += 1 + digest.size();
        return true;
    }

    bool writeHeader(FileIO& delta, Result& result) {
        std::vector<uint8_t> header;
        header.reserve(DELTA_HEADER_SIZE);
        header.insert(header.end(), std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
        pushUint32(header, DELTA_FORMAT_VERSION);
        if (!delta.write_chunk(header)) {
            result.error_message = "Failed to write delta header";
            return false;
        }
        result.bytes_written += header.size();
        return true;
    }

    /**
    * Open all required files with error handling
    */
    bool openFiles(FileIO& old, FileIO& file, FileIO& delta,
                   const std::filesystem::path& oldfile, const std::filesystem::path& file_to_check,
                   const std::filesystem::path& delta_file, Result& result) {
        if (!old.open(oldfile, FileMode::IN)) {
            result.error_message = "Failed to open old file: " + oldfile.string();
            return false;
        }
        if (!file.open(file_to_check, FileMode::IN)) {
            result.error_message = "Failed to open new file: " + file_to_check.string();
            return false;
        }
        if (!delta.open(delta_file, FileMode::OUT)) {
            result.error_message = "Failed to create delta file: " + delta_file.string();
            return false;
        }
        return true;
    }

    /**
    * Emit delta entries in target (new-file) order, followed by REMOVED entries
    * for any unmatched old chunks. This ordering lets the applier append each
    * entry directly to the output without reordering.
    */
    bool processMultipleChunks(const std::vector<SignedChunk<typename T::RollingHashType>>& original_chunks,
                               const std::vector<SignedChunk<typename T::RollingHashType>>& new_chunks,
                               const ChunkMap& chunk_map,
                               FileIO& old, FileIO& file, FileIO& delta, Result& result) {
        std::vector<bool> original_used(original_chunks.size(), false);

        for (size_t i = 0; i < new_chunks.size(); ++i) {
            DeltaEntry<typename T::RollingHashType> entry;

            // Identical chunk at same position.
            if (i < original_chunks.size() && !original_used[i] &&
                original_chunks[i] == new_chunks[i]) {
                entry.type = EntryType::ORIGINAL_CHUNK;
                entry.chunk_data = original_chunks[i];
                original_used[i] = true;
                if (!writeDeltaEntry(delta, entry, result)) return false;
                result.chunks_processed++;
                continue;
            }

            // Moved match: same content located elsewhere in old.
            auto it = chunk_map.find(new_chunks[i]);
            if (it != chunk_map.end() && !original_used[it->second]) {
                entry.type = EntryType::ORIGINAL_CHUNK;
                entry.chunk_data = new_chunks[i];
                original_used[it->second] = true;
                if (!writeDeltaEntry(delta, entry, result)) return false;
                result.chunks_processed++;
                continue;
            }

            // Modification of the same-position old chunk, otherwise an addition.
            bool is_modification = false;
            if (i < original_chunks.size() && !original_used[i]) {
                entry.type = EntryType::MODIFIED_CHUNK;
                entry.chunk_data = new_chunks[i];

                auto old_data = old.read_chunk(original_chunks[i].chunk_size,
                                              original_chunks[i].start_offset);
                auto new_data = file.read_chunk(new_chunks[i].chunk_size,
                                               new_chunks[i].start_offset);

                if (!old_data.empty() && !new_data.empty()) {
                    entry.chunk_data_raw = createDiff(old_data, new_data);
                    is_modification = true;
                    original_used[i] = true;
                }
            }

            if (!is_modification) {
                entry.type = EntryType::ADDED_CHUNK;
                entry.chunk_data = new_chunks[i];

                entry.chunk_data_raw = file.read_chunk(entry.chunk_data.chunk_size,
                                                       entry.chunk_data.start_offset);
            }

            if (!writeDeltaEntry(delta, entry, result)) return false;
            result.chunks_processed++;
        }

        // Removed chunks: any old chunk not consumed above.
        for (size_t i = 0; i < original_chunks.size(); ++i) {
            if (!original_used[i]) {
                DeltaEntry<typename T::RollingHashType> entry;
                entry.type = EntryType::REMOVED_CHUNK;
                entry.chunk_data = original_chunks[i];
                if (!writeDeltaEntry(delta, entry, result)) return false;
                result.chunks_processed++;
            }
        }
        return true;
    }

    // Produce a diff from old_data to new_data. Tries an optimal-SES Myers
    // diff first (Eugene W. Myers, 1986, "An O(ND) Difference Algorithm");
    // falls back to a fast greedy diff if D exceeds a memory cap. Both
    // emit the same D/I/X opcode format the applier consumes.
    std::vector<uint8_t> createDiff(const std::vector<uint8_t>& old_data,
                                    const std::vector<uint8_t>& new_data) {
        constexpr size_t D_LIMIT = 2048;
        std::vector<int8_t> ses;
        if (myersSES(old_data, new_data, D_LIMIT, ses))
            return emitOpcodesFromSES(old_data, new_data, ses);
        return createGreedyDiff(old_data, new_data);
    }

    // Myers' O(ND) shortest-edit-script. On success fills `out_ses` with one
    // entry per source position: -1 = delete a[x], +1 = insert b[y], 0 =
    // equal byte (advance both). Returns false (without modifying out_ses)
    // if the edit distance exceeds d_limit.
    bool myersSES(const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b,
                  size_t d_limit,
                  std::vector<int8_t>& out_ses) {
        const int N = static_cast<int>(a.size());
        const int M = static_cast<int>(b.size());

        if (N == 0 && M == 0) { out_ses.clear(); return true; }
        if (N == 0) { out_ses.assign(M, +1); return true; }
        if (M == 0) { out_ses.assign(N, -1); return true; }

        const int max_total = N + M;
        const int d_max = std::min(max_total, static_cast<int>(d_limit));
        const int offset = d_max;
        const int v_size = 2 * d_max + 1;

        std::vector<int> v(v_size, 0);
        std::vector<std::vector<int>> trace;
        trace.reserve(static_cast<size_t>(d_max) + 1);

        int final_d = -1;
        for (int d = 0; d <= d_max && final_d < 0; d++) {
            for (int k = -d; k <= d; k += 2) {
                const bool from_above = (k == -d) ||
                    (k != d && v[offset + k - 1] < v[offset + k + 1]);
                int x = from_above ? v[offset + k + 1] : v[offset + k - 1] + 1;
                int y = x - k;
                while (x < N && y < M && a[x] == b[y]) { x++; y++; }
                v[offset + k] = x;
                if (x >= N && y >= M) {
                    final_d = d;
                    break;
                }
            }
            trace.push_back(v);
        }

        if (final_d < 0)
            return false;  // edit distance exceeded the cap

        // Backtrack the trace to produce the SES in reverse, then reverse it.
        out_ses.clear();
        out_ses.reserve(static_cast<size_t>(N + M));
        int x = N, y = M;
        for (int d = final_d; d > 0; d--) {
            const auto& vp = trace[d - 1];
            const int k = x - y;
            const bool came_from_above = (k == -d) ||
                (k != d && vp[offset + k - 1] < vp[offset + k + 1]);
            const int prev_k = came_from_above ? k + 1 : k - 1;
            const int prev_x = vp[offset + prev_k];
            const int prev_y = prev_x - prev_k;

            // Unwind the diagonal snake before the edit step.
            while (x > prev_x && y > prev_y) {
                out_ses.push_back(0);
                x--; y--;
            }
            // Unwind the single edit step.
            if (came_from_above) {
                out_ses.push_back(+1);
                y--;
            } else {
                out_ses.push_back(-1);
                x--;
            }
        }
        // Initial snake from (0,0) up to the first edit.
        while (x > 0 && y > 0) {
            out_ses.push_back(0);
            x--; y--;
        }
        std::reverse(out_ses.begin(), out_ses.end());
        return true;
    }

    // Walk the SES and emit D/I/X opcodes. Adjacent inserts/deletes between
    // two equal runs are coalesced into one block; balanced delete+insert
    // blocks become D (replace-in-place); otherwise X for deletes and I for
    // inserts. All payload counts are u8-split.
    std::vector<uint8_t> emitOpcodesFromSES(const std::vector<uint8_t>& /*old_data*/,
                                            const std::vector<uint8_t>& new_data,
                                            const std::vector<int8_t>& ses) {
        std::vector<uint8_t> diff;
        size_t y = 0;
        size_t i = 0;

        while (i < ses.size()) {
            if (ses[i] == 0) { ++i; ++y; continue; }

            // Block of consecutive non-equal SES entries.
            const size_t block_start_y = y;
            size_t deletes = 0;
            std::vector<uint8_t> inserts;
            while (i < ses.size() && ses[i] != 0) {
                if (ses[i] == -1) {
                    ++deletes;
                } else {
                    inserts.push_back(new_data[y++]);
                }
                ++i;
            }

            emitBlock(diff, block_start_y, deletes, inserts);
        }
        return diff;
    }

    // Emit one logical change block. Caller has already pinned new-position.
    void emitBlock(std::vector<uint8_t>& diff, size_t pos,
                   size_t deletes, const std::vector<uint8_t>& inserts) {
        if (deletes > 0 && deletes == inserts.size()) {
            emitInlineBytes(diff, 'D', pos, inserts);
        } else {
            if (deletes > 0) {
                diff.push_back('X');
                pushUint32(diff, static_cast<uint32_t>(pos));
                pushUint32(diff, static_cast<uint32_t>(deletes));
            }
            if (!inserts.empty())
                emitInlineBytes(diff, 'I', pos, inserts);
        }
    }

    // Emit a D or I opcode stream for `bytes`, splitting on the u8 count cap.
    void emitInlineBytes(std::vector<uint8_t>& diff, char op,
                         size_t pos, const std::vector<uint8_t>& bytes) {
        size_t off = 0;
        while (off < bytes.size()) {
            const size_t count = std::min<size_t>(255, bytes.size() - off);
            diff.push_back(static_cast<uint8_t>(op));
            pushUint32(diff, static_cast<uint32_t>(pos + off));
            diff.push_back(static_cast<uint8_t>(count));
            diff.insert(diff.end(), bytes.begin() + off, bytes.begin() + off + count);
            off += count;
        }
    }

    // Fallback greedy diff used when the Myers edit distance exceeds D_LIMIT.
    // Produces a correct (but suboptimal) opcode stream in O(N+M).
    std::vector<uint8_t> createGreedyDiff(const std::vector<uint8_t>& old_data,
                                          const std::vector<uint8_t>& new_data) {
        std::vector<uint8_t> diff;
        size_t i = 0, j = 0;

        while (i < old_data.size() && j < new_data.size()) {
            while (i < old_data.size() && j < new_data.size() &&
                   old_data[i] == new_data[j]) {
                i++; j++;
            }
            size_t diff_start = i;
            std::vector<uint8_t> diff_bytes;
            while (i < old_data.size() && j < new_data.size() &&
                   old_data[i] != new_data[j] && diff_bytes.size() < 255) {
                diff_bytes.push_back(new_data[j]);
                i++; j++;
            }
            if (!diff_bytes.empty()) {
                diff.push_back('D');
                pushUint32(diff, static_cast<uint32_t>(diff_start));
                diff.push_back(static_cast<uint8_t>(diff_bytes.size()));
                diff.insert(diff.end(), diff_bytes.begin(), diff_bytes.end());
            }
        }

        if (i < old_data.size()) {
            diff.push_back('X');
            pushUint32(diff, static_cast<uint32_t>(i));
            pushUint32(diff, static_cast<uint32_t>(old_data.size() - i));
        }
        while (j < new_data.size()) {
            size_t count = std::min(size_t(255), new_data.size() - j);
            diff.push_back('I');
            pushUint32(diff, static_cast<uint32_t>(j));
            diff.push_back(static_cast<uint8_t>(count));
            for (size_t k = 0; k < count; ++k)
                diff.push_back(new_data[j + k]);
            j += count;
        }
        return diff;
    }

    /**
    * Helper to push 32-bit value as 4 bytes
    */
    void pushUint32(std::vector<uint8_t>& vec, uint32_t value) {
        auto encoded = value;
        if constexpr (std::endian::native == std::endian::little) {
            encoded = std::byteswap(encoded);
        }

        for (const auto byte : std::as_bytes(std::span{&encoded, 1})) {
            vec.push_back(std::to_integer<uint8_t>(byte));
        }
    }

    /**
    * Write delta entry to file
    */
    bool writeDeltaEntry(FileIO& delta, const DeltaEntry<typename T::RollingHashType>& entry,
                        Result& result) {
        if (!delta.write_chunk(static_cast<uint64_t>(std::to_underlying(entry.type)))) {
            result.error_message = "Failed to write entry type";
            return false;
        }
        result.bytes_written += sizeof(uint64_t);

        if (!delta.write_chunk(entry.chunk_data.signature)) {
            result.error_message = "Failed to write signature";
            return false;
        }
        result.bytes_written += sizeof(entry.chunk_data.signature);

        if (!delta.write_chunk(entry.chunk_data.hash)) {
            result.error_message = "Failed to write hash";
            return false;
        }
        result.bytes_written += entry.chunk_data.hash.size();

        if (!delta.write_chunk(entry.chunk_data.chunk_size)) {
            result.error_message = "Failed to write chunk size";
            return false;
        }
        result.bytes_written += sizeof(entry.chunk_data.chunk_size);

        if (entry.type == EntryType::ADDED_CHUNK || entry.type == EntryType::MODIFIED_CHUNK) {
            if (!delta.write_chunk(entry.chunk_data_raw)) {
                result.error_message = "Failed to write payload";
                return false;
            }
            result.bytes_written += entry.chunk_data_raw.size();
        }
        return true;
    }
};

#endif // DELTA_HPP
