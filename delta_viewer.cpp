#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cstdint>

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

void printDiffData(const std::vector<uint8_t>& data) {
    size_t i = 0;
    int count = 0;
    while (i < data.size()) {
        if (i + 5 >= data.size()) break;

        char op = data[i];
        uint32_t pos = (data[i+1] << 24) | (data[i+2] << 16) | (data[i+3] << 8) | data[i+4];

        if (op == 'M' && i + 6 <= data.size()) {
            uint8_t newVal = data[i+5];
            if (count < 10) {  // Show first 10 modifications
                std::cout << "        Modify byte at position " << pos << " to 0x"
                          << std::hex << (int)newVal << " ('" << (char)newVal << "')" << std::dec << std::endl;
            }
            i += 6;
            count++;
        } else if (op == 'A' && i + 6 <= data.size()) {
            uint8_t newVal = data[i+5];
            if (count < 10) {
                std::cout << "        Add byte at position " << pos << ": 0x"
                          << std::hex << (int)newVal << " ('" << (char)newVal << "')" << std::dec << std::endl;
            }
            i += 6;
            count++;
        } else if (op == 'R') {
            if (count < 10) {
                std::cout << "        Remove byte at position " << pos << std::endl;
            }
            i += 5;
            count++;
        } else {
            // Unknown operation, try to skip
            break;
        }
    }
    if (count > 10) {
        std::cout << "        ... and " << (count - 10) << " more changes" << std::endl;
    }
}

// Calculate actual size of diff data by parsing it
size_t calculateDiffSize(std::ifstream& file) {
    size_t size = 0;
    std::streampos startPos = file.tellg();

    while (file.good()) {
        char op;
        file.read(&op, 1);
        if (!file.good()) break;

        if (op == 'M' || op == 'A') {
            // M/A format: op (1) + position (4) + value (1) = 6 bytes
            file.seekg(5, std::ios::cur);
            size += 6;
        } else if (op == 'R') {
            // R format: op (1) + position (4) = 5 bytes
            file.seekg(4, std::ios::cur);
            size += 5;
        } else {
            // Not a valid diff operation, we've reached the end
            break;
        }
    }

    // Reset to start position
    file.clear();
    file.seekg(startPos);
    return size;
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

    std::cout << "Delta File Viewer (Fixed) - Analyzing: " << argv[1] << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    int chunkNum = 0;
    while (file.good()) {
        uint64_t entryType;
        file.read(reinterpret_cast<char*>(&entryType), sizeof(uint64_t));
        if (file.eof()) break;

        uint64_t signature;
        file.read(reinterpret_cast<char*>(&signature), sizeof(uint64_t));

        // Read 64-byte hash (BLAKE-512)
        std::vector<uint8_t> hash(64);
        file.read(reinterpret_cast<char*>(hash.data()), 64);

        uint64_t chunkSize;
        file.read(reinterpret_cast<char*>(&chunkSize), sizeof(uint64_t));

        std::cout << "Chunk #" << ++chunkNum << ":" << std::endl;
        std::cout << "  Type: " << entryTypeToString(static_cast<EntryType>(entryType))
                  << " (" << entryType << ")" << std::endl;
        std::cout << "  Signature: 0x" << std::hex << signature << std::dec << std::endl;
        std::cout << "  Chunk Size: " << chunkSize << " bytes" << std::endl;
        std::cout << "  Hash (first 8 bytes): ";
        for (int i = 0; i < 8 && i < hash.size(); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << (int)hash[i] << " ";
        }
        std::cout << "..." << std::dec << std::endl;

        // Read raw data for ADDED or MODIFIED chunks
        if (entryType == static_cast<uint64_t>(EntryType::ADDED_CHUNK)) {
            // For ADDED chunks, read chunkSize bytes
            std::vector<uint8_t> rawData(chunkSize);
            file.read(reinterpret_cast<char*>(rawData.data()), chunkSize);

            std::cout << "  Added Data (first 50 chars): \"";
            for (size_t i = 0; i < 50 && i < rawData.size(); i++) {
                if (rawData[i] >= 32 && rawData[i] < 127) {
                    std::cout << (char)rawData[i];
                } else if (rawData[i] == '\n') {
                    std::cout << "\\n";
                } else {
                    std::cout << ".";
                }
            }
            if (rawData.size() > 50) std::cout << "...";
            std::cout << "\"" << std::endl;

        } else if (entryType == static_cast<uint64_t>(EntryType::MODIFIED_CHUNK)) {
            // For MODIFIED chunks, calculate the actual diff data size
            size_t diffSize = calculateDiffSize(file);

            if (diffSize > 0) {
                std::vector<uint8_t> diffData(diffSize);
                file.read(reinterpret_cast<char*>(diffData.data()), diffSize);

                std::cout << "  Diff Data Size: " << diffSize << " bytes" << std::endl;
                std::cout << "  Modifications:" << std::endl;
                printDiffData(diffData);
            } else {
                std::cout << "  No diff data found (chunks might be identical)" << std::endl;
            }
        }

        std::cout << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Total chunks processed: " << chunkNum << std::endl;

    return 0;
}