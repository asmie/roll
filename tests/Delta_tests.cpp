#include "TestWorkspace.hpp"
#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaViewer.hpp"
#include "OutputTransaction.hpp"
#include "RK_finger.hpp"
#include "blake2b.h"
#include "gtest/gtest.h"

#include <array>
#include <random>

namespace {
using Sig = Signature<RKFinger, BLAKE2b>;
using Generator = Delta<RKFinger, BLAKE2b>;
using Applier = Apply<RKFinger, BLAKE2b>;

struct Files {
	testfiles::Workspace workspace;
	std::filesystem::path old = workspace.path() / "old";
	std::filesystem::path fresh = workspace.path() / "new";
	std::filesystem::path delta = workspace.path() / "delta";
	std::filesystem::path out = workspace.path() / "out";
	std::vector<uint8_t> bytes = std::vector<uint8_t>(1024, 'A');
	Sig os, ns;
	void prepare() {
		testfiles::write(old, bytes);
		testfiles::write(fresh, bytes);
		ASSERT_TRUE(os.generate_signatures(old));
		ASSERT_TRUE(ns.generate_signatures(fresh, WholeFileHash::Compute));
	}
	void generate() {
		Generator generator;
		ASSERT_TRUE(generator.generate_delta(os, ns, old, fresh, delta).has_value());
	}
};

class OutputAlias : public testing::TestWithParam<int> {};

TEST_P(OutputAlias, create_and_apply_preserve_aliased_inputs)
{
	Files f;
	f.prepare();
	f.generate();
	const int parameter = GetParam();
	const bool creating = parameter < 6;
	const auto input = creating ? (parameter % 6 < 3 ? f.old : f.fresh)
	                            : (parameter % 6 < 3 ? f.old : f.delta);
	const auto before = testfiles::read(input);
	auto target = input;
	const int link = parameter % 3;
	std::error_code ec;
	if (link != 0) {
		target = f.out;
		if (link == 1) std::filesystem::create_symlink(input, target, ec);
		else std::filesystem::create_hard_link(input, target, ec);
		if (ec == std::errc::operation_not_permitted || ec == std::errc::permission_denied)
			GTEST_SKIP() << "Link creation unavailable: " << ec.message();
		ASSERT_FALSE(ec) << ec.message();
	}
	if (creating) {
		Generator generator;
		const auto result = generator.generate_delta(f.os, f.ns, f.old, f.fresh, target);
		ASSERT_FALSE(result.has_value());
		EXPECT_EQ(result.error().code, DeltaErrc::invalid_argument);
	} else {
		Applier applier;
		const auto result = applier.apply_delta(f.old, f.delta, target);
		ASSERT_FALSE(result.has_value());
		EXPECT_EQ(result.error().code, DeltaErrc::invalid_argument);
	}
	EXPECT_EQ(testfiles::read(input), before);
}

INSTANTIATE_TEST_SUITE_P(Paths, OutputAlias, testing::Range(0, 12));

TEST(Delta, literal_wins_when_modified_record_has_equal_cost)
{
	Files f;
	f.bytes.assign(8, 'A');
	f.prepare();
	auto changed = f.bytes;
	changed[0] = 'B';
	testfiles::write(f.fresh, changed);
	ASSERT_TRUE(f.ns.generate_signatures(f.fresh, WholeFileHash::Compute));
	f.generate();
	FileIO input;
	ASSERT_TRUE(input.open(f.delta, FileMode::IN));
	DeltaReader reader(input, BLAKE2b{}.get_hash_size());
	ASSERT_TRUE(reader.read_header());
	DeltaEntryHeader entry;
	ASSERT_TRUE(reader.read_entry_header(entry));
	// A one-byte replacement needs seven opcode bytes, plus a source index.
	EXPECT_EQ(entry.type, EntryType::ADDED_CHUNK);
	Applier applier;
	ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
	EXPECT_EQ(testfiles::read(f.out), changed);
}

TEST(Delta, rejects_changed_inputs_and_preserves_existing_delta)
{
	for (int which = 0; which < 2; ++which) {
		for (int mutation = 0; mutation < 4; ++mutation) {
			SCOPED_TRACE(which * 4 + mutation);
			Files f;
			f.prepare();
			const std::vector<uint8_t> sentinel{'k','e','e','p'};
			testfiles::write(f.delta, sentinel);
			const auto input = which == 0 ? f.old : f.fresh;
			auto data = f.bytes;
			if (mutation == 0) data.clear();
			else if (mutation == 1) data[0] ^= 1;
			else if (mutation == 2) data.push_back('B');
			else {
				std::filesystem::rename(input, input.string() + ".saved");
				data[0] ^= 1;
			}
			testfiles::write(input, data);
			Generator generator;
			const auto result = generator.generate_delta(f.os, f.ns, f.old, f.fresh, f.delta);
			if (which == 1 && data.size() == f.bytes.size()) {
				// Reused chunks are emitted from the signature without reading
				// the new file, so the delta reproduces the signed content.
				ASSERT_TRUE(result.has_value());
				Applier applier;
				ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
				EXPECT_EQ(testfiles::read(f.out), f.bytes);
				continue;
			}
			ASSERT_FALSE(result.has_value());
			EXPECT_EQ(testfiles::read(f.delta), sentinel);
			for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path()))
				EXPECT_FALSE(entry.path().filename().string().starts_with(".rolling_hash-"));
		}
	}
}

