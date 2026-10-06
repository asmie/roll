#ifndef FILEIO_HPP
#define FILEIO_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

/**
* Helper enum to specify file access mode.
*/
enum class FileMode {
	IN,
	OUT,
	INOUT,
	EXCLUSIVE_OUT
};

/**
* Class responsible for File IO operations.
*/
class FileIO {
public:
	FileIO() = default;
	~FileIO();

	// Delete copy constructor and copy assignment to prevent copies
	FileIO(const FileIO&) = delete;
	FileIO& operator=(const FileIO&) = delete;

	// Allow move operations
	FileIO(FileIO&& other);
	FileIO& operator=(FileIO&& other);

	/**
	* Open file with given mode.
	* @param[in] file_path path to the file.
	* @param[in] mode file access mode.
	* @return True if file was opened successfully, false otherwise.
	*/
	[[nodiscard]] bool open(const std::filesystem::path& file_path, FileMode mode);

	/**
	* Close previously opened file.
	* @return True if the file closed cleanly (any pending writes flushed),
	*         false if a flush or close error occurred. Existing callers may
	*         discard the return value.
	*/
	[[nodiscard]] bool close();

	/**
	* Read single byte from stream.
	* @return Byte value (0..255) or EOF (-1) if no byte is available.
	*/
	[[nodiscard]] int read_byte();

	/**
	* Write single byte to stream.
	* @param[in] byte byte to write.
	* @return True if byte was written successfully, false otherwise.
	*/
	[[nodiscard]] bool write_byte(uint8_t byte);

	/**
	* Peek single byte from stream without consuming it.
	* @return Byte value (0..255) or EOF (-1) if no byte is available.
	*/
	[[nodiscard]] int peek_byte();
	
	/**
	* Read up to chunk_size bytes from the current stream position. The returned
	* vector is sized to the actual number of bytes read; an empty vector means
	* EOF or chunk_size == 0.
	*/
	[[nodiscard]] std::vector<uint8_t> read_chunk(size_t chunk_size);

	/**
	* Read up to chunk_size bytes starting from `position`. Resets the stream
	* state first so a prior EOF doesn't suppress the seek.
	*/
	[[nodiscard]] std::vector<uint8_t> read_chunk(size_t chunk_size, size_t position);

	/**
	* Write multiple bytes to stream.
	* @param[in] chunk bytes to write.
	* @return True if bytes were written successfully, false otherwise.
	*/
	[[nodiscard]] bool write_chunk(std::span<const uint8_t> chunk);

	/**
	* Write single value to stream.
	* @param[in] chunk value to write
	* @return True if value was written successfully, false otherwise.
	*/
	[[nodiscard]] bool write_chunk(uint64_t chunk);

	/**
	* Write multiple values to stream.
	* @param[in] chunk pointer to array of values to write
	* @param[in] chunk_size amount of data to be written
	* @return True if values were written successfully, false otherwise.
	*/
	[[nodiscard]] bool write_chunk(const uint8_t* chunk, size_t chunk_size);

	/**
	* Check if file is open.
	* @return True if file is opened, otherwise false.
	*/
	[[nodiscard]] bool is_open() const {
		return f_.is_open();
	}

	/**
	* Check if there was EOF reached.
	* @return True if EOF was reached, false otherwise. Buffered read-ahead
	*         counts as not-yet-EOF: bytes the caller has not consumed are
	*         still available even once the stream itself has hit the end.
	*/
	[[nodiscard]] bool is_eof() const {
		return rpos_ == rlen_ && f_.eof();
	}

	/// Sticky I/O error, distinct from a normal short read at EOF. Reset by open().
	[[nodiscard]] bool has_error() const noexcept { return io_error_; }

	/**
	* Size of the file as measured when open() succeeded, in bytes. Zero if the
	* file is not open. Updated after writes through this handle. Reads do not
	* clamp to this snapshot: another writer may have extended the file.
	*/
	[[nodiscard]] size_t size() const noexcept {
		return size_;
	}
private:
	/// Pull the next block into rbuf_. Returns false at end of file.
	bool refill();

	/// Drop the read-ahead without repositioning. Only valid when the caller is
	/// about to seek to an absolute position anyway.
	void discard_read_buffer() noexcept {
		rpos_ = 0;
		rlen_ = 0;
	}

	/// Drop the read-ahead and rewind the stream to the caller's logical read
	/// position, so un-consumed bytes are not silently skipped by whatever
	/// touches the stream next.
	void unread_buffer();

	std::fstream f_;
	size_t size_ { 0 };
	bool io_error_ { false };
	bool readable_ { false };

	// Read-ahead buffer backing read_byte()/peek_byte(). Signature consumes
	// whole files a byte at a time, and std::fstream::get() per byte measured
	// ~30x slower than bulk reads (0.27s vs 0.01s per 100 MB).
	//
	// Invariant: rbuf_[rpos_, rlen_) holds bytes already pulled from the stream
	// that the caller has not consumed. The caller's logical read position is
	// therefore the stream position minus (rlen_ - rpos_), and any operation
	// needing the stream itself to sit at the logical position must first
	// discard or unread the buffer.
	static constexpr size_t READ_BUFFER_SIZE = 64 * 1024;
	std::vector<uint8_t> rbuf_;
	size_t rpos_ { 0 };
	size_t rlen_ { 0 };
};

#endif
