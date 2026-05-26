#ifndef BLAKE2B_H
#define BLAKE2B_H

#include "IHash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// Forward-declare OpenSSL's context type so consumers of this header don't
// pull in <openssl/evp.h>.
typedef struct evp_md_ctx_st EVP_MD_CTX;

/**
* BLAKE2b-512 hash (RFC 7693). Thin RAII wrapper around OpenSSL's EVP
* digest interface — sequential mode, no key, 64-byte output.
*/
class BLAKE2b : public IHash
{
public:
	static constexpr size_t HASH_SIZE = 64;

	BLAKE2b();
	~BLAKE2b() override;

	BLAKE2b(const BLAKE2b&) = delete;
	BLAKE2b& operator=(const BLAKE2b&) = delete;

	BLAKE2b(BLAKE2b&& other) noexcept;
	BLAKE2b& operator=(BLAKE2b&& other) noexcept;

	size_t get_hash_size() const noexcept override { return HASH_SIZE; }

	void init() override;
	void update(std::span<const uint8_t> in) override;
	void finalize(std::span<uint8_t> out) override;

	void hash(uint8_t* out, const uint8_t* in, uint64_t inlen);

	using IHash::hash;  // bring the std::span overload into scope.

private:
	EVP_MD_CTX* ctx_ {nullptr};
};

#endif // BLAKE2B_H
