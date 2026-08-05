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

constexpr size_t HASH_SIZE = 64;

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

std::vector<uint8_t> make_hash(uint8_t seed)
{
	return std::vector<uint8_t>(HASH_SIZE, seed);
}

} // namespace

// ---- integer primitives --------------------------------------------------

TEST(DeltaCodec, integers_round_trip_big_endian)
{
	std::vector<uint8_t> out;
	push_u32_be(out, 0x01020304u);
	ASSERT_EQ(out.size(), 4u);
	EXPECT_EQ(out[0], 0x01u) << "most significant byte must come first";
	EXPECT_EQ(out[3], 0x04u);
	EXPECT_EQ(load_u32_be(out.data()), 0x01020304u);

	out.clear();
	push_u64_be(out, 0x0102030405060708ull);
	ASSERT_EQ(out.size(), 8u);
	EXPECT_EQ(out[0], 0x01u);
	EXPECT_EQ(out[7], 0x08u);
	EXPECT_EQ(load_u64_be(out.data()), 0x0102030405060708ull);
}

TEST(DeltaCodec, integers_round_trip_at_extremes)
{
	for (const uint64_t v : {uint64_t{0}, uint64_t{1}, ~uint64_t{0},
	                         uint64_t{1} << 63, uint64_t{0x00FF00FF00FF00FF}}) {
		std::vector<uint8_t> out;
		push_u64_be(out, v);
		EXPECT_EQ(load_u64_be(out.data()), v);
	}
	for (const uint32_t v : {uint32_t{0}, uint32_t{1}, ~uint32_t{0}, uint32_t{1} << 31}) {
		std::vector<uint8_t> out;
		push_u32_be(out, v);
		EXPECT_EQ(load_u32_be(out.data()), v);
	}
}

// ---- writer / reader round trip ------------------------------------------

TEST(DeltaCodec, writer_and_reader_round_trip_a_full_stream)
{
	const std::string PATH = codec_path("roundtrip");
	const std::vector<uint8_t> added_payload{1, 2, 3, 4, 5};
	const auto digest = make_hash(0xEE);

	{
		FileIO out;
		ASSERT_TRUE(out.open(PATH, FileMode::OUT));
		DeltaWriter writer(out);
		ASSERT_TRUE(writer.write_header());
		ASSERT_TRUE(writer.write_entry(EntryType::ORIGINAL_CHUNK, 0xAAAA,
		                               make_hash(0x11), 512, {}));
		ASSERT_TRUE(writer.write_entry(EntryType::ADDED_CHUNK, 0xBBBB,
		                               make_hash(0x22), added_payload.size(),
		                               added_payload));
		ASSERT_TRUE(writer.write_entry(EntryType::REMOVED_CHUNK, 0xCCCC,
		                               make_hash(0x33), 1024, {}));
		ASSERT_TRUE(writer.write_trailer(digest));
		EXPECT_TRUE(writer.error().empty());

		// bytes_written must account for every byte actually emitted.
		ASSERT_TRUE(out.close());
		EXPECT_EQ(writer.bytes_written(), read_all(PATH).size());
	}

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, HASH_SIZE);
	ASSERT_TRUE(reader.read_header()) << reader.error();

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	DeltaEntryHeader h;
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::ORIGINAL_CHUNK);
	EXPECT_EQ(h.signature, 0xAAAAu);
	EXPECT_EQ(h.hash, make_hash(0x11));
	EXPECT_EQ(h.chunk_size, 512u);

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::ADDED_CHUNK);
	EXPECT_EQ(h.chunk_size, added_payload.size());
	std::vector<uint8_t> payload;
	ASSERT_TRUE(reader.read_payload(static_cast<size_t>(h.chunk_size), payload));
	EXPECT_EQ(payload, added_payload);

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Entry);
	ASSERT_TRUE(reader.read_entry_header(h)) << reader.error();
	EXPECT_EQ(h.type, EntryType::REMOVED_CHUNK);

	ASSERT_EQ(reader.next_item(), DeltaReader::Item::Trailer);
	std::vector<uint8_t> got_digest;
	ASSERT_TRUE(reader.read_trailer(got_digest)) << reader.error();
	EXPECT_EQ(got_digest, digest);

	EXPECT_EQ(reader.next_item(), DeltaReader::Item::End);

	std::remove(PATH.c_str());
}

