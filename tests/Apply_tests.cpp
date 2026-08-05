#include "gtest/gtest.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaCodec.hpp"
#include "DeltaFormat.hpp"
#include "FileIO.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake2b.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

// Layout-derived constants so tests don't break silently if the entry
// header or opcode encoding changes. See writeDeltaEntry / createOptimizedDiff
// in src/Delta.hpp.
//   header  = entry_type:u64 | signature:u64 | hash:hash_size | chunk_size:u64
//   D-op    = 'D' | pos:u32 BE | count:u8 | count bytes
inline size_t entry_header_size()
{
	return 3 * sizeof(uint64_t) + BLAKE2b().get_hash_size();
}

constexpr size_t d_opcode_size(size_t inline_count)
{
	return 1 + sizeof(uint32_t) + 1 + inline_count;
}

// Build a unique path in the system temp directory so tests don't trample
// each other and can run in parallel.
std::string tpath(const char* name)
{
	static std::atomic<unsigned> counter{0};
	const auto p = std::filesystem::temp_directory_path() /
	               ("roll_" + std::to_string(counter++) + "_" + name);
	return p.string();
}

void write_random(const std::string& path, size_t bytes, uint32_t seed)
{
	std::mt19937 rng(seed);
	std::uniform_int_distribution<int> dist(0, 255);
	std::ofstream f(path, std::ios::binary);
	for (size_t i = 0; i < bytes; ++i)
		f.put(static_cast<char>(dist(rng)));
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
	auto size = f.tellg();
	f.seekg(0);
	std::vector<uint8_t> buf(static_cast<size_t>(size));
	if (size > 0)
		f.read(reinterpret_cast<char*>(buf.data()), buf.size());
	return buf;
}

void cleanup(std::initializer_list<std::string> paths)
{
	for (const auto& p : paths)
		std::remove(p.c_str());
}

// Append `value` as a big-endian u64, matching the entry header encoding in
// src/Delta.hpp.
void push_u64_be(std::vector<uint8_t>& out, uint64_t value)
{
	for (int shift = 56; shift >= 0; shift -= 8)
		out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
}

// Hand-build a valid-looking single-entry delta whose chunk_size field is
// `declared_size`. Used to check that readers bound the declared size *before*
// allocating a buffer for it.
std::vector<uint8_t> crafted_delta(EntryType type, uint64_t declared_size)
{
	std::vector<uint8_t> raw(std::begin(DELTA_MAGIC), std::end(DELTA_MAGIC));
	for (int shift = 24; shift >= 0; shift -= 8)
		raw.push_back(static_cast<uint8_t>((DELTA_FORMAT_VERSION >> shift) & 0xFF));
	push_u64_be(raw, static_cast<uint64_t>(type));
	push_u64_be(raw, 0xDEADBEEFu);                       // signature
	raw.insert(raw.end(), BLAKE2b().get_hash_size(), 0);  // hash
	push_u64_be(raw, declared_size);
	return raw;
}

bool roundtrip(const std::string& old_path, const std::string& new_path,
               const std::string& delta_path, const std::string& out_path,
               std::string* err = nullptr)
{
	Signature<RKFinger, BLAKE2b> old_sig, new_sig;
	if (!old_sig.generate_signatures(old_path)) {
		if (err) *err = "signature: cannot read " + old_path;
		return false;
	}
	if (!new_sig.generate_signatures(new_path)) {
		if (err) *err = "signature: cannot read " + new_path;
		return false;
	}

	Delta<RKFinger, BLAKE2b> delta;
	auto dr = delta.generate_delta(old_sig, new_sig, old_path, new_path, delta_path);
	if (!dr.has_value()) {
		if (err) *err = "delta: " + dr.error().message;
		return false;
	}

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(old_path, delta_path, out_path);
	if (!ar.has_value()) {
		if (err) *err = "apply: " + ar.error().message;
		return false;
	}
	return true;
}

} // namespace

