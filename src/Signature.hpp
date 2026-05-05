#ifndef SIGNATURE_HPP
#define SIGNATURE_HPP

#include "IHash.hpp"
#include "IRollingHash.hpp"
#include "FileIO.hpp"

#include <filesystem>
#include <vector>
#include <concepts>

template <class T>
concept RollingHashAlgorithm =
	requires { typename T::RollingHashType; } &&
	std::derived_from<T, IRollingHash<typename T::RollingHashType>>;

template <class U>
concept StrongHashAlgorithm = std::derived_from<U, IHash>;

/**
* Structure representing signed chunk of data.
*/
template <class T>
struct SignedChunk {
	T signature;						/*!< Rolling hash signature */
	std::vector<uint8_t> hash;			/*!< Hash (strong) of data */
	size_t start_offset;				/*!< Start offset of data in file */
	size_t chunk_size;					/*!< Size of the chunk */

	bool operator==(const SignedChunk<T>& rhs) const {						// Check if two chunks are equal (don't check start offset)
		return signature == rhs.signature && hash == rhs.hash && chunk_size == rhs.chunk_size;
	}
};

/**
* Signature class allowing generating signatures for specified file.
* Class is template with two template parameters: the rolling hash algorithm and the hash algorithm.
*/
template <RollingHashAlgorithm T, StrongHashAlgorithm U>
class Signature {
	static constexpr size_t MIN_CHUNK_SIZE = 512;     // Minimum chunk size in bytes
	static constexpr size_t MAX_CHUNK_SIZE = 16384;   // Maximum chunk size in bytes (16KB)
	static constexpr size_t TARGET_CHUNK_SIZE = 8192;  // Target average chunk size

public:
	/**
	* Generate signatures by opening the given path and processing its contents.
	* @param[in] datafile file with data for signatures to be generated
	*/
	void generate_signatures(const std::filesystem::path& datafile) {
		FileIO file;
		if (!file.open(datafile, FileMode::IN))
			return;
		generate_signatures(file);
	}

	/**
	* Generate signatures by reading from an already-open FileIO. Data is read
	* from offset 0; the file position at return is unspecified. The FileIO is
	* not closed by this call.
	* @param[in] file open FileIO to read from
	*/
	void generate_signatures(FileIO& file) {
		if (!file.is_open())
			return;

		T fingerprint;
		U hash_func;
		size_t bytes_read = 0;

		auto res = file.read_chunk(fingerprint.get_window_size(), 0);
		if (res->empty())
			return;

		fingerprint.initialize(*res);
		bytes_read += res->size();

		std::vector<uint8_t> chunk(*res);
		res.reset();
		typename T::RollingHashType current_fingerprint{};

		bool init = false;

		while (true)
		{
			if (init)														// init rolling hash for next chunk
			{
				res = file.read_chunk(fingerprint.get_window_size());
				if (res->empty())
					break;
				fingerprint.initialize(*res);
				bytes_read += res->size();
				chunk = *res;
				res.reset();

				init = false;
			}

			int b = file.read_byte();
			if (b == EOF)
				break;
			bytes_read++;
			uint8_t byte = static_cast<uint8_t>(b);
			uint8_t last = chunk.back();
			chunk.push_back(byte);

			current_fingerprint = fingerprint.compute_next(byte);

			// Adaptive boundary detection based on chunk size
			bool boundary_found = false;
			if (chunk.size() >= MAX_CHUNK_SIZE) {
				// Force boundary at maximum chunk size
				boundary_found = true;
			} else if (chunk.size() >= MIN_CHUNK_SIZE) {
				// Use adaptive mask based on chunk size for better small file handling
				uint32_t mask;
				if (chunk.size() < 2048) {
					mask = 0x1FF;  // 1/512 probability for small chunks
				} else if (chunk.size() < 4096) {
					mask = 0x7FF;  // 1/2048 probability for medium chunks
				} else {
					mask = 0x1FFF; // 1/8192 probability for large chunks
				}
				boundary_found = (((last << 8 | byte) & mask) == 0);
			}

			if (boundary_found)					// chunk boundary found
			{
				SignedChunk<typename T::RollingHashType> schunk;
				schunk.signature = current_fingerprint;
				schunk.hash.resize(hash_func.get_hash_size());
				hash_func.hash(schunk.hash, chunk);
				schunk.start_offset = bytes_read - chunk.size();
				schunk.chunk_size = chunk.size();
				chunks.push_back(std::move(schunk));
				chunk.clear();
				init = true;
			}
		}

		if (chunk.size() > 0)									// emit residual chunk at EOF
		{
			SignedChunk<typename T::RollingHashType> schunk;
			schunk.signature = current_fingerprint;
			schunk.hash.resize(hash_func.get_hash_size());
			hash_func.hash(schunk.hash, chunk);
			schunk.start_offset = bytes_read - chunk.size();
			schunk.chunk_size = chunk.size();
			chunks.push_back(std::move(schunk));
		}
	}

	/**
	* Get chunk list.
	* @return Vector of signed chunks.
	*/
	const std::vector<SignedChunk<typename T::RollingHashType>>& get_chunks() const noexcept {
		return chunks;
	}

private:
	std::vector<SignedChunk<typename T::RollingHashType>> chunks;
};



#endif
