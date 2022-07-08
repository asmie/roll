#ifndef FILEIO_HPP
#define FILEIO_HPP

#include <fstream>
#include <vector>
#include <memory>

/**
* Helper enum to specify file access mode.
*/
enum class FileMode {
	IN,
	OUT,
	INOUT
};

/**
* Class responsible for File IO operations.
*/
class FileIO {
public:
	FileIO() = default;
	~FileIO();

	/**
	* Open file with given mode.
	* @param[in] file_path path to the file.
	* @param[in] mode file access mode.
	* @return True if file was opened successfully, false otherwise.
	*/
	bool open(std::string file_path, FileMode mode);

	/**
	* Close previously opened file.
	*/
	void close();

	/**
	* Read single byte from stream.
	* @return Byte read from stream.
	*/
	uint8_t read_byte();

	/**
	* Write single byte to stream.
	* @param[in] byte byte to write.
	* @return True if byte was written successfully, false otherwise.
	*/
	bool write_byte(uint8_t byte);
	
	/**
	* Read multiple bytes from stream.
	* @param[in] chunk_size size of the chunk to read
	* @return Pointer to vector of bytes read from stream.
	*/
	std::unique_ptr<std::vector<uint8_t>> read_chunk(size_t chunk_size);

	/**
	* Read multiple bytes from stream starting from specified position.
	* @param[in] chunk_size size of the chunk to read
	* @param[in] position starting position
	* @return Pointer to vector of bytes read from stream.
	*/
	std::unique_ptr<std::vector<uint8_t>> read_chunk(size_t chunk_size, size_t position);
	
	/**
	* Write multiple bytes to stream.
	* @param[in] buffer pointer to vector of bytes to write.
	* @return True if bytes were written successfully, false otherwise.
	*/
	bool write_chunk(std::unique_ptr<std::vector<uint8_t>> buffer);

	/**
	* Write single value to stream.
	* @param[in] chunk value to write
	* @return True if value was written successfully, false otherwise.
	*/
	bool write_chunk(uint64_t chunk);

	/**
	* Write multiple values to stream.
	* @param[in] chunk pointer to array of values to write
	* @param[in] chunk_size amount of data to be written
	* @return True if values were written successfully, false otherwise.
	*/
	bool write_chunk(uint8_t* chunk, size_t chunk_size);

	/**
	* Check if file is open.
	* @return True if file is opened, otherwise false.
	*/
	bool is_open() const {
		return f_.is_open();
	}

	/**
	* Check if there was EOF reached.
	* @return True if EOF was reached, false otherwise.
	*/
	bool is_eof() const {
		return f_.eof();
	}
private:
	std::fstream f_;
};

#endif