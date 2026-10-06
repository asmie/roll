#ifndef OUTPUTTRANSACTION_HPP
#define OUTPUTTRANSACTION_HPP

#include "DeltaError.hpp"
#include "FileIO.hpp"

#include <expected>
#include <filesystem>
#include <initializer_list>
#include <vector>

/// Stage output beside its destination, replacing it only after a clean close
/// and a flush to stable storage. Failure or exception removes only the
/// exclusively created temporary file.
class OutputTransaction {
public:
	OutputTransaction() = default;
	~OutputTransaction();
	OutputTransaction(const OutputTransaction&) = delete;
	OutputTransaction& operator=(const OutputTransaction&) = delete;

	[[nodiscard]] std::expected<void, DeltaError> open(
		const std::filesystem::path& destination,
		std::initializer_list<std::filesystem::path> inputs);
	[[nodiscard]] std::expected<void, DeltaError> commit();
	FileIO& file() noexcept { return file_; }

	/// Remove the staged files of every open transaction in this process.
	/// Async-signal-safe on POSIX, so a signal handler can call it before
	/// re-raising; elsewhere it does nothing.
	static void discard_all_staged() noexcept;

private:
	[[nodiscard]] std::expected<void, DeltaError> validateDestination() const;
	[[nodiscard]] std::expected<void, DeltaError> copyPermissions() const;
	void track() noexcept;
	void untrack() noexcept;
	std::filesystem::path destination_, temporary_;
	std::vector<std::filesystem::path> inputs_;
	FileIO file_;
	int slot_ { -1 };
};

#endif
