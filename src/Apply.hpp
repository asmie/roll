#ifndef APPLY_HPP
#define APPLY_HPP

#include "Delta.hpp"
#include "FileIO.hpp"
#include "Signature.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
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
	struct Result {
		bool success;
		std::string error_message;
		size_t entries_processed;
		size_t bytes_written;
	};

	/**
	* Apply a delta file against an old file to produce the reconstructed new file.
	* @param[in] old_file_path path to the original file
	* @param[in] delta_file_path path to the delta file produced by Delta<T,U>
	* @param[in] output_file_path path where the reconstructed new file will be written
	* @return Result with success flag and statistics
	*/
	Result apply_delta(const std::filesystem::path& old_file_path,
	                   const std::filesystem::path& delta_file_path,
	                   const std::filesystem::path& output_file_path)
	{
		Result result{false, "", 0, 0};

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
					result.error_message = "Output path aliases the old file";
					return result;
				}
				if (fs::equivalent(output_file_path, delta_file_path, ec)) {
					result.error_message = "Output path aliases the delta file";
					return result;
				}
			}
		}

		FileIO old_file, delta, output;
		if (!old_file.open(old_file_path, FileMode::IN)) {
			result.error_message = "Failed to open old file: " + old_file_path.string();
			return result;
		}
		if (!delta.open(delta_file_path, FileMode::IN)) {
			result.error_message = "Failed to open delta file: " + delta_file_path.string();
			return result;
		}
		if (!output.open(output_file_path, FileMode::OUT)) {
			result.error_message = "Failed to create output file: " + output_file_path.string();
			return result;
		}

		Signature<T, U> old_sig;
		old_sig.generate_signatures(old_file);
		const auto& old_chunks = old_sig.get_chunks();

		auto chunk_map = buildChunkMap(old_chunks);
		std::vector<bool> original_used(old_chunks.size(), false);

		U hash_func;
		const size_t hash_size = hash_func.get_hash_size();
		size_t new_idx = 0;

		while (true) {
			int peek = delta.peek_byte();
			if (peek == EOF) break;

			uint64_t entry_type_raw;
			if (!readU64Native(delta, entry_type_raw)) {
				result.error_message = "Truncated delta: partial entry header";
				return result;
			}

			uint64_t signature;
			if (!readU64Native(delta, signature)) {
				result.error_message = "Truncated delta: missing signature";
				return result;
			}

			auto hash_buf = delta.read_chunk(hash_size);
			if (!hash_buf || hash_buf->size() != hash_size) {
				result.error_message = "Truncated delta: missing hash";
				return result;
			}

			uint64_t chunk_size;
			if (!readU64Native(delta, chunk_size)) {
				result.error_message = "Truncated delta: missing chunk_size";
				return result;
			}

			switch (static_cast<EntryType>(entry_type_raw)) {
				case EntryType::ORIGINAL_CHUNK: {
					SignedChunk<typename T::RollingHashType> probe;
					probe.signature = signature;
					probe.hash = std::move(*hash_buf);
					probe.chunk_size = chunk_size;
					probe.start_offset = 0;

					size_t k;
					if (!findUnusedMatch(old_chunks, original_used, chunk_map, probe, k)) {
						result.error_message = "ORIGINAL entry references unknown chunk";
						return result;
					}

					auto data = old_file.read_chunk(old_chunks[k].chunk_size,
					                                old_chunks[k].start_offset);
					if (!data || data->size() != old_chunks[k].chunk_size) {
						result.error_message = "Failed to read old chunk";
						return result;
					}
					if (!output.write_chunk(*data)) {
						result.error_message = "Failed to write output chunk";
						return result;
					}
					original_used[k] = true;
					result.bytes_written += data->size();
					new_idx++;
					break;
				}

				case EntryType::ADDED_CHUNK: {
					auto payload = delta.read_chunk(chunk_size);
					if (!payload || payload->size() != chunk_size) {
						result.error_message = "Truncated delta: short ADDED payload";
						return result;
					}
					if (!verifyHash(hash_func, hash_size, *payload, *hash_buf)) {
						result.error_message = "ADDED entry hash mismatch";
						return result;
					}
					if (!output.write_chunk(*payload)) {
						result.error_message = "Failed to write output chunk";
						return result;
					}
					result.bytes_written += payload->size();
					new_idx++;
					break;
				}

				case EntryType::MODIFIED_CHUNK: {
					if (new_idx >= old_chunks.size()) {
						result.error_message = "MODIFIED entry has no source old chunk";
						return result;
					}
					const auto& src = old_chunks[new_idx];
					auto old_data = old_file.read_chunk(src.chunk_size, src.start_offset);
					if (!old_data || old_data->size() != src.chunk_size) {
						result.error_message = "Failed to read MODIFIED source chunk";
						return result;
					}

					std::vector<uint8_t> reconstructed;
					if (!applyDiff(delta, *old_data, chunk_size, reconstructed, result))
						return result;

					if (!verifyHash(hash_func, hash_size, reconstructed, *hash_buf)) {
						result.error_message = "MODIFIED entry hash mismatch";
						return result;
					}
					if (!output.write_chunk(reconstructed)) {
						result.error_message = "Failed to write output chunk";
						return result;
					}
					original_used[new_idx] = true;
					result.bytes_written += reconstructed.size();
					new_idx++;
					break;
				}

				case EntryType::REMOVED_CHUNK: {
					// REMOVED entries produce no output and don't advance new_idx,
					// but each must consume a distinct unused old chunk so that
					// duplicate or extraneous REMOVEDs are rejected.
					SignedChunk<typename T::RollingHashType> probe;
					probe.signature = signature;
					probe.hash = std::move(*hash_buf);
					probe.chunk_size = chunk_size;
					probe.start_offset = 0;

					size_t k;
					if (!findUnusedMatch(old_chunks, original_used, chunk_map, probe, k)) {
						result.error_message = "REMOVED entry references unknown or already-consumed old chunk";
						return result;
					}
					original_used[k] = true;
					break;
				}

				default:
					result.error_message = "Unknown entry type in delta";
					return result;
			}

			result.entries_processed++;
		}

		old_file.close();
		delta.close();
		if (!output.close()) {
			result.error_message = "Failed to flush output file";
			return result;
		}

		result.success = true;
		return result;
	}

