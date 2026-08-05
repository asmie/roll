#ifndef DELTACODEC_HPP
#define DELTACODEC_HPP

#include "DeltaFormat.hpp"
#include "FileIO.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

/**
* Serialisation for the delta wire format.
*
* Reader and writer live together deliberately. The format previously had three
* independent implementations — the writer in Delta, the reader in Apply, and a
* second reader in DeltaViewer — each with its own big-endian integer helpers
* and its own idea of the digest length. They drifted: the viewer hard-coded a
* 64-byte hash while the applier asked the hash object, so changing the digest
* would have silently broken inspection only. Anything about the byte layout
* belongs here, so a format change is one edit rather than three.
*
* Layout:
*   header  : DELTA_MAGIC[4] | version:u32
*   entry   : type:u64 | signature:u64 | hash:hash_size | chunk_size:u64
*             followed by chunk_size payload bytes for ADDED, or a diff opcode
*             run for MODIFIED
*   opcode  : 'D'|'I' | pos:u32 | count:u8 | count bytes
*             'X'     | pos:u32 | length:u32
*   trailer : DELTA_TRAILER_TAG | hash:hash_size
*
* All multi-byte integers are big-endian.
*/

// ---- Big-endian integer primitives ---------------------------------------

inline void push_u32_be(std::vector<uint8_t>& out, uint32_t value)
{
	if constexpr (std::endian::native == std::endian::little)
		value = std::byteswap(value);
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(value));
}

inline void push_u64_be(std::vector<uint8_t>& out, uint64_t value)
{
	if constexpr (std::endian::native == std::endian::little)
		value = std::byteswap(value);
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(value));
}

inline uint32_t load_u32_be(const uint8_t* src)
{
	uint32_t value;
	std::memcpy(&value, src, sizeof(value));
	if constexpr (std::endian::native == std::endian::little)
		value = std::byteswap(value);
	return value;
}

inline uint64_t load_u64_be(const uint8_t* src)
{
	uint64_t value;
	std::memcpy(&value, src, sizeof(value));
	if constexpr (std::endian::native == std::endian::little)
		value = std::byteswap(value);
	return value;
}

// ---- Wire structures -----------------------------------------------------

/// One entry header as it appears on the wire, without its payload.
struct DeltaEntryHeader {
	EntryType type {};
	uint64_t signature {};
	std::vector<uint8_t> hash;
	uint64_t chunk_size {};
};

/// One diff opcode. `bytes` carries the inline payload for 'D'/'I';
/// `delete_length` is meaningful only for 'X'.
struct DiffOpcode {
	char op {};
	uint32_t pos {};
	uint32_t delete_length {};
	std::vector<uint8_t> bytes;
};

[[nodiscard]] inline bool is_diff_opcode(int byte)
{
	return byte == 'D' || byte == 'I' || byte == 'X';
}

// ---- Writer --------------------------------------------------------------

/**
* Writes the delta stream to a FileIO. Every method returns false and records a
* message in error() on the first failure; callers may stop at that point.
*/
class DeltaWriter {
public:
	explicit DeltaWriter(FileIO& out) : out_(out) {}

