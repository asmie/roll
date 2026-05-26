#ifndef IHASH_HPP
#define IHASH_HPP

#include <cstdint>
#include <cstddef>
#include <span>

/**
* Strong hash interface. Implementations expose a three-call state machine
* (init/update/finalize) so callers can stream arbitrary-sized data without
* buffering it, and a one-shot hash() convenience that drives the state
* machine for callers that already have the full input in memory.
*/
class IHash {
public:
	virtual ~IHash() = default;

	/// Digest size, in bytes.
	virtual size_t get_hash_size() const noexcept = 0;

	/// Reset state to start hashing a new message.
	virtual void init() = 0;

	/// Feed more input. Safe to call repeatedly between init() and finalize().
	virtual void update(std::span<const uint8_t> in) = 0;

	/// Write the digest to `out` (must be at least get_hash_size() bytes).
	/// State after this call is unspecified; call init() to start a new run.
	virtual void finalize(std::span<uint8_t> out) = 0;

	/// One-shot convenience. Default implementation drives the streaming API
	/// for any subclass that doesn't have a faster specialised path.
	virtual void hash(std::span<uint8_t> out, std::span<const uint8_t> in) {
		if (out.size() < get_hash_size())
			return;
		init();
		update(in);
		finalize(out);
	}
};


#endif
