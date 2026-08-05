#ifndef SIGNATURE_HPP
#define SIGNATURE_HPP

#include "DeltaFormat.hpp"
#include "FileIO.hpp"
#include "HashConcepts.hpp"

#include <concepts>
#include <filesystem>
#include <span>
#include <vector>

/// Whether generate_signatures should additionally hash the entire input.
enum class WholeFileHash {
	Skip,     ///< Chunk signatures only (default).
	Compute,  ///< Also produce a digest of the whole file, available from
	          ///< whole_file_hash(). Costs one extra hash pass over data that
	          ///< is already being read, not a second read of the file — the
	          ///< emitted chunks contiguously cover the input, so feeding each
	          ///< one to a second hasher digests the file as a side effect.
};

/**
* Structure representing signed chunk of data.
*/
template <class T>
struct SignedChunk {
	T signature;						/*!< Rolling hash signature */
	std::vector<uint8_t> hash;			/*!< Hash (strong) of data */
	size_t start_offset;				/*!< Start offset of data in file */
	size_t chunk_size;					/*!< Size of the chunk */

	// Chunk identity is the strong digest plus the length; start_offset is
	// deliberately excluded so equal content at different offsets compares
	// equal, which is what makes moved-chunk detection work. The rolling
	// signature is excluded too: it exists to find boundaries, and a 128-bit
	// cryptographic digest already decides identity, so including it would add
	// nothing while forcing the wire format to carry it.
	bool operator==(const SignedChunk<T>& rhs) const {
		return hash == rhs.hash && chunk_size == rhs.chunk_size;
	}
};

/**
* Signature class allowing generating signatures for specified file.
* Class is template with two template parameters: the rolling hash algorithm and the hash algorithm.
*/
template <RollingHashAlgorithm T, StrongHashAlgorithm U>
class Signature {
	// Sourced from DeltaFormat.hpp: the delta format bounds what a chunk may
	// be, and the chunker honors that bound rather than defining its own.
	static constexpr size_t MIN_CHUNK_SIZE = DELTA_MIN_CHUNK_SIZE;
	static constexpr size_t MAX_CHUNK_SIZE = DELTA_MAX_CHUNK_SIZE;
	static constexpr size_t TARGET_CHUNK_SIZE = DELTA_TARGET_CHUNK_SIZE;

public:
	/**
	* Generate signatures by opening the given path and processing its contents.
	* @return False if the path could not be opened, true otherwise (chunks may
	*         still be empty for a zero-byte file).
	*/
	[[nodiscard]] bool generate_signatures(const std::filesystem::path& datafile,
	                                       WholeFileHash mode = WholeFileHash::Skip) {
		FileIO file;
		if (!file.open(datafile, FileMode::IN)) {
			chunks.clear();
			whole_hash_.clear();
			return false;
		}
		return generate_signatures(file, mode);
	}

