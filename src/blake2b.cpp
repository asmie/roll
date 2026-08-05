#include "blake2b.h"

#include <openssl/evp.h>

#include <stdexcept>

BLAKE2b::BLAKE2b()
	: ctx_(EVP_MD_CTX_new())
{
	if (!ctx_)
		throw std::runtime_error("BLAKE2b: EVP_MD_CTX_new failed");
}

BLAKE2b::~BLAKE2b()
{
	if (ctx_) EVP_MD_CTX_free(ctx_);
}

BLAKE2b::BLAKE2b(BLAKE2b&& other) noexcept
	: ctx_(other.ctx_)
{
	other.ctx_ = nullptr;
}

BLAKE2b& BLAKE2b::operator=(BLAKE2b&& other) noexcept
{
	if (this != &other) {
		if (ctx_) EVP_MD_CTX_free(ctx_);
		ctx_ = other.ctx_;
		other.ctx_ = nullptr;
	}
	return *this;
}

void BLAKE2b::init()
{
	if (EVP_DigestInit_ex(ctx_, EVP_blake2b512(), nullptr) != 1)
		throw std::runtime_error("BLAKE2b: EVP_DigestInit_ex failed");
}

void BLAKE2b::update(std::span<const uint8_t> in)
{
	if (in.empty()) return;
	if (EVP_DigestUpdate(ctx_, in.data(), in.size()) != 1)
		throw std::runtime_error("BLAKE2b: EVP_DigestUpdate failed");
}

void BLAKE2b::finalize(std::span<uint8_t> out)
{
	if (out.size() < HASH_SIZE)
		return;
	unsigned int len = 0;
	if (EVP_DigestFinal_ex(ctx_, out.data(), &len) != 1)
		throw std::runtime_error("BLAKE2b: EVP_DigestFinal_ex failed");
}

void BLAKE2b::hash(std::span<uint8_t> out, std::span<const uint8_t> in)
{
	hash_oneshot(*this, out, in);
}

void BLAKE2b::hash(uint8_t* out, const uint8_t* in, uint64_t inlen)
{
	if (out == nullptr || (in == nullptr && inlen != 0))
		return;
	hash(std::span<uint8_t>{out, HASH_SIZE},
	     std::span<const uint8_t>{in, static_cast<size_t>(inlen)});
}
