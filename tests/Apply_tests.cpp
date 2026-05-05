#include "gtest/gtest.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
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
	return 3 * sizeof(uint64_t) + BLAKE512().get_hash_size();
}

constexpr size_t d_opcode_size(size_t inline_count)
{
	return 1 + sizeof(uint32_t) + 1 + inline_count;
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

void cleanup(std::initializer_list<const char*> paths)
{
	for (const auto* p : paths)
		std::remove(p);
}

bool roundtrip(const std::string& old_path, const std::string& new_path,
               const std::string& delta_path, const std::string& out_path,
               std::string* err = nullptr)
{
	Signature<RKFinger, BLAKE512> old_sig, new_sig;
	old_sig.generate_signatures(old_path);
	new_sig.generate_signatures(new_path);

	Delta<RKFinger, BLAKE512> delta;
	auto dr = delta.generate_delta(old_sig, new_sig, old_path, new_path, delta_path);
	if (!dr.success) {
		if (err) *err = "delta: " + dr.error_message;
		return false;
	}

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(old_path, delta_path, out_path);
	if (!ar.success) {
		if (err) *err = "apply: " + ar.error_message;
		return false;
	}
	return true;
}

} // namespace

TEST(Apply, identical_files)
{
	const char* OLD = "apply_t_identical_old";
	const char* NEW = "apply_t_identical_new";
	const char* DELTA = "apply_t_identical_delta";
	const char* OUT = "apply_t_identical_out";

	write_random(OLD, 8192, 0xC0FFEE);
	write_random(NEW, 8192, 0xC0FFEE);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, empty_old)
{
	const char* OLD = "apply_t_empty_old_old";
	const char* NEW = "apply_t_empty_old_new";
	const char* DELTA = "apply_t_empty_old_delta";
	const char* OUT = "apply_t_empty_old_out";

	write_bytes(OLD, {});
	write_random(NEW, 4096, 0xBEEFu);

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, empty_new)
{
	const char* OLD = "apply_t_empty_new_old";
	const char* NEW = "apply_t_empty_new_new";
	const char* DELTA = "apply_t_empty_new_delta";
	const char* OUT = "apply_t_empty_new_out";

	write_random(OLD, 4096, 0xCAFEu);
	write_bytes(NEW, {});

	std::string err;
	ASSERT_TRUE(roundtrip(OLD, NEW, DELTA, OUT, &err)) << err;
	EXPECT_EQ(read_all(NEW), read_all(OUT));

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, append_only)
{
	const char* OLD = "apply_t_append_old";
	const char* NEW = "apply_t_append_new";
	const char* DELTA = "apply_t_append_delta";
	const char* OUT = "apply_t_append_out";

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
	const char* OLD = "apply_t_trunc_old";
	const char* NEW = "apply_t_trunc_new";
	const char* DELTA = "apply_t_trunc_delta";
	const char* OUT = "apply_t_trunc_out";

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
	const char* OLD = "apply_t_inplace_old";
	const char* NEW = "apply_t_inplace_new";
	const char* DELTA = "apply_t_inplace_delta";
	const char* OUT = "apply_t_inplace_out";

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

TEST(Apply, in_chunk_deletion_consumes_x_opcode)
{
	const char* OLD = "apply_t_xop_old";
	const char* NEW = "apply_t_xop_new";
	const char* DELTA = "apply_t_xop_delta";
	const char* OUT = "apply_t_xop_out";

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
	const char* OLD = "apply_t_mid_old";
	const char* NEW = "apply_t_mid_new";
	const char* DELTA = "apply_t_mid_delta";
	const char* OUT = "apply_t_mid_out";

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
	const char* OLD = "apply_t_trunc_pl_old";
	const char* NEW = "apply_t_trunc_pl_new";
	const char* DELTA = "apply_t_trunc_pl_delta";
	const char* OUT = "apply_t_trunc_pl_out";

	// Empty old + non-empty new yields an all-ADDED delta whose tail is a
	// chunk payload — chopping bytes off the end leaves a short final payload.
	write_bytes(OLD, {});
	write_random(NEW, 4096, 0x123456u);

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	// Drop a single byte: with a non-empty new file the last entry's payload
	// is always >= 1 byte, so this always lands mid-payload regardless of the
	// chunk-size distribution chosen by CDC for this seed.
	auto raw = read_all(DELTA);
	ASSERT_FALSE(raw.empty());
	raw.pop_back();
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.success);

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_modified_after_header_fails)
{
	const char* OLD = "apply_t_modtrunc1_old";
	const char* NEW = "apply_t_modtrunc1_new";
	const char* DELTA = "apply_t_modtrunc1_delta";
	const char* OUT = "apply_t_modtrunc1_out";

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

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	const size_t header_size = entry_header_size();
	auto raw = read_all(DELTA);
	ASSERT_GT(raw.size(), header_size);
	raw.resize(header_size);  // header only, no diff opcodes
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.success);

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_modified_at_opcode_boundary_fails)
{
	const char* OLD = "apply_t_modtrunc2_old";
	const char* NEW = "apply_t_modtrunc2_new";
	const char* DELTA = "apply_t_modtrunc2_delta";
	const char* OUT = "apply_t_modtrunc2_out";

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

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	// Truncate after the first 'D' opcode (count=1) but before the second.
	const size_t cut = entry_header_size() + d_opcode_size(1);
	auto raw = read_all(DELTA);
	ASSERT_GT(raw.size(), cut);
	raw.resize(cut);
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.success);

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, rejects_output_aliasing_old)
{
	const char* OLD = "apply_t_alias_old_old";
	const char* NEW = "apply_t_alias_old_new";
	const char* DELTA = "apply_t_alias_old_delta";

	write_random(OLD, 4096, 0xA1u);
	write_random(NEW, 4096, 0xA2u);

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	auto old_before = read_all(OLD);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OLD);  // output == old
	EXPECT_FALSE(ar.success);

	// Old file must be untouched: aliased open would have truncated it.
	EXPECT_EQ(read_all(OLD), old_before);

	cleanup({OLD, NEW, DELTA});
}

