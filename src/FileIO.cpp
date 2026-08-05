#include "FileIO.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <system_error>

FileIO::~FileIO()
{
	f_.close();
}

bool FileIO::open(const std::filesystem::path& file_path, FileMode mode)
{
	std::fstream::openmode fmode = std::fstream::binary;

	if (mode == FileMode::IN || mode == FileMode::INOUT)
		fmode |= std::fstream::in;

	if (mode == FileMode::OUT || mode == FileMode::INOUT)
		fmode |= std::fstream::out;

	if (f_.is_open())
		f_.close();

	discard_read_buffer();

	// Clear any failbit/eofbit left over from a previous lifecycle so an open
	// on a fresh path isn't reported as failed.
	f_.clear();
	f_.open(file_path, fmode);

	if (!f_.good()) {
		size_ = 0;
		return false;
	}

	// Record the size up front so read_chunk() can clamp requested lengths
	// without seeking. Queried via <filesystem> rather than seekg/tellg so the
	// stream position stays where the caller expects it.
	std::error_code ec;
	const auto sz = std::filesystem::file_size(file_path, ec);
	size_ = ec ? 0 : static_cast<size_t>(sz);

	return true;
}

bool FileIO::close()
{
	f_.close();
	return !f_.fail();
}

bool FileIO::refill()
{
	rpos_ = 0;
	rlen_ = 0;

	if (!f_.is_open())
		return false;

	if (rbuf_.size() != READ_BUFFER_SIZE)
		rbuf_.resize(READ_BUFFER_SIZE);

	f_.read(reinterpret_cast<char*>(rbuf_.data()),
	        static_cast<std::streamsize>(rbuf_.size()));
	const std::streamsize got = f_.gcount();
	if (got <= 0)
		return false;

	rlen_ = static_cast<size_t>(got);

	// A partial fill leaves eofbit and failbit set. We still hold bytes to hand
	// out, and a positioned read may follow, so clear the flags here and let the
	// next refill rediscover the end of file. is_eof() reports the logical state
	// by also checking whether the buffer is drained.
	if (f_.fail())
		f_.clear();

	return true;
}

void FileIO::unread_buffer()
{
	const size_t buffered = rlen_ - rpos_;
	discard_read_buffer();

	if (buffered > 0) {
		f_.clear();
		f_.seekg(-static_cast<std::streamoff>(buffered), std::ios::cur);
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
	unread_buffer();
	f_.put(byte);
	return f_.good();
}

int FileIO::peek_byte()
{
	if (rpos_ == rlen_ && !refill())
		return EOF;
	return rbuf_[rpos_];
}

std::vector<uint8_t> FileIO::read_chunk(size_t chunk_size)
{
	if (!f_.is_open() || chunk_size == 0)
		return {};

	// Never pre-allocate more than the file could possibly supply. The contract
	// is "read up to chunk_size bytes", so this returns the same bytes — but it
	// stops a corrupt or hostile length field from becoming a huge allocation
	// and an uncaught std::bad_alloc.
	const size_t requested = chunk_size;
	if (size_ != 0 && chunk_size > size_)
		chunk_size = size_;

	std::vector<uint8_t> vec(chunk_size);
	size_t filled = 0;

	// Hand back anything read_byte()/peek_byte() pulled ahead but the caller has
	// not consumed, so the byte-wise and bulk APIs interleave transparently.
	// Apply relies on this: it peeks an opcode tag, then bulk-reads its payload.
	const size_t buffered = rlen_ - rpos_;
	if (buffered > 0) {
		filled = std::min(buffered, chunk_size);
		std::memcpy(vec.data(), rbuf_.data() + rpos_, filled);
		rpos_ += filled;
	}

	if (filled < chunk_size) {
		f_.read(reinterpret_cast<char*>(vec.data() + filled),
		        static_cast<std::streamsize>(chunk_size - filled));
		filled += static_cast<size_t>(f_.gcount());
	}

	vec.resize(filled);  // trim to actually read

	// Clamping must not hide end-of-file from callers. std::istream::read sets
	// eofbit *and* failbit when it cannot supply the full request, and code
	// here relies on that (is_eof(), and truncated-delta detection). Clamping
	// can make the shortened read succeed exactly, so restore the flags that
	// the unclamped request would have raised.
	if (vec.size() < requested)
		f_.setstate(std::ios::eofbit | std::ios::failbit);

	return vec;
}

std::vector<uint8_t> FileIO::read_chunk(size_t chunk_size, size_t position)
{
	// The read-ahead belongs to the old position, and we are about to seek to an
	// absolute offset, so drop it without rewinding.
	discard_read_buffer();

	// Clear EOF/fail state so seekg can re-position after a previous read
	// reached end-of-file (seekg is a no-op while failbit is set).
	f_.clear();
	f_.seekg(position);
	return read_chunk(chunk_size);
}

bool FileIO::write_chunk(std::span<const uint8_t> chunk)
{
	unread_buffer();
	f_.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
	return f_.good();
}

bool FileIO::write_chunk(uint64_t chunk)
{
	unread_buffer();
	if constexpr (std::endian::native == std::endian::little)
		chunk = std::byteswap(chunk);

	f_.write(reinterpret_cast<const char*>(&chunk), sizeof(chunk));
	return f_.good();
}

bool FileIO::write_chunk(const uint8_t* chunk, size_t chunk_size)
{
	return write_chunk(std::span<const uint8_t>{chunk, chunk_size});
}

