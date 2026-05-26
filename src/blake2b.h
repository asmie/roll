#ifndef BLAKE2B_H
#define BLAKE2B_H

#include "IHash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

/**
* BLAKE2b-512 hash (RFC 7693). 64-byte digest, sequential mode, no key.
*/
class BLAKE2b : public IHash
{
public:
	static constexpr size_t HASH_SIZE = 64;
	static constexpr size_t BLOCK_SIZE = 128;

	size_t get_hash_size() const noexcept override { return HASH_SIZE; }

	void init() override;
	void update(std::span<const uint8_t> in) override;
	void finalize(std::span<uint8_t> out) override;

	// Test-only convenience wrapper around the IHash one-shot hash().
	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen);

	using IHash::hash;  // bring the std::span overload into scope.

private:
	uint64_t h_[8] {};
	uint64_t t_[2] {};
	uint64_t f_[2] {};
	uint8_t  buf_[BLOCK_SIZE] {};
	size_t   buflen_ {};
};

#endif // BLAKE2B_H