	/**
	* Generate signatures by reading from an already-open FileIO. Data is read
	* from offset 0; the file position at return is unspecified. The FileIO is
	* not closed by this call.
	* @return False if `file` is not open, true otherwise.
	*/
	[[nodiscard]] bool generate_signatures(FileIO& file,
	                                       WholeFileHash mode = WholeFileHash::Skip) {
		chunks.clear();
		whole_hash_.clear();

		if (!file.is_open())
			return false;

		T fingerprint;
		U hash_func;
		size_t bytes_read = 0;

		U whole_hasher;
		U* whole = nullptr;
		if (mode == WholeFileHash::Compute) {
			whole_hasher.init();
			whole = &whole_hasher;
		}
		// Every successful return must run this, including the empty-file one:
		// the digest of an empty input is still a defined value.
		const auto finish = [&]() -> bool {
			if (whole) {
				whole_hash_.resize(whole->get_hash_size());
				whole->finalize(whole_hash_);
			}
			return true;
		};

		auto initial = file.read_chunk(fingerprint.get_window_size(), 0);
		if (initial.empty())
			return finish();  // empty file -> open succeeded, no chunks to emit

		bool full_window = fingerprint.initialize(initial);
		bytes_read += initial.size();

		std::vector<uint8_t> chunk(std::move(initial));
		typename T::RollingHashType current_fingerprint = fingerprint.get_current_fingerprint();

		// File smaller than one window: emit the polynomial-hash signature
		// computed by initialize() and bail — there is nothing to roll.
		if (!full_window) {
			emit_chunk(chunk, current_fingerprint, bytes_read - chunk.size(), hash_func, whole);
			return finish();
		}

		bool init = false;

		while (true)
		{
			if (init)														// init rolling hash for next chunk
			{
				auto next = file.read_chunk(fingerprint.get_window_size());
				if (next.empty())
					break;
				full_window = fingerprint.initialize(next);
				bytes_read += next.size();
				chunk = std::move(next);
				current_fingerprint = fingerprint.get_current_fingerprint();

				if (!full_window)
					break;	// EOF mid-window: emit `chunk` as the residual below.

				init = false;
			}

			int b = file.read_byte();
			if (b == EOF)
				break;
			bytes_read++;
			uint8_t byte = static_cast<uint8_t>(b);
			chunk.push_back(byte);

			current_fingerprint = fingerprint.compute_next(byte);

			// Adaptive boundary detection on the rolling fingerprint: FastCDC-style
			// normalized chunking with a tighter mask before the target size and a
			// looser one after, bounded by MIN_CHUNK_SIZE / MAX_CHUNK_SIZE. The
			// masked fingerprint is compared against DELTA_BOUNDARY_TARGET rather
			// than zero so constant content (whose fingerprint never changes)
			// cannot satisfy the predicate at every byte — see the constant's
			// definition for the analysis. A hit under the tight mask implies a
			// hit under the loose one, preserving the normalization behaviour.
			bool boundary_found = false;
			if (chunk.size() >= MAX_CHUNK_SIZE) {
				boundary_found = true;
			} else if (chunk.size() >= MIN_CHUNK_SIZE) {
				const uint64_t mask = (chunk.size() < TARGET_CHUNK_SIZE)
					? 0x3FFFULL   // 1/16384 below target — discourages early cuts
					: 0x0FFFULL;  // 1/4096  at/above target — encourages cutting near target
				boundary_found = ((current_fingerprint & mask) ==
				                  (DELTA_BOUNDARY_TARGET & mask));
			}

			if (boundary_found)					// chunk boundary found
			{
				emit_chunk(chunk, current_fingerprint, bytes_read - chunk.size(), hash_func, whole);
				chunk.clear();
				init = true;
			}
		}

		if (chunk.size() > 0)									// emit residual chunk at EOF
			emit_chunk(chunk, current_fingerprint, bytes_read - chunk.size(), hash_func, whole);
		return finish();
	}

	/**
	* Get chunk list.
	* @return Vector of signed chunks.
	*/
	[[nodiscard]] const std::vector<SignedChunk<typename T::RollingHashType>>& get_chunks() const noexcept {
		return chunks;
	}

	/**
	* Digest of the entire input from the last generate_signatures call, or an
	* empty vector when the call used WholeFileHash::Skip (the default) or
	* failed.
	*/
	[[nodiscard]] const std::vector<uint8_t>& whole_file_hash() const noexcept {
		return whole_hash_;
	}

private:
	void emit_chunk(const std::vector<uint8_t>& data,
	                typename T::RollingHashType signature,
	                size_t start_offset, U& hash_func, U* whole_hasher) {
		SignedChunk<typename T::RollingHashType> schunk;
		schunk.signature = signature;

		// The delta stores DELTA_DIGEST_BYTES per chunk, so identity and
		// verification both work on that prefix; keeping the full digest in
		// memory would let the two disagree.
		std::vector<uint8_t> full(hash_func.get_hash_size());
		hash_func.hash(full, data);
		full.resize(DELTA_DIGEST_BYTES);
		schunk.hash = std::move(full);

		schunk.start_offset = start_offset;
		schunk.chunk_size = data.size();
		chunks.push_back(std::move(schunk));

		if (whole_hasher)
			whole_hasher->update(data);
	}

	std::vector<SignedChunk<typename T::RollingHashType>> chunks;
	std::vector<uint8_t> whole_hash_;
};



#endif