	[[nodiscard]] bool write_header()
	{
		std::vector<uint8_t> header;
		header.reserve(DELTA_HEADER_SIZE);
		header.insert(header.end(), std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
		push_u32_be(header, DELTA_FORMAT_VERSION);
		return emit(header, "delta header");
	}

	/**
	* Write one entry. `payload` carries the raw chunk bytes for ADDED or the
	* diff opcode run for MODIFIED, and must be empty for ORIGINAL/REMOVED.
	*/
	[[nodiscard]] bool write_entry(EntryType type, uint64_t signature,
	                 std::span<const uint8_t> hash, uint64_t chunk_size,
	                 std::span<const uint8_t> payload)
	{
		std::vector<uint8_t> record;
		record.reserve(3 * sizeof(uint64_t) + hash.size() + payload.size());
		push_u64_be(record, static_cast<uint64_t>(type));
		push_u64_be(record, signature);
		record.insert(record.end(), hash.begin(), hash.end());
		push_u64_be(record, chunk_size);
		record.insert(record.end(), payload.begin(), payload.end());
		return emit(record, "delta entry");
	}

	[[nodiscard]] bool write_trailer(std::span<const uint8_t> digest)
	{
		std::vector<uint8_t> trailer;
		trailer.reserve(1 + digest.size());
		trailer.push_back(DELTA_TRAILER_TAG);
		trailer.insert(trailer.end(), digest.begin(), digest.end());
		return emit(trailer, "delta trailer");
	}

	[[nodiscard]] size_t bytes_written() const noexcept { return bytes_written_; }
	[[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
	bool emit(const std::vector<uint8_t>& bytes, const char* what)
	{
		if (!out_.write_chunk(bytes)) {
			error_ = std::string("Failed to write ") + what;
			return false;
		}
		bytes_written_ += bytes.size();
		return true;
	}

	FileIO& out_;
	size_t bytes_written_ { 0 };
	std::string error_;
};

// ---- Reader --------------------------------------------------------------

/**
* Reads the delta stream from a FileIO. `hash_size` is the digest length the
* stream was written with, taken from the strong hash in use rather than
* assumed, so a digest change cannot silently desynchronise one reader.
*/
class DeltaReader {
public:
	/// What comes next in the stream.
	enum class Item { Entry, Trailer, End };

	DeltaReader(FileIO& in, size_t hash_size) : in_(in), hash_size_(hash_size) {}

	[[nodiscard]] bool read_header()
	{
		auto buf = in_.read_chunk(DELTA_HEADER_SIZE);
		if (buf.size() != DELTA_HEADER_SIZE) {
			error_ = "Truncated delta: missing header";
			return false;
		}
		if (!std::equal(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC), buf.begin())) {
			error_ = "Bad delta magic";
			return false;
		}
		const uint32_t version = load_u32_be(buf.data() + sizeof(DELTA_MAGIC));
		if (version != DELTA_FORMAT_VERSION) {
			error_ = "Unsupported delta version: " + std::to_string(version);
			return false;
		}
		return true;
	}

	[[nodiscard]] Item next_item()
	{
		const int peek = in_.peek_byte();
		if (peek == EOF)
			return Item::End;
		if (peek == DELTA_TRAILER_TAG)
			return Item::Trailer;
		return Item::Entry;
	}

	/**
	* Read an entry header and validate the declared chunk size against the
	* format bound. The size field is attacker-controlled and feeds allocations
	* downstream, so it is checked here rather than at each use.
	*/
	[[nodiscard]] bool read_entry_header(DeltaEntryHeader& out)
	{
		std::vector<uint8_t> buf;
		if (!read_exact(2 * sizeof(uint64_t) + hash_size_ + sizeof(uint64_t), buf)) {
			error_ = "Truncated delta: partial entry header";
			return false;
		}

		const uint8_t* p = buf.data();
		out.type = static_cast<EntryType>(load_u64_be(p));
		out.signature = load_u64_be(p + sizeof(uint64_t));
		out.hash.assign(p + 2 * sizeof(uint64_t),
		                p + 2 * sizeof(uint64_t) + hash_size_);
		out.chunk_size = load_u64_be(p + 2 * sizeof(uint64_t) + hash_size_);

		if (out.chunk_size > DELTA_MAX_CHUNK_SIZE) {
			error_ = "Delta entry declares an out-of-range chunk size: " +
			         std::to_string(out.chunk_size);
			return false;
		}
		return true;
	}

	/// Read exactly `count` payload bytes.
	[[nodiscard]] bool read_payload(size_t count, std::vector<uint8_t>& out)
	{
		if (!read_exact(count, out)) {
			error_ = "Truncated delta: short payload";
			return false;
		}
		return true;
	}

	[[nodiscard]] bool at_diff_opcode() { return is_diff_opcode(in_.peek_byte()); }

	/**
	* Read one diff opcode. Call only while at_diff_opcode() is true; the opcode
	* run ends at the first byte that is not a recognised opcode.
	*/
	[[nodiscard]] bool read_diff_opcode(DiffOpcode& out)
	{
		const int tag = in_.read_byte();
		if (!is_diff_opcode(tag)) {
			error_ = "Truncated diff: missing opcode";
			return false;
		}
		out = DiffOpcode{};
		out.op = static_cast<char>(tag);

		std::vector<uint8_t> buf;
		if (!read_exact(sizeof(uint32_t), buf)) {
			error_ = "Truncated diff: missing pos";
			return false;
		}
		out.pos = load_u32_be(buf.data());

		if (out.op == 'X') {
			if (!read_exact(sizeof(uint32_t), buf)) {
				error_ = "Truncated diff: missing X length";
				return false;
			}
			out.delete_length = load_u32_be(buf.data());
			return true;
		}

		const int count = in_.read_byte();
		if (count == EOF) {
			error_ = "Truncated diff: missing count byte";
			return false;
		}
		if (!read_exact(static_cast<size_t>(count), out.bytes)) {
			error_ = "Truncated diff: missing inline bytes";
			return false;
		}
		return true;
	}

	[[nodiscard]] bool read_trailer(std::vector<uint8_t>& digest)
	{
		if (in_.read_byte() != DELTA_TRAILER_TAG) {
			error_ = "Truncated delta: missing trailer tag";
			return false;
		}
		if (!read_exact(hash_size_, digest)) {
			error_ = "Truncated delta: short trailer";
			return false;
		}
		return true;
	}

	[[nodiscard]] size_t hash_size() const noexcept { return hash_size_; }
	[[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
	bool read_exact(size_t count, std::vector<uint8_t>& out)
	{
		out = in_.read_chunk(count);
		return out.size() == count;
	}

	FileIO& in_;
	size_t hash_size_;
	std::string error_;
};

#endif // DELTACODEC_HPP
