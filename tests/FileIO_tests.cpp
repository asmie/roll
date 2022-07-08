#include "gtest/gtest.h"
#include "FileIO.hpp"

#include <string>
#include <sstream>

#define TEST_FILE "test_file"
#define TEST_STR "This is the test file\n"

void prepare_file();
void remove_file();


TEST(FileIO, open_close)
{
	FileIO fio;
	prepare_file();

	auto res = fio.open(TEST_FILE, FileMode::INOUT);
	EXPECT_EQ(res, true);

	fio.close();

	remove_file();
}

TEST(FileIO, is_open)
{
	FileIO fio;
	prepare_file();

	EXPECT_EQ(fio.is_open(), false);
	
	auto res = fio.open(TEST_FILE, FileMode::INOUT);
	EXPECT_EQ(res, true);

	EXPECT_EQ(fio.is_open(), true);

	fio.close();

	remove_file();
}

TEST(FileIO, is_eof)
{
	FileIO fio;
	prepare_file();

	EXPECT_EQ(fio.is_open(), false);

	auto res = fio.open(TEST_FILE, FileMode::INOUT);
	EXPECT_EQ(res, true);
	EXPECT_EQ(fio.is_eof(), false);

	auto res2 = fio.read_chunk(200);

	EXPECT_EQ(fio.is_eof(), true);

	fio.close();

	remove_file();
}

TEST(FileIO, open_non_existing)
{
	FileIO fio;

	auto res = fio.open("non-existing_file", FileMode::INOUT);
	EXPECT_EQ(res, false);

	fio.close();
}

TEST(FileIO, read)
{
	FileIO fio;
	prepare_file();

	auto res = fio.open(TEST_FILE, FileMode::INOUT);
	EXPECT_EQ(res, true);

	auto res2 = fio.read_chunk(22);

	EXPECT_EQ(res2.get()->size(), 22);

	std::string fRead(res2.get()->begin(), res2.get()->end());
	std::string patt(TEST_STR);

	EXPECT_EQ(fRead, patt);

	fio.close();

	remove_file();
}

TEST(FileIO, read_incorrect)
{
	FileIO fio;
	prepare_file();

	auto res = fio.open(TEST_FILE, FileMode::INOUT);
	EXPECT_EQ(res, true);

	auto res2 = fio.read_chunk(0);

	EXPECT_EQ(res2.get()->size(), 0);

	res2 = fio.read_chunk(22);

	EXPECT_EQ(res2.get()->size(), 22);

	std::string fRead(res2.get()->begin(), res2.get()->end());
	std::string patt(TEST_STR);

	EXPECT_EQ(fRead, patt);

	res2 = fio.read_chunk(100);						// Flush rest of file

	EXPECT_EQ(res2.get()->size(), 1);

	res2 = fio.read_chunk(100);						// Try to read after EOF

	EXPECT_EQ(res2.get()->size(), 0);

	fio.close();

	remove_file();
}

#include <fstream>
#include <iostream>
#include <cstdio>
void prepare_file()
{
	std::fstream f(TEST_FILE, std::fstream::out);

	f << TEST_STR << std::endl;

	f.close();
}

void remove_file()
{
	remove(TEST_FILE);
}
