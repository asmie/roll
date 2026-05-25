#include <limits.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include "gtest/gtest.h"

#include "RK_finger.hpp"
#include "blake.h"
#include "Signature.hpp"

TEST(Signature, generate_signature_null)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("");
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

TEST(Signature, generate_signature_incorrect)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("non-existing-file");
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

TEST(Signature, generate_signature)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("../tests/testfile");

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
}

namespace {

void write_tmp(const std::filesystem::path& p, const std::vector<uint8_t>& bytes)
{
	std::ofstream f(p, std::ios::binary);
	if (!bytes.empty())
		f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

TEST(Signature, small_file_under_window)
{
	// File shorter than WINDOW_DEF_SIZE: must still produce one chunk with a
	// deterministic, content-derived signature (not zero / not stale).
	const auto path = std::filesystem::temp_directory_path() / "sig_small_under_window";
	std::vector<uint8_t> data(WINDOW_DEF_SIZE / 2);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>(i * 13u + 7u);
	write_tmp(path, data);

	Signature<RKFinger, BLAKE512> a, b;
	a.generate_signatures(path);
	b.generate_signatures(path);

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

	const auto p_data = std::filesystem::temp_directory_path() / "sig_shift_orig";
	const auto p_shift = std::filesystem::temp_directory_path() / "sig_shift_plus1";
	write_tmp(p_data, data);
	write_tmp(p_shift, shifted);

	Signature<RKFinger, BLAKE512> a, b;
	a.generate_signatures(p_data);
	b.generate_signatures(p_shift);

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
	const auto p1 = std::filesystem::temp_directory_path() / "sig_regen_a";
	const auto p2 = std::filesystem::temp_directory_path() / "sig_regen_b";
	std::vector<uint8_t> a(WINDOW_DEF_SIZE * 4, 0xAA);
	std::vector<uint8_t> b(WINDOW_DEF_SIZE / 2, 0xBB);
	write_tmp(p1, a);
	write_tmp(p2, b);

	Signature<RKFinger, BLAKE512> sig;
	sig.generate_signatures(p1);
	const size_t first_count = sig.get_chunks().size();
	ASSERT_GT(first_count, 0u);

	sig.generate_signatures(p2);
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
	const auto p1 = std::filesystem::temp_directory_path() / "sig_small_a";
	const auto p2 = std::filesystem::temp_directory_path() / "sig_small_b";
	std::vector<uint8_t> a(WINDOW_DEF_SIZE / 2, 0xAA);
	std::vector<uint8_t> b = a;
	b[5] ^= 0x01;
	write_tmp(p1, a);
	write_tmp(p2, b);

	Signature<RKFinger, BLAKE512> sa, sb;
	sa.generate_signatures(p1);
	sb.generate_signatures(p2);

	ASSERT_EQ(sa.get_chunks().size(), 1u);
	ASSERT_EQ(sb.get_chunks().size(), 1u);
	EXPECT_NE(sa.get_chunks()[0].signature, sb.get_chunks()[0].signature);

	std::filesystem::remove(p1);
	std::filesystem::remove(p2);
}