#include <exception>
#include <iostream>
#include <string_view>

#include "rh_config.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaViewer.hpp"
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
	   << "  " << prog << " apply  <oldfile> <delta> <outfile>\n"
	   << "  " << prog << " view   <delta>\n"
	   << "  " << prog << " --version\n"
	   << "  " << prog << " --help\n";
}

int run_create(const char* old_path, const char* new_path, const char* delta_path)
{
	Signature<RKFinger, BLAKE2b> old_signature;
	Signature<RKFinger, BLAKE2b> new_signature;

	if (!old_signature.generate_signatures(old_path)) {
		std::cerr << "Failed to read old file: " << old_path << std::endl;
		return 1;
	}
	if (!new_signature.generate_signatures(new_path)) {
		std::cerr << "Failed to read new file: " << new_path << std::endl;
		return 1;
	}

	Delta<RKFinger, BLAKE2b> delta;
	auto result = delta.generate_delta(old_signature, new_signature, old_path, new_path, delta_path);

	if (!result.success) {
		std::cerr << "Error generating delta: " << result.error_message << std::endl;
		return 1;
	}
	return 0;
}

int run_apply(const char* old_path, const char* delta_path, const char* out_path)
{
	Apply<RKFinger, BLAKE2b> apply;
	auto result = apply.apply_delta(old_path, delta_path, out_path);

	if (!result.success) {
		std::cerr << "Error applying delta: " << result.error_message << std::endl;
		return 1;
	}

	std::cout << "Applied " << result.entries_processed << " entries, wrote "
	          << result.bytes_written << " bytes to " << out_path << std::endl;
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
		if (argc != 5) { print_usage(argv[0]); return 1; }
		return run_apply(argv[2], argv[3], argv[4]);
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
		return run(argc, argv);
	} catch (const std::exception& e) {
		std::cerr << "Fatal error: " << e.what() << std::endl;
		return 2;
	} catch (...) {
		std::cerr << "Fatal error: unknown exception" << std::endl;
		return 2;
	}
}
