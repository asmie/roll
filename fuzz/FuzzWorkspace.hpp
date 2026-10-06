#ifndef FUZZWORKSPACE_HPP
#define FUZZWORKSPACE_HPP

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <stdexcept>
#include <streambuf>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace fuzzing {

/**
* Per-process scratch directory for fuzz targets.
*
* The delta APIs take paths rather than buffers, so a target has to materialise
* its input on disk. One directory per process keeps parallel fuzzer workers
* from colliding, and removing it in the destructor keeps a long run from
* filling the temp filesystem. Paths are stable across iterations so the cost
* is a rewrite rather than a create/unlink pair.
*/
class Workspace {
public:
	Workspace()
	{
		std::random_device random;
		for (unsigned attempt = 0; attempt < 32; ++attempt) {
			auto candidate = std::filesystem::temp_directory_path() /
				("rh_fuzz_" + std::to_string(random()) + "_" + std::to_string(random()));
			if (std::filesystem::create_directory(candidate)) {
				root_ = std::move(candidate);
				return;
			}
		}
		throw std::runtime_error("Cannot create fuzz workspace");
	}

	~Workspace()
	{
		std::error_code ec;
		std::filesystem::remove_all(root_, ec);
	}

	Workspace(const Workspace&) = delete;
	Workspace& operator=(const Workspace&) = delete;

	std::string path(const char* name) const { return (root_ / name).string(); }

	static void write(const std::string& path, std::span<const uint8_t> bytes)
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f.exceptions(std::ios::badbit | std::ios::failbit);
		if (!bytes.empty())
			f.write(reinterpret_cast<const char*>(bytes.data()),
			        static_cast<std::streamsize>(bytes.size()));
		f.close();
	}

	static std::vector<uint8_t> read(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary | std::ios::ate);
		f.exceptions(std::ios::badbit | std::ios::failbit);
		const auto size = f.tellg();
		if (size < 0) throw std::runtime_error("Cannot size fuzz input");
		if (size == 0) return {};
		f.seekg(0);
		std::vector<uint8_t> buf(static_cast<size_t>(size));
		f.read(reinterpret_cast<char*>(buf.data()),
		       static_cast<std::streamsize>(buf.size()));
		return buf;
	}

	static bool exists(const std::string& path)
	{
		return std::filesystem::exists(path);
	}

	static void remove(const std::string& path)
	{
		std::filesystem::remove(path);
	}

private:
	std::filesystem::path root_;
};

/// Silence stdout for the duration of the scope. `view` prints a full report per
/// input, which would otherwise dominate a fuzzing run's output and its runtime.
class SuppressStdout {
	class Sink : public std::streambuf {
		int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
		std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
	};
public:
	SuppressStdout() : saved_(std::cout.rdbuf(&sink_)), errors_(std::cerr.rdbuf(&sink_)) {}
	~SuppressStdout() { std::cout.rdbuf(saved_); std::cerr.rdbuf(errors_); }
	SuppressStdout(const SuppressStdout&) = delete;
	SuppressStdout& operator=(const SuppressStdout&) = delete;
private:
	Sink sink_;
	std::streambuf* saved_;
	std::streambuf* errors_;
};

} // namespace fuzzing

#endif // FUZZWORKSPACE_HPP
