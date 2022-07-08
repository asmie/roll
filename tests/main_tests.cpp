/**
 *  @file   main_tests.cpp
 *  @brief  Main testing file.
 *
 *  @author Piotr Olszewski     asmie@asmie.pl
 *
 *  @date   2022.07.01
 *
 * Main testing file.
 *
 */

#include <limits.h>
#include "gtest/gtest.h"

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);

	return RUN_ALL_TESTS();
}