TEST(DeltaCodec, diff_opcodes_round_trip)
{
	const std::string PATH = codec_path("opcodes");
	const std::vector<uint8_t> inline_bytes{0xDE, 0xAD, 0xBE, 0xEF};

	// Opcodes are emitted by Delta into a buffer, so build the run by hand the
	// same way and check the reader agrees on every field.
	std::vector<uint8_t> stream(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(stream, DELTA_FORMAT_VERSION);

	stream.push_back('D');
	push_u32_be(stream, 7);
	stream.push_back(static_cast<uint8_t>(inline_bytes.size()));
	stream.insert(stream.end(), inline_bytes.begin(), inline_bytes.end());

	stream.push_back('I');
	push_u32_be(stream, 99);
	stream.push_back(0);  // zero-length insert must be legal

	stream.push_back('X');
	push_u32_be(stream, 123);
	push_u32_be(stream, 456);

	write_bytes(PATH, stream);

	FileIO in;
	ASSERT_TRUE(in.open(PATH, FileMode::IN));
	DeltaReader reader(in, HASH_SIZE);
	ASSERT_TRUE(reader.read_header()) << reader.error();

	ASSERT_TRUE(reader.at_diff_opcode());
	DiffOpcode op;
	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'D');
	EXPECT_EQ(op.pos, 7u);
	EXPECT_EQ(op.bytes, inline_bytes);

	ASSERT_TRUE(reader.at_diff_opcode());
	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'I');
	EXPECT_EQ(op.pos, 99u);
	EXPECT_TRUE(op.bytes.empty());

	ASSERT_TRUE(reader.at_diff_opcode());
	ASSERT_TRUE(reader.read_diff_opcode(op)) << reader.error();
	EXPECT_EQ(op.op, 'X');
	EXPECT_EQ(op.pos, 123u);
	EXPECT_EQ(op.delete_length, 456u);

	// The run ends at the first byte that is not an opcode — here, end of file.
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
		DeltaReader reader(in, HASH_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("magic"), std::string::npos) << reader.error();
	}

	std::vector<uint8_t> bad_version(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(bad_version, DELTA_FORMAT_VERSION + 1);
	write_bytes(PATH, bad_version);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("version"), std::string::npos) << reader.error();
	}

	// A header shorter than DELTA_HEADER_SIZE is truncation, not bad magic.
	write_bytes(PATH, {'R', 'H'});
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		EXPECT_FALSE(reader.read_header());
		EXPECT_NE(reader.error().find("Truncated"), std::string::npos) << reader.error();
	}

	std::remove(PATH.c_str());
}

// The chunk_size field is attacker-controlled and feeds allocations in every
// consumer, so the codec bounds it centrally rather than trusting each caller.
TEST(DeltaCodec, reader_rejects_out_of_range_chunk_size)
{
	const std::string PATH = codec_path("oversize");

	const auto build = [](uint64_t declared) {
		std::vector<uint8_t> s(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
		push_u32_be(s, DELTA_FORMAT_VERSION);
		push_u64_be(s, static_cast<uint64_t>(EntryType::ADDED_CHUNK));
		push_u64_be(s, 0xDEADBEEF);
		s.insert(s.end(), HASH_SIZE, 0x55);
		push_u64_be(s, declared);
		return s;
	};

	write_bytes(PATH, build(uint64_t{1} << 62));
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_FALSE(reader.read_entry_header(h));
		EXPECT_NE(reader.error().find("out-of-range chunk size"), std::string::npos)
			<< reader.error();
	}

	// The maximum itself must be accepted.
	write_bytes(PATH, build(DELTA_MAX_CHUNK_SIZE));
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_TRUE(reader.read_entry_header(h)) << reader.error();
		EXPECT_EQ(h.chunk_size, DELTA_MAX_CHUNK_SIZE);
	}

	std::remove(PATH.c_str());
}

TEST(DeltaCodec, reader_reports_truncation_in_entry_and_opcodes)
{
	const std::string PATH = codec_path("trunc");

	// Entry header cut short mid-hash.
	std::vector<uint8_t> partial(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(partial, DELTA_FORMAT_VERSION);
	push_u64_be(partial, static_cast<uint64_t>(EntryType::ORIGINAL_CHUNK));
	push_u64_be(partial, 1234);
	partial.insert(partial.end(), HASH_SIZE / 2, 0x77);
	write_bytes(PATH, partial);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		ASSERT_TRUE(reader.read_header());
		DeltaEntryHeader h;
		EXPECT_FALSE(reader.read_entry_header(h));
		EXPECT_NE(reader.error().find("partial entry header"), std::string::npos)
			<< reader.error();
	}

	// Opcode claiming inline bytes that are not present.
	std::vector<uint8_t> short_op(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(short_op, DELTA_FORMAT_VERSION);
	short_op.push_back('D');
	push_u32_be(short_op, 0);
	short_op.push_back(200);      // promises 200 bytes
	short_op.insert(short_op.end(), 3, 0xAB);  // supplies 3
	write_bytes(PATH, short_op);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		ASSERT_TRUE(reader.read_header());
		ASSERT_TRUE(reader.at_diff_opcode());
		DiffOpcode op;
		EXPECT_FALSE(reader.read_diff_opcode(op));
		EXPECT_NE(reader.error().find("inline bytes"), std::string::npos) << reader.error();
	}

	// Trailer tag present but digest cut short.
	std::vector<uint8_t> short_trailer(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	push_u32_be(short_trailer, DELTA_FORMAT_VERSION);
	short_trailer.push_back(DELTA_TRAILER_TAG);
	short_trailer.insert(short_trailer.end(), HASH_SIZE - 1, 0x99);
	write_bytes(PATH, short_trailer);
	{
		FileIO in;
		ASSERT_TRUE(in.open(PATH, FileMode::IN));
		DeltaReader reader(in, HASH_SIZE);
		ASSERT_TRUE(reader.read_header());
		ASSERT_EQ(reader.next_item(), DeltaReader::Item::Trailer);
		std::vector<uint8_t> digest;
		EXPECT_FALSE(reader.read_trailer(digest));
		EXPECT_NE(reader.error().find("trailer"), std::string::npos) << reader.error();
	}

	std::remove(PATH.c_str());
}

// The trailer tag was chosen so it cannot collide with the first byte of a
// big-endian u64 entry type, which is what makes next_item() unambiguous.
TEST(DeltaCodec, trailer_tag_cannot_collide_with_an_entry_type)
{
	std::vector<uint8_t> encoded;
	push_u64_be(encoded, static_cast<uint64_t>(EntryType::REMOVED_CHUNK));
	EXPECT_EQ(encoded[0], 0x00u)
		<< "entry types must encode with a zero leading byte";
	EXPECT_NE(DELTA_TRAILER_TAG, 0x00u)
		<< "trailer tag would be indistinguishable from an entry header";
}
