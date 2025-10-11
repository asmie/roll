# Rolling Hash Delta Generator

![CMake](https://github.com/asmie/roll/actions/workflows/cmake.yml/badge.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-brightgreen.svg)

A high-performance C++23 application for generating binary deltas between files using content-defined chunking with Rabin fingerprints and BLAKE-512 hashing.

## 🎯 Features

- **Content-Defined Chunking**: Dynamic chunk boundaries based on file content, not fixed positions
- **Shift-Resistant**: Handles insertions and deletions efficiently without affecting surrounding chunks
- **Dual Hashing**: Combines fast Rabin fingerprints with cryptographically strong BLAKE-512
- **Adaptive Chunking**: Intelligent chunk size adaptation for optimal performance across file sizes

## 📖 Table of Contents

- [Quick Start](#-quick-start)
- [Installation](#-installation)
- [Usage](#-usage)
- [How It Works](#-how-it-works)
- [Performance](#-performance)
- [Testing](#-testing)
- [Contributing](#-contributing)
- [License](#-license)

## 🚀 Quick Start

```bash
# Clone the repository
git clone https://github.com/asmie/roll.git
cd roll

# Build the project
mkdir build && cd build
cmake .. && make -j4

# Generate a delta between two files
./rolling_hash oldfile.txt newfile.txt delta.bin
```

## 💻 Installation

### Prerequisites

- **Compiler**: GCC 11+, Clang 13+, or MSVC 2019+ with C++23 support
- **Build System**: CMake 3.16 or newer
- **Memory**: ~100MB for compilation
- **Optional**: Google Test (automatically fetched if not found)

### Building from Source

#### Standard Build
```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

#### Debug Build
```bash
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

#### Release Build with Optimizations
```bash
mkdir build-release
cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

#### Windows (Visual Studio)
```cmd
mkdir build
cd build
cmake -G "Visual Studio 16 2019" ..
cmake --build . --config Release
```

## 📋 Usage

### Basic Usage

Generate a delta file between two versions:
```bash
./rolling_hash original.txt modified.txt changes.delta
```

### Understanding the Output

The delta file is a **binary format** containing:
- Chunk signatures (64-bit Rabin fingerprints)
- BLAKE-512 hashes (64 bytes per chunk)
- Chunk metadata (size, offset)
- Binary diff data for modified chunks

### Viewing Delta Contents

A delta viewer utility is provided to inspect delta files:

```bash
# Build the delta viewer
g++ -std=c++11 delta_viewer.cpp -o delta_viewer

# View delta file contents
./delta_viewer changes.delta
```

Example output:
```
Delta File Viewer - Analyzing: changes.delta
========================================

Chunk #1:
  Type: MODIFIED
  Signature: 0x7cf82b36
  Chunk Size: 1012 bytes
  Hash (first 8 bytes): f7 7d 84 02 04 ee 91 fd ...
  Modifications:
    Modify byte at position 172 to 0x4e ('N')
    Modify byte at position 173 to 0x45 ('E')
    ... and 158 more changes
```

## 🔬 How It Works

### Algorithm Overview

1. **Chunking Phase**
   - Files are divided into variable-sized chunks using a rolling hash window
   - Chunk boundaries occur when the hash matches specific criteria
   - Adaptive sizing: 512 bytes (min) to 16KB (max)

2. **Signature Generation**
   - Each chunk gets a Rabin fingerprint (fast, weak hash)
   - Each chunk gets a BLAKE-512 hash (slow, strong hash)
   - Creates a signature list for each file

3. **Delta Computation**
   - Compares chunk lists between original and new files
   - Identifies: unchanged, added, removed, and modified chunks
   - For modified chunks, generates byte-level diffs

4. **Output Generation**
   - Writes binary delta file with all changes
   - Optimized format for minimal size and fast application

### Chunking Strategy

The application uses **content-defined chunking** with adaptive boundaries:

```cpp
// Adaptive boundary detection based on chunk size
if (chunk.size() >= MAX_CHUNK_SIZE) {
    // Force boundary at maximum size
    boundary_found = true;
} else if (chunk.size() >= MIN_CHUNK_SIZE) {
    // Use adaptive mask based on current size
    uint32_t mask = chunk.size() < 2048 ? 0x1FF :    // 1/512 probability
                    chunk.size() < 4096 ? 0x7FF :    // 1/2048 probability
                                          0x1FFF;     // 1/8192 probability
    boundary_found = (((last << 8 | b) & mask) == 0);
}
```

This approach ensures:
- Small files generate appropriate chunks
- Large files maintain efficiency
- Chunk boundaries remain content-dependent

## 📊 Performance

### Benchmarks

| File Size | Chunk Count | Delta Generation | Memory Usage |
|-----------|-------------|------------------|--------------|
| 1 KB      | 1-2         | < 1ms           | ~1 MB        |
| 100 KB    | 10-20       | ~5ms            | ~2 MB        |
| 10 MB     | 500-1000    | ~100ms          | ~15 MB       |
| 1 GB      | 50K-100K    | ~10s            | ~500 MB      |

### Optimization Features

- **Minimized Memory Allocation**: Smart pointer usage
- **Efficient I/O**: Buffered reading with configurable chunk sizes
- **Parallel-Ready**: Thread-safe design for future parallelization
- **Cache-Friendly**: Sequential memory access patterns

## 🧪 Testing

### Running Unit Tests

```bash
# Build and run all tests
make rolling_hash_unit
./rolling_hash_unit

# Run tests with detailed output
./rolling_hash_unit --gtest_verbose
```

### Test Coverage

- **Unit Tests**: 20+ test cases covering all major components
- **Memory Tests**: Valgrind verified (no leaks)
- **Integration Tests**: Full pipeline testing with various file types

### Memory Testing

```bash
valgrind --leak-check=full ./rolling_hash file1.txt file2.txt delta.out
```


## 🤝 Contributing

We welcome contributions! Please follow these guidelines:

### Development Setup

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Code Style

- Use descriptive variable names
- Follow existing formatting (tabs for indentation)
- Add comments for complex logic
- Include unit tests for new features
- Update documentation as needed

### Testing Requirements

- All tests must pass
- No memory leaks (Valgrind clean)
- No compiler warnings
- Code coverage for new features

## 🐛 Bug Reporting

Found a bug? Please report it on the [GitHub issue tracker](https://github.com/asmie/roll/issues) with:
- Description of the issue
- Steps to reproduce
- Expected vs actual behavior
- System information (OS, compiler version)
- Sample files if applicable

## 📜 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 👨‍💻 Authors

- **Piotr Olszewski** - *Initial work* - [asmie](https://github.com/asmie)

## 🙏 Acknowledgments

- BLAKE hash implementation from the public domain reference
- Inspired by rsync's rolling checksum algorithm
- Content-defined chunking concepts from LBFS

## 📈 Project Status

![Version](https://img.shields.io/badge/Version-0.0.2-blue.svg)
![Status](https://img.shields.io/badge/Status-Active-green.svg)