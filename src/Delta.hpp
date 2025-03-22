#ifndef DELTA_HPP
#define DELTA_HPP

#include "Signature.hpp"
#include "FileIO.hpp"
#include "Delta.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

/**
* Entry type for each delta record.
* Each record represent original, added, modified or removed chunk.
*/
enum class EntryType {
	ORIGINAL_CHUNK,
	ADDED_CHUNK,
	MODIFIED_CHUNK,
	REMOVED_CHUNK
};

/**
* Structure representing single delta record.
*/
template <class T>
struct DeltaEntry {
	EntryType type;									/*!< Entry type (original, added etc) */
	SignedChunk<T> chunk_data;						/*!< Signed chunk structure connected to the delta */
	std::vector<uint8_t> chunk_data_raw;			/*!< Raw chunk data (only for added and modified) */
};


/**
* Class for generating deltas between two files using their signatures.
*/
template<class T, class U>
class Delta {
public:
	static_assert(std::is_base_of<IRollingHash<typename T::RollingHashType>, T>::value, "T must derive from IRollingHash");
	static_assert(std::is_base_of<IHash, U>::value, "U must derive from IHash");
	
	/**
	* Generates delta between two files.
	* @param[in] original original file signatures
	* @param[in] newfile new file signatures
	* @param[in] oldfile old file path
	* @param[in] file_to_check new file path
	* @param[in] delta_file delta file path
	*/
	void generate_delta(const Signature<T, U>& original, const Signature<T, U>& newfile, std::string oldfile, std::string file_to_check, std::string delta_file)
	{
		std::vector<DeltaEntry<typename T::RollingHashType>> deltas;
		FileIO old;
		FileIO file;
		FileIO delta;
		T fingerprint;
		
		auto original_chunks = original.get_chunks();
		auto new_chunks = newfile.get_chunks();

		old.open(oldfile, FileMode::IN);
		file.open(file_to_check, FileMode::IN);
		delta.open(delta_file, FileMode::OUT);

		if (!file.is_open() || !old.is_open() || !delta.is_open())
			return;

		size_t original_position = 0;
		size_t new_position = 0;
		while (new_position < new_chunks.size() || original_position < original_chunks.size())
		{
			if (original_position >= original_chunks.size()) {		// New file has more chunks than old file - add all new chunks
				DeltaEntry<typename T::RollingHashType> de;
				de.type = EntryType::ADDED_CHUNK;
				de.chunk_data = new_chunks[new_position];
				de.chunk_data_raw = std::move(*(file.read_chunk(de.chunk_data.chunk_size, de.chunk_data.start_offset).get()));
				deltas.push_back(de);
				new_position++;
				continue;
			}

			if (new_position >= new_chunks.size()) {				// New file has less chunks than old file - remove all chunks
				DeltaEntry<typename T::RollingHashType> de;
				de.type = EntryType::REMOVED_CHUNK;
				de.chunk_data = original_chunks[original_position];
				deltas.push_back(de);
				original_position++;
				continue;
			}

			if (new_chunks[new_position] == original_chunks[original_position]) {		// Chunks are equal on the some positions
				DeltaEntry<typename T::RollingHashType> de;
				de.type = EntryType::ORIGINAL_CHUNK;
				de.chunk_data = original_chunks[original_position];
				deltas.push_back(de);
				original_position++;
				new_position++;
				continue;
			}
			
			bool isFound = false;
			// Try to find the same chunk on the other further position, if so, chunks need to be removed
			for (auto j = original_position; j < original_chunks.size(); j++) {
				if (new_chunks[new_position] == original_chunks[j]) {			// Chunk found in original file further
					while (original_position != j) {
						DeltaEntry<typename T::RollingHashType> de;
						de.type = EntryType::REMOVED_CHUNK;
						de.chunk_data = original_chunks[original_position];
						deltas.push_back(de);
						original_position++;
					}											// We need to add all chunks as removed deltas
					isFound = true;
					break;
				}
			}

			if (isFound)
				continue;						// Chunk found, don't increment new_position as we will compare this chunk in next loop
			
			// If chunk is not found on the other position, it is added or modified
			// If next chunks are ok, this chunk is modified
			// If next chunks are not ok, this chunk can be treated as added
			for (auto j = original_position; j < original_chunks.size(); j++) {
				//std::cout << "Comparing pos: " << new_position  << " with " << j << ": " << new_chunks[new_position + 1].signature << " and " << original_chunks[j].signature << std::endl;
				if (new_chunks[new_position+1] == original_chunks[j]) {			// Chunk found - modify chunk
					DeltaEntry<typename T::RollingHashType> de;
					de.type = EntryType::MODIFIED_CHUNK;
					de.chunk_data = new_chunks[new_position];
					de.chunk_data_raw = std::move(*(
						make_diff(old.read_chunk(de.chunk_data.chunk_size, de.chunk_data.start_offset), file.read_chunk(de.chunk_data.chunk_size, de.chunk_data.start_offset))));
					deltas.push_back(de);
					isFound = true;
					new_position++;
					original_position++;
					break;
				}
			}
			
			if (isFound)
				continue;
			
			// If chunk is not found on the other position, it is added
			DeltaEntry<typename T::RollingHashType> de;
			de.type = EntryType::ADDED_CHUNK;
			de.chunk_data = new_chunks[new_position];
			de.chunk_data_raw = std::move(*(file.read_chunk(de.chunk_data.chunk_size, de.chunk_data.start_offset).get()));
			deltas.push_back(de);
			new_position++;
		}

		int i = 0;
		for (auto& v : deltas)
		{
			delta.write_chunk(std::to_underlying(v.type));
			delta.write_chunk(v.chunk_data.signature);
			delta.write_chunk(v.chunk_data.hash.data(), v.chunk_data.hash.size());
			delta.write_chunk(v.chunk_data.chunk_size);
			if (v.type == EntryType::ADDED_CHUNK || v.type == EntryType::MODIFIED_CHUNK)
				delta.write_chunk(v.chunk_data_raw.data(), v.chunk_data_raw.size());
		}
		
		old.close();
		file.close();
		delta.close();
	}


