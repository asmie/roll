#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

#ifndef _WIN32
#include <csignal>
#include <signal.h>
#endif

#include "rh_config.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaError.hpp"
#include "DeltaViewer.hpp"
#include "OutputTransaction.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake2b.h"

namespace {

void print_version(const char* prog)
{
	std::cout << prog << " v" << RH_VERSION_MAJOR << "." << RH_VERSION_MINOR
	          << "." << RH_VERSION_REV << std::endl;
}

void print_usage(const char* prog, std::ostream& os = std::cerr)
{
	os << "Usage:\n"
	   << "  " << prog << " create <oldfile> <newfile> <delta>\n"
	   << "  " << prog << " apply  [--max-output SIZE] <oldfile> <delta> <outfile>\n"
	   << "  " << prog << " view   <delta>\n"
	   << "  " << prog << " --version\n"
	   << "  " << prog << " --help\n"
	   << "\n"
	   << "SIZE is a byte count with an optional K, M, G or T (binary) suffix.\n";
}

// Parse a byte count such as 4096, 64K or 2G (powers of 1024).
std::optional<uint64_t> parse_size(std::string_view text)
{
	uint64_t value = 0;
	size_t i = 0;
	for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
		const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
		if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
			return std::nullopt;
		value = value * 10 + digit;
	}
	if (i == 0 || text.size() - i > 1)
		return std::nullopt;
	if (i == text.size())
		return value;
	const std::string_view suffixes = "KMGT";
	const auto power = suffixes.find(static_cast<char>(text[i] & ~0x20));
	if (power == std::string_view::npos)
		return std::nullopt;
	const unsigned shift = 10 * static_cast<unsigned>(power + 1);
	if (value > (std::numeric_limits<uint64_t>::max() >> shift))
		return std::nullopt;
	return value << shift;
}

#ifndef _WIN32
extern "C" void discard_staged_and_reraise(int sig)
{
	OutputTransaction::discard_all_staged();
	// SA_RESETHAND restored the default action; the signal stays blocked until
	// this handler returns, then terminates the process with the usual status.
	std::raise(sig);
}

// Remove staged output when interrupted, rather than leaving a hidden
// temporary beside the destination. SIGKILL and power loss cannot be handled;
// see the README for cleaning up after those.
void install_signal_cleanup()
{
	struct sigaction action {};
	action.sa_handler = discard_staged_and_reraise;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESETHAND;
	for (const int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
		struct sigaction previous {};
		// Keep a signal the parent chose to ignore (nohup, background jobs) ignored.
		if (sigaction(sig, nullptr, &previous) == 0 && previous.sa_handler != SIG_IGN)
			(void) sigaction(sig, &action, nullptr);
	}
	// Exceeding a file-size limit (ulimit -f) then fails the write, so the
	// normal error path discards the staged file instead of the process dying.
	(void) std::signal(SIGXFSZ, SIG_IGN);
}
#endif

// Map a failure category to an exit status: bad input the user can act on is
// distinguished from an environment or I/O problem.
int exit_status_for(DeltaErrc code)
{
	switch (code) {
		case DeltaErrc::corrupt_delta:
		case DeltaErrc::integrity_mismatch:
		case DeltaErrc::invalid_argument:
		case DeltaErrc::limit_exceeded:
			return 1;
		case DeltaErrc::io_error:
		case DeltaErrc::internal_error:
			return 2;
	}
	return 1;
}

void report(const char* action, const DeltaError& error)
{
	std::cerr << "Error " << action << ": " << error.message
	          << " (" << to_string(error.code) << ")" << std::endl;
}

int run_create(const char* old_path, const char* new_path, const char* delta_path)
{
	Signature<RKFinger, BLAKE2b> old_signature;
	Signature<RKFinger, BLAKE2b> new_signature;

	// The two signature passes read different files and share no state, so run
	// the old file's on a worker thread while this thread handles the new
	// file's. The new file's pass also computes the whole-file trailer digest,
	// which would otherwise cost generate_delta a second full read of it.
	auto old_ok = std::async(std::launch::async, [&old_signature, old_path] {
		return old_signature.generate_signatures(old_path);
	});
	const bool new_ok = new_signature.generate_signatures(new_path, WholeFileHash::Compute);

	// Join the worker before acting on either result; get() also rethrows any
	// exception from the worker on this thread, where main()'s handler sees it.
	if (!old_ok.get()) {
		std::cerr << "Failed to read old file: " << old_path << std::endl;
		return 2;
	}
	if (!new_ok) {
		std::cerr << "Failed to read new file: " << new_path << std::endl;
		return 2;
	}

	Delta<RKFinger, BLAKE2b> delta;
	const auto result = delta.generate_delta(old_signature, new_signature,
	                                         old_path, new_path, delta_path);

	if (!result) {
		report("generating delta", result.error());
		return exit_status_for(result.error().code);
	}
	return 0;
}

int run_apply(const char* old_path, const char* delta_path, const char* out_path,
              uint64_t max_output)
{
	Apply<RKFinger, BLAKE2b> apply;
	const auto result = apply.apply_delta(old_path, delta_path, out_path, max_output);

	if (!result) {
		report("applying delta", result.error());
		return exit_status_for(result.error().code);
	}

	std::cout << "Applied " << result->entries_processed << " entries, wrote "
	          << result->bytes_written << " bytes to " << out_path << std::endl;
	return 0;
}

int run(int argc, const char** argv)
{
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	const std::string_view command{argv[1]};

	if (command == "--version" || command == "-v") {
		print_version(argv[0]);
		return 0;
	}

	if (command == "--help" || command == "-h") {
		print_usage(argv[0], std::cout);
		return 0;
	}

	if (command == "create") {
		if (argc != 5) { print_usage(argv[0]); return 1; }
		return run_create(argv[2], argv[3], argv[4]);
	}

	if (command == "apply") {
		uint64_t max_output = std::numeric_limits<uint64_t>::max();
		int first = 2;
		if (argc == 7 && std::string_view{argv[2]} == "--max-output") {
			const auto parsed = parse_size(argv[3]);
			if (!parsed) {
				std::cerr << "Invalid --max-output size: " << argv[3] << std::endl;
				return 1;
			}
			max_output = *parsed;
			first = 4;
		} else if (argc != 5) {
			print_usage(argv[0]);
			return 1;
		}
		return run_apply(argv[first], argv[first + 1], argv[first + 2], max_output);
	}

	if (command == "view") {
		if (argc != 3) { print_usage(argv[0]); return 1; }
		return view_delta(argv[2]);
	}

	std::cerr << "Unknown command: " << command << std::endl;
	print_usage(argv[0]);
	return 1;
}

} // namespace

int main(int argc, const char** argv)
{
	// The hash backend and the fingerprint constructor both report failure by
	// throwing, and a corrupt length field can still surface as bad_alloc.
	// Catch here so those exit with a diagnostic instead of a SIGABRT from
	// std::terminate.
	try {
#ifndef _WIN32
		install_signal_cleanup();
#endif
		return run(argc, argv);
	} catch (const std::exception& e) {
		std::cerr << "Fatal error: " << e.what() << std::endl;
		return 2;
	} catch (...) {
		std::cerr << "Fatal error: unknown exception" << std::endl;
		return 2;
	}
}
