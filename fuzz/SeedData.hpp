#ifndef FUZZ_SEEDDATA_HPP
#define FUZZ_SEEDDATA_HPP
#include <cstdint>
#include <cstddef>
#include <vector>
namespace fuzzing {
// A fixed old file, so a failure reproduces from the input alone. Content is
// deliberately compressible-but-not-constant and spans several chunks, giving
// the applier real chunks to match ORIGINAL and MODIFIED entries against.
inline std::vector<uint8_t> make_old_content()
{
	std::vector<uint8_t> data(48u * 1024u);
	uint32_t state = 0x1234567u;
	for (size_t i = 0; i < data.size(); ++i) {
		state = state * 1103515245u + 12345u;
		data[i] = static_cast<uint8_t>((state >> 16) ^ (i & 0x3F));
	}
	return data;
}

}
#endif
