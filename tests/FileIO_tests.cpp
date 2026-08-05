#include "gtest/gtest.h"
#include "FileIO.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define TEST_STR "This is the test file\n"

namespace {

std::string make_test_path()
{
	static std::atomic<unsigned> counter{0};
	const auto p = std::filesystem::temp_directory_path() /
	               ("roll_fio_" + std::to_string(counter++) + "_test_file");
	return p.string();
}

void prepare_file(const std::string& path)
{
	std::fstream f(path, std::fstream::out);
	f << TEST_STR << std::endl;
	f.close();
}

void remove_file(const std::string& path)
{
	std::remove(path.c_str());
}

} // namespace

TEST(FileIO, open_close)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_TRUE(fio.open(path, FileMode::INOUT));
	EXPECT_TRUE(fio.close());

	remove_file(path);
}

TEST(FileIO, is_open)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_FALSE(fio.is_open());
	EXPECT_TRUE(fio.open(path, FileMode::INOUT));
	EXPECT_TRUE(fio.is_open());

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

TEST(FileIO, is_eof)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_FALSE(fio.is_open());
	EXPECT_TRUE(fio.open(path, FileMode::INOUT));
	EXPECT_FALSE(fio.is_eof());

	// Asking for more than the file holds returns what there was, and leaves the
	// handle at end of file.
	EXPECT_EQ(fio.read_chunk(200).size(), std::filesystem::file_size(path));
	EXPECT_TRUE(fio.is_eof());

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

TEST(FileIO, open_non_existing)
{
	FileIO fio;
	EXPECT_FALSE(fio.open("non-existing_file_xyzzy_roll_test", FileMode::INOUT));
	EXPECT_TRUE(fio.close());
}

TEST(FileIO, read)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_TRUE(fio.open(path, FileMode::INOUT));
	auto buf = fio.read_chunk(22);
	EXPECT_EQ(buf.size(), 22u);

	const std::string read_back(buf.begin(), buf.end());
	EXPECT_EQ(read_back, std::string(TEST_STR));

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

TEST(FileIO, read_incorrect)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_TRUE(fio.open(path, FileMode::INOUT));

	auto buf = fio.read_chunk(0);
	EXPECT_EQ(buf.size(), 0u);

	buf = fio.read_chunk(22);
	EXPECT_EQ(buf.size(), 22u);
	EXPECT_EQ(std::string(buf.begin(), buf.end()), std::string(TEST_STR));

	buf = fio.read_chunk(100);
	EXPECT_EQ(buf.size(), 1u);

	buf = fio.read_chunk(100);
	EXPECT_EQ(buf.size(), 0u);

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

// read_byte()/peek_byte() are served from a 64 KiB read-ahead buffer. None of
// this was covered before, and the buffer has to stay invisible to callers:
// byte-wise and bulk reads interleave freely, and the buffer spans refills.
TEST(FileIO, read_byte_and_peek_byte_sequence)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	ASSERT_TRUE(fio.open(path, FileMode::IN));

	// peek must not consume, and must agree with the following read.
	const int peeked = fio.peek_byte();
	EXPECT_EQ(peeked, static_cast<int>('T'));
	EXPECT_EQ(fio.peek_byte(), peeked) << "peek_byte consumed a byte";
	EXPECT_EQ(fio.read_byte(), peeked);
	EXPECT_EQ(fio.read_byte(), static_cast<int>('h'));

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

TEST(FileIO, read_byte_reaches_eof_exactly_once)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	const size_t expected = std::filesystem::file_size(path);
	ASSERT_TRUE(fio.open(path, FileMode::IN));

	size_t counted = 0;
	while (fio.read_byte() != EOF)
		++counted;

	EXPECT_EQ(counted, expected) << "byte-wise read did not cover the whole file";
	EXPECT_TRUE(fio.is_eof());
	EXPECT_EQ(fio.read_byte(), EOF) << "reads past EOF must keep returning EOF";
	EXPECT_EQ(fio.peek_byte(), EOF);

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