TEST(Apply, identical_files)
{
	const std::string OLD = tpath("apply_t_identical_old");
	const std::string NEW = tpath("apply_t_identical_new");
	const std::string DELTA = tpath("apply_t_identical_delta");
	const std::string OUT = tpath("apply_t_identical_out");

	write_random(OLD, 8192, 0xC0FFEE);
	write_random(NEW, 8192, 0xC0FFEE);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, empty_old)
{
	const std::string OLD = tpath("apply_t_empty_old_old");
	const std::string NEW = tpath("apply_t_empty_old_new");
	const std::string DELTA = tpath("apply_t_empty_old_delta");
	const std::string OUT = tpath("apply_t_empty_old_out");

	write_bytes(OLD, {});
	write_random(NEW, 4096, 0xBEEFu);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, empty_new)
{
	const std::string OLD = tpath("apply_t_empty_new_old");
	const std::string NEW = tpath("apply_t_empty_new_new");
	const std::string DELTA = tpath("apply_t_empty_new_delta");
	const std::string OUT = tpath("apply_t_empty_new_out");

	write_random(OLD, 4096, 0xCAFEu);
	write_bytes(NEW, {});

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, append_only)
{
	const std::string OLD = tpath("apply_t_append_old");
	const std::string NEW = tpath("apply_t_append_new");
	const std::string DELTA = tpath("apply_t_append_delta");
	const std::string OUT = tpath("apply_t_append_out");

	write_random(OLD, 8192, 0xDEADu);

	auto base = read_all(OLD);
	std::vector<uint8_t> appended = base;
	std::mt19937 rng(0xBEEFu);
	std::uniform_int_distribution<int> dist(0, 255);
	for (size_t i = 0; i < 4096; ++i)
		appended.push_back(static_cast<uint8_t>(dist(rng)));
	write_bytes(NEW, appended);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncate_only)
{
	const std::string OLD = tpath("apply_t_trunc_old");
	const std::string NEW = tpath("apply_t_trunc_new");
	const std::string DELTA = tpath("apply_t_trunc_delta");
	const std::string OUT = tpath("apply_t_trunc_out");

	write_random(OLD, 8192 + 4096, 0xFEEDu);
	auto big = read_all(OLD);
	std::vector<uint8_t> trimmed(big.begin(), big.begin() + 8192);
	write_bytes(NEW, trimmed);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, in_chunk_modification_dwords)
{
	const std::string OLD = tpath("apply_t_inplace_old");
	const std::string NEW = tpath("apply_t_inplace_new");
	const std::string DELTA = tpath("apply_t_inplace_delta");
	const std::string OUT = tpath("apply_t_inplace_out");

	// 256-byte file: below MIN_CHUNK_SIZE=512, so it stays a single chunk and
	// content-defined boundary detection cannot shift between old and new.
	std::vector<uint8_t> data(256, 0xAA);
	write_bytes(OLD, data);

	auto modified = data;
	modified[100] = 0x11;
	modified[101] = 0x22;
	modified[102] = 0x33;
	modified[103] = 0x44;
	write_bytes(NEW, modified);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, myers_delete_at_start_yields_tiny_delta)
{
	// Single-byte deletion at the start of a chunk: a proper SES-based diff
	// produces an X(0,1) opcode plus an unchanged-tail match copy — total
	// diff payload of ~9 bytes regardless of chunk size. The greedy diff
	// produced a full-chunk D + trailing X for this case.
	const std::string OLD = tpath("apply_t_myers_shift_old");
	const std::string NEW = tpath("apply_t_myers_shift_new");
	const std::string DELTA = tpath("apply_t_myers_shift_delta");
	const std::string OUT = tpath("apply_t_myers_shift_out");

	std::vector<uint8_t> data(256);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>(i);
	write_bytes(OLD, data);

	std::vector<uint8_t> shifted(data.begin() + 1, data.end());
	write_bytes(NEW, shifted);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	// Header (8) + entry header (88) + X opcode (9) + trailer (65) = 170.
	// Allow some slack for chunking edge cases but assert the delta is small
	// compared to the chunk size.
	const auto delta_size = read_all(DELTA).size();
	EXPECT_LT(delta_size, 200u) << "delta unexpectedly large: " << delta_size;

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, in_chunk_deletion_consumes_x_opcode)
{
	const std::string OLD = tpath("apply_t_xop_old");
	const std::string NEW = tpath("apply_t_xop_new");
	const std::string DELTA = tpath("apply_t_xop_delta");
	const std::string OUT = tpath("apply_t_xop_out");

	// 256 distinct bytes form a single chunk (< MIN_CHUNK_SIZE). Removing one
	// byte in the middle makes every byte after the deletion point differ from
	// old, so createOptimizedDiff fills target_size with 'D' replacement bytes
	// and emits a trailing 'X' to delete the leftover old tail. The diff parser
	// must consume that 'X' even after output has reached target_size.
	std::vector<uint8_t> data(256);
	for (size_t i = 0; i < 256; ++i)
		data[i] = static_cast<uint8_t>(i);
	write_bytes(OLD, data);

	std::vector<uint8_t> deleted(data);
	deleted.erase(deleted.begin() + 100);
	write_bytes(NEW, deleted);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, middle_modification_preserves_order)
{
	const std::string OLD = tpath("apply_t_mid_old");
	const std::string NEW = tpath("apply_t_mid_new");
	const std::string DELTA = tpath("apply_t_mid_delta");
	const std::string OUT = tpath("apply_t_mid_out");

	// 64 KB ensures the CDC produces multiple chunks (MAX_CHUNK_SIZE = 16 KB).
	// Flipping a small middle range modifies a middle chunk while surrounding
	// chunks stay identical, so the delta contains ORIGINAL entries on both
	// sides of a MODIFIED entry. This exposes any reorder of entry processing.
	write_random(OLD, 65536, 0xABCDEFu);
	auto data = read_all(OLD);
	for (size_t i = 32000; i < 32016; ++i)
		data[i] ^= 0xFF;
	write_bytes(NEW, data);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_added_payload_fails)
{
	const std::string OLD = tpath("apply_t_trunc_pl_old");
	const std::string NEW = tpath("apply_t_trunc_pl_new");
	const std::string DELTA = tpath("apply_t_trunc_pl_delta");
	const std::string OUT = tpath("apply_t_trunc_pl_out");

	// Empty old + non-empty new yields an all-ADDED delta whose tail is a
	// chunk payload — chopping bytes off the end leaves a short final payload.
	write_bytes(OLD, {});
	write_random(NEW, 4096, 0x123456u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Drop a single byte: with a non-empty new file the last entry's payload
	// is always >= 1 byte, so this always lands mid-payload regardless of the
	// chunk-size distribution chosen by CDC for this seed.
	auto raw = read_all(DELTA);
	ASSERT_FALSE(raw.empty());
	raw.pop_back();
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_modified_after_header_fails)
{
	const std::string OLD = tpath("apply_t_modtrunc1_old");
	const std::string NEW = tpath("apply_t_modtrunc1_new");
	const std::string DELTA = tpath("apply_t_modtrunc1_delta");
	const std::string OUT = tpath("apply_t_modtrunc1_out");

	// 256 distinct bytes form a single chunk; flipping one byte yields a
	// MODIFIED entry. Truncating the delta to the first 88 bytes leaves the
	// entry header with zero opcode bytes; tail-copy from old would otherwise
	// silently reproduce the old chunk.
	std::vector<uint8_t> data(256);
	for (size_t i = 0; i < 256; ++i)
		data[i] = static_cast<uint8_t>(i);
	write_bytes(OLD, data);

	auto modified = data;
	modified[100] = 0x99;
	write_bytes(NEW, modified);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	const size_t header_size = entry_header_size();
	auto raw = read_all(DELTA);
	ASSERT_GT(raw.size(), header_size);
	raw.resize(header_size);  // header only, no diff opcodes
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_modified_at_opcode_boundary_fails)
{
	const std::string OLD = tpath("apply_t_modtrunc2_old");
	const std::string NEW = tpath("apply_t_modtrunc2_new");
	const std::string DELTA = tpath("apply_t_modtrunc2_delta");
	const std::string OUT = tpath("apply_t_modtrunc2_out");

	// Two well-separated 1-byte changes produce two 'D' opcodes in the diff.
	// Truncating between them lets the parser successfully read one opcode and
	// then see EOF; tail-copy fills the rest from old, leaving the second
	// modification unapplied. Only hash verification catches this.
	std::vector<uint8_t> data(256);
	for (size_t i = 0; i < 256; ++i)
		data[i] = static_cast<uint8_t>(i);
	write_bytes(OLD, data);

	auto modified = data;
	modified[100] = 0x99;
	modified[200] = 0x77;
	write_bytes(NEW, modified);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Truncate after the first 'D' opcode (count=1) but before the second.
	const size_t cut = entry_header_size() + d_opcode_size(1);
	auto raw = read_all(DELTA);
	ASSERT_GT(raw.size(), cut);
	raw.resize(cut);
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_output_aliasing_old)
{
	const std::string OLD = tpath("apply_t_alias_old_old");
	const std::string NEW = tpath("apply_t_alias_old_new");
	const std::string DELTA = tpath("apply_t_alias_old_delta");

	write_random(OLD, 4096, 0xA1u);
	write_random(NEW, 4096, 0xA2u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	auto old_before = read_all(OLD);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OLD);  // output == old
	EXPECT_FALSE(ar.has_value());

	// Old file must be untouched: aliased open would have truncated it.
	EXPECT_EQ(read_all(OLD), old_before);

	cleanup({OLD, NEW, DELTA});
}

TEST(Apply, rejects_output_aliasing_delta)
{
	const std::string OLD = tpath("apply_t_alias_delta_old");
	const std::string NEW = tpath("apply_t_alias_delta_new");
	const std::string DELTA = tpath("apply_t_alias_delta_delta");

	write_random(OLD, 4096, 0xB1u);
	write_random(NEW, 4096, 0xB2u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	auto delta_before = read_all(DELTA);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, DELTA);  // output == delta
	EXPECT_FALSE(ar.has_value());

	// Delta file must be untouched: aliased open would have truncated it,
	// after which the apply loop would see EOF and report success silently.
	EXPECT_EQ(read_all(DELTA), delta_before);

	cleanup({OLD, NEW, DELTA});
}

TEST(Apply, duplicate_removed_entry_rejected)
{
	const std::string OLD = tpath("apply_t_dupr_old");
	const std::string NEW = tpath("apply_t_dupr_new");
	const std::string DELTA = tpath("apply_t_dupr_delta");
	const std::string OUT = tpath("apply_t_dupr_out");

	// Empty new + non-empty old yields an all-REMOVED delta. Each REMOVED
	// entry's header is exactly entry_header_size() bytes, no payload. We
	// duplicate the last entry to simulate a malformed delta that references
	// the same old chunk twice; Apply must reject this even though the chunk
	// content does exist in old (chunk_map.find would succeed).
	write_random(OLD, 4096, 0xD1u);
	write_bytes(NEW, {});

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	const size_t header_size = entry_header_size();
	constexpr size_t trailer_size = 1 + BLAKE2b::HASH_SIZE;
	auto raw = read_all(DELTA);
	ASSERT_GE(raw.size(), trailer_size + header_size);
	// Duplicate the last REMOVED entry, inserted before the trailer so the
	// parser sees it as another entry rather than skipping past EOF.
	raw.insert(raw.end() - trailer_size,
	           raw.end() - trailer_size - header_size, raw.end() - trailer_size);
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_mutated_old_between_create_and_apply)
{
	// Generate a delta against an old file, then change a byte in the old
	// before applying. The regenerated signature in Apply will produce
	// different chunks for the changed region, so ORIGINAL entries from the
	// delta will fail to find a match. (Worst case for unchanged chunking,
	// MODIFIED reconstruction's hash check still rejects.)
	const std::string OLD = tpath("apply_t_old_mutated_old");
	const std::string NEW = tpath("apply_t_old_mutated_new");
	const std::string DELTA = tpath("apply_t_old_mutated_delta");
	const std::string OUT = tpath("apply_t_old_mutated_out");

	write_random(OLD, 32 * 1024, 0xF1u);
	auto base = read_all(OLD);
	auto modified = base;
	modified[10000] ^= 0x55;
	write_bytes(NEW, modified);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Tamper with the OLD file after the delta is produced.
	auto tampered = base;
	tampered[5000] ^= 0xAA;
	write_bytes(OLD, tampered);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_corrupted_original_hash)
{
	// Generate a delta of identical files (all-ORIGINAL entries) and corrupt
	// one byte inside the first entry's hash field. findUnusedMatch must not
	// find a chunk with that bogus hash → apply rejects.
	const std::string OLD = tpath("apply_t_corr_hash_old");
	const std::string NEW = tpath("apply_t_corr_hash_new");
	const std::string DELTA = tpath("apply_t_corr_hash_delta");
	const std::string OUT = tpath("apply_t_corr_hash_out");

	write_random(OLD, 32 * 1024, 0x55u);
	write_random(NEW, 32 * 1024, 0x55u);  // same seed -> identical content

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Header (8) + entry_type (8) + signature (8) lands us at the hash bytes.
	auto raw = read_all(DELTA);
	const size_t hash_offset = DELTA_HEADER_SIZE + 2 * sizeof(uint64_t);
	ASSERT_GT(raw.size(), hash_offset);
	raw[hash_offset] ^= 0xFF;
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, failed_apply_removes_output_stub)
{
	const std::string OLD = tpath("apply_t_nostub_old");
	const std::string NEW = tpath("apply_t_nostub_new");
	const std::string DELTA = tpath("apply_t_nostub_delta");
	const std::string OUT = tpath("apply_t_nostub_out");

	write_random(OLD, 2048, 0xE1u);
	write_random(NEW, 2048, 0xE2u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Corrupt the header so apply fails after creating the output file.
	auto raw = read_all(DELTA);
	raw[0] ^= 0xFF;
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());
	EXPECT_FALSE(std::filesystem::exists(OUT))
		<< "output stub was left on disk after a failed apply";

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_bad_magic)
{
	const std::string OLD = tpath("apply_t_badmagic_old");
	const std::string NEW = tpath("apply_t_badmagic_new");
	const std::string DELTA = tpath("apply_t_badmagic_delta");
	const std::string OUT = tpath("apply_t_badmagic_out");

	write_random(OLD, 4096, 0xD1u);
	write_random(NEW, 4096, 0xD2u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	auto raw = read_all(DELTA);
	ASSERT_GE(raw.size(), 4u);
	raw[0] ^= 0xFF;  // corrupt the magic
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_partial_header_fails)
{
	const std::string OLD = tpath("apply_t_trunc_hdr_old");
	const std::string NEW = tpath("apply_t_trunc_hdr_new");
	const std::string DELTA = tpath("apply_t_trunc_hdr_delta");
	const std::string OUT = tpath("apply_t_trunc_hdr_out");

	// Empty new + non-empty old yields an all-REMOVED delta with no payloads,
	// so a short trailing fragment is unambiguously an incomplete next header.
	write_random(OLD, 4096, 0x99EEu);
	write_bytes(NEW, {});

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	auto raw = read_all(DELTA);
	constexpr size_t trailer_size = 1 + BLAKE2b::HASH_SIZE;
	// Guard the iterator arithmetic below: `raw.end() - trailer_size` is out of
	// range, not merely wrong, if the delta is ever shorter than its trailer.
	ASSERT_GE(raw.size(), trailer_size);
	const uint8_t partial[] = {0x00, 0x01, 0x02, 0x03};  // < 8 bytes of u64 entry_type
	raw.insert(raw.end() - trailer_size, std::begin(partial), std::end(partial));
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_missing_trailer)
{
	const std::string OLD = tpath("apply_t_no_trail_old");
	const std::string NEW = tpath("apply_t_no_trail_new");
	const std::string DELTA = tpath("apply_t_no_trail_delta");
	const std::string OUT = tpath("apply_t_no_trail_out");

	write_random(OLD, 4096, 0xA0u);
	write_random(NEW, 4096, 0xA1u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Strip the trailer (1 tag + hash_size).
	auto raw = read_all(DELTA);
	constexpr size_t trailer_size = 1 + BLAKE2b::HASH_SIZE;
	// Clamp rather than relying on the assertion above to bound the subtraction:
	// ASSERT_GT returns at run time, but the optimiser cannot see that, and an
	// unbounded size_t subtraction here trips -Wstringop-overflow.
	ASSERT_GT(raw.size(), trailer_size);
	raw.resize(raw.size() > trailer_size ? raw.size() - trailer_size : 0);
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_corrupted_whole_file_trailer)
{
	const std::string OLD = tpath("apply_t_trail_corrupt_old");
	const std::string NEW = tpath("apply_t_trail_corrupt_new");
	const std::string DELTA = tpath("apply_t_trail_corrupt_delta");
	const std::string OUT = tpath("apply_t_trail_corrupt_out");

	write_random(OLD, 4096, 0xA2u);
	write_random(NEW, 4096, 0xA3u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value());

	// Flip a byte inside the trailer hash.
	auto raw = read_all(DELTA);
	raw.back() ^= 0xFF;
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());

	cleanup({OLD, NEW, DELTA, OUT});
}

// A declared chunk_size is attacker-controlled: before it was bounded, a huge
// value reached `std::vector<uint8_t> vec(chunk_size)` and aborted the process
// with an uncaught std::bad_alloc, which also skipped the output-stub cleanup.
TEST(Apply, rejects_oversized_added_chunk_size)
{
	const std::string OLD = tpath("apply_t_oversize_added_old");
	const std::string DELTA = tpath("apply_t_oversize_added_delta");
	const std::string OUT = tpath("apply_t_oversize_added_out");

	write_random(OLD, 1024, 0xB1u);
	write_bytes(DELTA, crafted_delta(EntryType::ADDED_CHUNK, uint64_t{1} << 62));

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());
	EXPECT_NE(ar.error().message.find("out-of-range chunk size"), std::string::npos)
		<< "actual: " << ar.error().message;
	EXPECT_FALSE(std::filesystem::exists(OUT))
		<< "output stub was left on disk after a rejected delta";

	cleanup({OLD, DELTA, OUT});
}

TEST(Apply, rejects_oversized_modified_chunk_size)
{
	const std::string OLD = tpath("apply_t_oversize_mod_old");
	const std::string DELTA = tpath("apply_t_oversize_mod_delta");
	const std::string OUT = tpath("apply_t_oversize_mod_out");

	write_random(OLD, 1024, 0xB2u);
	// MODIFIED reaches the bound via applyDiff's output.reserve(target_size).
	write_bytes(DELTA, crafted_delta(EntryType::MODIFIED_CHUNK, ~uint64_t{0}));

	Apply<RKFinger, BLAKE2b> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.has_value());
	EXPECT_NE(ar.error().message.find("out-of-range chunk size"), std::string::npos)
		<< "actual: " << ar.error().message;
	EXPECT_FALSE(std::filesystem::exists(OUT));

	cleanup({OLD, DELTA, OUT});
}

