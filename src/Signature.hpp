#ifndef SIGNATURE_HPP
#define SIGNATURE_HPP

#include "IHash.hpp"
#include "IRollingHash.hpp"
#include "FileIO.hpp"

#include <string>
#include <vector>
#include <type_traits>

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
template <class T, class U>
class Signature {
	static_assert(std::is_base_of<IRollingHash<typename T::RollingHashType>, T>::value, "T must derive from IRollingHash");
	static_assert(std::is_base_of<IHash, U>::value, "U must derive from IHash");
	
public:
	/**
	* Generate signatures for specified file and keep it in the object.
	* Signatures are generated using provided rolling hash and hash algorithms.
	* @param[in] datafile file with data for signatures to be generated
	*/
	void generate_signatures(std::string datafile) {
		FileIO file;
		T fingerprint;
		U hash_func;
		size_t bytes_read{ 0 };

		file.open(datafile, FileMode::IN);

		if (!file.is_open())
			return;

		auto res = file.read_chunk(fingerprint.get_window_size());
		if (res.get() == nullptr)											// There was no data in file
			return; 
		
		fingerprint.initialize(*res);
		bytes_read += res.get()->size();

		std::vector<uint8_t> chunk(*res);
		res.reset();
		typename T::RollingHashType current_fingerprint{};

		bool init = false;

		while (!file.is_eof())
		{
			if (init)														// init rolling hash for each chunk
			{
				res = file.read_chunk(fingerprint.get_window_size());
				fingerprint.initialize(*res);								// *res can't be null here as there is at least 1 byte in file
				bytes_read += res.get()->size();
				chunk = *res;
				res.reset();

				init = false;
			}
			
			auto b = file.read_byte();
			bytes_read++;
			uint8_t last = chunk.back();
			chunk.push_back(b);

			current_fingerprint = fingerprint.compute_next(b);

			if (((last << 8 | b) & 0x1FFFL) == 0)					// chunk boundary found
			{
				SignedChunk<typename T::RollingHashType> schunk;
				schunk.signature = current_fingerprint;
				schunk.hash.resize(hash_func.get_hash_size());
				hash_func.hash(schunk.hash.data(), chunk.data(), chunk.size());
				schunk.start_offset = bytes_read - chunk.size();
				schunk.chunk_size = chunk.size();
				chunks.push_back(std::move(schunk));
				chunk.clear();
				init = true;
			}
		}

		if (chunk.size() > 0)									// Get rid of the last elements in file
		{
			SignedChunk<typename T::RollingHashType> schunk;
			schunk.signature = current_fingerprint;
			schunk.hash.resize(hash_func.get_hash_size());
			hash_func.hash(schunk.hash.data(), chunk.data(), chunk.size());
			schunk.start_offset = bytes_read - chunk.size();
			schunk.chunk_size = chunk.size();
			chunks.push_back(std::move(schunk));
		}

		file.close();
	}

	/**
	* Get chunk list.
	* @return Vector of signed chunks.
	*/
	const std::vector<struct SignedChunk<typename T::RollingHashType>>& get_chunks() const noexcept {
		return chunks;
	}

private:
	std::vector<struct SignedChunk<typename T::RollingHashType>> chunks;
};



#endif