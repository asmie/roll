#ifndef IROLLINGHASH_HPP
#define IROLLINGHASH_HPP

#include <vector>
#include <cstdint>
#include <span>

/**
* Rolling hash interface for all rolling hash implementations.
* Template T is used for rolling hash return type.
*/
template <class T>
class IRollingHash {
public:
	// Typedef for static asserts.
	typedef T RollingHashType;

	/**
	* Virtual destructor for proper cleanup in derived classes.
	*/
	virtual ~IRollingHash() = default;

	/**
	* Initializes rolling hash with initial values. Can depend on window size. Amount of the
	* elements is taken directly from vector size.
	* @param[in] initial byte window with initial values
	* @return True if initialization was successful.
	*/
	virtual bool initialize(std::span<const uint8_t> initial) noexcept = 0;

	/**
	* Updates rolling hash with new element.
	* @param[in] byte new element to add to rolling hash
	* @return Current rolling hash value.
	*/
	virtual T compute_next(uint8_t byte) noexcept = 0;

	/**
	* Get alphabet size.
	* @return Alphabet size.
	*/
	virtual unsigned int get_alphabet_size() const = 0;

	/**
	* Return rolling hash window size.
	* @return Window size.
	*/
	virtual unsigned int get_window_size() const = 0;

	/**
	* Get current rolling hash value.
	* @return Current rolling hash value.
	*/
	virtual T get_current_fingerprint() const = 0;
	
};


#endif
