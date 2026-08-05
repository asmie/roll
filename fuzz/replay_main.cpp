// Standalone driver for the fuzz targets.
//
// libFuzzer needs Clang, and this project's std::expected requirement means
// Clang also needs libc++ — a combination not every environment has. Linking the
// same LLVMFuzzerTestOneInput against this driver gives a plain executable that
// replays files instead of generating them, so a committed corpus and any
// recorded crash input stay runnable under the project's normal compiler and
// remain useful as regression tests in CI.
//
//   fuzz_apply_replay corpus/*            # feed each file to the target
//   fuzz_apply_replay < input             # or one input on stdin

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::vector<uint8_t> read_file(const char* path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::cerr << "cannot open " << path << std::endl;
		return {};
	}
	return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),
	                            std::istreambuf_iterator<char>{}};
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2) {
		const std::vector<uint8_t> input{std::istreambuf_iterator<char>(std::cin),
		                                 std::istreambuf_iterator<char>{}};
		return LLVMFuzzerTestOneInput(input.data(), input.size());
	}

	size_t replayed = 0;
	for (int i = 1; i < argc; ++i) {
		const auto input = read_file(argv[i]);
		// A target signals a violated invariant by aborting, so reaching the
		// next iteration means this input passed.
		(void) LLVMFuzzerTestOneInput(input.data(), input.size());
		++replayed;
	}

	std::cout << "replayed " << replayed << " input(s) with no invariant violations"
	          << std::endl;
	return 0;
}
