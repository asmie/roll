#ifndef BLAKE2B_H
#define BLAKE2B_H

#include "IHash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

/**
* BLAKE2b-512 hash (RFC 7693). 64-byte digest, sequential mode, no key, no salt.
*/
class BLAKE2b : public IHash
{
public:
	size_t get_hash_size() const noexcept override { return 64; }

	void hash(std::span<uint8_t> out, std::span<const uint8_t> in) override;

	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen);
};

#endif // BLAKE2B_H
