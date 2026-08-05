// Fuzz target: hostile delta streams through `apply` and `view`.
//
// Both subcommands parse fully attacker-controlled input, so this target treats
// the fuzz input as a candidate delta and drives both readers over it. Beyond
// "must not crash", it checks two properties the implementation promises and
// that a plain crash-only target would not notice:
//
//   1. A failed apply leaves no output file. Apply removes its own stub on
//      failure so a partial reconstruction cannot be mistaken for a complete
//      one; an exception escaping the cleanup would break that silently.
//   2. A successful apply is deterministic. Applying the same delta twice must
//      produce identical bytes, which catches state that leaks between runs.

#include "Apply.hpp"
#include "DeltaViewer.hpp"
#include "FuzzWorkspace.hpp"
#include "RK_finger.hpp"
#include "blake2b.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {

// A fixed old file, so a failure reproduces from the input alone. Content is
// deliberately compressible-but-not-constant and spans several chunks, giving
// the applier real chunks to match ORIGINAL and MODIFIED entries against.
std::vector<uint8_t> make_old_content()
{
	std::vector<uint8_t> data(48u * 1024u);
	uint32_t state = 0x1234567u;
	for (size_t i = 0; i < data.size(); ++i) {
		state = state * 1103515245u + 12345u;
		data[i] = static_cast<uint8_t>((state >> 16) ^ (i & 0x3F));
	}
	return data;
}

struct Fixture {
	fuzzing::Workspace workspace;
	std::string old_path = workspace.path("old.bin");
	std::string delta_path = workspace.path("input.delta");
	std::string out_path = workspace.path("out.bin");
	std::string out_again_path = workspace.path("out2.bin");

	Fixture() { fuzzing::Workspace::write(old_path, make_old_content()); }
};

Fixture& fixture()
{
	static Fixture instance;
	return instance;
}

[[noreturn]] void fail(const char* what)
{
	std::cerr << "fuzz invariant violated: " << what << std::endl;
	std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	Fixture& fx = fixture();
	const std::span<const uint8_t> input{data, size};

	fuzzing::Workspace::write(fx.delta_path, input);
	fuzzing::Workspace::remove(fx.out_path);
	fuzzing::Workspace::remove(fx.out_again_path);

	{
		// view must survive any input; its return value is not constrained.
		fuzzing::SuppressStdout quiet;
		(void) view_delta(fx.delta_path);
	}

	Apply<RKFinger, BLAKE2b> apply;
	const auto result = apply.apply_delta(fx.old_path, fx.delta_path, fx.out_path);

	if (!result.has_value()) {
		if (fuzzing::Workspace::exists(fx.out_path))
			fail("failed apply left an output file behind");
		return 0;
	}

	const auto first = fuzzing::Workspace::read(fx.out_path);
	if (first.size() != result->bytes_written)
		fail("reported bytes_written disagrees with the output file size");

	Apply<RKFinger, BLAKE2b> again;
	const auto repeat = again.apply_delta(fx.old_path, fx.delta_path, fx.out_again_path);
	if (!repeat.has_value())
		fail("apply succeeded once and then failed on the same input");
	if (fuzzing::Workspace::read(fx.out_again_path) != first)
		fail("apply is not deterministic for the same delta");

	return 0;
}
