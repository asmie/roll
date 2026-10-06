#include "Delta.hpp"
#include "FuzzWorkspace.hpp"
#include "RK_finger.hpp"
#include "SeedData.hpp"
#include "blake2b.h"

#include <array>
#include <iostream>

int main(int argc, char** argv)
{
	if (argc != 2) { std::cerr << "usage: fuzz_make_corpus <directory>\n"; return 2; }
	try {
		const auto root = std::filesystem::path(argv[1]);
		std::filesystem::create_directories(root / "apply");
		std::filesystem::create_directories(root / "roundtrip");
		fuzzing::Workspace workspace;
		const auto old = fuzzing::make_old_content();
		const auto old_path = workspace.path("old");
		const auto new_path = workspace.path("new");
		fuzzing::Workspace::write(old_path, old);
		Signature<RKFinger, BLAKE2b> os;
		if (!os.generate_signatures(old_path)) throw std::runtime_error("Cannot sign corpus old file");
		std::vector<std::vector<uint8_t>> variants{old, {}, std::vector<uint8_t>(1024, 'A'), old, old, old, old};
		variants[3][1024] ^= 1;
		variants[4].insert(variants[4].begin(), {'n','e','w'});
		variants[5].erase(variants[5].begin(), variants[5].begin() + 128);
		variants[6].insert(variants[6].end(), old.begin(), old.end());
		for (size_t i = 0; i < variants.size(); ++i) {
			fuzzing::Workspace::write(new_path, variants[i]);
			Signature<RKFinger, BLAKE2b> ns;
			if (!ns.generate_signatures(new_path, WholeFileHash::Compute)) throw std::runtime_error("Cannot sign corpus new file");
			Delta<RKFinger, BLAKE2b> generator;
			if (!generator.generate_delta(os, ns, old_path, new_path, root / "apply" / (std::to_string(i) + ".delta")))
				throw std::runtime_error("Cannot generate corpus delta");
		}
		const std::vector<uint8_t> base(old.begin(), old.begin() + 4096);
		std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> pairs{
			{{}, {}}, {{}, base}, {base, {}}, {base, base}, {base, base}, {base, base},
			{std::vector<uint8_t>(512, 'A'), std::vector<uint8_t>(512, 'B')}};
		pairs[4].second[1024] ^= 1;
		pairs[5].second.insert(pairs[5].second.begin(), 'X');
		for (size_t i = 0; i < pairs.size(); ++i) {
			const auto& [a, b] = pairs[i];
			std::vector<uint8_t> encoded{static_cast<uint8_t>(a.size() >> 8), static_cast<uint8_t>(a.size())};
			encoded.insert(encoded.end(), a.begin(), a.end());
			encoded.insert(encoded.end(), b.begin(), b.end());
			fuzzing::Workspace::write((root / "roundtrip" / std::to_string(i)).string(), encoded);
		}
		std::cout << "Generated 7 apply and 7 round-trip seeds in " << root << '\n';
	} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
