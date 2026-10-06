#ifndef DELTACODEC_HPP
#define DELTACODEC_HPP

#include "DeltaFormat.hpp"
#include "FileIO.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

/**
* Serialisation for the delta wire format.
*
* Reader and writer live together deliberately. The format previously had three
* independent implementations — the writer in Delta, the reader in Apply, and a
* second reader in DeltaViewer — each with its own integer helpers and its own
* idea of the digest length. They drifted: the viewer hard-coded a 64-byte hash
* while the applier asked the hash object, so changing the digest would have
* silently broken inspection only. Anything about the byte layout belongs here,
* so a format change is one edit rather than three.
*
* Layout:
*   header   : DELTA_MAGIC[4] | version:u32be
*   entry    : type:u8 | body, where body is
*                ORIGINAL : old_index:varint | digest
*                ADDED    : out_size:varint  | digest | out_size payload bytes
*                MODIFIED : old_index:varint | out_size:varint | digest | opcodes
*   opcode   : 'D'|'I' | pos:u32be | count:u8 | count bytes
*              'X'     | pos:u32be | length:u32be
*   trailer  : DELTA_TRAILER_TAG | whole-file digest
*
* Lengths and indices are LEB128 varints: a chunk index and a chunk size both
* fit in one or two bytes in practice, but must remain able to express a 64-bit
* value. Fixed-width fields (the version, opcode positions) stay big-endian.
*/

// ---- Integer primitives --------------------------------------------------

inline void push_u32_be(std::vector<uint8_t>& out, uint32_t value)
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

/// Longest LEB128 encoding of a 64-bit value: nine full groups plus a final bit.
inline constexpr size_t VARINT_MAX_BYTES = 10;

