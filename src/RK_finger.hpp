#ifndef RKFINGER_HPP
#define RKFINGER_HPP

#include "IRollingHash.hpp"

#include <vector>
#include <cstdint>
#include <climits>

constexpr unsigned int ALPHABET_DEF_SIZE = 256;
constexpr unsigned int WINDOW_DEF_SIZE = 48;

/**
* RKFinger is a class that implements the Rabin fingerprints.
* It is used to fingerprint a given data using rolling hash approach (compute_next).
* 
* Class has only primitive types so moving and copying can be done using default copy constructor and assignment operator.
* 
*/
class RKFinger : public IRollingHash<uint64_t> {
public:
	RKFinger() = default;
	RKFinger(unsigned int alphabet_size, unsigned int window_size, uint64_t modulus) 
		: alphabet_size_(alphabet_size), window_size_(window_size), modulus_(modulus) {
		for (unsigned int i = 0; i < window_size_ - 1; i++)
			h_ = (h_ * alphabet_size_) % modulus_;
	}
	
	RKFinger(const RKFinger& other) = default;
	RKFinger(RKFinger&& other) = default;
	RKFinger& operator=(const RKFinger& other) = default;
	RKFinger& operator=(RKFinger&& other) = default;
	
	/**
	* Computes initial hash value.
	* Can be used to clear current hash and initalize RKFinger object with new data.
	* @param initial[in] initial data to be hashed - must be at least window_size length (but still only window_size bytes will be used).
	* @return True if init was successfull, otherwise false (if initial data is too short).
	*/
	bool initialize(const std::vector<uint8_t>& initial) noexcept override {
		if (initial.size() < window_size_)
			return false;

		fingerprint_ = 0;
		last_byte = initial.back();
	
		for (unsigned int i = 0; i < window_size_; i++)
			fingerprint_ = (alphabet_size_ * fingerprint_ + initial[i]) % modulus_;
		return true;
	}

	/**
	* Compute the next hash value for the given data.
	* @param[in] data the data byte to be hashed.
	* @return the new rolling hash value.
	*/
	uint64_t compute_next(uint8_t byte) noexcept override {
		fingerprint_ = (alphabet_size_ * (fingerprint_ - last_byte * h_) + byte) % modulus_;
		last_byte = byte;
		return fingerprint_;
	}

	/**
	* Get alphabet size.
	* @return Alphabet size.
	*/
	unsigned int get_alphabet_size() const override {
		return alphabet_size_;
	}
	
	/**
	* Return rolling hash window size.
	* @return Window size.
	*/
	unsigned int get_window_size() const override {
		return window_size_;
	}

	/**
	* Return modulus.
	* @return Modulus.
	*/
	uint64_t get_modulus() const {
		return modulus_;
	}

	/**
	* Get current rolling hash value.
	* @return Current rolling hash value.
	*/
	uint64_t get_current_fingerprint() const override {
		return fingerprint_;
	}
	
private:
	unsigned int alphabet_size_ { ALPHABET_DEF_SIZE };			/*!< Possible alphabet size */
	unsigned int window_size_ { WINDOW_DEF_SIZE };				/*!< Window size */
	uint64_t modulus_ {INT_MAX};								/*!< Modulus (this rolling hash is using modulus */
	uint64_t fingerprint_ {0};									/*!< Current fingerprint */
	uint64_t h_{ 0 };											/*!< Hash function coefficient */
	uint8_t last_byte{ 0 };										/*!< Last byte of the window remembered for compute_next operation */
};


#endif