#ifndef DELTA_HPP
#define DELTA_HPP

#include "ChunkIndex.hpp"
#include "DeltaCodec.hpp"
#include "DeltaError.hpp"
#include "DeltaFormat.hpp"
#include "Signature.hpp"
#include "FileIO.hpp"
#include "OutputTransaction.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

/**
* Generates a binary delta between two file signatures by emitting one entry
* per chunk of the new file. Entries reference old chunks by index, so an old
* chunk may back any number of new ones and unreferenced old chunks need no
* mention at all.
*/
template<RollingHashAlgorithm T, StrongHashAlgorithm U>
class Delta {
public:
    /**
    * Generates delta between two files with optimized algorithm.
    * @param[in] original original file signatures
    * @param[in] newfile new file signatures
    * @param[in] oldfile old file path
    * @param[in] file_to_check new file path
    * @param[in] delta_file delta file path
    * @return Statistics on success, or the failure that stopped generation.
    */
    [[nodiscard]] std::expected<DeltaStats, DeltaError>
    generate_delta(const Signature<T, U>& original,
                         const Signature<T, U>& newfile,
                         const std::filesystem::path& oldfile,
                         const std::filesystem::path& file_to_check,
                         const std::filesystem::path& delta_file)
    {
        try {
            return generateImpl(original, newfile, oldfile, file_to_check, delta_file);
        } catch (const std::exception& e) {
            return std::unexpected(DeltaError{DeltaErrc::internal_error, e.what()});
        }
    }

private:
    using Chunk = SignedChunk<typename T::RollingHashType>;

    // Internal bookkeeping threaded through the generation helpers. Kept
    // separate from the public std::expected so the helpers can keep reporting
    // failure with a bool and filling in an error as they go.
    struct Progress {
        size_t chunks_processed { 0 };
        DeltaError error;
    };

