#ifndef APPLY_HPP
#define APPLY_HPP

#include "ChunkIndex.hpp"
#include "Delta.hpp"
#include "DeltaCodec.hpp"
#include "DeltaError.hpp"
#include "DeltaFormat.hpp"
#include "FileIO.hpp"
#include "Signature.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

/**
* Class for applying a delta produced by Delta<T,U> against an old file to
* reconstruct the new file.
*
* Format contract assumed by this applier (must stay in sync with Delta<T,U>):
*  - Entries appear in target (new-file) chunk-position order. REMOVED entries
*    (which produce no output) appear after all non-REMOVED entries.
*  - For a MODIFIED entry at the i-th non-REMOVED position, the source old
*    chunk is old_chunks[i] (the same-position chunk in the regenerated old
*    signature). There is no source-anchor field on MODIFIED entries; if Delta
*    ever pairs MODIFIED with a different old chunk, this applier will need a
*    format change to follow.
*/
template<RollingHashAlgorithm T, StrongHashAlgorithm U>
class Apply {
public:
	using Result = std::expected<ApplyStats, DeltaError>;

	/**
	* Apply a delta file against an old file to produce the reconstructed new file.
	* @param[in] old_file_path path to the original file
	* @param[in] delta_file_path path to the delta file produced by Delta<T,U>
	* @param[in] output_file_path path where the reconstructed new file will be written
	* @return Statistics on success, or the failure that stopped application.
	*/
	[[nodiscard]] Result apply_delta(const std::filesystem::path& old_file_path,
	                                 const std::filesystem::path& delta_file_path,
	                                 const std::filesystem::path& output_file_path)
	{
		bool output_opened = false;
		Result result;
		// Convert escaping exceptions into a failed Result so the stub cleanup
		// below still runs. Without this, an exception (a hash-backend error, a
		// bad_alloc) unwinds past the cleanup and leaves a partial output file
		// that looks like a successful reconstruction.
		try {
			result = apply_delta_impl(old_file_path, delta_file_path, output_file_path,
			                         output_opened);
		} catch (const std::exception& e) {
			result = std::unexpected(DeltaError{DeltaErrc::internal_error,
			                                    std::string("Unexpected error: ") + e.what()});
		}
		// Don't leave a half-written stub on disk: if we opened the output and
		// the run failed, remove the file. Alias checks run before open, so we
		// will never delete the user's old/delta here.
		if (!result.has_value() && output_opened) {
			std::error_code ec;
			std::filesystem::remove(output_file_path, ec);
		}
		return result;
	}

private:
	Result apply_delta_impl(const std::filesystem::path& old_file_path,
	                        const std::filesystem::path& delta_file_path,
	                        const std::filesystem::path& output_file_path,
	                        bool& output_opened)
	{
		ApplyStats stats;

		// Reject output paths that alias either input. Opening the output in
		// FileMode::OUT truncates the target, which would destroy old or delta
		// before they're read. std::filesystem::equivalent compares filesystem
		// objects, so symlinks/hardlinks/relative paths to the same file are
		// caught — but it requires both paths to exist, hence the exists guard.
		{
			namespace fs = std::filesystem;
			std::error_code ec;
			if (fs::exists(output_file_path, ec)) {
				if (fs::equivalent(output_file_path, old_file_path, ec)) {
					return std::unexpected(DeltaError{DeltaErrc::invalid_argument,
					                                    "Output path aliases the old file"});
				}
				if (fs::equivalent(output_file_path, delta_file_path, ec)) {
					return std::unexpected(DeltaError{DeltaErrc::invalid_argument,
					                                    "Output path aliases the delta file"});
				}
			}
		}

		FileIO old_file, delta, output;
		if (!old_file.open(old_file_path, FileMode::IN)) {
			return std::unexpected(DeltaError{DeltaErrc::io_error,
			                                    "Failed to open old file: " + old_file_path.string()});
		}
		if (!delta.open(delta_file_path, FileMode::IN)) {
			return std::unexpected(DeltaError{DeltaErrc::io_error,
			                                    "Failed to open delta file: " + delta_file_path.string()});
		}
		if (!output.open(output_file_path, FileMode::OUT)) {
			return std::unexpected(DeltaError{DeltaErrc::io_error,
			                                    "Failed to create output file: " + output_file_path.string()});
		}
		output_opened = true;

		DeltaReader reader(delta, U{}.get_hash_size());
		if (!reader.read_header()) {
			return std::unexpected(DeltaError{DeltaErrc::corrupt_delta, reader.error()});
		}

		Signature<T, U> old_sig;
		if (!old_sig.generate_signatures(old_file))
			return std::unexpected(DeltaError{DeltaErrc::io_error,
			                                 "Failed to read old file: " + old_file_path.string()});
		const auto& old_chunks = old_sig.get_chunks();

		auto chunk_map = build_chunk_map(old_chunks);
		std::vector<bool> original_used(old_chunks.size(), false);

		U hash_func;
		const size_t hash_size = hash_func.get_hash_size();
		size_t new_idx = 0;
		bool seen_removed = false;

		U whole_file_hash;
		whole_file_hash.init();
		bool saw_trailer = false;

		while (true) {
			const auto item = reader.next_item();
			if (item == DeltaReader::Item::End) break;

			if (item == DeltaReader::Item::Trailer) {
				std::vector<uint8_t> trailer;
				if (!reader.read_trailer(trailer)) {
					return std::unexpected(DeltaError{DeltaErrc::corrupt_delta, reader.error()});
				}
				std::vector<uint8_t> computed(hash_size);
				whole_file_hash.finalize(computed);
				if (computed != trailer) {
					return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
					                                    "Whole-file hash mismatch"});
				}
				saw_trailer = true;
				break;
			}

