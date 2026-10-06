// Fuzz target: the whole create/apply pipeline must round-trip exactly.
//
// The parser target checks robustness against hostile deltas. This one checks
// correctness on well-formed ones: the fuzz input is split into an old and a new
// file, a delta is generated between them, and the reconstruction must equal the
// new file byte for byte. Any disagreement between the chunker, the diff, and
// the applier shows up here — boundary handling, opcode emission, entry
// selection, the literal-versus-diff decision, duplicate-content reuse — none of
// which a delta-parsing target can reach, because it never generates a delta.
//
// The split point is taken from the input so the fuzzer can steer it, which lets
// it reach the interesting shapes on its own: empty halves, a one-byte file, a
// sub-window file, and old/new pairs that share long runs.

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaViewer.hpp"
#include "FuzzWorkspace.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake2b.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {

struct Fixture {
	fuzzing::Workspace workspace;
	std::string old_path = workspace.path("old.bin");
	std::string new_path = workspace.path("new.bin");
	std::string delta_path = workspace.path("gen.delta");
	std::string out_path = workspace.path("out.bin");
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
	// Two leading bytes steer the split; the rest is content.
	if (size < 2)
		return 0;

	const size_t body = size - 2;
	const size_t split = ((static_cast<size_t>(data[0]) << 8) | data[1]) % (body + 1);
	const std::span<const uint8_t> content{data + 2, body};

	Fixture& fx = fixture();
	fuzzing::Workspace::write(fx.old_path, content.first(split));
	fuzzing::Workspace::write(fx.new_path, content.subspan(split));
	fuzzing::Workspace::remove(fx.delta_path);
	fuzzing::Workspace::remove(fx.out_path);

	Signature<RKFinger, BLAKE2b> old_sig, new_sig;
	if (!old_sig.generate_signatures(fx.old_path))
		fail("signature generation failed on a file that was just written");
	if (!new_sig.generate_signatures(fx.new_path, WholeFileHash::Compute))
		fail("signature generation failed on a file that was just written");

	Delta<RKFinger, BLAKE2b> delta;
	const auto generated = delta.generate_delta(old_sig, new_sig, fx.old_path,
	                                            fx.new_path, fx.delta_path);
	if (!generated.has_value())
		fail("delta generation failed for two readable files");

	if (generated->bytes_written != fuzzing::Workspace::read(fx.delta_path).size())
		fail("reported delta bytes disagree with the delta file size");

	{
		fuzzing::SuppressStdout quiet;
		if (view_delta(fx.delta_path) != 0) fail("view rejected a generated delta");
	}
	Apply<RKFinger, BLAKE2b> apply;
	const auto applied = apply.apply_delta(fx.old_path, fx.delta_path, fx.out_path);
	if (!applied.has_value())
		fail("a freshly generated delta failed to apply");

	if (fuzzing::Workspace::read(fx.out_path) != fuzzing::Workspace::read(fx.new_path))
		fail("round trip did not reproduce the new file");

	return 0;
}