TEST(Delta, rejects_changes_to_new_bytes_it_emits)
{
	Files f;
	f.prepare();
	std::vector<uint8_t> changed(f.bytes.size(), 'B');
	testfiles::write(f.fresh, changed);
	ASSERT_TRUE(f.ns.generate_signatures(f.fresh, WholeFileHash::Compute));
	changed[0] = 'C';
	testfiles::write(f.fresh, changed);
	Generator generator;
	const auto result = generator.generate_delta(f.os, f.ns, f.old, f.fresh, f.delta);
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error().code, DeltaErrc::integrity_mismatch);
	EXPECT_FALSE(std::filesystem::exists(f.delta));
}

TEST(Delta, rejects_changes_to_unreferenced_old_chunks)
{
	Files f;
	f.bytes.assign(DELTA_MAX_CHUNK_SIZE, 'A');
	f.bytes.insert(f.bytes.end(), DELTA_MAX_CHUNK_SIZE, 'B');
	f.prepare();
	ASSERT_EQ(f.os.get_chunks().size(), 2u);
	const std::vector<uint8_t> newBytes(DELTA_MAX_CHUNK_SIZE, 'B');
	testfiles::write(f.fresh, newBytes);
	ASSERT_TRUE(f.ns.generate_signatures(f.fresh, WholeFileHash::Compute));
	// Only old chunk 1 is reused. Mutating chunk 0 can move its index when
	// the applier re-chunks the old file, even though chunk 1's bytes survive.
	std::mt19937 rng(20260907);
	for (size_t i = 0; i < DELTA_MAX_CHUNK_SIZE; ++i)
		f.bytes[i] = static_cast<uint8_t>(rng());
	testfiles::write(f.old, f.bytes);
	Sig changed;
	ASSERT_TRUE(changed.generate_signatures(f.old));
	ASSERT_NE(changed.get_chunks().front().chunk_size, f.os.get_chunks().front().chunk_size);
	const std::vector<uint8_t> sentinel{'k','e','e','p'};
	testfiles::write(f.delta, sentinel);
	Generator generator;
	EXPECT_FALSE(generator.generate_delta(f.os, f.ns, f.old, f.fresh, f.delta).has_value());
	EXPECT_EQ(testfiles::read(f.delta), sentinel);
}

TEST(Apply, failures_preserve_existing_output_and_success_replaces_it)
{
	Files f;
	f.prepare();
	f.generate();
	const auto valid = testfiles::read(f.delta);
	const std::vector<uint8_t> sentinel{'k','e','e','p'};
	Applier applier;
	for (size_t byte : {size_t{0}, valid.size() - 1}) {
		auto broken = valid;
		broken[byte] ^= 1;
		testfiles::write(f.delta, broken);
		testfiles::write(f.out, sentinel);
		ASSERT_FALSE(applier.apply_delta(f.old, f.delta, f.out).has_value());
		EXPECT_EQ(testfiles::read(f.out), sentinel);
	}
	testfiles::write(f.delta, valid);
	ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
	EXPECT_EQ(testfiles::read(f.out), f.bytes);
}

#ifndef _WIN32
TEST(Apply, replacing_a_destination_keeps_its_permissions)
{
	Files f;
	f.prepare();
	f.generate();
	testfiles::write(f.out, std::vector<uint8_t>{'x'});
	const auto mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
	std::filesystem::permissions(f.out, mode);
	Applier applier;
	ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
	EXPECT_EQ(std::filesystem::status(f.out).permissions(), mode);
	EXPECT_EQ(testfiles::read(f.out), f.bytes);
}
#endif

