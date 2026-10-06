# Rolling Hash Delta Generator

![CMake](https://github.com/asmie/roll/actions/workflows/cmake.yml/badge.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-brightgreen.svg)

`rolling_hash` is a C++23 command-line tool for generating, applying, and
inspecting binary deltas between two files. It uses content-defined chunking
with Rabin-Karp rolling fingerprints for chunk boundaries and BLAKE2b-512
(RFC 7693, via OpenSSL) for strong chunk identity checks.

The single `rolling_hash` binary exposes three subcommands:

- `create`: generate a delta from an old file and a new file.
- `apply`: reconstruct a new file from an old file and a delta.
- `view`: print a human-readable inspection of a delta file.

The project also ships `rolling_hash_unit`, a GoogleTest-based suite covering
hashing, file I/O, signatures, delta generation and application, and rolling
fingerprints. Release history is in [CHANGELOG.md](CHANGELOG.md).

## Features

- Content-defined chunking, so insertions and deletions do not force every later
  chunk to change.
- Adaptive chunk boundaries with a 512 byte minimum, 16 KiB maximum, and an
  8 KiB target average chunk size.
- Chunk identity checks using a 128-bit prefix of BLAKE2b-512 plus chunk length;
  rolling fingerprints select boundaries.
- Delta entries for original, added, and modified chunks. Unreferenced old chunks
  need no removal records.
- Repeated content is reused rather than re-sent: a reordered or duplicated block
  costs a ~19-byte reference per occurrence instead of a full copy. Doubling a
  40 MB zero-filled file produces a 103 KB delta.
- A byte-level diff is emitted only when it is actually smaller than storing the
  chunk outright, which bounds a delta at roughly the size of its input.
- Delta application verifies each generated payload against its chunk hash and
  the whole reconstructed file against a trailer hash. Both creation and application
  reject output paths that alias an input and stage writes in an exclusively created
  temporary file beside the destination. Successful validation and close precede
  replacement; failures preserve an existing destination. The staged file takes an
  existing destination's permissions before any data is written, and is flushed to
  stable storage before it replaces the destination. Replacement swaps a destination
  symlink or hard link itself, leaving its former target or other names intact.
- `apply --max-output SIZE` bounds the reconstructed size, because a small hostile
  delta can reference one old chunk any number of times.

## Requirements

- CMake 3.24 or newer, including when using presets.
- A C++23 compiler and standard library — see *Compiler requirements* below, as
  one plausible-looking combination does not work.
- OpenSSL 1.1.0 or newer, for BLAKE2b-512 via the EVP digest interface. This is
  a hard dependency: configure fails without it.
- Network access during first configure, because CMake fetches GoogleTest v1.18.0
  for the test target. Configure with `-DBUILD_TESTING=OFF` to skip both.

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
| `RH_WARNINGS_AS_ERRORS` | `OFF` | Treats project warnings as errors; enabled in CI |
| `RH_ENABLE_ASAN` | `OFF` | Builds with AddressSanitizer and UBSan |

### Fuzzing

`apply` and `view` parse untrusted input, so the parsers have fuzz targets:

```bash
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DRH_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz -j$(nproc)
./fuzz/make_corpus.sh build-fuzz/rolling_hash corpus
build-fuzz/fuzz_apply corpus/apply -max_total_time=60      # libFuzzer builds
build-fuzz/fuzz_apply_replay corpus/apply/*.delta          # otherwise
```

`fuzz_apply` drives hostile deltas through both readers; `fuzz_roundtrip`
generates a delta between two halves of the input and requires an exact
reconstruction. Both check invariants beyond "did not crash": a failed apply
must preserve an existing destination and leave no new output, a successful one
must be deterministic and accepted by the viewer, and a freshly generated delta
must always apply. All core sources receive sanitizer and coverage instrumentation
in libFuzzer builds. The `corpus/apply` seeds use the harness's exact old-file
fixture; `corpus/roundtrip` contains separate old/new pairs. Run the latter with
`build-fuzz/fuzz_roundtrip corpus/roundtrip -max_total_time=60` (or
`build-fuzz/fuzz_roundtrip_replay corpus/roundtrip/*`).

Where the compiler supports libFuzzer these are coverage-guided fuzzers;
elsewhere they build as replay drivers that re-check the corpus, so the targets
stay useful as regression tests. Note that Clang 18 cannot build them: libFuzzer's
runtime links against libstdc++ while this project needs libc++ there, so use
Clang 19+ (which works with libstdc++) for real fuzzing.

The corpus is generated rather than committed — a stored delta is pinned to the
format version that produced it, and after a version bump would only exercise
the version-rejection path.

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

Staged output files are created exclusively with `std::ios::noreplace` where the
standard library provides it (libstdc++ 13, libc++ 18, recent MSVC), and with C11
`fopen` mode `"x"` otherwise.

## Usage

Generate a delta:

```bash
./rolling_hash create oldfile.txt newfile.txt changes.delta
```

Apply a delta:

```bash
./rolling_hash apply oldfile.txt changes.delta reconstructed.txt
```

Bound the output size when the delta is untrusted (`K`, `M`, `G`, `T` are powers
of 1024):

```bash
./rolling_hash apply --max-output 2G oldfile.txt changes.delta reconstructed.txt
```

Inspect a delta:

```bash
./rolling_hash view changes.delta
```

The delta file is a versioned binary stream. It opens with a 4-byte magic and a
big-endian format version, followed by one entry per chunk of the reconstructed
file, and closes with a trailer holding a hash of the whole result.

Entries name their source chunk in the old file **by index**, so one old chunk
can back any number of new ones — repeated content costs about 19 bytes per
occurrence instead of a copy. Each entry carries a 128-bit digest truncated from
BLAKE2b-512, which both identifies the chunk and verifies it; lengths and indices
are varints. `src/DeltaCodec.hpp` is the single definition of the layout, and
`rolling_hash view` prints it.

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

1. `Signature` reads each input file and splits it into variable-sized chunks,
   cutting where masked bits of a Rabin-Karp rolling fingerprint over the last
   48 bytes match a fixed target.
2. Every chunk is identified by its length and a 128-bit prefix of its
   BLAKE2b-512 hash.
3. `Delta` compares the old and new signatures and emits one entry per new
   chunk. A chunk whose content exists anywhere in the old file becomes a
   reference to it rather than a copy — however many times it recurs, and
   without needing a matching entry for old chunks that are simply gone.
4. For chunks without an exact old-file match, generation tries a byte-level
   diff against the old chunk at the same index, computed with Myers'
   O(ND) algorithm (falling back to a greedy diff when the edit distance is
   large):
   - `D`: replace bytes at a position.
   - `I`: insert bytes at a position.
   - `X`: delete bytes at a position.

   The complete modified record, including its source-index varint, is kept
   only if it costs less than the complete literal record; otherwise the chunk
   is emitted whole.
5. `Apply` reads the old file and delta records in target-file order, verifies
   each generated payload against its recorded hash, and writes the
   reconstructed output. A trailing whole-file hash is checked at the end, so
   truncation or reordering is caught even when every individual chunk verifies.

Fixed-width integer fields are big-endian; lengths and indices use LEB128 varints. The stream carries a format
version and readers reject anything they do not recognise, but the format is
still an internal one: it is not a compatibility promise across versions.
Readers require exactly one final trailer and reject bytes after it.

Inputs must be stable, seekable regular files. Read and seek failures are distinct
from EOF. Generation rejects a size change in either input, checks every new chunk
it copies into the delta against its signature, and re-reads the whole old file to
confirm it still matches its signature, because the applier resolves records
against it. Reused new chunks are encoded from the signature without a second
read, so the delta reproduces the new file as it was signed. These checks reject
observed changes; they are not a filesystem snapshot or a lock against
concurrent writers. Use snapshots or otherwise stop writes when that guarantee is
needed.

Output is staged in a hidden `.rolling_hash-*` file beside the destination. The
staged file is flushed to stable storage before it replaces the destination, and
the directory is synced afterwards where the filesystem supports it, so a power
loss leaves either the previous destination or the complete new one. On Linux
and macOS, SIGINT, SIGTERM, SIGHUP and SIGQUIT remove the staged file before the
process exits, and exceeding a file-size limit (`ulimit -f`) fails with status 2
instead of killing the process. SIGKILL, a crash, a power loss, or Ctrl+C on
Windows can still leave a staged file behind; when no `rolling_hash` is running,
any `.rolling_hash-*` file is safe to delete.

## Exit status

| Status | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Bad input: malformed or truncated delta, hash mismatch, an output path that aliases an input, an exceeded `--max-output`, or invalid arguments |
| `2` | Environment failure: a file could not be opened, read, or written |

## Tests

Build and run the GoogleTest suite:

```bash
cd build
ctest --output-on-failure
./rolling_hash_unit
```

The current test suite covers:

- File I/O: opening, reading, writing, buffered byte-wise reads interleaved with
  bulk reads, EOF behaviour, close semantics, and invalid paths.
- BLAKE2b-512 against the RFC 7693 vectors, streaming versus one-shot.
- Rabin-Karp fingerprints checked against a naive reference at every position,
  for both the Mersenne fast path and the general modulus.
- Signature generation, including sub-window files and resynchronisation after
  an insertion.
- The delta codec: big-endian round trips, header and version rejection, and
  truncation in the entry header, opcodes, and trailer.
- The chunk index, including reuse of repeated content.
- Concept conformance, including implementations that inherit from nothing.
- Delta generation: record-size selection, a fixed v5 byte vector, and inputs
  that change, vanish, or turn out to be directories after signing.
- Delta application for identical files, empty inputs, append and truncate
  cases, in-chunk modifications, malformed and hostile deltas, out-of-range
  chunk sizes, failure categories, output alias protection, and `--max-output`.
- Output staging: existing destinations, links and permissions are preserved on
  failure, and staged files are removed on errors, exceptions and signals.
- The CLI's exit-status contract (`tests/cli_tests.cmake`, run by CTest).

Tests create their files in a private temporary directory per process, so
`ctest --parallel` is safe.

## Project Layout

```text
src/
  main.cpp          rolling_hash CLI entry point and subcommand dispatcher
  Apply.hpp         delta application logic
  Delta.hpp         delta generation, Myers diff, entry selection
  Signature.hpp     content-defined chunk signature generation
  RK_finger.hpp     Rabin-Karp rolling fingerprint implementation
  ChunkIndex.hpp    content index shared by Delta and Apply
  DeltaCodec.hpp    the delta wire format: reader, writer, and layout
  DeltaFormat.hpp   format constants, entry types, and chunk-size bounds
  DeltaError.hpp    failure categories and success statistics
  HashConcepts.hpp  requirements on the rolling and strong hash algorithms
  DeltaViewer.*     delta inspection command implementation
  FileIO.*          buffered file I/O helper
  OutputTransaction.*  staged, synced, atomic replacement of output files
  blake2b.*         BLAKE2b-512 via OpenSSL EVP
  rh_config.h.in    configure-time toolchain probes
tests/
  *_tests.cpp       GoogleTest unit tests
  TestWorkspace.hpp per-process temporary directory for test files
  cli_tests.cmake   CLI exit-status and round-trip checks
fuzz/               libFuzzer targets, replay driver, and corpus generator
CMakeLists.txt      build and test configuration
CMakePresets.json   release / debug / asan / no-tests presets
```

## Development

Keep changes warning-clean under the CMake options in `CMakeLists.txt`
(`-Wall -Wextra` for non-MSVC builds, `/W4` for MSVC); CI treats warnings as
build failures. Add focused tests under `tests/` when changing file I/O,
chunking, hashing, delta generation, or delta application behaviour.

Before proposing a change that touches the delta format or the diff algorithm,
check the effect on both correctness and size: a create/apply round trip must
reproduce the new file byte for byte, and a refactor that is not meant to change
output should produce byte-identical deltas. Run the sanitised build too:

```bash
cmake --preset asan && cmake --build build/asan -j$(nproc) && ctest --preset asan
```

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).

## Author

Piotr Olszewski ([asmie](https://github.com/asmie))