inline void push_varint(std::vector<uint8_t>& out, uint64_t value)
{
	while (value >= 0x80) {
		out.push_back(static_cast<uint8_t>(value) | 0x80);
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

[[nodiscard]] inline size_t varint_size(uint64_t value)
{
	size_t n = 1;
	while (value >= 0x80) {
		value >>= 7;
		++n;
	}
	return n;
}

// ---- Wire structures -----------------------------------------------------

/// One entry header as it appears on the wire, without any payload. Which
/// fields carry meaning depends on `type`; see the layout above.
struct DeltaEntryHeader {
	EntryType type {};
	uint64_t old_index {};   ///< ORIGINAL, MODIFIED: source chunk in the old file
	uint64_t out_size {};    ///< ADDED, MODIFIED: bytes this entry contributes
	std::vector<uint8_t> digest;

	[[nodiscard]] bool references_old() const noexcept
	{
		return type == EntryType::ORIGINAL_CHUNK || type == EntryType::MODIFIED_CHUNK;
	}
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

	[[nodiscard]] static size_t added_size(size_t payload_size) noexcept {
		return 1 + varint_size(payload_size) + DELTA_DIGEST_BYTES + payload_size;
	}
	[[nodiscard]] static size_t modified_size(uint64_t old_index, size_t out_size, size_t diff_size) noexcept {
		return 1 + varint_size(old_index) + varint_size(out_size) + DELTA_DIGEST_BYTES + diff_size;
	}

	[[nodiscard]] bool write_header()
	{
		std::vector<uint8_t> header;
		header.reserve(DELTA_HEADER_SIZE);
		header.insert(header.end(), std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
		push_u32_be(header, DELTA_FORMAT_VERSION);
		return emit(header, "delta header");
	}

	/// Reuse an old chunk verbatim.
	[[nodiscard]] bool write_original(uint64_t old_index, std::span<const uint8_t> digest)
	{
		return emitRecord(EntryType::ORIGINAL_CHUNK, {old_index}, digest, {}, "ORIGINAL entry");
	}

	/// Supply new bytes verbatim.
	[[nodiscard]] bool write_added(std::span<const uint8_t> digest,
	                               std::span<const uint8_t> payload)
	{
		return emitRecord(EntryType::ADDED_CHUNK, {payload.size()}, digest, payload, "ADDED entry");
	}

	/// Rebuild a chunk from an old one plus a diff opcode run.
	[[nodiscard]] bool write_modified(uint64_t old_index, uint64_t out_size,
	                                  std::span<const uint8_t> digest,
	                                  std::span<const uint8_t> opcodes)
	{
		return emitRecord(EntryType::MODIFIED_CHUNK, {old_index, out_size}, digest, opcodes,
		                  "MODIFIED entry");
	}

	[[nodiscard]] bool write_trailer(std::span<const uint8_t> digest)
	{
		const uint8_t tag = DELTA_TRAILER_TAG;
		return emit({&tag, 1}, "delta trailer") && emit(digest, "delta trailer");
	}

	[[nodiscard]] size_t bytes_written() const noexcept { return bytes_written_; }
	[[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
	// Tag, varint fields and digest, then the body straight from the caller's
	// buffer rather than copied into the record.
	bool emitRecord(EntryType type, std::initializer_list<uint64_t> fields,
	                std::span<const uint8_t> digest, std::span<const uint8_t> body,
	                const char* what)
	{
		if (fields.size() > 2 || digest.size() != DELTA_DIGEST_BYTES) {
			error_ = std::string("Invalid ") + what;
			return false;
		}
		uint8_t head[1 + 2 * VARINT_MAX_BYTES + DELTA_DIGEST_BYTES];
		size_t length = 0;
		head[length++] = static_cast<uint8_t>(type);
		for (uint64_t value : fields) {
			for (; value >= 0x80; value >>= 7)
				head[length++] = static_cast<uint8_t>(value) | 0x80;
			head[length++] = static_cast<uint8_t>(value);
		}
		std::memcpy(head + length, digest.data(), DELTA_DIGEST_BYTES);
		length += DELTA_DIGEST_BYTES;
		return emit({head, length}, what) && (body.empty() || emit(body, what));
	}

	bool emit(std::span<const uint8_t> bytes, const char* what)
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
* Reads the delta stream from a FileIO.
*
* `trailer_digest_size` is the whole-file digest length, taken from the strong
* hash in use rather than assumed. Per-entry digests are DELTA_DIGEST_BYTES.
*/
class DeltaReader {
public:
	/// What comes next in the stream.
	enum class Item { Entry, Trailer, End };

	DeltaReader(FileIO& in, size_t trailer_digest_size)
		: in_(in), trailer_digest_size_(trailer_digest_size) {}

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
	* Read an entry header, validating the type tag and bounding any declared
	* output size against the format maximum. That size field is
	* attacker-controlled and feeds allocations downstream, so it is checked here
	* rather than at each use.
	*/
	[[nodiscard]] bool read_entry_header(DeltaEntryHeader& out)
	{
		const int tag = in_.read_byte();
		if (tag == EOF) {
			error_ = "Truncated delta: missing entry type";
			return false;
		}
		if (static_cast<uint8_t>(tag) >= DELTA_ENTRY_TYPE_LIMIT) {
			error_ = "Unknown entry type in delta: " + std::to_string(tag);
			return false;
		}

		out = DeltaEntryHeader{};
		out.type = static_cast<EntryType>(tag);

		if (out.references_old() && !read_varint(out.old_index, "old chunk index"))
			return false;

		if (out.type != EntryType::ORIGINAL_CHUNK) {
			if (!read_varint(out.out_size, "chunk size"))
				return false;
			if (out.out_size > DELTA_MAX_CHUNK_SIZE) {
				error_ = "Delta entry declares an out-of-range chunk size: " +
				         std::to_string(out.out_size);
				return false;
			}
		}

		if (!read_exact(DELTA_DIGEST_BYTES, out.digest)) {
			error_ = "Truncated delta: missing entry digest";
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
		if (!read_exact(trailer_digest_size_, digest)) {
			error_ = "Truncated delta: short trailer";
			return false;
		}
		if (in_.peek_byte() != EOF) {
			error_ = "Unexpected bytes after delta trailer";
			return false;
		}
		if (in_.has_error()) {
			error_ = "Failed to read delta trailer";
			return false;
		}
		return true;
	}

	[[nodiscard]] bool finish(bool saw_trailer) {
		if (in_.has_error()) error_ = "Failed to read delta";
		else if (!saw_trailer) error_ = "Missing delta trailer";
		else return true;
		return false;
	}
	[[nodiscard]] bool io_error() const noexcept { return in_.has_error(); }
	[[nodiscard]] bool finish_diff(size_t opcodes) {
		if (opcodes != 0) return true;
		error_ = "MODIFIED entry has no diff opcodes";
		return false;
	}

	[[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
	bool read_exact(size_t count, std::vector<uint8_t>& out)
	{
		out = in_.read_chunk(count);
		return out.size() == count;
	}

	/// Decode a LEB128 varint, refusing an encoding wider than 64 bits so a
	/// hostile stream cannot spin here or wrap silently.
	bool read_varint(uint64_t& value, const char* what)
	{
		value = 0;
		for (size_t i = 0; i < VARINT_MAX_BYTES; ++i) {
			const int byte = in_.read_byte();
			if (byte == EOF) {
				error_ = std::string("Truncated delta: missing ") + what;
				return false;
			}
			const uint64_t bits = static_cast<uint64_t>(byte) & 0x7F;
			if (i == VARINT_MAX_BYTES - 1 && bits > 1) {
				error_ = std::string("Malformed varint for ") + what;
				return false;
			}
			value |= bits << (7 * i);
			if ((byte & 0x80) == 0)
				return true;
		}
		error_ = std::string("Malformed varint for ") + what;
		return false;
	}

	FileIO& in_;
	size_t trailer_digest_size_;
	std::string error_;
};

#endif // DELTACODEC_HPP
