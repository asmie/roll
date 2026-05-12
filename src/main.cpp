#include <iostream>
#include <string_view>

#include "rh_config.h"

#include "Apply.hpp"
#include "Delta.hpp"
#include "DeltaViewer.hpp"
#include "RK_finger.hpp"
#include "Signature.hpp"
#include "blake.h"

namespace {

void print_usage(const char* prog)
{
	std::cout << "Usage:" << std::endl;
	std::cout << "  " << prog << " create <oldfile> <newfile> <delta>" << std::endl;
	std::cout << "  " << prog << " apply  <oldfile> <delta> <outfile>" << std::endl;
	std::cout << "  " << prog << " view   <delta>" << std::endl;
}

int run_create(const char* old_path, const char* new_path, const char* delta_path)
{
	Signature<RKFinger, BLAKE512> old_signature;
	Signature<RKFinger, BLAKE512> new_signature;

	old_signature.generate_signatures(old_path);
	new_signature.generate_signatures(new_path);

	Delta<RKFinger, BLAKE512> delta;
	auto result = delta.generate_delta(old_signature, new_signature, old_path, new_path, delta_path);

	if (!result.success) {
		std::cerr << "Error generating delta: " << result.error_message << std::endl;
		return 1;
	}
	return 0;
}

int run_apply(const char* old_path, const char* delta_path, const char* out_path)
{
	Apply<RKFinger, BLAKE512> apply;
	auto result = apply.apply_delta(old_path, delta_path, out_path);

	if (!result.success) {
		std::cerr << "Error applying delta: " << result.error_message << std::endl;
		return 1;
	}

	std::cout << "Applied " << result.entries_processed << " entries, wrote "
	          << result.bytes_written << " bytes to " << out_path << std::endl;
	return 0;
}

} // namespace

int main(int argc, const char** argv)
{
	std::cout << argv[0] << " v. " << RH_VERSION_MAJOR << "." << RH_VERSION_MINOR
	          << "." << RH_VERSION_REV << std::endl;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	const std::string_view command{argv[1]};

	if (command == "create") {
		if (argc != 5) {
			print_usage(argv[0]);
			return 1;
		}
		return run_create(argv[2], argv[3], argv[4]);
	}

	if (command == "apply") {
		if (argc != 5) {
			print_usage(argv[0]);
			return 1;
		}
		return run_apply(argv[2], argv[3], argv[4]);
	}

	if (command == "view") {
		if (argc != 3) {
			print_usage(argv[0]);
			return 1;
		}
		return view_delta(argv[2]);
	}

	std::cerr << "Unknown command: " << command << std::endl;
	print_usage(argv[0]);
	return 1;
}
