# Rolling Hash Delta Generator

![CMake](https://github.com/asmie/roll/actions/workflows/cmake.yml/badge.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-brightgreen.svg)

`rolling_hash` is a C++23 command-line tool for generating, applying, and
inspecting binary deltas between two files. It uses content-defined chunking
with Rabin-Karp rolling fingerprints for chunk boundaries and BLAKE-512 hashes
for strong chunk identity checks.

The single `rolling_hash` binary exposes three subcommands:

- `create`: generate a delta from an old file and a new file.
- `apply`: reconstruct a new file from an old file and a delta.
- `view`: print a human-readable inspection of a delta file.

The project also ships `rolling_hash_unit`, a GoogleTest-based suite covering
hashing, file I/O, signatures, delta application, and rolling fingerprints.

## Features

- Content-defined chunking, so insertions and deletions do not force every later
  chunk to change.
- Adaptive chunk boundaries with a 512 byte minimum, 16 KiB maximum, and an
  8 KiB target average chunk size.
- Dual chunk identity checks using a rolling fingerprint plus BLAKE-512.
- Delta entries for original, added, modified, and removed chunks.
- Delta application with payload hash verification and truncation/aliasing
  checks.

## Requirements

- CMake 3.16 or newer.
- A C++23 compiler such as recent GCC, Clang, or MSVC.
- Network access during first configure if GoogleTest is not already available,
  because CMake fetches GoogleTest v1.14.0 for the test target.

## Build

The quickest route is a configure preset:

```bash
cmake --preset release          # or: debug, asan, no-tests
cmake --build build/release -j$(nproc)
ctest --preset release
```

| Preset | Purpose |
| --- | --- |
| `release` | Optimised build with tests |
| `debug` | Unoptimised, full debug info |
| `asan` | Debug plus AddressSanitizer and UBSan |
| `no-tests` | Binary only; GoogleTest is never fetched |

Without presets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

On Windows with Visual Studio:

```cmd
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | `OFF` skips the test target, so GoogleTest is not downloaded and configure needs no network |
| `RH_BUILD_TESTS` | follows `BUILD_TESTING` | Overrides the test target independently |
| `RH_ENABLE_ASAN` | `OFF` | Builds with AddressSanitizer and UBSan |

### Install

```bash
cmake --install build --prefix /usr/local
```

### Compiler requirements

`std::expected` is required, which rules out one combination that otherwise
looks supported: Clang before 19 reports `__cpp_concepts` as `201907`, and
libstdc++ gates `<expected>` on `202002`, so **Clang 18 with libstdc++ compiles
as C++23 but has no `std::expected`**. Use GCC 13 or newer, Clang 19 or newer,
or Clang with `-stdlib=libc++`. Configure fails with an explanatory message
rather than a wall of template errors.

## Usage

Generate a delta:

```bash
./rolling_hash create oldfile.txt newfile.txt changes.delta
```

Apply a delta:

```bash
./rolling_hash apply oldfile.txt changes.delta reconstructed.txt
```

Inspect a delta:

```bash
./rolling_hash view changes.delta
```

The delta file is a binary stream of chunk records. Each record stores an entry
type, rolling signature, BLAKE-512 hash, chunk size, and optional payload data for
added or modified chunks.

## Example

From the repository root:

```bash
printf "hello\nold line\n" > old.txt
printf "hello\nnew line\n" > new.txt

cmake -S . -B build
cmake --build build -j$(nproc)

./build/rolling_hash create old.txt new.txt changes.delta
./build/rolling_hash apply old.txt changes.delta reconstructed.txt
cmp new.txt reconstructed.txt
```

If `cmp` exits successfully, the reconstructed file matches the new file.

## How It Works

1. `Signature` reads each input file and splits it into variable-sized chunks.
2. Every chunk receives a Rabin-Karp rolling fingerprint and a BLAKE-512 hash.
3. `Delta` compares the old and new signatures, emitting records for reused,
   added, modified, and removed chunks.
4. Modified chunks store compact byte-level diff opcodes:
   - `D`: replace bytes at a position.
   - `I`: insert bytes at a position.
   - `X`: delete bytes at a position.
5. `Apply` reads the old file and delta records in target-file order, verifies
   hashes for generated payloads, and writes the reconstructed output.

The delta format is big-endian for all multi-byte integer fields. Treat
generated deltas as an internal format for matching builds unless
compatibility is explicitly versioned.

## Tests

Build and run the GoogleTest suite:

```bash
cd build
ctest --output-on-failure
./rolling_hash_unit
```

The current test suite covers:

- File opening, reading, writing, EOF behavior, and invalid paths.
- BLAKE-512 hashing.
- Signature generation.
- Rabin-Karp rolling fingerprint behavior.
- Delta application for identical files, empty inputs, append/truncate cases,
  in-chunk modifications, malformed deltas, and output alias protection.

## Project Layout

```text
src/
  main.cpp          rolling_hash CLI entry point and subcommand dispatcher
  Apply.hpp         delta application logic
  Delta.hpp         delta generation and binary record writing
  Signature.hpp     content-defined chunk signature generation
  RK_finger.hpp     Rabin-Karp rolling fingerprint implementation
  DeltaViewer.*     delta inspection command implementation
  FileIO.*          file I/O helper
  blake.*           BLAKE-512 implementation
tests/
  *_tests.cpp       GoogleTest unit tests
CMakeLists.txt      build and test configuration
```

## Development

Keep changes warning-clean under the CMake options in `CMakeLists.txt`
(`-Wall -Wextra` for non-MSVC builds, `/W4` for MSVC). Add focused tests under
`tests/` when changing file I/O, chunking, hashing, delta generation, or delta
application behavior.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).

## Author

Piotr Olszewski ([asmie](https://github.com/asmie))
