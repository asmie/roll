#include "gtest/gtest.h"

#include "DeltaCodec.hpp"
#include "DeltaFormat.hpp"
#include "FileIO.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr size_t TRAILER_SIZE = 64;

std::string codec_path(const char* name)
{
	static std::atomic<unsigned> counter{0};
	const auto p = std::filesystem::temp_directory_path() /
	               ("roll_codec_" + std::to_string(counter++) + "_" + name);
	return p.string();
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
	std::ofstream f(path, std::ios::binary);
	if (!bytes.empty())
		f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<uint8_t> read_all(const std::string& path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return {};
	const auto size = f.tellg();
	f.seekg(0);
	std::vector<uint8_t> buf(static_cast<size_t>(size));
	if (size > 0)
		f.read(reinterpret_cast<char*>(buf.data()), buf.size());
	return buf;
}

std::vector<uint8_t> make_digest(uint8_t seed)
{
	return std::vector<uint8_t>(DELTA_DIGEST_BYTES, seed);
}

std::vector<uint8_t> header_bytes()
{
	std::vector<uint8_t> s(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(s, DELTA_FORMAT_VERSION);
	return s;
}

} // namespace

// ---- integer primitives --------------------------------------------------

TEST(DeltaCodec, u32_round_trips_big_endian)
{
	std::vector<uint8_t> out;
	push_u32_be(out, 0x01020304u);
	ASSERT_EQ(out.size(), 4u);
	EXPECT_EQ(out[0], 0x01u) << "most significant byte must come first";
	EXPECT_EQ(out[3], 0x04u);
	EXPECT_EQ(load_u32_be(out.data()), 0x01020304u);

	for (const uint32_t v : {uint32_t{0}, uint32_t{1}, ~uint32_t{0}, uint32_t{1} << 31}) {
		std::vector<uint8_t> buf;
		push_u32_be(buf, v);
		EXPECT_EQ(load_u32_be(buf.data()), v);
	}
}

TEST(DeltaCodec, varints_round_trip_and_stay_compact)
{
	// Boundaries of the 7-bit groups, plus the extremes.
	const uint64_t values[] = {0, 1, 0x7F, 0x80, 0x3FFF, 0x4000, 16384, 65535,
	                           uint64_t{1} << 31, uint64_t{1} << 62, ~uint64_t{0}};

	for (const uint64_t v : values) {
		const std::string path = codec_path("varint");
		std::vector<uint8_t> encoded;
		push_varint(encoded, v);
		EXPECT_EQ(encoded.size(), varint_size(v))
			<< "varint_size disagrees with the encoding for " << v;
		EXPECT_LE(encoded.size(), VARINT_MAX_BYTES);

		// Decoding goes through DeltaReader, which is where the bounds live.
		auto stream = header_bytes();
		stream.push_back(static_cast<uint8_t>(EntryType::ORIGINAL_CHUNK));
		stream.insert(stream.end(), encoded.begin(), encoded.end());
		const auto digest = make_digest(0x5A);
		stream.insert(stream.end(), digest.begin(), digest.end());
		write_bytes(path, stream);

		FileIO in;
		ASSERT_TRUE(in.open(path, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header()) << reader.error();
		DeltaEntryHeader h;
		ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
		EXPECT_EQ(h.old_index, v) << "varint did not survive the round trip";

		std::remove(path.c_str());
	}

	// Small values must actually be small — that is the point of the encoding.
	std::vector<uint8_t> one;
	push_varint(one, 127);
	EXPECT_EQ(one.size(), 1u);
}

// ---- writer / reader round trip ------------------------------------------

TEST(DeltaCodec, writer_and_reader_round_trip_every_entry_kind)
{
	const std::string PATH = codec_path("roundtrip");
	const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
	const std::vector<uint8_t> opcodes{'I', 0, 0, 0, 0, 2, 0xAA, 0xBB};
	const std::vector<uint8_t> trailer(TRAILER_SIZE, 0xEE);

	{
		FileIO out;
		ASSERT_TRUE(out.open(PATH, FileMode::OUT));
		DeltaWriter writer(out);
		ASSERT_TRUE(writer.write_header());
		ASSERT_TRUE(writer.write_original(7, make_digest(0x11)));
		ASSERT_TRUE(writer.write_added(make_digest(0x22), payload));
		ASSERT_TRUE(writer.write_modified(9, 4096, make_digest(0x33), opcodes));
		ASSERT_TRUE(writer.write_trailer(trailer));
		EXPECT_TRUE(writer.error().empty());

		ASSERT_TRUE(out.close());
		EXPECT_EQ(writer.bytes_written(), read_all(PATH).size())
			<< "bytes_written must account for every byte emitted";
	}

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, TRAILER_SIZE);
	ASSERT_TRUE(reader.read_header()) << reader.error();

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	DeltaEntryHeader h;
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::ORIGINAL_CHUNK);
	EXPECT_TRUE(h.references_old());
	EXPECT_EQ(h.old_index, 7u);
	EXPECT_EQ(h.digest, make_digest(0x11));

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::ADDED_CHUNK);
	EXPECT_FALSE(h.references_old());
	EXPECT_EQ(h.out_size, payload.size());
	std::vector<uint8_t> got_payload;
	ASSERT_TRUE(reader.read_payload(static_cast<size_t>(h.out_size), got_payload));
	EXPECT_EQ(got_payload, payload);

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::MODIFIED_CHUNK);
	EXPECT_EQ(h.old_index, 9u);
	EXPECT_EQ(h.out_size, 4096u);
	ASSERT_TRUE(reader.at_diff_opcode());
	DiffOpcode op;
	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'I');
	EXPECT_EQ(op.bytes, std::vector<uint8_t>({0xAA, 0xBB}));
	EXPECT_FALSE(reader.at_diff_opcode());

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Trailer);
	std::vector<uint8_t> got_trailer;
	ASSERT_TRUE(reader.read_trailer(got_trailer)) << reader.error();
	EXPECT_EQ(got_trailer, trailer);

	EXPECT_EQ(reader.next_item(), DeltaReader::Item::End);

	std::remove(PATH.c_str());
}

