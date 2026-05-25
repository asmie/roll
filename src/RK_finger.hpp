#ifndef RKFINGER_HPP
#define RKFINGER_HPP

#include "IRollingHash.hpp"

#include <cstdint>
#include <climits>
#include <span>
#include <vector>

constexpr unsigned int ALPHABET_DEF_SIZE = 256;
constexpr unsigned int WINDOW_DEF_SIZE = 48;

/**
* Rabin-Karp rolling fingerprint over a sliding window of bytes.
*/
class RKFinger : public IRollingHash<uint64_t> {
public:
	RKFinger() { init_state(); }
	RKFinger(unsigned int alphabet_size, unsigned int window_size, uint64_t modulus)
		: alphabet_size_(alphabet_size), window_size_(window_size), modulus_(modulus) {
		init_state();
	}

	RKFinger(const RKFinger& other) = default;
	RKFinger(RKFinger&& other) = default;
	RKFinger& operator=(const RKFinger& other) = default;
	RKFinger& operator=(RKFinger&& other) = default;

	/**
	* Computes initial hash value and seeds the sliding window.
	* @param initial[in] initial data to be hashed - must be at least window_size length.
	* @return True if init was successful, otherwise false (if initial data is too short).
	*/
	bool initialize(std::span<const uint8_t> initial) noexcept override {
		if (initial.size() < window_size_)
			return false;

		fingerprint_ = 0;
		for (unsigned int i = 0; i < window_size_; i++) {
			window_[i] = initial[i];
			fingerprint_ = (alphabet_size_ * fingerprint_ + initial[i]) % modulus_;
		}
		window_head_ = 0;
		return true;
	}

	/**
	* Roll the fingerprint one byte forward: evict the oldest byte of the window
	* and append the new one.
	*/
	uint64_t compute_next(uint8_t byte) noexcept override {
		const uint8_t evicted = window_[window_head_];
		fingerprint_ = (alphabet_size_ * ((fingerprint_ + modulus_) - (evicted * h_) % modulus_) + byte) % modulus_;
		window_[window_head_] = byte;
		window_head_ = (window_head_ + 1) % window_size_;
		return fingerprint_;
	}

	unsigned int get_alphabet_size() const override { return alphabet_size_; }
	unsigned int get_window_size() const override { return window_size_; }
	uint64_t get_modulus() const { return modulus_; }
	uint64_t get_current_fingerprint() const override { return fingerprint_; }

private:
	void init_state() {
		window_.assign(window_size_, 0);
		window_head_ = 0;
		fingerprint_ = 0;
		h_ = 1;
		for (unsigned int i = 0; i < window_size_ - 1; i++)
			h_ = (h_ * alphabet_size_) % modulus_;
	}

	unsigned int alphabet_size_ { ALPHABET_DEF_SIZE };			/*!< Possible alphabet size */
	unsigned int window_size_ { WINDOW_DEF_SIZE };				/*!< Window size */
	uint64_t modulus_ { INT_MAX };								/*!< Modulus */
	uint64_t fingerprint_ { 0 };								/*!< Current fingerprint */
	uint64_t h_ { 0 };											/*!< alphabet_size^(window-1) mod modulus */
	std::vector<uint8_t> window_;								/*!< Ring buffer of the current window's bytes */
	size_t window_head_ { 0 };									/*!< Index of the oldest byte (evicted next) */
};


#endif
