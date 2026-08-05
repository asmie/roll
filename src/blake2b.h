#ifndef BLAKE2B_H
#define BLAKE2B_H

#include "HashConcepts.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// Forward-declare OpenSSL's context type so consumers of this header don't
// pull in <openssl/evp.h>.
typedef struct evp_md_ctx_st EVP_MD_CTX;

/**
* BLAKE2b-512 hash (RFC 7693). Thin RAII wrapper around OpenSSL's EVP
* digest interface — sequential mode, no key, 64-byte output.
*
* Satisfies StrongHashAlgorithm. Not virtual: the hash is a template argument,
* so no call site needs run-time dispatch.
*/
class BLAKE2b
{
public:
	static constexpr size_t HASH_SIZE = 64;

	BLAKE2b();
	~BLAKE2b();

	BLAKE2b(const BLAKE2b&) = delete;
	BLAKE2b& operator=(const BLAKE2b&) = delete;

	BLAKE2b(BLAKE2b&& other) noexcept;
	BLAKE2b& operator=(BLAKE2b&& other) noexcept;

	size_t get_hash_size() const noexcept { return HASH_SIZE; }

	void init();
	void update(std::span<const uint8_t> in);
	void finalize(std::span<uint8_t> out);

	/// One-shot over spans.
	void hash(std::span<uint8_t> out, std::span<const uint8_t> in);

	/// One-shot over raw pointers. A null `out`, or a null `in` with a non-zero
	/// length, is ignored rather than dereferenced.
	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen);

private:
	EVP_MD_CTX* ctx_ {nullptr};
};

static_assert(StrongHashAlgorithm<BLAKE2b>,
              "BLAKE2b must satisfy StrongHashAlgorithm");

#endif // BLAKE2B_H
