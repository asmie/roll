#include "DeltaViewer.hpp"

#include "DeltaCodec.hpp"
#include "DeltaFormat.hpp"
#include "FileIO.hpp"
#include "blake2b.h"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* entryTypeToString(EntryType type) {
	switch (type) {
		case EntryType::ORIGINAL_CHUNK: return "ORIGINAL";
		case EntryType::ADDED_CHUNK:    return "ADDED";
		case EntryType::MODIFIED_CHUNK: return "MODIFIED";
	}
	return "UNKNOWN";
}

std::string printableByte(uint8_t byte) {
	switch (byte) {
		case '\n': return "\\n";
		case '\r': return "\\r";
		case '\t': return "\\t";
		default:
			if (std::isprint(byte)) return std::string(1, static_cast<char>(byte));
			return ".";
	}
}

void printBytePreview(const std::vector<uint8_t>& data, size_t limit) {
	for (size_t i = 0; i < limit && i < data.size(); i++) {
		std::cout << printableByte(data[i]);
	}
	if (data.size() > limit) std::cout << "...";
}

std::string hexByte(uint8_t byte) {
	std::ostringstream out;
	out << "0x" << std::hex << std::setw(2) << std::setfill('0')
	    << static_cast<int>(byte);
	return out.str();
}

void printHex(const std::vector<uint8_t>& data) {
	for (const uint8_t b : data)
		std::cout << std::hex << std::setw(2) << std::setfill('0')
		          << static_cast<int>(b);
	std::cout << std::dec;
}

void printInlineBytes(const std::vector<uint8_t>& data) {
	constexpr size_t maxBytes = 12;
	std::cout << "\"";
	printBytePreview(data, maxBytes);
	std::cout << "\"";

	if (!data.empty()) {
		std::cout << " [";
		for (size_t i = 0; i < maxBytes && i < data.size(); i++) {
			if (i != 0) std::cout << " ";
			std::cout << hexByte(data[i]);
		}
		if (data.size() > maxBytes) std::cout << " ...";
		std::cout << "]";
	}
}

// Walk the diff opcode run, printing the first few and tallying the rest. The
// parsing itself lives in DeltaReader, so the viewer and the applier cannot
// disagree about the encoding.
bool parseDiffData(DeltaReader& reader, size_t& diffSize, size_t& opCount) {
	constexpr size_t maxPrintedOps = 10;
	diffSize = 0;
	opCount = 0;

	while (reader.at_diff_opcode()) {
		DiffOpcode opcode;
		if (!reader.read_diff_opcode(opcode))
			return false;

		diffSize += 1 + sizeof(uint32_t);
		if (opcode.op == 'X')
			diffSize += sizeof(uint32_t);
		else
			diffSize += 1 + opcode.bytes.size();

		if (opCount < maxPrintedOps) {
			if (opcode.op == 'X') {
				std::cout << "        Delete " << opcode.delete_length
				          << " byte(s) at position " << opcode.pos << std::endl;
			} else {
				std::cout << "        "
				          << (opcode.op == 'D' ? "Replace" : "Insert")
				          << " " << opcode.bytes.size() << " byte(s) at position "
				          << opcode.pos << ": ";
				printInlineBytes(opcode.bytes);
				std::cout << std::endl;
			}
		}

		opCount++;
	}

	if (opCount > maxPrintedOps) {
		std::cout << "        ... and " << (opCount - maxPrintedOps)
		          << " more diff opcodes" << std::endl;
	}
	return reader.finish_diff(opCount);
}

} // namespace

int view_delta(const std::filesystem::path& delta_file) {
	FileIO file;
	if (!file.open(delta_file, FileMode::IN)) {
		std::cerr << "Error: Cannot open file " << delta_file << std::endl;
		return 2;
	}

	std::cout << "Delta File Viewer - Analyzing: " << delta_file << std::endl;
	std::cout << "========================================" << std::endl << std::endl;

	// The digest length comes from the hash the tool is built with rather than a
	// literal, so swapping the strong hash cannot desynchronise the viewer from
	// the applier. The viewer is specialised to the shipped hash because it is
	// not templated the way Delta/Apply are.
	DeltaReader reader(file, BLAKE2b::HASH_SIZE);
	if (!reader.read_header()) {
		std::cerr << "Error: " << reader.error() << std::endl;
		return reader.io_error() ? 2 : 1;
	}
	std::cout << "Format version: " << DELTA_FORMAT_VERSION << std::endl << std::endl;

	size_t chunkNum = 0;
	bool saw_trailer = false;
	while (true) {
		const auto item = reader.next_item();
		if (item == DeltaReader::Item::End) break;

		if (item == DeltaReader::Item::Trailer) {
			std::vector<uint8_t> trailer;
			if (!reader.read_trailer(trailer)) {
				std::cerr << "Error: " << reader.error() << std::endl;
				return reader.io_error() ? 2 : 1;
			}
			std::cout << "Whole-file hash: ";
			printHex(trailer);
			std::cout << std::endl;
			saw_trailer = true;
			break;
		}

		DeltaEntryHeader header;
		if (!reader.read_entry_header(header)) {
			std::cerr << "Error: " << reader.error() << " in chunk #"
			          << (chunkNum + 1) << std::endl;
			return reader.io_error() ? 2 : 1;
		}

		std::cout << "Chunk #" << ++chunkNum << ":" << std::endl;
		std::cout << "  Type: " << entryTypeToString(header.type)
		          << " (" << static_cast<unsigned>(header.type) << ")" << std::endl;
		if (header.references_old())
			std::cout << "  Source: old chunk #" << header.old_index << std::endl;
		if (header.type != EntryType::ORIGINAL_CHUNK)
			std::cout << "  Output Size: " << header.out_size << " bytes" << std::endl;
		std::cout << "  Digest: ";
		printHex(header.digest);
		std::cout << std::endl;

		if (header.type == EntryType::ADDED_CHUNK) {
			std::vector<uint8_t> rawData;
			if (!reader.read_payload(static_cast<size_t>(header.out_size), rawData)) {
				std::cerr << "Error: Truncated ADDED payload in chunk #"
				          << chunkNum << std::endl;
				return reader.io_error() ? 2 : 1;
			}

			std::cout << "  Added Data (first 50 chars): \"";
			printBytePreview(rawData, 50);
			std::cout << "\"" << std::endl;

		} else if (header.type == EntryType::MODIFIED_CHUNK) {
			size_t diffSize = 0;
			size_t opCount = 0;

			std::cout << "  Diff Operations:" << std::endl;
			if (!parseDiffData(reader, diffSize, opCount)) {
				std::cerr << "Error: " << reader.error() << " in chunk #"
				          << chunkNum << std::endl;
				return reader.io_error() ? 2 : 1;
			}

			std::cout << "  Diff Data Size: " << diffSize << " bytes" << std::endl;
			std::cout << "  Diff Opcode Count: " << opCount << std::endl;

		}

		std::cout << std::endl;
	}

	if (!reader.finish(saw_trailer)) {
		std::cerr << "Error: " << reader.error() << std::endl;
		return reader.io_error() ? 2 : 1;
	}
	std::cout << "========================================" << std::endl;
	std::cout << "Total chunks processed: " << chunkNum << std::endl;

	return 0;
}