// Pin the boundary: one byte over the format maximum is rejected by the size
// check, while the maximum itself falls through to the normal (truncated
// payload) failure path rather than being refused for its size.
TEST(Apply, chunk_size_bound_is_exact)
{
	const std::string OLD = tpath("apply_t_bound_old");
	const std::string DELTA = tpath("apply_t_bound_delta");
	const std::string OUT = tpath("apply_t_bound_out");

	write_random(OLD, 1024, 0xB3u);

	write_bytes(DELTA, crafted_delta(EntryType::ADDED_CHUNK, DELTA_MAX_CHUNK_SIZE + 1));
	Apply<RKFinger, BLAKE2b> over;
	auto over_result = over.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(over_result.has_value());
	EXPECT_NE(over_result.error().message.find("out-of-range chunk size"), std::string::npos)
		<< "actual: " << over_result.error().message;

	write_bytes(DELTA, crafted_delta(EntryType::ADDED_CHUNK, DELTA_MAX_CHUNK_SIZE));
	Apply<RKFinger, BLAKE2b> at_max;
	auto at_max_result = at_max.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(at_max_result.has_value());
	EXPECT_EQ(at_max_result.error().message.find("out-of-range chunk size"), std::string::npos)
		<< "the maximum legal chunk size must not be rejected for its size: "
		<< at_max_result.error().message;

	cleanup({OLD, DELTA, OUT});
}

