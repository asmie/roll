# rollin hashes

![CMake](https://github.com/asmie/roll/actions/workflows/cmake.yml/badge.svg)

## General description & the algorithm

Rolling (rollin' on the river ;)) hashes is an app to track changes between files. It uses Rabin fingerprints to quickly scan file chunks, and the BLAKE-512 hashing algorithm to generate strong hashes for verifying Rabin matches.

Both files are divided into chunks using a 48-byte sliding window, which scans the data and looks for chunk boundaries. These chunks are dynamic in size, based on the content of the file. A chunk boundary is identified when the lower 13 bits of the windowed data equal zero. This dynamic chunking approach makes the algorithm more accurate and resistant to shifts in the data. Once boundaries are found, the app writes down the Rabin fingerprint and the BLAKE hash—effectively creating a list of chunks for each file.

Next up: creating the deltas.

Now, in theory, we could stop here. Thanks to shift-resistant dynamic chunking, we could build a delta file using just the chunk info—treating any unmatched data as additions. This is similar to what LBFS (Low Bandwidth File System) does, where every change just creates a new chunk to be sent. Taking this approach would make the code a lot simpler, although the delta files might end up slightly larger. But honestly, the size difference might not be big enough to justify a more complex solution.

But... app must detect chunk removals and modifications.

To handle this, we compare the chunk lists from the original and new files and try to figure out what changed. Thanks to the dynamic chunking, we can be confident that matching chunks will be identified in both files—even if their positions have shifted.

So, the algorithm takes chunks from the new file one by one and tries to match them with chunks from the original file. If a chunk matches at the same position, the delta file simply notes that they're identical. If it doesn't match at that spot, we search further in the original file. If we find the chunk elsewhere, we have to account for all the chunks between the current position and the match. Those intermediate chunks are either removed (if they’re only in the original file) or marked as modified (this part can get tricky when there are lots of chunks in between). Any chunks that are in the new file but not in the original are marked as additions.

At the end of it all, the app writes out the deltas to the delta file. For chunks marked as modified, the original and new chunks are read again, and a binary diff is applied to ensure that only the exact changes are included.

## Tests

The application includes unit tests written using the Google Test framework. Tests should be run locally, as running GTest on GitHub fails to start execution.

The application has been tested under Valgrind—no memory leaks were detected.

## Compilation

### Prerequisites

Application has no external dependencies. roll demands:
* compiler compatible with GCC, Clang or MSVC, supporting the C++23 standard;
* standard C++ library;
* cmake 3.16 or newer.

To properly build and run tests there is a need to have:
* gtest that can be found by cmake buildsystem.

### Building

Simply go into the source directory and the type:
```
mkdir build
cd build
cmake ..
```

This should generate all the build files and check if the compiler is appropriate and contain all needed headers, functions and other stuff. Cross-compilation and other actions can be done according to the cmake manual.

Afterwards, still beeing in the build directory just type:
```
make
```
which should produce binary itself.


## Bug reporting

Bugs can be reported using [GitHub issue tracker](https://github.com/asmie/roll/issues).

## Authors

* **Piotr Olszewski** - *Original work* - [asmie](https://github.com/asmie)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details
