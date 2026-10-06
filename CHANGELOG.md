# Changelog

All notable changes to `rolling_hash` are recorded here. Versions follow
[Semantic Versioning](https://semver.org/); before 1.0.0, a minor version may
break compatibility.

## [0.3.0] - 2026-10-06

### Breaking changes

- The delta format is now version 5; 0.2.0 wrote version 1. Deltas made by
  0.2.0 are rejected with a version error, so regenerate them with 0.3.0.
- OpenSSL 1.1.0 or newer is now required. It provides BLAKE2b-512, which
  replaces the in-tree BLAKE-512.
- CMake 3.24 or newer is required.
- Exit statuses are now 0 for success, 1 for bad input, and 2 for a file that
  cannot be opened, read or written, for every command.

### Added

- `apply --max-output SIZE` bounds the reconstructed size of an untrusted delta
  (`K`, `M`, `G` and `T` suffixes are powers of 1024).
- A whole-file BLAKE2b-512 trailer, checked after reconstruction.
- `--version` and `--help`.
- libFuzzer targets for the delta readers and for create/apply round trips,
  with replay drivers where libFuzzer is unavailable.
- CMake presets (`release`, `debug`, `asan`, `no-tests`), an install rule, and
  an `RH_WARNINGS_AS_ERRORS` option. CI covers Linux, macOS and Windows.

### Changed

- Delta entries reference old chunks by index, so repeated or reordered content
  costs about 19 bytes per occurrence. Old chunks that are not reused need no
  records.
- Chunk identity uses a 128-bit BLAKE2b-512 prefix plus the chunk length.
- In-chunk edits use a Myers O(ND) diff. A diff is kept only when its complete
  record is smaller than the literal record.
- Generation is faster: the two signature passes run concurrently, Rabin-Karp
  reduction modulo 2^31-1 uses a fold, and reads are buffered.

### Fixed

- `create` and `apply` refuse an output path that is, or links to, one of their
  inputs; previously `create old new old` replaced `old` with the delta.
- A failed `apply` or `create` keeps an existing destination. Output is staged in
  a hidden `.rolling_hash-*` file and replaces the destination only after it
  verifies, closes and is flushed to stable storage. It also keeps the
  destination's permissions.
- On Linux and macOS, SIGINT, SIGTERM, SIGHUP and SIGQUIT remove the staged file.
  Exceeding `ulimit -f` fails cleanly instead of killing the process.
- Read and seek failures are no longer treated as end of file. Directories and
  other non-regular inputs are rejected, where `create` previously produced a
  delta of an empty file.
- `create` rejects inputs that changed after they were signed, instead of
  writing a delta that `apply` would reject.
- `view` and `apply` both require a trailer, reject bytes after it, and reject
  MODIFIED entries without diff operations.
- Custom Rabin-Karp parameters that would overflow 64-bit arithmetic are
  rejected instead of producing wrong fingerprints.
- Out-of-range chunk sizes in a delta are rejected before any allocation.

### Dependencies

- GoogleTest 1.18.0 (was 1.14.0), pinned by SHA-256.
- `actions/checkout` v7 in CI.

## [0.2.0]

Earlier release; no changelog was kept.

[0.3.0]: https://github.com/asmie/roll/releases/tag/v0.3.0
