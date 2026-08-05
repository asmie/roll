#ifndef FUZZWORKSPACE_HPP
#define FUZZWORKSPACE_HPP

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <unistd.h>
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
		std::error_code ec;
		root_ = std::filesystem::temp_directory_path() /
		        ("rh_fuzz_" + std::to_string(static_cast<long>(::getpid())));
		std::filesystem::create_directories(root_, ec);
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
		if (!bytes.empty())
			f.write(reinterpret_cast<const char*>(bytes.data()),
			        static_cast<std::streamsize>(bytes.size()));
	}

	static std::vector<uint8_t> read(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary | std::ios::ate);
		if (!f) return {};
		const auto size = f.tellg();
		if (size <= 0) return {};
		f.seekg(0);
		std::vector<uint8_t> buf(static_cast<size_t>(size));
		f.read(reinterpret_cast<char*>(buf.data()),
		       static_cast<std::streamsize>(buf.size()));
		return buf;
	}

	static bool exists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::exists(path, ec);
	}

	static void remove(const std::string& path)
	{
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

private:
	std::filesystem::path root_;
};

/// Silence stdout for the duration of the scope. `view` prints a full report per
/// input, which would otherwise dominate a fuzzing run's output and its runtime.
class SuppressStdout {
public:
	SuppressStdout() : sink_("/dev/null", std::ios::out), saved_(std::cout.rdbuf(sink_.rdbuf())) {}
	~SuppressStdout() { std::cout.rdbuf(saved_); }

	SuppressStdout(const SuppressStdout&) = delete;
	SuppressStdout& operator=(const SuppressStdout&) = delete;

private:
	std::ofstream sink_;
	std::streambuf* saved_;
};

} // namespace fuzzing

#endif // FUZZWORKSPACE_HPP