    std::expected<DeltaStats, DeltaError> generateImpl(
        const Signature<T, U>& original, const Signature<T, U>& newfile,
        const std::filesystem::path& oldfile, const std::filesystem::path& file_to_check,
        const std::filesystem::path& delta_file) {
        Progress progress;

        FileIO old, file;
        if (!openFiles(old, file, oldfile, file_to_check, progress))
            return std::unexpected(std::move(progress.error));

        OutputTransaction transaction;
        if (auto opened = transaction.open(delta_file, {oldfile, file_to_check}); !opened)
            return std::unexpected(opened.error());

        const auto& original_chunks = original.get_chunks();
        const auto& new_chunks = newfile.get_chunks();

        // The signatures come from an earlier pass. A size change is the
        // cheapest sign that an input changed since then.
        if (!coversExactly(original_chunks, old.size()) || !coversExactly(new_chunks, file.size()))
            return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
                                              "Input size changed since signature generation"});

        DeltaWriter writer(transaction.file());
        if (!writer.write_header())
            return std::unexpected(DeltaError{DeltaErrc::io_error, writer.error()});

        ChunkIndex<typename T::RollingHashType> chunk_index(original_chunks);
        if (!processMultipleChunks(original_chunks, new_chunks, chunk_index,
                                   old, file, writer, progress) ||
            !verifyOldFile(original_chunks, old, progress) ||
            !writeTrailer(new_chunks, file, writer, progress, newfile.whole_file_hash()))
            return std::unexpected(std::move(progress.error));

        if (!old.close() || !file.close())
            return std::unexpected(DeltaError{DeltaErrc::io_error, "Failed to read input files"});
        if (auto committed = transaction.commit(); !committed)
            return std::unexpected(committed.error());

        return DeltaStats{progress.chunks_processed, writer.bytes_written()};
    }

    // Emit (DELTA_TRAILER_TAG | hash) so the applier can verify end-to-end
    // reconstruction, not just per-chunk hashes. When the new file's Signature
    // was generated with WholeFileHash::Compute its digest is reused; otherwise
    // the new file is read again chunk by chunk, verified against its
    // signature, and hashed here.
    bool writeTrailer(const std::vector<Chunk>& new_chunks, FileIO& file, DeltaWriter& writer,
                      Progress& progress, std::span<const uint8_t> precomputed) {
        std::vector<uint8_t> digest(precomputed.begin(), precomputed.end());
        if (digest.empty()) {
            U hash_func, chunk_hash;
            hash_func.init();
            std::vector<uint8_t> data;
            for (const auto& chunk : new_chunks) {
                if (!readSignedChunk(file, chunk, chunk_hash, data, progress))
                    return false;
                hash_func.update(data);
            }
            digest.resize(hash_func.get_hash_size());
            hash_func.finalize(digest);
        }

        if (!file.read_chunk(1, file.size()).empty() || file.has_error()) {
            progress.error = DeltaError{DeltaErrc::integrity_mismatch,
                                        "New input changed or failed while reading"};
            return false;
        }
        if (!writer.write_trailer(digest)) {
            progress.error = DeltaError{DeltaErrc::io_error, writer.error()};
            return false;
        }
        return true;
    }

    /**
    * Open all required files with error handling
    */
    bool openFiles(FileIO& old, FileIO& file,
                   const std::filesystem::path& oldfile, const std::filesystem::path& file_to_check,
                   Progress& progress) {
        if (!old.open(oldfile, FileMode::IN)) {
            progress.error = DeltaError{DeltaErrc::io_error,
                                        "Failed to open old file: " + oldfile.string()};
            return false;
        }
        if (!file.open(file_to_check, FileMode::IN)) {
            progress.error = DeltaError{DeltaErrc::io_error,
                                        "Failed to open new file: " + file_to_check.string()};
            return false;
        }
        return true;
    }

    // True when the chunks tile [0, size) with valid lengths.
    static bool coversExactly(const std::vector<Chunk>& chunks, size_t size) {
        size_t covered = 0;
        for (const auto& chunk : chunks) {
            if (chunk.start_offset != covered || chunk.chunk_size == 0 ||
                chunk.chunk_size > DELTA_MAX_CHUNK_SIZE || chunk.chunk_size > size - covered)
                return false;
            covered += chunk.chunk_size;
        }
        return covered == size;
    }

    // Read one signed chunk and require the bytes the signature described.
    // A short read, an I/O error, or a digest mismatch all fail generation, so
    // no record is built from bytes that differ from the signed content.
    bool readSignedChunk(FileIO& file, const Chunk& chunk, U& hasher,
                         std::vector<uint8_t>& data, Progress& progress) {
        data = file.read_chunk(chunk.chunk_size, chunk.start_offset);
        if (file.has_error() || data.size() != chunk.chunk_size) {
            progress.error = DeltaError{DeltaErrc::io_error, "Failed to read signed input chunk"};
            return false;
        }
        std::vector<uint8_t> digest(hasher.get_hash_size());
        hasher.hash(digest, data);
        if (chunk.hash.size() != DELTA_DIGEST_BYTES || digest.size() < DELTA_DIGEST_BYTES ||
            !std::equal(chunk.hash.begin(), chunk.hash.end(), digest.begin())) {
            progress.error = DeltaError{DeltaErrc::integrity_mismatch,
                                        "Input changed since signature generation"};
            return false;
        }
        return true;
    }

    // Apply resolves every record against a fresh chunking of the old file, so
    // a change anywhere in it -- even in a chunk nothing references, whose
    // boundaries decide later indices -- would make the delta unusable. This
    // runs after the records are built, so it also covers the old bytes that
    // MODIFIED diffs were computed from.
    bool verifyOldFile(const std::vector<Chunk>& original_chunks, FileIO& old, Progress& progress) {
        U hasher;
        std::vector<uint8_t> data;
        for (const auto& chunk : original_chunks) {
            if (!readSignedChunk(old, chunk, hasher, data, progress))
                return false;
        }
        if (!old.read_chunk(1, old.size()).empty() || old.has_error()) {
            progress.error = DeltaError{DeltaErrc::integrity_mismatch,
                                        "Old input changed or failed while reading"};
            return false;
        }
        return true;
    }

    /**
    * Emit one entry per chunk of the new file, in order, so the applier can
    * append each entry's output without reordering.
    *
    * Entries name their source chunk in the old file by index. That lets one old
    * chunk back any number of new chunks — repeated content costs a ~19-byte
    * reference per occurrence instead of a full copy after the first — and it
    * removes the consumption bookkeeping the previous format needed, along with
    * the REMOVED entries whose only job was to let the applier check it. Old
    * chunks that nothing references are simply never mentioned.
    */
    bool processMultipleChunks(const std::vector<Chunk>& original_chunks,
                               const std::vector<Chunk>& new_chunks,
                               ChunkIndex<typename T::RollingHashType>& chunk_index,
                               FileIO& old, FileIO& file, DeltaWriter& writer, Progress& progress) {
        // No chunk is ever consumed now, so nothing is ever marked used; the
        // index still wants the flags, and an all-false view means every
        // position stays available for reuse.
        const std::vector<bool> none_used(original_chunks.size(), false);
        U hasher;

        for (size_t i = 0; i < new_chunks.size(); ++i) {
            // Reuse verbatim: prefer the same position, since a same-position
            // match keeps the source of a later MODIFIED entry nearby and costs
            // the smallest index varint. The record reproduces the signed
            // content, so the new file is not read here; verifyOldFile checks
            // the source afterwards.
            size_t source = 0;
            bool reusable = false;
            if (i < original_chunks.size() && original_chunks[i] == new_chunks[i]) {
                source = i;
                reusable = true;
            } else if (chunk_index.find_unused(none_used, new_chunks[i], source)) {
                reusable = true;
            }

            if (reusable) {
                if (!writer.write_original(source, new_chunks[i].hash)) {
                    progress.error = DeltaError{DeltaErrc::io_error, writer.error()};
                    return false;
                }
                progress.chunks_processed++;
                continue;
            }

            std::vector<uint8_t> new_data;
            if (!readSignedChunk(file, new_chunks[i], hasher, new_data, progress))
                return false;

            // Otherwise rebuild from the same-position old chunk if a diff pays
            // for itself, and fall back to shipping the bytes.
            if (i < original_chunks.size()) {
                auto old_data = old.read_chunk(original_chunks[i].chunk_size,
                                               original_chunks[i].start_offset);
                if (old.has_error() || old_data.size() != original_chunks[i].chunk_size) {
                    progress.error = DeltaError{DeltaErrc::io_error, "Failed to read old chunk"};
                    return false;
                }

                auto diff = createDiff(old_data, new_data);

                // Keep the diff only when the complete record, source index
                // included, costs less than the literal one; without this an
                // edit scattered through a chunk could encode larger than the
                // bytes it describes.
                if (!diff.empty() &&
                    DeltaWriter::modified_size(i, new_data.size(), diff.size()) <
                        DeltaWriter::added_size(new_data.size())) {
                    if (!writer.write_modified(i, new_data.size(), new_chunks[i].hash, diff)) {
                        progress.error = DeltaError{DeltaErrc::io_error, writer.error()};
                        return false;
                    }
                    progress.chunks_processed++;
                    continue;
                }
            }

            if (!writer.write_added(new_chunks[i].hash, new_data)) {
                progress.error = DeltaError{DeltaErrc::io_error, writer.error()};
                return false;
            }
            progress.chunks_processed++;
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

        // Scratch buffers are members reused across chunks (see the note on
        // their declarations): assign/clear keep the existing capacity, so
        // after the first few chunks these loops stop allocating entirely.
        std::vector<int>& v = myers_v_;
        std::vector<int>& trace = myers_trace_;
        v.assign(static_cast<size_t>(v_size), 0);
        trace.clear();

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
            trace.insert(trace.end(), v.begin() + (offset - d), v.begin() + (offset + d + 1));

            // Early abandon on dissimilar content. Running every hopeless pair
            // to d_max is where delta generation on unrelated files spent its
            // time: each such chunk burned the full O(d_max^2) search and then
            // fell back to greedy regardless (50 MB of disjoint data took ~22s,
            // almost all of it here). At a few checkpoints, measure how much of
            // the two inputs the best path has consumed: x+y grows by 1 per
            // edit and 2 per matched byte, so matched = (x+y - d) / 2. Genuine
            // in-place edits accumulate matches far faster than one per two
            // edits; random unrelated bytes almost never match. The sacrifice
            // is content displaced by more than the first checkpoint with no
            // common prefix — Myers at d_max could still have found those, and
            // they now take the greedy/literal path instead.
            if (d == 256 || d == 512 || d == 1024) {
                int best_xy = 0;
                for (int k = -d; k <= d; ++k) {
                    const int x = v[offset + k];
                    best_xy = std::max(best_xy, 2 * x - k);
                }
                const int matched = (best_xy - d) / 2;
                if (matched < d / 2)
                    return false;  // noise-similar; let greedy handle it
            }
        }

        if (final_d < 0)
            return false;  // edit distance exceeded the cap

        // Backtrack the trace to produce the SES in reverse, then reverse it.
        out_ses.clear();
        out_ses.reserve(static_cast<size_t>(N + M));
        int x = N, y = M;
        for (int d = final_d; d > 0; d--) {
            // Level d-1 spans [(d-1)^2, d^2); diagonal k sits at base + k.
            const size_t base = static_cast<size_t>(d - 1) * static_cast<size_t>(d - 1) +
                                static_cast<size_t>(d - 1);
            const int k = x - y;
            const bool came_from_above = (k == -d) ||
                (k != d && trace[base + k - 1] < trace[base + k + 1]);
            const int prev_k = came_from_above ? k + 1 : k - 1;
            const int prev_x = trace[base + prev_k];
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
                push_u32_be(diff, static_cast<uint32_t>(pos));
                push_u32_be(diff, static_cast<uint32_t>(deletes));
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
            push_u32_be(diff, static_cast<uint32_t>(pos + off));
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
                push_u32_be(diff, static_cast<uint32_t>(diff_start));
                diff.push_back(static_cast<uint8_t>(diff_bytes.size()));
                diff.insert(diff.end(), diff_bytes.begin(), diff_bytes.end());
            }
        }

        if (i < old_data.size()) {
            diff.push_back('X');
            push_u32_be(diff, static_cast<uint32_t>(i));
            push_u32_be(diff, static_cast<uint32_t>(old_data.size() - i));
        }
        while (j < new_data.size()) {
            size_t count = std::min(size_t(255), new_data.size() - j);
            diff.push_back('I');
            push_u32_be(diff, static_cast<uint32_t>(j));
            diff.push_back(static_cast<uint8_t>(count));
            for (size_t k = 0; k < count; ++k)
                diff.push_back(new_data[j + k]);
            j += count;
        }
        return diff;
    }

    // Scratch buffers for myersSES, reused across chunks.
    //
    // `myers_trace_` holds the per-level snapshots backtracking needs, flattened
    // into one buffer: only diagonals k in [-d, d] are read back from level d,
    // and those 2d+1 widths sum to exactly d*d, so level d occupies
    // [d*d, (d+1)*(d+1)) and diagonal k sits at (d*d + d) + k with no offset
    // bookkeeping.
    //
    // Both the trimming and the reuse are load-bearing for speed. Sizing
    // snapshots from d_limit rather than the distance actually reached made
    // every modified chunk pay the worst case: at d_limit=2048 each level
    // copied ~16 KB, so a chunk needing a handful of edits still generated
    // megabytes of allocation traffic. Re-growing the buffer per chunk is just
    // as costly the other way, because a buffer this size is served by mmap and
    // every growth step faults in fresh pages. Holding the high-water-mark
    // capacity across chunks avoids both.
    //
    // These make myersSES stateful: one Delta instance cannot generate two
    // deltas concurrently. generate_delta is a single sequential pass, so that
    // costs nothing today — but parallelising it means one Delta per worker.
    std::vector<int> myers_v_;
    std::vector<int> myers_trace_;

};

#endif // DELTA_HPP
