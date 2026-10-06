#include "TestWorkspace.hpp"
#include <limits.h>
#include <limits>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <vector>
#include "gtest/gtest.h"

#include "DeltaFormat.hpp"
#include "RK_finger.hpp"
#include "blake2b.h"
#include "Signature.hpp"

TEST(Signature, generate_signature_null)
{
	Signature<RKFinger, BLAKE2b> signatures;

	EXPECT_FALSE(signatures.generate_signatures(""))
		<< "an unopenable path must report failure, not just produce no chunks";
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

TEST(Signature, generate_signature_incorrect)
{
	Signature<RKFinger, BLAKE2b> signatures;

	EXPECT_FALSE(signatures.generate_signatures("non-existing-file"))
		<< "a missing file must report failure, not just produce no chunks";
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

namespace {

void write_tmp(const std::filesystem::path& p, const std::vector<uint8_t>& bytes)
{
	testfiles::write(p, bytes);
}

} // namespace

TEST(Signature, generate_signature)
{
	// Generate the 512 KiB input under temp_directory_path() from a fixed seed
	// rather than reading a repo-relative fixture: "../tests/testfile" only
	// resolves when the build directory happens to sit directly inside the
	// repository root, so the test failed in any other build location
	// (sanitizer builds, IDE build dirs, packaging trees).
	const auto path = testfiles::directory() / "sig_generate_signature";
	std::mt19937 rng(0x5EEDu);
	std::vector<uint8_t> data(512 * 1024);
	for (auto& b : data)
		b = static_cast<uint8_t>(rng());
	write_tmp(path, data);

	Signature<RKFinger, BLAKE2b> signatures;

	ASSERT_TRUE(signatures.generate_signatures(path));

	auto chunks = signatures.get_chunks();

	// 512 KiB input, 8 KiB target average -> ~64 chunks; allow generous slack
	// for content-dependent boundary distribution.
	ASSERT_GT(chunks.size(), 16u);
	ASSERT_LT(chunks.size(), 256u);

	// Verify chunks were created and have valid data
	ASSERT_GT(chunks[0].signature, 0);
	ASSERT_GT(chunks[0].chunk_size, 0);
	ASSERT_EQ(chunks[0].start_offset, 0);  // First chunk starts at 0

	// Verify chunks are contiguous
	for (size_t i = 1; i < chunks.size(); ++i) {
		ASSERT_EQ(chunks[i].start_offset, chunks[i-1].start_offset + chunks[i-1].chunk_size);
	}

	// Every chunk must respect the format bounds, except the residual at EOF
	// which is legitimately allowed to be shorter than the minimum.
	for (size_t i = 0; i < chunks.size(); ++i) {
		EXPECT_LE(chunks[i].chunk_size, DELTA_MAX_CHUNK_SIZE) << "chunk " << i;
		if (i + 1 < chunks.size()) {
			EXPECT_GE(chunks[i].chunk_size, DELTA_MIN_CHUNK_SIZE) << "chunk " << i;
		}
	}
	EXPECT_EQ(chunks.back().start_offset + chunks.back().chunk_size, data.size())
		<< "chunks must cover the whole input";

	std::filesystem::remove(path);
}

TEST(Signature, small_file_under_window)
{
	// File shorter than WINDOW_DEF_SIZE: must still produce one chunk with a
	// deterministic, content-derived signature (not zero / not stale).
	const auto path = testfiles::directory() / "sig_small_under_window";
	std::vector<uint8_t> data(WINDOW_DEF_SIZE / 2);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>(i * 13u + 7u);
	write_tmp(path, data);

	Signature<RKFinger, BLAKE2b> a, b;
	ASSERT_TRUE(a.generate_signatures(path));
	ASSERT_TRUE(b.generate_signatures(path));

	ASSERT_EQ(a.get_chunks().size(), 1u);
	ASSERT_EQ(b.get_chunks().size(), 1u);
	EXPECT_EQ(a.get_chunks()[0].signature, b.get_chunks()[0].signature);
	EXPECT_NE(a.get_chunks()[0].signature, 0u);
	EXPECT_EQ(a.get_chunks()[0].chunk_size, data.size());

	std::filesystem::remove(path);
}

TEST(Signature, shift_invariance_resyncs_after_insertion)
{
	// CDC with a rolling fingerprint should re-synchronise after a single-byte
	// insertion at the start: some tail of the chunks produced over (0x42 +
	// data) must match a tail of the chunks produced over (data) by content.
	std::mt19937 rng(0xC0FFEEu);
	std::vector<uint8_t> data(64 * 1024);
	for (auto& b : data) b = static_cast<uint8_t>(rng());

	std::vector<uint8_t> shifted;
	shifted.reserve(data.size() + 1);
	shifted.push_back(0x42);
	shifted.insert(shifted.end(), data.begin(), data.end());

	const auto p_data = testfiles::directory() / "sig_shift_orig";
	const auto p_shift = testfiles::directory() / "sig_shift_plus1";
	write_tmp(p_data, data);
	write_tmp(p_shift, shifted);

	Signature<RKFinger, BLAKE2b> a, b;
	ASSERT_TRUE(a.generate_signatures(p_data));
	ASSERT_TRUE(b.generate_signatures(p_shift));

	const auto& ca = a.get_chunks();
	const auto& cb = b.get_chunks();
	ASSERT_GT(ca.size(), 2u);
	ASSERT_GT(cb.size(), 2u);

	// Find any (i, j) where b[i].hash == a[j].hash and assert chunks line up
	// from that point. With a working rolling hash and content-defined
	// chunking, this match must exist well before either tail runs out.
	bool found = false;
	for (size_t j = 0; j < ca.size() && !found; ++j) {
		for (size_t i = 0; i < cb.size() && !found; ++i) {
			if (cb[i].hash != ca[j].hash) continue;
			if (cb.size() - i != ca.size() - j) continue;
			bool aligned = true;
			for (size_t k = 0; k < cb.size() - i; ++k) {
				if (cb[i + k].hash != ca[j + k].hash) { aligned = false; break; }
			}
			if (aligned) found = true;
		}
	}
	EXPECT_TRUE(found) << "no resynchronisation found after a 1-byte shift";

	std::filesystem::remove(p_data);
	std::filesystem::remove(p_shift);
}

TEST(Signature, regenerate_replaces_previous_chunks)
{
	// Calling generate_signatures twice on the same instance must not append
	// to the previous run's chunks.
	const auto p1 = testfiles::directory() / "sig_regen_a";
	const auto p2 = testfiles::directory() / "sig_regen_b";
	std::vector<uint8_t> a(WINDOW_DEF_SIZE * 4, 0xAA);
	std::vector<uint8_t> b(WINDOW_DEF_SIZE / 2, 0xBB);
	write_tmp(p1, a);
	write_tmp(p2, b);

	Signature<RKFinger, BLAKE2b> sig;
	ASSERT_TRUE(sig.generate_signatures(p1));
	const size_t first_count = sig.get_chunks().size();
	ASSERT_GT(first_count, 0u);

	ASSERT_TRUE(sig.generate_signatures(p2));
	ASSERT_EQ(sig.get_chunks().size(), 1u);
	EXPECT_EQ(sig.get_chunks()[0].chunk_size, b.size());

	std::filesystem::remove(p1);
	std::filesystem::remove(p2);
}

TEST(Signature, small_file_distinct_signatures)
{
	// Two distinct sub-window files must yield distinct signatures — the bug
	// pinned every short-file signature to 0 / to whatever happened to be in
	// current_fingerprint, conflating different content.
	const auto p1 = testfiles::directory() / "sig_small_a";
	const auto p2 = testfiles::directory() / "sig_small_b";
	std::vector<uint8_t> a(WINDOW_DEF_SIZE / 2, 0xAA);
	std::vector<uint8_t> b = a;
	b[5] ^= 0x01;
	write_tmp(p1, a);
	write_tmp(p2, b);

	Signature<RKFinger, BLAKE2b> sa, sb;
	ASSERT_TRUE(sa.generate_signatures(p1));
	ASSERT_TRUE(sb.generate_signatures(p2));

	ASSERT_EQ(sa.get_chunks().size(), 1u);
	ASSERT_EQ(sb.get_chunks().size(), 1u);
	EXPECT_NE(sa.get_chunks()[0].signature, sb.get_chunks()[0].signature);

	std::filesystem::remove(p1);
	std::filesystem::remove(p2);
}
// Constant content holds the rolling fingerprint at one fixed value, so the
// boundary predicate either fires at every byte or never. With the old
// compare-against-zero predicate, all-zero input (fingerprint 0) fired at
// every byte past the minimum: a zero-filled file degenerated into maximal
// chunk counts at MIN_CHUNK_SIZE, with 17% metadata overhead in its deltas.
// The target value is chosen so no single-byte-constant content can fire,
// meaning constant runs must always cut at MAX_CHUNK_SIZE.
TEST(Signature, constant_content_cuts_at_max_chunk_size)
{
	for (unsigned value = 0; value < 256; ++value) {
		const auto byte = static_cast<uint8_t>(value);
		const auto path = testfiles::directory() /
		                  ("sig_const_" + std::to_string(byte));
		const size_t file_size = 4 * DELTA_MAX_CHUNK_SIZE;
		write_tmp(path, std::vector<uint8_t>(file_size, byte));

		Signature<RKFinger, BLAKE2b> sig;
		ASSERT_TRUE(sig.generate_signatures(path));
		const auto& chunks = sig.get_chunks();

		ASSERT_EQ(chunks.size(), 4u)
			<< "constant 0x" << std::hex << int(byte)
			<< " content must produce maximal chunks, not degenerate to minimal ones";
		for (const auto& chunk : chunks)
			EXPECT_EQ(chunk.chunk_size, DELTA_MAX_CHUNK_SIZE);

		std::filesystem::remove(path);
	}
}

// WholeFileHash::Compute digests the input as a side effect of chunking. The
// digest must equal an independent one-shot hash of the file — the emitted
// chunks contiguously cover the input, which is the property this relies on.
TEST(Signature, whole_file_hash_matches_a_one_shot_of_the_file)
{
	const auto path = testfiles::directory() / "sig_whole_hash";
	std::mt19937 rng(0xF00Du);
	std::vector<uint8_t> data(100 * 1024);
	for (auto& b : data)
		b = static_cast<uint8_t>(rng());
	write_tmp(path, data);

	Signature<RKFinger, BLAKE2b> sig;
	ASSERT_TRUE(sig.generate_signatures(path, WholeFileHash::Compute));
	ASSERT_GT(sig.get_chunks().size(), 1u) << "input should span several chunks";

	BLAKE2b one_shot;
	std::vector<uint8_t> expected(one_shot.get_hash_size());
	one_shot.hash(expected, data);

	EXPECT_EQ(sig.whole_file_hash(), expected);

	std::filesystem::remove(path);
}

TEST(Signature, whole_file_hash_is_empty_unless_requested)
{
	const auto path = testfiles::directory() / "sig_whole_skip";
	write_tmp(path, std::vector<uint8_t>(2048, 0x42));

	Signature<RKFinger, BLAKE2b> sig;
	ASSERT_TRUE(sig.generate_signatures(path));
	EXPECT_TRUE(sig.whole_file_hash().empty());

	// A later Compute run fills it; a following Skip run clears it again, so a
	// stale digest can never be attributed to the wrong input.
	ASSERT_TRUE(sig.generate_signatures(path, WholeFileHash::Compute));
	EXPECT_FALSE(sig.whole_file_hash().empty());
	ASSERT_TRUE(sig.generate_signatures(path));
	EXPECT_TRUE(sig.whole_file_hash().empty());

	std::filesystem::remove(path);
}

TEST(Signature, whole_file_hash_of_an_empty_file_is_the_empty_digest)
{
	const auto path = testfiles::directory() / "sig_whole_empty";
	write_tmp(path, {});

	Signature<RKFinger, BLAKE2b> sig;
	ASSERT_TRUE(sig.generate_signatures(path, WholeFileHash::Compute));
	ASSERT_EQ(sig.get_chunks().size(), 0u);

	BLAKE2b one_shot;
	std::vector<uint8_t> expected(one_shot.get_hash_size());
	one_shot.hash(expected, std::span<const uint8_t>{});

	EXPECT_EQ(sig.whole_file_hash(), expected);

	std::filesystem::remove(path);
}

TEST(Signature, propagates_a_seek_error_and_clears_previous_results)
{
	const auto path = testfiles::directory() / "sig_io_error";
	write_tmp(path, std::vector<uint8_t>(1024, 'A'));
	Signature<RKFinger, BLAKE2b> signature;
	ASSERT_TRUE(signature.generate_signatures(path, WholeFileHash::Compute));
	FileIO file;
	ASSERT_TRUE(file.open(path, FileMode::IN));
	EXPECT_TRUE(file.read_chunk(1, std::numeric_limits<size_t>::max()).empty());
	EXPECT_FALSE(signature.generate_signatures(file, WholeFileHash::Compute));
	EXPECT_TRUE(signature.get_chunks().empty());
	EXPECT_TRUE(signature.whole_file_hash().empty());
}