// The failure category is the point of returning std::expected over a bool:
// callers (and these tests) can react to a kind of failure without matching on
// message text, which would make every reworded message a test change.
TEST(Apply, failure_categories_distinguish_causes)
{
	const std::string OLD = tpath("apply_t_cat_old");
	const std::string NEW = tpath("apply_t_cat_new");
	const std::string DELTA = tpath("apply_t_cat_delta");
	const std::string OUT = tpath("apply_t_cat_out");

	write_random(OLD, 4096, 0xCA71u);
	write_random(NEW, 4096, 0xCA72u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	ASSERT_TRUE(d.generate_delta(os, ns, OLD, NEW, DELTA).has_value());
	const auto good_delta = read_all(DELTA);

	Apply<RKFinger, BLAKE2b> apply;

	// A missing input is an environment problem, not a malformed delta.
	{
		const auto r = apply.apply_delta(tpath("apply_t_cat_absent"), DELTA, OUT);
		ASSERT_FALSE(r.has_value());
		EXPECT_EQ(r.error().code, DeltaErrc::io_error) << r.error().message;
	}

	// Corrupt magic is a malformed stream.
	{
		auto raw = good_delta;
		raw[0] ^= 0xFF;
		write_bytes(DELTA, raw);
		const auto r = apply.apply_delta(OLD, DELTA, OUT);
		ASSERT_FALSE(r.has_value());
		EXPECT_EQ(r.error().code, DeltaErrc::corrupt_delta) << r.error().message;
	}

	// A flipped bit inside the whole-file trailer is an integrity failure.
	{
		auto raw = good_delta;
		raw.back() ^= 0xFF;
		write_bytes(DELTA, raw);
		const auto r = apply.apply_delta(OLD, DELTA, OUT);
		ASSERT_FALSE(r.has_value());
		EXPECT_EQ(r.error().code, DeltaErrc::integrity_mismatch) << r.error().message;
	}

	// Aliasing the output onto an input is a caller mistake.
	{
		write_bytes(DELTA, good_delta);
		const auto r = apply.apply_delta(OLD, DELTA, OLD);
		ASSERT_FALSE(r.has_value());
		EXPECT_EQ(r.error().code, DeltaErrc::invalid_argument) << r.error().message;
	}

	cleanup({OLD, NEW, DELTA, OUT});
}

// Statistics are reachable only through a successful result, so a failed run can
// no longer be misread as having written bytes.
TEST(Apply, success_carries_statistics)
{
	const std::string OLD = tpath("apply_t_stats_old");
	const std::string NEW = tpath("apply_t_stats_new");
	const std::string DELTA = tpath("apply_t_stats_delta");
	const std::string OUT = tpath("apply_t_stats_out");

	write_random(OLD, 8192, 0x5747u);
	write_random(NEW, 8192, 0x5748u);

	Signature<RKFinger, BLAKE2b> os, ns;
	ASSERT_TRUE(os.generate_signatures(OLD));
	ASSERT_TRUE(ns.generate_signatures(NEW));
	Delta<RKFinger, BLAKE2b> d;
	const auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.has_value()) << dr.error().message;
	EXPECT_GT(dr->chunks_processed, 0u);
	EXPECT_EQ(dr->bytes_written, read_all(DELTA).size())
		<< "reported bytes must match the delta actually written";

	Apply<RKFinger, BLAKE2b> apply;
	const auto ar = apply.apply_delta(OLD, DELTA, OUT);
	ASSERT_TRUE(ar.has_value()) << ar.error().message;
	EXPECT_GT(ar->entries_processed, 0u);
	EXPECT_EQ(ar->bytes_written, read_all(NEW).size());

	cleanup({OLD, NEW, DELTA, OUT});
}

