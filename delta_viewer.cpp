#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>

enum class EntryType : uint64_t {
    ORIGINAL_CHUNK = 0,
    ADDED_CHUNK = 1,
    MODIFIED_CHUNK = 2,
    REMOVED_CHUNK = 3
};

const char* entryTypeToString(EntryType type) {
    switch(type) {
        case EntryType::ORIGINAL_CHUNK: return "ORIGINAL";
        case EntryType::ADDED_CHUNK: return "ADDED";
        case EntryType::MODIFIED_CHUNK: return "MODIFIED";
        case EntryType::REMOVED_CHUNK: return "REMOVED";
        default: return "UNKNOWN";
    }
}

bool readExact(std::ifstream& file, void* out, std::streamsize size) {
    file.read(reinterpret_cast<char*>(out), size);
    return file.gcount() == size;
}

bool readUint64(std::ifstream& file, uint64_t& value) {
    return readExact(file, &value, sizeof(value));
}

bool readUint32BE(std::ifstream& file, uint32_t& value) {
    uint8_t bytes[4]{};
    if (!readExact(file, bytes, sizeof(bytes))) return false;
    value = (static_cast<uint32_t>(bytes[0]) << 24) |
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) |
            static_cast<uint32_t>(bytes[3]);
    return true;
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

bool parseDiffData(std::ifstream& file, size_t& diffSize, size_t& opCount, std::string& error) {
    constexpr size_t maxPrintedOps = 10;
    diffSize = 0;
    opCount = 0;

    while (true) {
        int next = file.peek();
        if (next == EOF || (next != 'D' && next != 'X' && next != 'I')) break;

        char op = static_cast<char>(file.get());
        diffSize += 1;
        uint32_t pos = 0;
        if (!readUint32BE(file, pos)) {
            error = "Truncated diff opcode: missing position";
            return false;
        }
        diffSize += sizeof(uint32_t);

        if (op == 'D' || op == 'I') {
            int countRaw = file.get();
            if (countRaw == EOF) {
                error = "Truncated diff opcode: missing byte count";
                return false;
            }

            const auto count = static_cast<size_t>(static_cast<uint8_t>(countRaw));
            diffSize += 1 + count;
            std::vector<uint8_t> bytes(count);
            if (count > 0 && !readExact(file, bytes.data(), static_cast<std::streamsize>(count))) {
                error = "Truncated diff opcode: missing inline bytes";
                return false;
            }

            if (opCount < maxPrintedOps) {
                std::cout << "        "
                          << (op == 'D' ? "Replace" : "Insert")
                          << " " << count << " byte(s) at position " << pos << ": ";
                printInlineBytes(bytes);
                std::cout << std::endl;
            }
        } else {
            uint32_t length = 0;
            if (!readUint32BE(file, length)) {
                error = "Truncated diff opcode: missing delete length";
                return false;
            }
            diffSize += sizeof(uint32_t);

            if (opCount < maxPrintedOps) {
                std::cout << "        Delete " << length
                          << " byte(s) at position " << pos << std::endl;
            }
        }

        opCount++;
    }

    if (opCount > maxPrintedOps) {
        std::cout << "        ... and " << (opCount - maxPrintedOps)
                  << " more diff opcodes" << std::endl;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <delta_file>" << std::endl;
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Error: Cannot open file " << argv[1] << std::endl;
        return 1;
    }

    std::cout << "Delta File Viewer - Analyzing: " << argv[1] << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    constexpr size_t hashSize = 64; // BLAKE-512
    int chunkNum = 0;
    while (true) {
        if (file.peek() == EOF) break;

        uint64_t entryType;
        if (!readUint64(file, entryType)) {
            std::cerr << "Error: Truncated delta entry type" << std::endl;
            return 1;
        }

        uint64_t signature;
        if (!readUint64(file, signature)) {
            std::cerr << "Error: Truncated signature in chunk #" << (chunkNum + 1) << std::endl;
            return 1;
        }

        // Read 64-byte hash (BLAKE-512)
        std::vector<uint8_t> hash(hashSize);
        if (!readExact(file, hash.data(), hashSize)) {
            std::cerr << "Error: Truncated hash in chunk #" << (chunkNum + 1) << std::endl;
            return 1;
        }

        uint64_t chunkSize;
        if (!readUint64(file, chunkSize)) {
            std::cerr << "Error: Truncated chunk size in chunk #" << (chunkNum + 1) << std::endl;
            return 1;
        }

        if (entryType > static_cast<uint64_t>(EntryType::REMOVED_CHUNK)) {
            std::cerr << "Error: Unknown entry type " << entryType
                      << " in chunk #" << (chunkNum + 1) << std::endl;
            return 1;
        }

        std::cout << "Chunk #" << ++chunkNum << ":" << std::endl;
        std::cout << "  Type: " << entryTypeToString(static_cast<EntryType>(entryType))
                  << " (" << entryType << ")" << std::endl;
        std::cout << "  Signature: 0x" << std::hex << signature << std::dec << std::endl;
        std::cout << "  Chunk Size: " << chunkSize << " bytes" << std::endl;
        std::cout << "  Hash (first 8 bytes): ";
        for (size_t i = 0; i < 8 && i < hash.size(); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << (int)hash[i] << " ";
        }
        std::cout << "..." << std::dec << std::endl;

        // Read raw data for ADDED or MODIFIED chunks
        if (entryType == static_cast<uint64_t>(EntryType::ADDED_CHUNK)) {
            if (chunkSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                std::cerr << "Error: ADDED payload is too large to inspect" << std::endl;
                return 1;
            }

            // For ADDED chunks, read chunkSize bytes
            std::vector<uint8_t> rawData(static_cast<size_t>(chunkSize));
            if (!rawData.empty() &&
                !readExact(file, rawData.data(), static_cast<std::streamsize>(rawData.size()))) {
                std::cerr << "Error: Truncated ADDED payload in chunk #" << chunkNum << std::endl;
                return 1;
            }

            std::cout << "  Added Data (first 50 chars): \"";
            printBytePreview(rawData, 50);
            std::cout << "\"" << std::endl;

        } else if (entryType == static_cast<uint64_t>(EntryType::MODIFIED_CHUNK)) {
            size_t diffSize = 0;
            size_t opCount = 0;
            std::string error;

            std::cout << "  Diff Operations:" << std::endl;
            if (!parseDiffData(file, diffSize, opCount, error)) {
                std::cerr << "Error: " << error << " in chunk #" << chunkNum << std::endl;
                return 1;
            }

            std::cout << "  Diff Data Size: " << diffSize << " bytes" << std::endl;
            std::cout << "  Diff Opcode Count: " << opCount << std::endl;
            if (opCount == 0) {
                std::cout << "  No diff data found (chunks might be identical)" << std::endl;
            }
        }

        std::cout << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Total chunks processed: " << chunkNum << std::endl;

    return 0;
}
