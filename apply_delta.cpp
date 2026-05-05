#include <iostream>

#include "Apply.hpp"
#include "RK_finger.hpp"
#include "blake.h"

static void usage(const char* prog)
{
	std::cout << "Usage: " << prog << " oldfile delta outfile" << std::endl;
}

int main(int argc, const char** argv)
{
	if (argc != 4) {
		usage(argv[0]);
		return 1;
	}

	Apply<RKFinger, BLAKE512> apply;
	auto result = apply.apply_delta(argv[1], argv[2], argv[3]);

	if (!result.success) {
		std::cerr << "Error applying delta: " << result.error_message << std::endl;
		return 1;
	}

	std::cout << "Applied " << result.entries_processed << " entries, wrote "
	          << result.bytes_written << " bytes to " << argv[3] << std::endl;
	return 0;
}
