#ifndef BLAKE_H
#define BLAKE_H

#include "IHash.hpp"

#include <cstdint>
#include <cstddef>
#include <span>

/**
* BLAKE-512 wrapper class. Simply wraps the underlying C implementation.
* Implements IHash interface.
*/
class BLAKE512 : public IHash
{
public:
	/**
	* Get hash size in bytes.
	* @return hash size in bytes.
	*/
	virtual size_t get_hash_size() const noexcept override {
		return 64;
	}
	
	/**
	* Computes hash and stores it in the given buffer.
	* @param out[out] buffer to store hash.
	* @param in[in] input to be hashed
	*/
	void hash(std::span<uint8_t> out, std::span<const uint8_t> in) override {
		if (out.size() < get_hash_size())
			return;
		
		blake512_hash(out.data(), in.data(), in.size());
	}

	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen) {
		if (out == nullptr || in == nullptr)
			return;

		hash(std::span<uint8_t>{out, get_hash_size()}, std::span<const uint8_t>{in, inlen});
	}
	
	
private:
	void blake512_hash(uint8_t* out, const uint8_t* in, uint64_t inlen);
};

#endif // BLAKE_H
