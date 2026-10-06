#ifndef TESTWORKSPACE_HPP
#define TESTWORKSPACE_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace testfiles {

class Workspace {
public:
	Workspace() {
		std::random_device random;
		for (unsigned attempt = 0; attempt < 32; ++attempt) {
			auto candidate = std::filesystem::temp_directory_path() /
				("rh_test_" + std::to_string(random()) + "_" + std::to_string(random()));
			if (std::filesystem::create_directory(candidate)) {
				root_ = std::move(candidate);
				return;
			}
		}
		throw std::runtime_error("Cannot create test workspace");
	}
	~Workspace() {
		std::error_code ec;
		std::filesystem::remove_all(root_, ec);
	}
	Workspace(const Workspace&) = delete;
	Workspace& operator=(const Workspace&) = delete;
	const std::filesystem::path& path() const noexcept { return root_; }
private:
	std::filesystem::path root_;
};

inline const std::filesystem::path& directory() {
	static Workspace workspace;
	return workspace.path();
}

inline void write(const std::filesystem::path& path, std::span<const uint8_t> data) {
	std::ofstream f(path, std::ios::binary);
	f.exceptions(std::ios::badbit | std::ios::failbit);
	if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	f.close();
}

inline std::vector<uint8_t> read(const std::filesystem::path& path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	f.exceptions(std::ios::badbit | std::ios::failbit);
	const auto size = f.tellg();
	if (size < 0) throw std::runtime_error("Cannot size test input");
	f.seekg(0);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	if (!data.empty()) f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
	return data;
}

} // namespace testfiles
#endif
