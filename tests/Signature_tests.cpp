#include <limits.h>
#include "gtest/gtest.h"

#include "RK_finger.hpp"
#include "blake.h"
#include "Signature.hpp"

TEST(Signature, generate_signature_null)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("");
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

TEST(Signature, generate_signature_incorrect)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("non-existing-file");
	auto chunks = signatures.get_chunks();
	ASSERT_EQ(chunks.size(), 0);
}

TEST(Signature, generate_signature)
{
	Signature<RKFinger, BLAKE512> signatures;

	signatures.generate_signatures("testfile");

	auto chunks = signatures.get_chunks();

	ASSERT_EQ(chunks.size(), 71);
	ASSERT_EQ(chunks[0].signature, 170639562);
	ASSERT_EQ(chunks[10].signature, 80674034);
	ASSERT_EQ(chunks[20].signature, 1216690479);
	ASSERT_EQ(chunks[30].signature, 2067808399);
	ASSERT_EQ(chunks[40].signature, 337825292);
	ASSERT_EQ(chunks[60].signature, 1035150333);
	ASSERT_EQ(chunks[69].signature, 1270043230);
}