			DeltaEntryHeader header;
			if (!reader.read_entry_header(header)) {
				return std::unexpected(DeltaError{DeltaErrc::corrupt_delta, reader.error()});
			}

			const auto entry_type = header.type;
			const uint64_t signature = header.signature;
			const uint64_t chunk_size = header.chunk_size;
			auto hash_buf = std::move(header.hash);
			if (seen_removed && entry_type != EntryType::REMOVED_CHUNK) {
				return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
				                                    "Non-REMOVED entry after REMOVED"});
			}

			switch (entry_type) {
				case EntryType::ORIGINAL_CHUNK: {
					SignedChunk<typename T::RollingHashType> probe;
					probe.signature = signature;
					probe.hash = std::move(hash_buf);
					probe.chunk_size = chunk_size;
					probe.start_offset = 0;

					size_t k;
					if (!find_unused_match(old_chunks, original_used, chunk_map, probe, k)) {
						return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
						                                    "ORIGINAL entry references unknown chunk"});
					}

					auto data = old_file.read_chunk(old_chunks[k].chunk_size,
					                                old_chunks[k].start_offset);
					if (data.size() != old_chunks[k].chunk_size) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to read old chunk"});
					}
					if (!output.write_chunk(data)) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to write output chunk"});
					}
					whole_file_hash.update(data);
					original_used[k] = true;
					stats.bytes_written += data.size();
					new_idx++;
					break;
				}

				case EntryType::ADDED_CHUNK: {
					std::vector<uint8_t> payload;
					if (!reader.read_payload(chunk_size, payload)) {
						return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
						                                    "Truncated delta: short ADDED payload"});
					}
					if (!verifyHash(hash_func, hash_size, payload, hash_buf)) {
						return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
						                                    "ADDED entry hash mismatch"});
					}
					if (!output.write_chunk(payload)) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to write output chunk"});
					}
					whole_file_hash.update(payload);
					stats.bytes_written += payload.size();
					new_idx++;
					break;
				}

				case EntryType::MODIFIED_CHUNK: {
					if (new_idx >= old_chunks.size()) {
						return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
						                                    "MODIFIED entry has no source old chunk"});
					}
					const auto& src = old_chunks[new_idx];
					auto old_data = old_file.read_chunk(src.chunk_size, src.start_offset);
					if (old_data.size() != src.chunk_size) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to read MODIFIED source chunk"});
					}

					std::vector<uint8_t> reconstructed;
					DeltaError diff_error;
					if (!applyDiff(reader, old_data, chunk_size, reconstructed, diff_error))
						return std::unexpected(std::move(diff_error));

					if (!verifyHash(hash_func, hash_size, reconstructed, hash_buf)) {
						return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
						                                    "MODIFIED entry hash mismatch"});
					}
					if (!output.write_chunk(reconstructed)) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to write output chunk"});
					}
					whole_file_hash.update(reconstructed);
					original_used[new_idx] = true;
					stats.bytes_written += reconstructed.size();
					new_idx++;
					break;
				}

				case EntryType::REMOVED_CHUNK: {
					// REMOVED entries produce no output and don't advance new_idx,
					// but each must consume a distinct unused old chunk so that
					// duplicate or extraneous REMOVEDs are rejected.
					SignedChunk<typename T::RollingHashType> probe;
					probe.signature = signature;
					probe.hash = std::move(hash_buf);
					probe.chunk_size = chunk_size;
					probe.start_offset = 0;

					size_t k;
					if (!find_unused_match(old_chunks, original_used, chunk_map, probe, k)) {
						return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
						                                    "REMOVED entry references unknown or already-consumed old chunk"});
					}
					original_used[k] = true;
					seen_removed = true;
					break;
				}

				default:
					return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
					                                    "Unknown entry type in delta"});
			}

			stats.entries_processed++;
		}

		if (!saw_trailer) {
			return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
			                                    "Missing delta trailer"});
		}

		(void) old_file.close();  // reads have nothing to flush
		(void) delta.close();
		if (!output.close()) {
			return std::unexpected(DeltaError{DeltaErrc::io_error,
			                                    "Failed to flush output file"});
		}

		return stats;
	}

	using ChunkMap = ::ChunkMap<typename T::RollingHashType>;

	bool verifyHash(U& hash_func, size_t hash_size,
	                std::span<const uint8_t> chunk_data,
	                const std::vector<uint8_t>& expected) {
		if (expected.size() != hash_size) return false;
		std::vector<uint8_t> computed(hash_size);
		hash_func.hash(computed, chunk_data);
		return computed == expected;
	}

	/**
	* Parse 'D'/'X'/'I' opcodes from delta against old_data, producing exactly
	* target_size output bytes. Stops when next byte is not a recognized opcode;
	* remaining bytes are tail-copied from old_data. Requires at least one
	* opcode — a MODIFIED entry with no opcodes is malformed.
	*/
	bool applyDiff(DeltaReader& reader, const std::vector<uint8_t>& old_data,
	               uint64_t target_size, std::vector<uint8_t>& output, DeltaError& error) {
		output.reserve(target_size);
		size_t old_pos = 0;
		size_t new_pos = 0;
		size_t opcodes_seen = 0;

		// Drain every opcode in this entry's diff payload. Stopping at
		// output.size() == target_size would leave a trailing 'X' (delete tail)
		// unread, which the outer parser would then misread as the next entry.
		while (reader.at_diff_opcode()) {
			DiffOpcode opcode;
			if (!reader.read_diff_opcode(opcode)) {
				error = DeltaError{DeltaErrc::corrupt_delta, reader.error()};
				return false;
			}
			++opcodes_seen;

			if (opcode.pos < new_pos) {
				error = DeltaError{DeltaErrc::corrupt_delta, "Diff position went backwards"};
				return false;
			}
			size_t match_len = opcode.pos - new_pos;
			if (old_pos + match_len > old_data.size() ||
			    output.size() + match_len > target_size) {
				error = DeltaError{DeltaErrc::corrupt_delta, "Diff out of bounds (match copy)"};
				return false;
			}
			output.insert(output.end(),
			              old_data.begin() + old_pos,
			              old_data.begin() + old_pos + match_len);
			old_pos += match_len;
			new_pos += match_len;

			if (opcode.op == 'D' || opcode.op == 'I') {
				const size_t count = opcode.bytes.size();
				if (output.size() + count > target_size) {
					error = DeltaError{DeltaErrc::corrupt_delta, "Diff out of bounds (inline write)"};
					return false;
				}
				output.insert(output.end(), opcode.bytes.begin(), opcode.bytes.end());
				new_pos += count;
				if (opcode.op == 'D') {
					if (old_pos + count > old_data.size()) {
						error = DeltaError{DeltaErrc::corrupt_delta, "Diff out of bounds (D advance)"};
						return false;
					}
					old_pos += count;
				}
			} else { // 'X'
				if (old_pos + opcode.delete_length > old_data.size()) {
					error = DeltaError{DeltaErrc::corrupt_delta, "Diff out of bounds (X delete)"};
					return false;
				}
				old_pos += opcode.delete_length;
			}
		}

		if (opcodes_seen == 0) {
			error = DeltaError{DeltaErrc::corrupt_delta, "MODIFIED entry has no diff opcodes"};
			return false;
		}

		// Tail copy: any remaining target bytes come from the matching old-data tail.
		if (output.size() < target_size) {
			size_t remaining = target_size - output.size();
			if (old_pos + remaining > old_data.size()) {
				error = DeltaError{DeltaErrc::corrupt_delta, "Diff tail copy out of bounds"};
				return false;
			}
			output.insert(output.end(),
			              old_data.begin() + old_pos,
			              old_data.begin() + old_pos + remaining);
		}

		return output.size() == target_size;
	}
};

#endif // APPLY_HPP
