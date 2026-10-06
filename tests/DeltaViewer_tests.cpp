#include "TestWorkspace.hpp"
#include "gtest/gtest.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaFormat.hpp"
#include "DeltaViewer.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake2b.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string viewer_path(const char* name)
{
	static std::atomic<unsigned> counter{0};
	const auto p = testfiles::directory() /
	               ("roll_viewer_" + std::to_string(counter++) + "_" + name);
	return p.string();
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
	testfiles::write(path, bytes);
}

std::vector<uint8_t> read_all(const std::string& path)
{
	return testfiles::read(path);
}

void write_random(const std::string& path, size_t bytes, uint32_t seed)
{
	std::mt19937 rng(seed);
	std::uniform_int_distribution<int> dist(0, 255);
	std::vector<uint8_t> data(bytes);
	for (auto& byte : data) byte = static_cast<uint8_t>(dist(rng));
	testfiles::write(path, data);
}

// Swallow std::cout/std::cerr for the duration of the scope so view_delta's
// normal output doesn't pollute the test runner's log.
//
// Buffers in memory rather than opening "/dev/null", which does not exist on
// Windows: there the open failed and output was discarded only because writes
// to a failed stream are dropped silently. That happened to look like success,
// which is worse than failing.
struct SilenceCout {
	std::ostringstream sink;
	std::streambuf* old_cout;
	std::streambuf* old_cerr;

	SilenceCout()
		: old_cout(std::cout.rdbuf(sink.rdbuf())),
		  old_cerr(std::cerr.rdbuf(sink.rdbuf())) {}

	~SilenceCout() {
		std::cout.rdbuf(old_cout);
		std::cerr.rdbuf(old_cerr);
	}

	SilenceCout(const SilenceCout&) = delete;
	SilenceCout& operator=(const SilenceCout&) = delete;
};

} // namespace

TEST(DeltaViewer, succeeds_on_well_formed_delta)
{
	const std::string OLD = viewer_path("old");
	const std::string NEW = viewer_path("new");
	const std::string DELTA = viewer_path("delta");

	write_random(OLD, 4096, 0x1234u);
	write_random(NEW, 4096, 0x5678u);
	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	SilenceCout s;
	EXPECT_EQ(view_delta(DELTA), 0);

	std::remove(OLD.c_str());
	std::remove(NEW.c_str());
	std::remove(DELTA.c_str());
}

TEST(DeltaViewer, rejects_missing_file)
{
	SilenceCout s;
	EXPECT_NE(view_delta(viewer_path("no_such_file")), 0);
}

TEST(DeltaViewer, rejects_bad_magic)
{
	const std::string OLD = viewer_path("badmagic_old");
	const std::string NEW = viewer_path("badmagic_new");
	const std::string DELTA = viewer_path("badmagic_delta");

	write_random(OLD, 4096, 0xAAu);
	write_random(NEW, 4096, 0xBBu);
	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	auto raw = read_all(DELTA);
	raw[0] ^= 0xFF;
	write_bytes(DELTA, raw);

	SilenceCout s;
	EXPECT_NE(view_delta(DELTA), 0);

	std::remove(OLD.c_str());
	std::remove(NEW.c_str());
	std::remove(DELTA.c_str());
}

TEST(DeltaViewer, rejects_unknown_version)
{
	const std::string OLD = viewer_path("badver_old");
	const std::string NEW = viewer_path("badver_new");
	const std::string DELTA = viewer_path("badver_delta");

	write_random(OLD, 4096, 0xC1u);
	write_random(NEW, 4096, 0xC2u);
	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Bump the version field (BE u32 at offset 4) to a value we don't know.
	auto raw = read_all(DELTA);
	raw[sizeof(DELTA_MAGIC) + 0] = 0xFF;
	raw[sizeof(DELTA_MAGIC) + 1] = 0xFF;
	raw[sizeof(DELTA_MAGIC) + 2] = 0xFF;
	raw[sizeof(DELTA_MAGIC) + 3] = 0xFE;
	write_bytes(DELTA, raw);

	SilenceCout s;
	EXPECT_NE(view_delta(DELTA), 0);

	std::remove(OLD.c_str());
	std::remove(NEW.c_str());
	std::remove(DELTA.c_str());
}

// The viewer allocates a buffer for the ADDED payload from the declared
// chunk_size, so it needs the same bound as Apply — before this check, a
// hostile size field aborted the process with std::bad_alloc.
TEST(DeltaViewer, rejects_oversized_chunk_size)
{
	const std::string DELTA = viewer_path("oversized_chunk");

	std::vector<uint8_t> raw(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	for (int shift = 24; shift >= 0; shift -= 8)
		raw.push_back(static_cast<uint8_t>((DELTA_FORMAT_VERSION >> shift) & 0xFF));
	const auto push_u64_be = [&raw](uint64_t value) {
		for (int shift = 56; shift >= 0; shift -= 8)
			raw.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
	};
	push_u64_be(static_cast<uint64_t>(EntryType::ADDED_CHUNK));
	push_u64_be(0xDEADBEEFu);                             // signature
	raw.insert(raw.end(), BLAKE2b().get_hash_size(), 0);  // hash
	push_u64_be(uint64_t{1} << 62);                       // 4 EiB chunk_size
	write_bytes(DELTA, raw);

	SilenceCout s;
	EXPECT_NE(view_delta(DELTA), 0);

	std::remove(DELTA.c_str());
}