TEST(OutputTransaction, exception_and_commit_error_discard_only_the_temporary)
{
	Files f;
	f.prepare();
	testfiles::write(f.out, f.bytes);
	try {
		OutputTransaction transaction;
		ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
		ASSERT_TRUE(transaction.file().write_byte('X'));
		throw std::runtime_error("injected failure");
	} catch (const std::runtime_error&) {}
	EXPECT_EQ(testfiles::read(f.out), f.bytes);
	{
		OutputTransaction transaction;
		ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
		// An illegal read puts the write-only handle into a sticky I/O error.
		EXPECT_EQ(transaction.file().read_byte(), EOF);
		ASSERT_FALSE(transaction.commit().has_value());
		ASSERT_FALSE(transaction.commit().has_value());
	}
	EXPECT_EQ(testfiles::read(f.out), f.bytes);
	for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path()))
		EXPECT_FALSE(entry.path().filename().string().starts_with(".rolling_hash-"));
}

TEST(Apply, replacing_an_unrelated_link_preserves_its_former_target)
{
	for (bool symbolic : {false, true}) {
		Files f;
		f.prepare();
		f.generate();
		const std::vector<uint8_t> sentinel{'k','e','e','p'};
		testfiles::write(f.fresh, sentinel);
		std::error_code ec;
		if (symbolic) std::filesystem::create_symlink(f.fresh, f.out, ec);
		else std::filesystem::create_hard_link(f.fresh, f.out, ec);
		if (ec == std::errc::operation_not_permitted || ec == std::errc::permission_denied)
			continue; // Platforms may restrict unprivileged symlink creation.
		ASSERT_FALSE(ec) << ec.message();
		Applier applier;
		const auto valid = testfiles::read(f.delta);
		auto invalid = valid;
		if (invalid.empty()) FAIL() << "Expected a nonempty generated delta";
		invalid.back() ^= 1;
		testfiles::write(f.delta, invalid);
		ASSERT_FALSE(applier.apply_delta(f.old, f.delta, f.out).has_value());
		EXPECT_EQ(testfiles::read(f.out), sentinel);
		EXPECT_EQ(testfiles::read(f.fresh), sentinel);
		testfiles::write(f.delta, valid);
		ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
		EXPECT_EQ(testfiles::read(f.out), f.bytes);
		EXPECT_EQ(testfiles::read(f.fresh), sentinel);
	}
}

TEST(OutputTransaction, rename_failure_preserves_destination)
{
	Files f;
	f.prepare();
	{
		OutputTransaction transaction;
		ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
		ASSERT_TRUE(transaction.file().write_byte('X'));
		std::filesystem::create_directory(f.out);
		ASSERT_FALSE(transaction.commit().has_value());
		EXPECT_TRUE(std::filesystem::is_directory(f.out));
	}
	for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path()))
		EXPECT_FALSE(entry.path().filename().string().starts_with(".rolling_hash-"));
}

TEST(Delta, directory_inputs_are_errors)
{
	Files f;
	f.prepare();
	Sig sig;
	EXPECT_FALSE(sig.generate_signatures(f.workspace.path(), WholeFileHash::Compute));
	EXPECT_TRUE(sig.get_chunks().empty());
	EXPECT_TRUE(sig.whole_file_hash().empty());
	Generator generator;
	const auto result = generator.generate_delta(f.os, sig, f.old, f.workspace.path(), f.delta);
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error().code, DeltaErrc::io_error);
	EXPECT_FALSE(std::filesystem::exists(f.delta));
}

TEST(DeltaReaders, reject_incomplete_streams_and_trailing_bytes)
{
	Files f;
	f.prepare();
	f.generate();
	const auto valid = testfiles::read(f.delta);
	std::vector<std::vector<uint8_t>> malformed;
	malformed.emplace_back(valid.begin(), valid.begin() + DELTA_HEADER_SIZE);
	malformed.emplace_back(valid.begin(), valid.end() - 65);
	malformed.push_back(valid);
	malformed.back().push_back('X');
	auto no_ops = std::vector<uint8_t>(valid.begin(), valid.begin() + DELTA_HEADER_SIZE);
	no_ops.push_back(static_cast<uint8_t>(EntryType::MODIFIED_CHUNK));
	push_varint(no_ops, 0);
	push_varint(no_ops, f.bytes.size());
	no_ops.insert(no_ops.end(), f.os.get_chunks()[0].hash.begin(), f.os.get_chunks()[0].hash.end());
	no_ops.insert(no_ops.end(), valid.end() - 65, valid.end());
	malformed.push_back(no_ops);
	for (const auto& raw : malformed) {
		testfiles::write(f.delta, raw);
		Applier applier;
		const auto result = applier.apply_delta(f.old, f.delta, f.out);
		ASSERT_FALSE(result.has_value());
		EXPECT_EQ(result.error().code, DeltaErrc::corrupt_delta);
		EXPECT_EQ(view_delta(f.delta), 1);
		EXPECT_FALSE(std::filesystem::exists(f.out));
	}
}

