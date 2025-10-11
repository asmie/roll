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

	signatures.generate_signatures("../tests/testfile");

	auto chunks = signatures.get_chunks();

	// With adaptive chunking, we expect more chunks (smaller average size)
	// The 512KB file should generate between 100-500 chunks with our new algorithm
	ASSERT_GT(chunks.size(), 100);  // Should have at least 100 chunks
	ASSERT_LT(chunks.size(), 500);  // Should have less than 500 chunks

	// Verify chunks were created and have valid data
	ASSERT_GT(chunks[0].signature, 0);
	ASSERT_GT(chunks[0].chunk_size, 0);
	ASSERT_EQ(chunks[0].start_offset, 0);  // First chunk starts at 0

	// Verify chunks are contiguous
	for (size_t i = 1; i < chunks.size(); ++i) {
		ASSERT_EQ(chunks[i].start_offset, chunks[i-1].start_offset + chunks[i-1].chunk_size);
	}
}