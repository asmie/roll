// BLAKE2b implementation, RFC 7693.
// Sequential mode, no key, no salt, no personalization.

#include "blake2b.h"

#include <cstring>

namespace {

constexpr uint64_t IV[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
	0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

constexpr uint8_t SIGMA[12][16] = {
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
	{ 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
	{  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
	{  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
	{  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
	{ 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
	{ 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
	{  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
	{ 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

inline uint64_t rotr64(uint64_t x, unsigned n) noexcept
{
	return (x >> n) | (x << (64 - n));
}

inline uint64_t load64_le(const uint8_t* p) noexcept
{
	return  static_cast<uint64_t>(p[0])
	     | (static_cast<uint64_t>(p[1]) <<  8)
	     | (static_cast<uint64_t>(p[2]) << 16)
	     | (static_cast<uint64_t>(p[3]) << 24)
	     | (static_cast<uint64_t>(p[4]) << 32)
	     | (static_cast<uint64_t>(p[5]) << 40)
	     | (static_cast<uint64_t>(p[6]) << 48)
	     | (static_cast<uint64_t>(p[7]) << 56);
}

void compress(uint64_t h[8], uint64_t t[2], uint64_t f[2], const uint8_t block[128]) noexcept
{
	uint64_t m[16];
	for (int i = 0; i < 16; i++)
		m[i] = load64_le(block + i * 8);

	uint64_t v[16];
	for (int i = 0; i < 8; i++) v[i]     = h[i];
	for (int i = 0; i < 8; i++) v[i + 8] = IV[i];
	v[12] ^= t[0];
	v[13] ^= t[1];
	v[14] ^= f[0];
	v[15] ^= f[1];

	auto G = [&](unsigned a, unsigned b, unsigned c, unsigned d, uint64_t x, uint64_t y) {
		v[a] = v[a] + v[b] + x;
		v[d] = rotr64(v[d] ^ v[a], 32);
		v[c] = v[c] + v[d];
		v[b] = rotr64(v[b] ^ v[c], 24);
		v[a] = v[a] + v[b] + y;
		v[d] = rotr64(v[d] ^ v[a], 16);
		v[c] = v[c] + v[d];
		v[b] = rotr64(v[b] ^ v[c], 63);
	};

	for (int r = 0; r < 12; r++) {
		const auto& s_ = SIGMA[r];
		G(0, 4,  8, 12, m[s_[ 0]], m[s_[ 1]]);
		G(1, 5,  9, 13, m[s_[ 2]], m[s_[ 3]]);
		G(2, 6, 10, 14, m[s_[ 4]], m[s_[ 5]]);
		G(3, 7, 11, 15, m[s_[ 6]], m[s_[ 7]]);
		G(0, 5, 10, 15, m[s_[ 8]], m[s_[ 9]]);
		G(1, 6, 11, 12, m[s_[10]], m[s_[11]]);
		G(2, 7,  8, 13, m[s_[12]], m[s_[13]]);
		G(3, 4,  9, 14, m[s_[14]], m[s_[15]]);
	}

	for (int i = 0; i < 8; i++)
		h[i] ^= v[i] ^ v[i + 8];
}

} // namespace

void BLAKE2b::init()
{
	for (int i = 0; i < 8; i++) h_[i] = IV[i];
	// Parameter block (sequential, no key/salt/personal): outlen | fanout<<16 | depth<<24.
	h_[0] ^= 0x01010000ULL | static_cast<uint64_t>(HASH_SIZE);
	t_[0] = t_[1] = 0;
	f_[0] = f_[1] = 0;
	buflen_ = 0;
}

void BLAKE2b::update(std::span<const uint8_t> in)
{
	if (in.empty()) return;

	const uint8_t* p = in.data();
	size_t inlen = in.size();
	const size_t left = buflen_;
	const size_t fill = BLOCK_SIZE - left;

	if (inlen > fill) {
		std::memcpy(buf_ + left, p, fill);
		t_[0] += BLOCK_SIZE;
		if (t_[0] < BLOCK_SIZE) t_[1] += 1;
		compress(h_, t_, f_, buf_);
		p += fill;
		inlen -= fill;
		buflen_ = 0;

		while (inlen > BLOCK_SIZE) {
			t_[0] += BLOCK_SIZE;
			if (t_[0] < BLOCK_SIZE) t_[1] += 1;
			compress(h_, t_, f_, p);
			p += BLOCK_SIZE;
			inlen -= BLOCK_SIZE;
		}
	}

	std::memcpy(buf_ + buflen_, p, inlen);
	buflen_ += inlen;
}

void BLAKE2b::finalize(std::span<uint8_t> out)
{
	if (out.size() < HASH_SIZE)
		return;

	t_[0] += buflen_;
	if (t_[0] < buflen_) t_[1] += 1;
	f_[0] = ~0ULL;
	for (size_t i = buflen_; i < BLOCK_SIZE; i++) buf_[i] = 0;
	compress(h_, t_, f_, buf_);

	for (size_t i = 0; i < HASH_SIZE; i++)
		out[i] = static_cast<uint8_t>(h_[i / 8] >> ((i % 8) * 8));
}

void BLAKE2b::hash(uint8_t* out, const uint8_t* in, uint64_t inlen)
{
	if (out == nullptr || (in == nullptr && inlen != 0))
		return;
	IHash::hash(std::span<uint8_t>{out, HASH_SIZE},
	            std::span<const uint8_t>{in, static_cast<size_t>(inlen)});
}
