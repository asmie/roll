#ifndef IHASH_HPP
#define IHASH_HPP

#include <cstdint>
#include <cstddef>
#include <span>

/**
* Interface for string hashing functions.
*/
class IHash {
public:
	/**
	* Virtual destructor for proper cleanup in derived classes.
	*/
	virtual ~IHash() = default;

	/**
	* Get hash size in bytes.
	* @return hash size in bytes.
	*/
	virtual size_t get_hash_size() const noexcept = 0;

	/**
	* Computes hash and stores it in the given buffer.
	* @param out[out] buffer to store hash.
	* @param in[in] input to be hashed
	*/
	virtual void hash(std::span<uint8_t> out, std::span<const uint8_t> in) = 0;
};


#endif
