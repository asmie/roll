#include "FileIO.hpp"

#include <fstream>
#include <string>
#include <future>

FileIO::~FileIO()
{
	f_.close();
}

bool FileIO::open(std::string file_path, FileMode mode)
{
	std::fstream::openmode fmode = std::fstream::binary;

	if (mode == FileMode::IN || mode == FileMode::INOUT)
		fmode |= std::fstream::in;
	
	if (mode == FileMode::OUT || mode == FileMode::INOUT)
		fmode |= std::fstream::out;
	
	try {
		f_.open(file_path, fmode);
	}
	catch (std::fstream::failure&) {
		return false;
	}
	
	if (f_.good())
		return true;
	return false;
}

void FileIO::close()
{
	f_.close();
}

// This can be optimized - there is possibility to read data chunk and store it to the buffer, then 
// read single byte from that buffer. If buffer drops under the specified size we can launch async job
// to get new chunk to the buffer.
uint8_t FileIO::read_byte()
{
	return f_.get();
}

bool FileIO::write_byte(uint8_t byte)
{
	f_.put(byte);
	return f_.good();
}

std::unique_ptr<std::vector<uint8_t>> FileIO::read_chunk(size_t chunk_size)
{
	auto vec = std::make_unique<std::vector<uint8_t>>(chunk_size);
	
	try {
		f_.read(reinterpret_cast<char*>(vec->data()), chunk_size);
		vec.get()->resize(f_.gcount());									// Needed to return only amount of read bytes
	}
	catch (std::fstream::failure&) { }
	
	return vec;
}


std::unique_ptr<std::vector<uint8_t>> FileIO::read_chunk(size_t chunk_size, size_t position)
{
	f_.seekg(position);
	return read_chunk(chunk_size);
}

bool FileIO::write_chunk(std::unique_ptr<std::vector<uint8_t>> buffer)
{
	try {
		f_.write(reinterpret_cast<char*>(buffer->data()), buffer->size());
	}
	catch (std::fstream::failure&) {}
	
	return f_.good();
}

bool FileIO::write_chunk(uint64_t chunk)
{
	try {
		f_.write((const char *) &chunk, sizeof(chunk));
	}
	catch (std::fstream::failure&) {}
	
	return f_.good();
}

bool FileIO::write_chunk(uint8_t* chunk, size_t chunk_size)
{
	try {
		f_.write(reinterpret_cast<char*>(chunk), chunk_size);
	}
	catch (std::fstream::failure&) {}
	
	return f_.good();
}


