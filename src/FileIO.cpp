#include "FileIO.hpp"

#include <bit>
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

// This can be optimized - there is possibility to read data chunk and store it to the buffer, then 
// read single byte from that buffer. If buffer drops under the specified size we can launch async job
// to get new chunk to the buffer.
int FileIO::read_byte()
{
	return f_.get();
}

bool FileIO::write_byte(uint8_t byte)
{
	f_.put(byte);
	return f_.good();
}

int FileIO::peek_byte()
{
	return f_.peek();
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
	f_.read(reinterpret_cast<char*>(vec.data()), chunk_size);
	vec.resize(static_cast<size_t>(f_.gcount()));  // trim to actually read

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
	// Clear EOF/fail state so seekg can re-position after a previous read
	// reached end-of-file (seekg is a no-op while failbit is set).
	f_.clear();
	f_.seekg(position);
	return read_chunk(chunk_size);
}

bool FileIO::write_chunk(std::span<const uint8_t> chunk)
{
	f_.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
	return f_.good();
}

bool FileIO::write_chunk(uint64_t chunk)
{
	if constexpr (std::endian::native == std::endian::little)
		chunk = std::byteswap(chunk);

	f_.write(reinterpret_cast<const char*>(&chunk), sizeof(chunk));
	return f_.good();
}

bool FileIO::write_chunk(const uint8_t* chunk, size_t chunk_size)
{
	return write_chunk(std::span<const uint8_t>{chunk, chunk_size});
}

