# rollin hashes

![CMake](https://github.com/asmie/roll/actions/workflows/cmake.yml/badge.svg)

## General Description & the algorithm

Rolling (rollin' on river ;)) hashes is app to track changes between the files. It uses Rabin fingerprints to fast scan file chunks and BLAKE-512 hash algorithm for creating strong hashes to verify Rabin matches.

Both files are divided into chunks and scanned using 48-byte scanning window and searched for chunk boundaries - those chunks are dynamic size, based on the file data. If lower 13 bits of data windows is set to zero chunk boundary is found. Dynamic chunks approach makes algorithm to be more accourant and not dependant on chunk shift. When chunk boundaries are identified application write the Rabin fingerprint and BLAKE hash. Doing so, there is created a list of chunks for each file.

The next step is connected with creating deltas.

In fact, we could stop everything right here. Dynamic chunks gives us shift-resistance and therefore, delta file could be built only using existing chunks info and treat all the other data as additions. This could be similar to the approach used in LBFS (Low Bandwitch File System), where every modification creates just new chunk to be sent. 
Taking above approach could lead to much simplier code but probably slightly bigger delta files (as everything would be treated as addition), but the difference could be not big enough to create more compilcated code.

However, recruitment task conditions stated clearly, that there is a must to identify chunk removal and chunk modification.

To do so, we are taking into account both lists (chunk list from original and new file) and try to  find out what have changed. Thanks to dynamic chunk size we are sure that each same chunk will be idnetified in both files (even with shift).

So, algorithm takes list of the new chunks one by one and try to match them with chunks from original file. If chunk matches, in delta file will be note that the chunk are identical. If not matches on the same position we're trying to look for the chunk in file in some further position. If chunk matches on the other position we need to take care chunks between the actual position and chunk position in original file. The chunks will be either marked for removal (because they are in the original file and not in the new one) or modification (this is the weakest point when there are many chunks between).
If there are any chunks that are present in the new file but not in the original file, they will be marked for addition.

At the end of the procedure all deltas are written into the delta file. For those chunks, that are marked as modified, chunks from original and new file are read one more time and there is bin diff applied on them, just to ensure that only exact changes will be in the delta file.

## Tests

Application has unit tests done using Google Test framework.
Tests should be done on the computer itself as GTest on GitHub fails when starting execution.

Tested under valgrind - no leaks detected.

## Compilation

### Prerequisites

Application has no external dependencies. roll demands:
* compiler compatible with GCC, Clang or MSVC, supporting at C++23 standard;
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