TEST(Apply, rejects_output_aliasing_delta)
{
	const char* OLD = "apply_t_alias_delta_old";
	const char* NEW = "apply_t_alias_delta_new";
	const char* DELTA = "apply_t_alias_delta_delta";

	write_random(OLD, 4096, 0xB1u);
	write_random(NEW, 4096, 0xB2u);

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	auto delta_before = read_all(DELTA);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, DELTA);  // output == delta
	EXPECT_FALSE(ar.success);

	// Delta file must be untouched: aliased open would have truncated it,
	// after which the apply loop would see EOF and report success silently.
	EXPECT_EQ(read_all(DELTA), delta_before);

	cleanup({OLD, NEW, DELTA});
}

TEST(Apply, duplicate_removed_entry_rejected)
{
	const char* OLD = "apply_t_dupr_old";
	const char* NEW = "apply_t_dupr_new";
	const char* DELTA = "apply_t_dupr_delta";
	const char* OUT = "apply_t_dupr_out";

	// Empty new + non-empty old yields an all-REMOVED delta. Each REMOVED
	// entry's header is exactly entry_header_size() bytes, no payload. We
	// duplicate the last entry to simulate a malformed delta that references
	// the same old chunk twice; Apply must reject this even though the chunk
	// content does exist in old (chunk_map.find would succeed).
	write_random(OLD, 4096, 0xD1u);
	write_bytes(NEW, {});

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	const size_t header_size = entry_header_size();
	auto raw = read_all(DELTA);
	ASSERT_GE(raw.size(), header_size);
	// Append a copy of the last REMOVED entry — duplicate consumption attempt.
	raw.insert(raw.end(), raw.end() - header_size, raw.end());
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.success);

	cleanup({OLD, NEW, DELTA, OUT});
}

TEST(Apply, truncated_partial_header_fails)
{
	const char* OLD = "apply_t_trunc_hdr_old";
	const char* NEW = "apply_t_trunc_hdr_new";
	const char* DELTA = "apply_t_trunc_hdr_delta";
	const char* OUT = "apply_t_trunc_hdr_out";

	// Empty new + non-empty old yields an all-REMOVED delta with no payloads,
	// so a short trailing fragment is unambiguously an incomplete next header.
	write_random(OLD, 4096, 0x99EEu);
	write_bytes(NEW, {});

	Signature<RKFinger, BLAKE512> os, ns;
	os.generate_signatures(OLD);
	ns.generate_signatures(NEW);
	Delta<RKFinger, BLAKE512> d;
	auto dr = d.generate_delta(os, ns, OLD, NEW, DELTA);
	ASSERT_TRUE(dr.success);

	auto raw = read_all(DELTA);
	raw.push_back(0x00);
	raw.push_back(0x01);
	raw.push_back(0x02);
	raw.push_back(0x03);
	write_bytes(DELTA, raw);

	Apply<RKFinger, BLAKE512> apply;
	auto ar = apply.apply_delta(OLD, DELTA, OUT);
	EXPECT_FALSE(ar.success);

	cleanup({OLD, NEW, DELTA, OUT});
}