private:
	struct ChunkHash {
		size_t operator()(const SignedChunk<typename T::RollingHashType>& c) const {
			size_t h1 = std::hash<typename T::RollingHashType>{}(c.signature);
			size_t h2 = 0;
			if (c.hash.size() >= sizeof(size_t))
				std::memcpy(&h2, c.hash.data(), sizeof(size_t));
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

	ChunkMap buildChunkMap(const std::vector<SignedChunk<typename T::RollingHashType>>& chunks) {
		ChunkMap map;
		map.reserve(chunks.size());
		for (size_t i = 0; i < chunks.size(); ++i)
			map[chunks[i]] = i;
		return map;
	}

	bool findUnusedMatch(const std::vector<SignedChunk<typename T::RollingHashType>>& old_chunks,
	                     const std::vector<bool>& original_used,
	                     const ChunkMap& chunk_map,
	                     const SignedChunk<typename T::RollingHashType>& probe,
	                     size_t& out_index) {
		auto it = chunk_map.find(probe);
		if (it != chunk_map.end() && !original_used[it->second]) {
			out_index = it->second;
			return true;
		}
		// Map's preferred index is consumed (duplicate chunk content) — scan for any unused match.
		for (size_t i = 0; i < old_chunks.size(); ++i) {
			if (!original_used[i] && old_chunks[i] == probe) {
				out_index = i;
				return true;
			}
		}
		return false;
	}

	bool verifyHash(U& hash_func, size_t hash_size,
	                std::span<const uint8_t> chunk_data,
	                const std::vector<uint8_t>& expected) {
		if (expected.size() != hash_size) return false;
		std::vector<uint8_t> computed(hash_size);
		hash_func.hash(computed, chunk_data);
		return computed == expected;
	}

	bool readU64Native(FileIO& f, uint64_t& out) {
		auto buf = f.read_chunk(sizeof(uint64_t));
		if (!buf || buf->size() != sizeof(uint64_t)) return false;
		std::memcpy(&out, buf->data(), sizeof(uint64_t));
		return true;
	}

	bool readU32BE(FileIO& f, uint32_t& out) {
		auto buf = f.read_chunk(sizeof(uint32_t));
		if (!buf || buf->size() != sizeof(uint32_t)) return false;
		uint32_t v;
		std::memcpy(&v, buf->data(), sizeof(uint32_t));
		if constexpr (std::endian::native == std::endian::little)
			v = std::byteswap(v);
		out = v;
		return true;
	}

	/**
	* Parse 'D'/'X'/'I' opcodes from delta against old_data, producing exactly
	* target_size output bytes. Stops when next byte is not a recognized opcode;
	* remaining bytes are tail-copied from old_data. Requires at least one
	* opcode — a MODIFIED entry with no opcodes is malformed.
	*/
	bool applyDiff(FileIO& delta, const std::vector<uint8_t>& old_data,
	               uint64_t target_size, std::vector<uint8_t>& output, Result& result) {
		output.reserve(target_size);
		size_t old_pos = 0;
		size_t new_pos = 0;
		size_t opcodes_seen = 0;

		// Drain every opcode in this entry's diff payload. Stopping at
		// output.size() == target_size would leave a trailing 'X' (delete tail)
		// unread, which the outer parser would then misread as the next entry.
		while (true) {
			int peek = delta.peek_byte();
			if (peek != 'D' && peek != 'X' && peek != 'I') break;

			(void) delta.read_byte();
			char op = static_cast<char>(peek);
			++opcodes_seen;

			uint32_t pos;
			if (!readU32BE(delta, pos)) {
				result.error_message = "Truncated diff: missing pos";
				return false;
			}

			if (pos < new_pos) {
				result.error_message = "Diff position went backwards";
				return false;
			}
			size_t match_len = pos - new_pos;
			if (old_pos + match_len > old_data.size() ||
			    output.size() + match_len > target_size) {
				result.error_message = "Diff out of bounds (match copy)";
				return false;
			}
			output.insert(output.end(),
			              old_data.begin() + old_pos,
			              old_data.begin() + old_pos + match_len);
			old_pos += match_len;
			new_pos += match_len;

			if (op == 'D' || op == 'I') {
				int count_int = delta.read_byte();
				if (count_int == EOF) {
					result.error_message = "Truncated diff: missing count byte";
					return false;
				}
				uint8_t count = static_cast<uint8_t>(count_int);
				auto inline_buf = delta.read_chunk(count);
				if (!inline_buf || inline_buf->size() != count) {
					result.error_message = "Truncated diff: missing inline bytes";
					return false;
				}
				if (output.size() + count > target_size) {
					result.error_message = "Diff out of bounds (inline write)";
					return false;
				}
				output.insert(output.end(), inline_buf->begin(), inline_buf->end());
				new_pos += count;
				if (op == 'D') {
					if (old_pos + count > old_data.size()) {
						result.error_message = "Diff out of bounds (D advance)";
						return false;
					}
					old_pos += count;
				}
			} else { // 'X'
				uint32_t length;
				if (!readU32BE(delta, length)) {
					result.error_message = "Truncated diff: missing X length";
					return false;
				}
				if (old_pos + length > old_data.size()) {
					result.error_message = "Diff out of bounds (X delete)";
					return false;
				}
				old_pos += length;
			}
		}

		if (opcodes_seen == 0) {
			result.error_message = "MODIFIED entry has no diff opcodes";
			return false;
		}

		// Tail copy: any remaining target bytes come from the matching old-data tail.
		if (output.size() < target_size) {
			size_t remaining = target_size - output.size();
			if (old_pos + remaining > old_data.size()) {
				result.error_message = "Diff tail copy out of bounds";
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
