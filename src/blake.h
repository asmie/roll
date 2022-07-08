#include "IHash.hpp"

#include <cstdint>
#include <cstddef>
#include <iostream>
using namespace std;

/**
* BLAKE-512 wrapper class. Simply wraps the underlying C implementation.
* Implements IHash interface.
*/
class BLAKE512 : public IHash
{
public:
	/**
	* Get hash size in bytes.
	* Method is constexpr to allow to be used in compile time expressions (like array declarations).
	* @return hash size in bytes.
	*/
	constexpr virtual size_t get_hash_size() const noexcept {
		return 64;
	}
	
	/**
	* Computes hash and stores it in the given buffer.
	* @param out[out] buffer to store hash.
	* @param in[in] input to be hashed
	* @param inlen[in] length of the input
	*/
	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen) {
		if (out == nullptr || in == nullptr)
			return;
		
		blake512_hash(out, in, inlen);
	}
	
	
private:
	void blake512_hash(uint8_t* out, const uint8_t* in, uint64_t inlen);
};
