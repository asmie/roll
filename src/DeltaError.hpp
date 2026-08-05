#ifndef DELTAERROR_HPP
#define DELTAERROR_HPP

#include <string>
#include <string_view>
#include <utility>

/**
* Failure categories for delta generation and application.
*
* Delta and Apply previously each returned a hand-rolled struct carrying
* `bool success` alongside statistics, which left every field ambiguous on
* failure — a caller could read bytes_written from a run that never wrote
* anything. They now return std::expected, so statistics exist only on success
* and an error only on failure, and neither can be misread as the other.
*
* The category exists so callers can react to *kinds* of failure without
* matching on message text: main() maps it to an exit status, and tests can
* assert a category instead of grepping a substring, which would otherwise make
* every message reword a test change.
*/
enum class DeltaErrc {
	io_error,             ///< Open, read, write or flush failed.
	corrupt_delta,        ///< Delta stream malformed, truncated, or out of order.
	integrity_mismatch,   ///< A hash did not match the data it covers.
	invalid_argument,     ///< Caller-supplied paths are unusable (e.g. aliasing).
	internal_error,       ///< Unexpected exception escaped the operation.
};

/**
* A failure: a category for programmatic handling plus a human-readable message
* naming the specific object involved.
*/
struct DeltaError {
	DeltaErrc code {};
	std::string message;

	DeltaError() = default;
	DeltaError(DeltaErrc c, std::string msg) : code(c), message(std::move(msg)) {}
};

inline std::string_view to_string(DeltaErrc code) noexcept
{
	switch (code) {
		case DeltaErrc::io_error:           return "I/O error";
		case DeltaErrc::corrupt_delta:      return "corrupt delta";
		case DeltaErrc::integrity_mismatch: return "integrity mismatch";
		case DeltaErrc::invalid_argument:   return "invalid argument";
		case DeltaErrc::internal_error:     return "internal error";
	}
	return "unknown error";
}

/// Statistics from a successful delta generation.
struct DeltaStats {
	size_t chunks_processed {};
	size_t bytes_written {};
};

/// Statistics from a successful delta application.
struct ApplyStats {
	size_t entries_processed {};
	size_t bytes_written {};
};

#endif // DELTAERROR_HPP