namespace {

// Count entry types in a delta, so compression behaviour can be asserted on the
// representation chosen rather than only on the total size.
std::map<EntryType, size_t> count_entry_types(const std::string& delta_path)
{
	std::map<EntryType, size_t> counts;
	FileIO in;
	if (!in.open(delta_path, FileMode::IN)) return counts;

	DeltaReader reader(in, BLAKE2b::HASH_SIZE);
	if (!reader.read_header()) return counts;

	while (reader.next_item() == DeltaReader::Item::Entry) {
		DeltaEntryHeader header;
		if (!reader.read_entry_header(header)) break;
		counts[header.type]++;

		if (header.type == EntryType::ADDED_CHUNK) {
			std::vector<uint8_t> payload;
			if (!reader.read_payload(static_cast<size_t>(header.chunk_size), payload))
				break;
		} else if (header.type == EntryType::MODIFIED_CHUNK) {
			while (reader.at_diff_opcode()) {
				DiffOpcode op;
				if (!reader.read_diff_opcode(op)) return counts;
			}
		}
	}
	return counts;
}

} // namespace

// A chunk peppered with edits encodes larger as a diff than as the literal
// bytes. Emitting the diff anyway let a delta exceed the size of the file it
// describes, so such a chunk must be stored as ADDED instead.
TEST(Apply, dense_edits_are_stored_as_a_literal_not_a_diff)
{
	const std::string OLD = tpath("apply_t_dense_old");
	const std::string NEW = tpath("apply_t_dense_new");
	const std::string DELTA = tpath("apply_t_dense_delta");
	const std::string OUT = tpath("apply_t_dense_out");

	// 256 bytes stays one chunk (below MIN_CHUNK_SIZE), so boundaries cannot
	// shift and the entry choice is purely the diff-vs-literal decision.
	std::vector<uint8_t> data(256, 0xAA);
	write_bytes(OLD, data);
	for (size_t i = 0; i < data.size(); i += 2)
		data[i] ^= 0xFF;  // every other byte differs
	write_bytes(NEW, data);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	const auto counts = count_entry_types(DELTA);
	EXPECT_EQ(counts.count(EntryType::MODIFIED_CHUNK), 0u)
		<< "a diff costlier than the literal must not be emitted";
	EXPECT_GT(counts.at(EntryType::ADDED_CHUNK), 0u);

	// And the delta must stay within the literal plus a bounded header overhead.
	const size_t overhead = 4 * delta_entry_header_size(BLAKE2b::HASH_SIZE) +
	                        DELTA_HEADER_SIZE + 1 + BLAKE2b::HASH_SIZE;
	EXPECT_LE(read_all(DELTA).size(), data.size() + overhead);

	cleanup({OLD, NEW, DELTA, OUT});
}