// A reused chunk is the common case in any real delta, so its entry has to be
// small: a type byte, a short index, and the digest.
TEST(DeltaCodec, an_original_entry_is_compact)
{
	const std::string PATH = codec_path("compact");
	{
		FileIO out;
		ASSERT_TRUE(out.open(PATH, FileMode::OUT));
		DeltaWriter writer(out);
		ASSERT_TRUE(writer.write_original(1000, make_digest(0x44)));
		ASSERT_TRUE(out.close());
		EXPECT_EQ(writer.bytes_written(), 1u + varint_size(1000) + DELTA_DIGEST_BYTES);
		EXPECT_LE(writer.bytes_written(), 20u)
			<< "a reused-chunk entry should cost about 19 bytes";
	}
	std::remove(PATH.c_str());
}

TEST(DeltaCodec, diff_opcodes_round_trip)
{
	const std::string PATH = codec_path("opcodes");
	const std::vector<uint8_t> inline_bytes{0xDE, 0xAD, 0xBE, 0xEF};

	auto stream = header_bytes();
	stream.push_back('D');
	push_u32_be(stream, 7);
	stream.push_back(static_cast<uint8_t>(inline_bytes.size()));
	stream.insert(stream.end(), inline_bytes.begin(), inline_bytes.end());
	stream.push_back('I');
	push_u32_be(stream, 99);
	stream.push_back(0);  // a zero-length insert is legal
	stream.push_back('X');
	push_u32_be(stream, 123);
	push_u32_be(stream, 456);
	write_bytes(PATH, stream);

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, TRAILER_SIZE);
	ASSERT_TRUE(reader.read_header()) << reader.error();

	ASSERT_TRUE(reader.at_diff_opcode());
	DiffOpcode op;
	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'D');
	EXPECT_EQ(op.pos, 7u);
	EXPECT_EQ(op.bytes, inline_bytes);

	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'I');
	EXPECT_EQ(op.pos, 99u);
	EXPECT_TRUE(op.bytes.empty());

	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'X');
	EXPECT_EQ(op.pos, 123u);
	EXPECT_EQ(op.delete_length, 456u);

	EXPECT_FALSE(reader.at_diff_opcode());
	std::remove(PATH.c_str());
}

// ---- rejection paths -----------------------------------------------------

TEST(DeltaCodec, reader_rejects_bad_magic_and_version)
{
	const std::string PATH = codec_path("badhdr");

	std::vector<uint8_t> bad_magic(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	bad_magic[0] ^= 0xFF;
	push_u32_be(bad_magic, DELTA_FORMAT_VERSION);
	write_bytes(PATH, bad_magic);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("magic"), std::string::npos) << reader.error();
	}

	// An older delta must be refused by version rather than mis-parsed: the
	// entry layout and the chunk boundaries both changed across versions.
	for (const uint32_t version : {DELTA_FORMAT_VERSION - 1, DELTA_FORMAT_VERSION + 1}) {
		std::vector<uint8_t> other(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
		push_u32_be(other, version);
		write_bytes(PATH, other);
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("version"), std::string::npos) << reader.error();
	}

	write_bytes(PATH, {'R', 'H'});
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("Truncated"), std::string::npos) << reader.error();
	}

	std::remove(PATH.c_str());
}

