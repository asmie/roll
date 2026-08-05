#ifndef APPLY_HPP
#define APPLY_HPP

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
* Every entry produces one chunk of output, in order, and names its source in
* the old file by index — so this applier just follows the stream and never has
* to work out which old chunk an entry meant. The previous format left MODIFIED
* entries anchored positionally, which coupled the two sides and was noted here
* as needing a format change to fix; that coupling is gone.
*
* What the applier still owes the caller: the old file it re-chunks must be the
* one the delta was built against. A per-entry digest catches a changed old file
* at the entry that first diverges, and the trailer digest catches anything the
* per-entry checks could miss across the whole reconstruction.
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

		U hash_func;
		const size_t trailer_size = hash_func.get_hash_size();

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
				std::vector<uint8_t> computed(trailer_size);
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

			// Resolve the referenced old chunk once, for both entry kinds that
			// have one. An index past the end means the delta does not match this
			// old file — most often because the old file changed since the delta
			// was made.
			const SignedChunk<typename T::RollingHashType>* source = nullptr;
			if (header.references_old()) {
				if (header.old_index >= old_chunks.size()) {
					return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
					                                    "Entry references old chunk " +
					                                    std::to_string(header.old_index) +
					                                    ", but the old file has " +
					                                    std::to_string(old_chunks.size())});
				}
				source = &old_chunks[static_cast<size_t>(header.old_index)];
			}

			std::vector<uint8_t> produced;

			switch (header.type) {
				case EntryType::ORIGINAL_CHUNK: {
					// The recorded digest is the old chunk's own, so comparing it
					// against the regenerated signature detects a changed old file
					// without re-hashing the bytes.
					if (source->hash != header.digest) {
						return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
						                                    "ORIGINAL entry digest does not match the old chunk"});
					}
					produced = old_file.read_chunk(source->chunk_size, source->start_offset);
					if (produced.size() != source->chunk_size) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to read old chunk"});
					}
					break;
				}

				case EntryType::ADDED_CHUNK: {
					if (!reader.read_payload(static_cast<size_t>(header.out_size), produced)) {
						return std::unexpected(DeltaError{DeltaErrc::corrupt_delta,
						                                    "Truncated delta: short ADDED payload"});
					}
					if (!verifyDigest(hash_func, produced, header.digest)) {
						return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
						                                    "ADDED entry hash mismatch"});
					}
					break;
				}

				case EntryType::MODIFIED_CHUNK: {
					auto old_data = old_file.read_chunk(source->chunk_size, source->start_offset);
					if (old_data.size() != source->chunk_size) {
						return std::unexpected(DeltaError{DeltaErrc::io_error,
						                                    "Failed to read MODIFIED source chunk"});
					}

					DeltaError diff_error;
					if (!applyDiff(reader, old_data, header.out_size, produced, diff_error))
						return std::unexpected(std::move(diff_error));

					if (!verifyDigest(hash_func, produced, header.digest)) {
						return std::unexpected(DeltaError{DeltaErrc::integrity_mismatch,
						                                    "MODIFIED entry hash mismatch"});
					}
					break;
				}
			}

			if (!output.write_chunk(produced)) {
				return std::unexpected(DeltaError{DeltaErrc::io_error,
				                                    "Failed to write output chunk"});
			}
			whole_file_hash.update(produced);
			stats.bytes_written += produced.size();
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

	/// Hash `data` and compare against a DELTA_DIGEST_BYTES-truncated digest.
	bool verifyDigest(U& hash_func, std::span<const uint8_t> data,
	                  const std::vector<uint8_t>& expected) {
		if (expected.size() != DELTA_DIGEST_BYTES) return false;
		std::vector<uint8_t> computed(hash_func.get_hash_size());
		hash_func.hash(computed, data);
		computed.resize(DELTA_DIGEST_BYTES);
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
