#include "OutputTransaction.hpp"

#include <array>
#include <atomic>
#include <random>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32
// Staged paths for discard_all_staged(). The slots are lock-free, so a signal
// handler may read them. A transaction that finds no free slot still works;
// it is only not removed on a signal.
std::array<std::atomic<const char*>, 16> staged_paths{};
static_assert(std::atomic<const char*>::is_always_lock_free);
#endif

// Flush the file's data to stable storage. This runs before the rename, so a
// failure still leaves the destination untouched.
bool sync_file(const std::filesystem::path& path)
{
#ifdef _WIN32
	const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
	                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return false;
	const bool flushed = FlushFileBuffers(handle) != 0;
	return CloseHandle(handle) != 0 && flushed;
#else
	// The staged file carries the destination's permissions, which may allow
	// only reading or only writing; fsync works through either kind of handle.
	int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0) return false;
#ifdef F_FULLFSYNC
	// On macOS, fsync() does not flush the drive's write cache.
	const bool flushed = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
	const bool flushed = ::fsync(fd) == 0;
#endif
	return ::close(fd) == 0 && flushed;
#endif
}

// Persist the rename. Best effort: the destination is already replaced by now,
// and some filesystems cannot sync a directory.
void sync_directory([[maybe_unused]] const std::filesystem::path& directory) noexcept
{
#ifndef _WIN32
	const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		(void) ::fsync(fd);
		(void) ::close(fd);
	}
#endif
}

} // namespace

OutputTransaction::~OutputTransaction()
{
	(void) file_.close(); // A failed transaction is discarded, never published.
	if (!temporary_.empty()) {
		std::error_code ec;
		std::filesystem::remove(temporary_, ec);
	}
	untrack();
}

void OutputTransaction::track() noexcept
{
#ifndef _WIN32
	for (size_t i = 0; i < staged_paths.size(); ++i) {
		const char* expected = nullptr;
		if (staged_paths[i].compare_exchange_strong(expected, temporary_.c_str())) {
			slot_ = static_cast<int>(i);
			return;
		}
	}
#endif
}

void OutputTransaction::untrack() noexcept
{
#ifndef _WIN32
	if (slot_ >= 0)
		staged_paths[static_cast<size_t>(slot_)].store(nullptr);
#endif
	slot_ = -1;
}

void OutputTransaction::discard_all_staged() noexcept
{
#ifndef _WIN32
	for (auto& slot : staged_paths) {
		if (const char* path = slot.load())
			(void) ::unlink(path);
	}
#endif
}

std::expected<void, DeltaError> OutputTransaction::validateDestination() const
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const auto status = fs::status(destination_, ec);
	if (ec && ec != std::errc::no_such_file_or_directory)
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot inspect output: " + ec.message()});
	if (!fs::exists(status)) return {};
	if (!fs::is_regular_file(status))
		return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "Output must name a regular file"});
	for (const auto& input : inputs_) {
		ec.clear();
		const bool aliases = fs::equivalent(destination_, input, ec);
		if (ec)
			return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot compare output and input: " + ec.message()});
		if (aliases)
			return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "Output path aliases input: " + input.string()});
	}
	return {};
}

std::expected<void, DeltaError> OutputTransaction::open(
	const std::filesystem::path& destination,
	std::initializer_list<std::filesystem::path> inputs)
{
	if (!destination_.empty())
		return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "Output transaction already opened"});
	destination_ = destination;
	inputs_ = inputs;
	if (destination_.empty() || destination_.filename().empty())
		return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "Empty output filename"});
	if (auto checked = validateDestination(); !checked) return checked;

	static std::atomic<unsigned long long> counter{0};
	std::random_device random;
	for (unsigned attempt = 0; attempt < 32; ++attempt) {
		auto candidate = destination.parent_path() /
			(".rolling_hash-" + std::to_string(random()) + "-" +
			 std::to_string(random()) + "-" + std::to_string(counter++));
		if (file_.open(candidate, FileMode::EXCLUSIVE_OUT)) {
			temporary_ = std::move(candidate);
			track();
			return copyPermissions();
		}
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec)) break;
	}
	return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot create temporary output beside: " + destination.string()});
}

// Give the staged file an existing destination's permissions before any data
// is written, as writing in place did, so a private file never becomes
// readable by others, neither while staged nor after replacement.
std::expected<void, DeltaError> OutputTransaction::copyPermissions() const
{
	std::error_code ec;
	const auto existing = std::filesystem::status(destination_, ec);
	if (!ec && std::filesystem::exists(existing))
		std::filesystem::permissions(temporary_, existing.permissions(), ec);
	if (ec && ec != std::errc::no_such_file_or_directory)
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot copy output permissions: " + ec.message()});
	return {};
}

std::expected<void, DeltaError> OutputTransaction::commit()
{
	if (temporary_.empty())
		return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "No staged output"});
	if (!file_.close() || file_.has_error() || !sync_file(temporary_))
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Failed to flush output file"});
	if (auto checked = validateDestination(); !checked) return checked;
	std::error_code ec;
	std::filesystem::rename(temporary_, destination_, ec);
	if (ec)
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Failed to replace output: " + ec.message()});
	untrack();
	temporary_.clear();
	sync_directory(destination_.parent_path());
	return {};
}
