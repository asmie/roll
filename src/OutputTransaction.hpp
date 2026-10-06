#ifndef OUTPUTTRANSACTION_HPP
#define OUTPUTTRANSACTION_HPP

#include "DeltaError.hpp"
#include "FileIO.hpp"

#include <expected>
#include <filesystem>
#include <initializer_list>
#include <vector>

/// Stage output beside its destination, replacing it only after a clean close.
/// Failure or exception removes only the exclusively created temporary file.
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

private:
	[[nodiscard]] std::expected<void, DeltaError> validateDestination() const;
	std::filesystem::path destination_, temporary_;
	std::vector<std::filesystem::path> inputs_;
	FileIO file_;
};

#endif