// Content present in the old file must be reused however many times it appears.
// The index records one position per distinct content, so without the fallback
// scan every copy after the first was re-sent in full.
TEST(Apply, repeated_content_is_reused_rather_than_resent)
{
	const std::string OLD = tpath("apply_t_dup_old");
	const std::string NEW = tpath("apply_t_dup_new");
	const std::string DELTA = tpath("apply_t_dup_delta");
	const std::string OUT = tpath("apply_t_dup_out");

	// Two distinct 64 KiB blocks, reordered between old and new. Every byte of
	// the new file is already present in the old one.
	std::mt19937 rng(0x9901u);
	std::vector<uint8_t> a(64 * 1024), b(64 * 1024);
	for (auto& x : a) x = static_cast<uint8_t>(rng());
	for (auto& x : b) x = static_cast<uint8_t>(rng());

	std::vector<uint8_t> old_data, new_data;
	for (int i = 0; i < 4; ++i) old_data.insert(old_data.end(), a.begin(), a.end());
	for (int i = 0; i < 4; ++i) old_data.insert(old_data.end(), b.begin(), b.end());
	for (int i = 0; i < 4; ++i) new_data.insert(new_data.end(), b.begin(), b.end());
	for (int i = 0; i < 4; ++i) new_data.insert(new_data.end(), a.begin(), a.end());
	write_bytes(OLD, old_data);
	write_bytes(NEW, new_data);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	// Reordered-only content should cost a small fraction of the file. Measured
	// at ~0.11x; assert well clear of that so the check is about reuse happening
	// at all, not about a precise ratio.
	const size_t delta_size = read_all(DELTA).size();
	EXPECT_LT(delta_size, new_data.size() / 3)
		<< "delta " << delta_size << " for a " << new_data.size()
		<< " byte reordering suggests repeated content is being re-sent";

	const auto counts = count_entry_types(DELTA);
	EXPECT_GT(counts.at(EntryType::ORIGINAL_CHUNK), 0u);

	cleanup({OLD, NEW, DELTA, OUT});
}

