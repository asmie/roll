#include <iostream>
#include <future>
#include <cstring>

#include "rh_config.h"

#include "Signature.hpp"
#include "Delta.hpp"
#include "RK_finger.hpp"
#include "blake.h"

void usage(const char **argv)
{
	std::cout << "Usage: " << std::endl;
	std::cout << argv[0] << " oldfile newfile delta" << std::endl;
}

int main(int argc, const char **argv)
{
	std::cout << argv[0] << " v. " << RH_VERSION_MAJOR << "." << RH_VERSION_MINOR << "." << RH_VERSION_REV << std::endl;
	
	if (argc < 4)					// at least 4 arguments are required
	{
		usage(argv);
		return 1;
	}
	
	Signature<RKFinger, BLAKE512> old_signature;
	Signature<RKFinger, BLAKE512> new_signature;

	old_signature.generate_signatures(argv[1]);
	new_signature.generate_signatures(argv[2]);

	Delta<RKFinger, BLAKE512> delta;
	delta.generate_delta(old_signature, new_signature, argv[1], argv[2], argv[3]);

	return 0;
}