TEST(FileIO, byte_and_chunk_reads_interleave)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	const auto expected = std::filesystem::file_size(path);
	ASSERT_TRUE(fio.open(path, FileMode::IN));

	// Consume one byte through the buffered path, then switch to a bulk read:
	// read_chunk must continue from the logical position, handing back the
	// un-consumed read-ahead first rather than skipping or repeating it.
	EXPECT_EQ(fio.read_byte(), static_cast<int>('T'));
	auto rest = fio.read_chunk(4);
	ASSERT_EQ(rest.size(), 4u);
	EXPECT_EQ(std::string(rest.begin(), rest.end()), "his ");

	// And back to byte-wise reads after the bulk read.
	EXPECT_EQ(fio.read_byte(), static_cast<int>('i'));
	EXPECT_EQ(fio.read_byte(), static_cast<int>('s'));

	// The remainder must account for exactly the bytes not yet consumed.
	size_t remaining = 0;
	while (fio.read_byte() != EOF)
		++remaining;
	EXPECT_EQ(remaining + 7, expected);

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

// A positioned read invalidates the read-ahead; the bytes it returns must come
// from the requested offset, and byte-wise reads must resume from there.
TEST(FileIO, positioned_read_after_byte_reads)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	ASSERT_TRUE(fio.open(path, FileMode::IN));
	EXPECT_EQ(fio.read_byte(), static_cast<int>('T'));
	EXPECT_EQ(fio.read_byte(), static_cast<int>('h'));

	auto at8 = fio.read_chunk(4, 8);
	ASSERT_EQ(at8.size(), 4u);
	EXPECT_EQ(std::string(at8.begin(), at8.end()), std::string(TEST_STR).substr(8, 4));

	EXPECT_EQ(fio.read_byte(), static_cast<int>(TEST_STR[12]));

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

// The read-ahead spans more than one refill for inputs larger than the buffer,
// and every byte must be delivered exactly once in order.
TEST(FileIO, byte_reads_span_multiple_refills)
{
	FileIO fio;
	const auto path = make_test_path();

	// Larger than READ_BUFFER_SIZE (64 KiB) so refill() runs several times.
	std::vector<uint8_t> data(200u * 1024u);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
	{
		std::ofstream f(path, std::ios::binary);
		f.write(reinterpret_cast<const char*>(data.data()), data.size());
	}

	ASSERT_TRUE(fio.open(path, FileMode::IN));
	for (size_t i = 0; i < data.size(); ++i) {
		const int got = fio.read_byte();
		ASSERT_EQ(got, static_cast<int>(data[i])) << "mismatch at byte " << i;
	}
	EXPECT_EQ(fio.read_byte(), EOF);

	EXPECT_TRUE(fio.close());
	remove_file(path);
}

// close() must report whether the close itself succeeded. It used to return
// !fail() outright, so a file read to completion reported a close failure —
// a short read at EOF sets failbit — which made the result useless to callers
// checking for flush errors.
TEST(FileIO, close_succeeds_after_reading_to_eof)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	ASSERT_TRUE(fio.open(path, FileMode::IN));
	while (fio.read_byte() != EOF) { }
	ASSERT_TRUE(fio.is_eof());

	EXPECT_TRUE(fio.close())
		<< "a benign end-of-file must not be reported as a close failure";

	remove_file(path);
}

TEST(FileIO, close_on_an_unopened_handle_succeeds)
{
	FileIO fio;
	EXPECT_TRUE(fio.close()) << "nothing was open, so nothing could fail";
}

TEST(FileIO, close_reports_success_after_writing)
{
	FileIO fio;
	const auto path = make_test_path();

	ASSERT_TRUE(fio.open(path, FileMode::OUT));
	const std::vector<uint8_t> payload{1, 2, 3, 4};
	ASSERT_TRUE(fio.write_chunk(payload));
	EXPECT_TRUE(fio.close()) << "a clean flush must report success";

	EXPECT_EQ(std::filesystem::file_size(path), payload.size());
	remove_file(path);
}