TEST(Delta, v5_golden_bytes_match_an_independent_encoding)
{
	Files f;
	const std::vector<uint8_t> old{'o','l','d'}, fresh{'n','e','w'};
	testfiles::write(f.old, old);
	testfiles::write(f.fresh, fresh);
	ASSERT_TRUE(f.os.generate_signatures(f.old));
	ASSERT_TRUE(f.ns.generate_signatures(f.fresh));
	f.generate();
	// Frozen v5 wire bytes, including BLAKE2b-512 digests calculated independently.
	const std::vector<uint8_t> golden{
		0x52, 0x48, 0x44, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01, 0x03, 0xd1, 0x83,
		0x51, 0x2b, 0xf0, 0x0d, 0x54, 0x54, 0x9b, 0x15, 0xa5, 0x88, 0xfc, 0xf0,
		0x51, 0xfc, 0x6e, 0x65, 0x77, 0xff, 0xd1, 0x83, 0x51, 0x2b, 0xf0, 0x0d,
		0x54, 0x54, 0x9b, 0x15, 0xa5, 0x88, 0xfc, 0xf0, 0x51, 0xfc, 0xe2, 0x3d,
		0x10, 0x21, 0x7b, 0x64, 0x22, 0x94, 0xf8, 0xeb, 0x89, 0x22, 0x84, 0x1f,
		0x1c, 0x0f, 0xfa, 0x68, 0x47, 0xf6, 0x08, 0xb5, 0x13, 0x49, 0xd4, 0x62,
		0x76, 0xeb, 0xdd, 0xc8, 0xd9, 0x9b, 0x05, 0xb7, 0xfb, 0xe3, 0x82, 0xee,
		0x57, 0x7f, 0x29, 0xe6, 0x73, 0x84, 0x83, 0x24, 0xc5, 0x98
	};
	EXPECT_EQ(testfiles::read(f.delta), golden);
	testfiles::write(f.delta, golden);
	Applier applier;
	ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out).has_value());
	EXPECT_EQ(testfiles::read(f.out), fresh);
}

} // namespace

TEST(Apply, output_limit_fails_cleanly_and_an_exact_limit_succeeds)
{
	Files f;
	f.prepare();
	f.generate();
	const std::vector<uint8_t> sentinel{'k','e','e','p'};
	testfiles::write(f.out, sentinel);
	Applier applier;
	const auto limited = applier.apply_delta(f.old, f.delta, f.out, f.bytes.size() - 1);
	ASSERT_FALSE(limited.has_value());
	EXPECT_EQ(limited.error().code, DeltaErrc::limit_exceeded);
	EXPECT_EQ(testfiles::read(f.out), sentinel);
	for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path()))
		EXPECT_FALSE(entry.path().filename().string().starts_with(".rolling_hash-"));
	ASSERT_TRUE(applier.apply_delta(f.old, f.delta, f.out, f.bytes.size()).has_value());
	EXPECT_EQ(testfiles::read(f.out), f.bytes);
}

#ifndef _WIN32
TEST(OutputTransaction, signal_cleanup_removes_only_staged_files)
{
	Files f;
	f.prepare();
	const auto staged = [&] {
		size_t count = 0;
		for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path()))
			count += entry.path().filename().string().starts_with(".rolling_hash-");
		return count;
	};
	{
		OutputTransaction transaction;
		ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
		ASSERT_TRUE(transaction.file().write_byte('X'));
		ASSERT_EQ(staged(), 1u);
		OutputTransaction::discard_all_staged();
		EXPECT_EQ(staged(), 0u);
	}
	{
		OutputTransaction transaction;
		ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
		ASSERT_TRUE(transaction.file().write_byte('Y'));
		ASSERT_TRUE(transaction.commit().has_value());
	}
	// A committed transaction no longer names anything to discard.
	OutputTransaction::discard_all_staged();
	EXPECT_EQ(testfiles::read(f.out), std::vector<uint8_t>{'Y'});
	EXPECT_EQ(testfiles::read(f.old), f.bytes);
}

TEST(OutputTransaction, private_destination_is_never_staged_readable)
{
	Files f;
	f.prepare();
	testfiles::write(f.out, std::vector<uint8_t>{'x'});
	const auto mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
	std::filesystem::permissions(f.out, mode);
	OutputTransaction transaction;
	ASSERT_TRUE(transaction.open(f.out, {f.old}).has_value());
	for (const auto& entry : std::filesystem::directory_iterator(f.workspace.path())) {
		if (entry.path().filename().string().starts_with(".rolling_hash-")) {
			EXPECT_EQ(entry.status().permissions(), mode);
		}
	}
}
#endif