	/**
	* Create a data vector with differences between two data chunks
	* This could be really optimized to produce more efficient data diff - now we are changing every single byte but
	* there could be done it to incorporate slices of data and also we can try to find original data with shifts (now shifts are ignored).
	* This solution is cheap when little changes are made in the data but when there are many single changes the cost is bigger.
	* The format for now is:
	* A/M/R pos byte
	* like:
	* M0300000049	which means modification of 4th byte to 0x49.
	*/
	std::unique_ptr<std::vector<uint8_t>> make_diff(std::unique_ptr<std::vector<uint8_t>> b1, std::unique_ptr<std::vector<uint8_t>> b2)
	{
		std::unique_ptr<std::vector<uint8_t>> diff = std::make_unique<std::vector<uint8_t>>();

		size_t i = 0;
		size_t j = 0;
		
		while (i < b1->size() && j < b2->size())
		{
			if (b1->at(i) != b2->at(j))
			{

				// Write the position
				diff->push_back('M');			// Modify byte
				diff->push_back(i << 24);
				diff->push_back(i << 16);
				diff->push_back(i << 8);
				diff->push_back(i);

				// Write the new value
				diff->push_back(b1->at(i));
			}
			i++;
			j++;
		}

		while (i < b1->size())				// Some data left - we need to remove it.
		{
			// Write the position
			diff->push_back('R');			// Remove byte
			diff->push_back(i << 24);
			diff->push_back(i << 16);
			diff->push_back(i << 8);
			diff->push_back(i);
			i++;
		}
		
		while (j < b2->size())			// Some data left - we need to add it.
		{
			// Write the position
			diff->push_back('A');			// Add byte
			diff->push_back(j << 24);
			diff->push_back(j << 16);
			diff->push_back(j << 8);
			diff->push_back(j);

			// Write the new value
			diff->push_back(b2->at(j));
			j++;
		}
		
		return diff;
	}
};



#endif