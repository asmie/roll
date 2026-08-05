#ifndef HASHCONCEPTS_HPP
#define HASHCONCEPTS_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

/**
* Requirements on the two hash algorithms the delta pipeline is built from.
*
* These were previously abstract base classes (IRollingHash<T> and IHash) that
* the concepts then checked with std::derived_from. That meant the project
* carried two mechanisms for one abstraction: templates constrained at compile
* time *and* virtual dispatch at run time. Nothing ever held a base pointer or
* reference, so every vtable was overhead — and it sat on the hottest path in
* the program, since compute_next() runs once per input byte.
*
* Constraining the operations directly keeps the checking (with clearer errors,
* because a mismatch names the missing operation rather than a missing base) and
* drops the indirection. Adding an algorithm means providing these operations,
* not inheriting from anything.
*/

/**
* A rolling fingerprint over a sliding window of bytes.
*
* initialize() seeds the window and returns false when fewer than
* get_window_size() bytes were supplied, meaning compute_next() must not be
* called because there is no full window to roll.
*/
template <class T>
concept RollingHashAlgorithm =
	std::default_initializable<T> &&
	requires { typename T::RollingHashType; } &&
	requires (T fingerprint, std::span<const uint8_t> window, uint8_t byte) {
		{ fingerprint.initialize(window) } -> std::same_as<bool>;
		{ fingerprint.compute_next(byte) } -> std::same_as<typename T::RollingHashType>;
		{ fingerprint.get_current_fingerprint() } -> std::same_as<typename T::RollingHashType>;
		{ fingerprint.get_window_size() } -> std::convertible_to<unsigned int>;
		{ fingerprint.get_alphabet_size() } -> std::convertible_to<unsigned int>;
	};

/**
* A strong (cryptographic) hash exposing both a streaming state machine and a
* one-shot form.
*
* init()/update()/finalize() let callers hash arbitrarily large input without
* buffering it; hash() is the one-shot equivalent for callers that already hold
* the whole message. finalize() and hash() write get_hash_size() bytes and must
* tolerate an undersized destination without writing past it.
*/
template <class U>
concept StrongHashAlgorithm =
	std::default_initializable<U> &&
	requires (U hash, std::span<uint8_t> out, std::span<const uint8_t> in) {
		{ hash.get_hash_size() } -> std::convertible_to<size_t>;
		hash.init();
		hash.update(in);
		hash.finalize(out);
		hash.hash(out, in);
	};

/**
* Default one-shot implementation, for algorithms without a faster specialised
* path. This was IHash::hash()'s virtual default; as a free function it stays
* available to every implementation without requiring a base class.
*/
template <class U>
void hash_oneshot(U& hash, std::span<uint8_t> out, std::span<const uint8_t> in)
{
	if (out.size() < hash.get_hash_size())
		return;
	hash.init();
	hash.update(in);
	hash.finalize(out);
}

#endif // HASHCONCEPTS_HPP
