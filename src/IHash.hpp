#ifndef IHASH_HPP
#define IHASH_HPP

#include <cstdint>
#include <cstddef>

/**
* Interface for string hashing functions.
*/
class IHash {
public:
	/**
	* Get hash size in bytes. 
	* Method is constexpr to allow to be used in compile time expressions (like array declarations).
	* @return hash size in bytes.
	*/
	constexpr virtual size_t get_hash_size() const noexcept = 0;
	
	/**
	* Computes hash and stores it in the given buffer.
	* @param out[out] buffer to store hash.
	* @param in[in] input to be hashed
	* @param inlen[in] length of the input
	*/
	virtual void hash(uint8_t* out, const uint8_t* in, uint64_t inlen) = 0;
};


#endif