// Chunks with unrelated content must not pay for an exhaustive diff search:
// the Myers pass abandons early on noise-similar input and the literal guard
// ships the chunk as ADDED. This pins the outcome (representation and bounded
// size); the time saved is measured end-to-end rather than asserted here.
TEST(Apply, unrelated_content_becomes_a_bounded_literal)
{
	const std::string OLD = tpath("apply_t_unrel_old");
	const std::string NEW = tpath("apply_t_unrel_new");
	const std::string DELTA = tpath("apply_t_unrel_delta");
	const std::string OUT = tpath("apply_t_unrel_out");

	// Same length, completely different random content, several chunks' worth.
	write_random(OLD, 64 * 1024, 0xAAAA1111u);
	write_random(NEW, 64 * 1024, 0xBBBB2222u);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	// Everything is unrelated, so the delta is essentially the new file plus
	// bounded per-entry metadata. With ~8 KiB average chunks the byte overhead
	// is a few percent; assert a generous 10% so the test tracks the invariant
	// rather than the chunker's tuning.
	const size_t delta_size = read_all(DELTA).size();
	const size_t file_size = read_all(NEW).size();
	EXPECT_LT(delta_size, file_size + file_size / 10)
		<< "delta for unrelated content should be near the file size, got "
		<< delta_size << " for " << file_size;

	cleanup({OLD, NEW, DELTA, OUT});
}
