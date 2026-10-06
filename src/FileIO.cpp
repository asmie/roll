#include "FileIO.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

FileIO::FileIO(FileIO&& other)
	: f_(std::move(other.f_)), size_(std::exchange(other.size_, 0)),
	  io_error_(std::exchange(other.io_error_, false)), readable_(std::exchange(other.readable_, false)),
	  rbuf_(std::move(other.rbuf_)), rpos_(std::exchange(other.rpos_, 0)), rlen_(std::exchange(other.rlen_, 0))
{}

FileIO& FileIO::operator=(FileIO&& other)
{
	if (this == &other) return *this;
	const bool closed = close();
	f_ = std::move(other.f_);
	size_ = std::exchange(other.size_, 0);
	io_error_ = std::exchange(other.io_error_, false) || !closed;
	readable_ = std::exchange(other.readable_, false);
	rbuf_ = std::move(other.rbuf_);
	rpos_ = std::exchange(other.rpos_, 0);
	rlen_ = std::exchange(other.rlen_, 0);
	return *this;
}

FileIO::~FileIO()
{
	f_.close();
}

bool FileIO::open(const std::filesystem::path& file_path, FileMode mode)
{
	std::fstream::openmode fmode = std::fstream::binary;

	if (mode == FileMode::IN || mode == FileMode::INOUT)
		fmode |= std::fstream::in;

	if (mode == FileMode::OUT || mode == FileMode::INOUT || mode == FileMode::EXCLUSIVE_OUT)
		fmode |= std::fstream::out;
	if (mode == FileMode::EXCLUSIVE_OUT)
		fmode |= std::ios::noreplace;

	if (f_.is_open())
		f_.close();

	discard_read_buffer();
	size_ = 0;
	io_error_ = false;
	readable_ = mode == FileMode::IN || mode == FileMode::INOUT;
	std::error_code ec;
	if (readable_ && !std::filesystem::is_regular_file(file_path, ec)) {
		io_error_ = true;
		return false;
	}

	// Clear any failbit/eofbit left over from a previous lifecycle so an open
	// on a fresh path isn't reported as failed.
	f_.clear();
	f_.open(file_path, fmode);

	if (!f_.good()) {
		io_error_ = true;
		return false;
	}

	// Size is a snapshot for callers, not a limit on later reads.
	const auto sz = std::filesystem::file_size(file_path, ec);
	if (readable_ && (ec || sz > std::numeric_limits<size_t>::max())) {
		io_error_ = true;
		f_.close();
		return false;
	}
	size_ = ec ? 0 : static_cast<size_t>(sz);

	return true;
}

bool FileIO::close()
{
	discard_read_buffer();

	if (!f_.is_open())
		return true;  // nothing open: nothing could fail

	// Report whether *this* close succeeded. Returning !fail() outright made a
	// benign earlier read report failure, because a short read at EOF sets
	// failbit — so a caller checking close() on a fully-consumed file would see
	// a flush error that never happened. badbit is different: it marks a real
	// stream error, so preserve it across the clear.
	const bool had_stream_error = f_.bad();
	f_.clear();
	f_.close();

	io_error_ = io_error_ || f_.fail() || had_stream_error;
	size_ = 0;
	return !io_error_;
}

bool FileIO::refill()
{
	rpos_ = 0;
	rlen_ = 0;

	if (!f_.is_open() || !readable_ || io_error_) {
		io_error_ = true;
		return false;
	}

	if (rbuf_.size() != READ_BUFFER_SIZE)
		rbuf_.resize(READ_BUFFER_SIZE);

	f_.read(reinterpret_cast<char*>(rbuf_.data()),
	        static_cast<std::streamsize>(rbuf_.size()));
	const std::streamsize got = f_.gcount();
	io_error_ = f_.bad() || (f_.fail() && !f_.eof());
	if (got <= 0)
		return false;

	rlen_ = static_cast<size_t>(got);

	// Keep EOF/error state; buffered bytes remain available until consumed.

	return true;
}

void FileIO::unread_buffer()
{
	const size_t buffered = rlen_ - rpos_;
	discard_read_buffer();

	if (buffered > 0) {
		f_.clear();
		f_.seekg(-static_cast<std::streamoff>(buffered), std::ios::cur);
		io_error_ = io_error_ || f_.fail();
	}
}

int FileIO::read_byte()
{
	if (rpos_ == rlen_ && !refill())
		return EOF;
	return rbuf_[rpos_++];
}

bool FileIO::write_byte(uint8_t byte)
{
	return write_chunk(std::span<const uint8_t>(&byte, 1));
}

int FileIO::peek_byte()
{
	if (rpos_ == rlen_ && !refill())
		return EOF;
	return rbuf_[rpos_];
}

std::vector<uint8_t> FileIO::read_chunk(size_t chunk_size)
{
	if (chunk_size == 0)
		return {};
	if (!f_.is_open() || !readable_ || io_error_) {
		io_error_ = true;
		return {};
	}

	// Grow by bounded steps, so an empty file and a hostile request never
	// trigger an allocation proportional to the requested length.
	std::vector<uint8_t> vec;
	while (vec.size() < chunk_size) {
		const size_t step = std::min(chunk_size - vec.size(), READ_BUFFER_SIZE);
		const size_t start = vec.size();
		vec.resize(start + step);
		const size_t buffered = std::min(step, rlen_ - rpos_);
		if (buffered != 0) {
			std::memcpy(vec.data() + start, rbuf_.data() + rpos_, buffered);
			rpos_ += buffered;
		}
		size_t got = buffered;
		if (got < step) {
			f_.read(reinterpret_cast<char*>(vec.data() + start + got),
			        static_cast<std::streamsize>(step - got));
			got += static_cast<size_t>(f_.gcount());
			io_error_ = f_.bad() || (f_.fail() && !f_.eof());
		}
		vec.resize(start + got);
		if (got < step || io_error_)
			break;
	}

	return vec;
}

std::vector<uint8_t> FileIO::read_chunk(size_t chunk_size, size_t position)
{
	// The read-ahead belongs to the old position, and we are about to seek to an
	// absolute offset, so drop it without rewinding.
	discard_read_buffer();

	// Clear EOF/fail state so seekg can re-position after a previous read
	// reached end-of-file (seekg is a no-op while failbit is set).
	if (!f_.is_open() || !readable_ || io_error_ ||
	    position > static_cast<size_t>(std::numeric_limits<std::streamoff>::max())) {
		io_error_ = true;
		return {};
	}
	f_.clear();
	f_.seekg(static_cast<std::streamoff>(position));
	if (f_.fail()) {
		io_error_ = true;
		return {};
	}
	return read_chunk(chunk_size);
}

bool FileIO::write_chunk(std::span<const uint8_t> chunk)
{
	if (io_error_) return false;
	unread_buffer();
	if (io_error_ || chunk.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
		io_error_ = true;
		return false;
	}
	f_.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
	io_error_ = !f_.good();
	if (!io_error_) {
		if (!readable_) size_ += chunk.size();
		else {
			const auto pos = f_.tellp();
			if (pos >= 0) size_ = std::max(size_, static_cast<size_t>(pos));
			else io_error_ = true;
		}
	}
	return !io_error_;
}

bool FileIO::write_chunk(uint64_t chunk)
{
	if constexpr (std::endian::native == std::endian::little)
		chunk = std::byteswap(chunk);

	return write_chunk(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&chunk), sizeof(chunk)));
}

bool FileIO::write_chunk(const uint8_t* chunk, size_t chunk_size)
{
	return write_chunk(std::span<const uint8_t>{chunk, chunk_size});
}
