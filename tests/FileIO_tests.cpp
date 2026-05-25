#include "gtest/gtest.h"
#include "FileIO.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
	fio.close();

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

	fio.close();
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

	fio.read_chunk(200);
	EXPECT_TRUE(fio.is_eof());

	fio.close();
	remove_file(path);
}

TEST(FileIO, open_non_existing)
{
	FileIO fio;
	EXPECT_FALSE(fio.open("non-existing_file_xyzzy_roll_test", FileMode::INOUT));
	fio.close();
}

TEST(FileIO, read)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_TRUE(fio.open(path, FileMode::INOUT));
	auto buf = fio.read_chunk(22);
	EXPECT_EQ(buf->size(), 22u);

	const std::string read_back(buf->begin(), buf->end());
	EXPECT_EQ(read_back, std::string(TEST_STR));

	fio.close();
	remove_file(path);
}

TEST(FileIO, read_incorrect)
{
	FileIO fio;
	const auto path = make_test_path();
	prepare_file(path);

	EXPECT_TRUE(fio.open(path, FileMode::INOUT));

	auto buf = fio.read_chunk(0);
	EXPECT_EQ(buf->size(), 0u);

	buf = fio.read_chunk(22);
	EXPECT_EQ(buf->size(), 22u);
	EXPECT_EQ(std::string(buf->begin(), buf->end()), std::string(TEST_STR));

	buf = fio.read_chunk(100);
	EXPECT_EQ(buf->size(), 1u);

	buf = fio.read_chunk(100);
	EXPECT_EQ(buf->size(), 0u);

	fio.close();
	remove_file(path);
}