TEST(DeltaCodec, reader_rejects_an_unknown_entry_type)
{
	const std::string PATH = codec_path("badtype");
	auto stream = header_bytes();
	stream.push_back(DELTA_ENTRY_TYPE_LIMIT);  // one past the last valid type
	write_bytes(PATH, stream);

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, TRAILER_SIZE);
	ASSERT_TRUE(reader.read_header());
	DeltaEntryHeader h;
	EXPECT_FALSE(reader.read_entry_header(h));
	EXPECT_NE(reader.error().find("Unknown entry type"), std::string::npos) << reader.error();

	std::remove(PATH.c_str());
}

// The declared output size is attacker-controlled and feeds allocations, so the
// codec bounds it centrally rather than trusting each consumer.
TEST(DeltaCodec, reader_rejects_out_of_range_output_size)
{
	const std::string PATH = codec_path("oversize");

	const auto build = [](uint64_t declared) {
		auto s = header_bytes();
		s.push_back(static_cast<uint8_t>(EntryType::ADDED_CHUNK));
		push_varint(s, declared);
		const auto d = make_digest(0x55);
		s.insert(s.end(), d.begin(), d.end());
		return s;
	};

	write_bytes(PATH, build(uint64_t{1} << 62));
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_FALSE(reader.read_entry_header(h));
		EXPECT_NE(reader.error().find("out-of-range chunk size"), std::string::npos)
			<< reader.error();
	}

	write_bytes(PATH, build(DELTA_MAX_CHUNK_SIZE));
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_TRUE(reader.read_entry_header(h)) << reader.error();
		EXPECT_EQ(h.out_size, DELTA_MAX_CHUNK_SIZE);
	}

	std::remove(PATH.c_str());
}

// A varint with the continuation bit set forever must not spin or wrap.
TEST(DeltaCodec, reader_rejects_an_overlong_varint)
{
	const std::string PATH = codec_path("badvarint");
	auto stream = header_bytes();
	stream.push_back(static_cast<uint8_t>(EntryType::ORIGINAL_CHUNK));
	stream.insert(stream.end(), 32, 0xFF);  // never terminates
	write_bytes(PATH, stream);

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, TRAILER_SIZE);
	ASSERT_TRUE(reader.read_header());
	DeltaEntryHeader h;
	EXPECT_FALSE(reader.read_entry_header(h));
	EXPECT_NE(reader.error().find("varint"), std::string::npos) << reader.error();

	std::remove(PATH.c_str());
}

TEST(DeltaCodec, reader_reports_truncation_in_entry_opcode_and_trailer)
{
	const std::string PATH = codec_path("trunc");

	// Digest cut short.
	auto partial = header_bytes();
	partial.push_back(static_cast<uint8_t>(EntryType::ORIGINAL_CHUNK));
	push_varint(partial, 3);
	partial.insert(partial.end(), DELTA_DIGEST_BYTES / 2, 0x77);
	write_bytes(PATH, partial);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_FALSE(reader.read_entry_header(h));
		EXPECT_NE(reader.error().find("digest"), std::string::npos) << reader.error();
	}

	// Opcode promising more inline bytes than are present.
	auto short_op = header_bytes();
	short_op.push_back('D');
	push_u32_be(short_op, 0);
	short_op.push_back(200);
	short_op.insert(short_op.end(), 3, 0xAB);
	write_bytes(PATH, short_op);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header());
		ASSERT_TRUE(reader.at_diff_opcode());
		DiffOpcode op;
		EXPECT_FALSE(reader.read_diff_opcode(op));
		EXPECT_NE(reader.error().find("inline bytes"), std::string::npos) << reader.error();
	}

	// Trailer tag present, digest cut short.
	auto short_trailer = header_bytes();
	short_trailer.push_back(DELTA_TRAILER_TAG);
	short_trailer.insert(short_trailer.end(), TRAILER_SIZE - 1, 0x99);
	write_bytes(PATH, short_trailer);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, TRAILER_SIZE);
		ASSERT_TRUE(reader.read_header());
		ASSERT_EQ(reader.next_item(), DeltaReader::Item::Trailer);
		std::vector<uint8_t> digest;
		EXPECT_FALSE(reader.read_trailer(digest));
		EXPECT_NE(reader.error().find("trailer"), std::string::npos) << reader.error();
	}

	std::remove(PATH.c_str());
}

// next_item() distinguishes a trailer from an entry by one byte, so the tag must
// stay outside the range of valid entry types.
TEST(DeltaCodec, trailer_tag_cannot_collide_with_an_entry_type)
{
	EXPECT_GE(DELTA_TRAILER_TAG, DELTA_ENTRY_TYPE_LIMIT)
		<< "trailer tag would be indistinguishable from an entry header";
	EXPECT_LT(static_cast<uint8_t>(EntryType::MODIFIED_CHUNK), DELTA_ENTRY_TYPE_LIMIT);
}
