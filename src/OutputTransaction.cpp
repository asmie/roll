#include "OutputTransaction.hpp"

#include <atomic>
#include <random>
#include <string>
#include <system_error>

OutputTransaction::~OutputTransaction()
{
	(void) file_.close(); // A failed transaction is discarded, never published.
	if (!temporary_.empty()) {
		std::error_code ec;
		std::filesystem::remove(temporary_, ec);
	}
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
			return {};
		}
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec)) break;
	}
	return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot create temporary output beside: " + destination.string()});
}

std::expected<void, DeltaError> OutputTransaction::commit()
{
	if (temporary_.empty())
		return std::unexpected(DeltaError{DeltaErrc::invalid_argument, "No staged output"});
	if (!file_.close() || file_.has_error())
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Failed to flush output file"});
	if (auto checked = validateDestination(); !checked) return checked;
	// Keep an existing destination's permissions, as writing it in place did,
	// so replacing a private file does not widen access to it.
	std::error_code ec;
	const auto existing = std::filesystem::status(destination_, ec);
	if (!ec && std::filesystem::exists(existing))
		std::filesystem::permissions(temporary_, existing.permissions(), ec);
	if (ec && ec != std::errc::no_such_file_or_directory)
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Cannot copy output permissions: " + ec.message()});
	ec.clear();
	std::filesystem::rename(temporary_, destination_, ec);
	if (ec)
		return std::unexpected(DeltaError{DeltaErrc::io_error, "Failed to replace output: " + ec.message()});
	temporary_.clear();
	return {};
}
