# CMakeLists file written by Piotr "asmie" Olszewski
# Project: rolling_hash

cmake_minimum_required(VERSION 3.24 FATAL_ERROR)
project(rolling_hash CXX)

# Versioning
set (RH_VERSION_MAJOR 0)
set (RH_VERSION_MINOR 3)
set (RH_VERSION_REV 0)
set (RH_VERSION "${RH_VERSION_MAJOR}.${RH_VERSION_MINOR}.${RH_VERSION_REV}" )

# Configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compilation options
if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pipe)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -ggdb")
    # Keep CMake's default Release optimisation (-O3 -DNDEBUG) and only add the
    # debug-info choice. Do NOT append another -O level here: these flags land
    # after the defaults, and the last -O on the command line is the one that
    # takes effect. A `-Os` in this slot silently beat the default -O3 and cost
    # 1.6x on delta generation (2.04s vs 1.31s over 100 MB) — and it also made
    # -O2/-O3 experiments look like no-ops, since every variant still ended
    # in -Os.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g0")
endif()

option(RH_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

# Optional sanitizers (Debug builds only by default).
option(RH_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
if (RH_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- Compile-environment probing -----------------------------------------
#
# CheckIncludeFileCXX / CheckCXXSourceCompiles invoke the compiler, so they
# must honor CMAKE_CXX_STANDARD set above (otherwise <bit>/<span>/concepts
# are probed without -std=c++23 and the check fails on every toolchain).

cmake_policy(SET CMP0067 NEW)
include(CheckIncludeFileCXX)
include(CheckCXXSourceCompiles)

# Headers used by the production sources.
foreach(_h
        algorithm atomic bit cctype climits concepts cstddef cstdint cstdio
        cstdlib cstring expected filesystem fstream functional future iomanip iostream
        limits span sstream stdexcept string string_view system_error type_traits
        unordered_map utility vector)
    string(TOUPPER "${_h}" _H)
    check_include_file_cxx(${_h} RH_HAVE_${_H})
endforeach()

# C++17/20/23 library symbols the project actually depends on. We probe by
# compiling a tiny program that *uses* the symbol — qualified namespace
# names can't be checked with check_cxx_symbol_exists.
check_cxx_source_compiles("
    #include <bit>
    #include <cstdint>
    int main() { return static_cast<int>(std::byteswap<std::uint32_t>(0)); }
" RH_HAVE_STD_BYTESWAP)

check_cxx_source_compiles("
    #include <bit>
    int main() {
        return std::endian::native == std::endian::little ? 0 : 1;
    }
" RH_HAVE_STD_ENDIAN)

check_cxx_source_compiles("
    #include <span>
    #include <cstdint>
    int main() {
        std::uint8_t a[4]{};
        std::span<std::uint8_t> s(a, 4);
        return static_cast<int>(s.size());
    }
" RH_HAVE_STD_SPAN)

check_cxx_source_compiles("
    #include <filesystem>
    int main() {
        std::filesystem::path p(\"x\");
        return p.empty() ? 0 : 1;
    }
" RH_HAVE_STD_FILESYSTEM)

check_cxx_source_compiles("
    #include <utility>
    enum class E { X };
    int main() { return static_cast<int>(std::to_underlying(E::X)); }
" RH_HAVE_STD_TO_UNDERLYING)

check_cxx_source_compiles("
    #include <expected>
    #include <string>
    int main() {
        std::expected<int, std::string> ok{1};
        std::expected<int, std::string> bad{std::unexpected(std::string(\"e\"))};
        return ok.has_value() && !bad.has_value() ? 0 : 1;
    }
" RH_HAVE_STD_EXPECTED)

check_cxx_source_compiles("
    #include <fstream>
    int main() { auto mode = std::ios::out | std::ios::noreplace; (void) mode; }
" RH_HAVE_STD_NOREPLACE)

check_cxx_source_compiles("
    #include <concepts>
    struct B {};
    struct D : B {};
    template<class T> requires std::derived_from<T, B> int f(T) { return 0; }
    int main() { return f(D{}); }
" RH_HAVE_STD_CONCEPTS_DERIVED_FROM)

foreach(_symbol BYTESWAP ENDIAN SPAN FILESYSTEM TO_UNDERLYING EXPECTED CONCEPTS_DERIVED_FROM)
    if (NOT RH_HAVE_STD_${_symbol})
        message(FATAL_ERROR "rolling_hash requires C++23 library support: missing library feature ${_symbol}. Use GCC 13+, Clang 19+, or Clang with libc++.")
    endif()
endforeach()

# Configuration file
configure_file(
    "${PROJECT_SOURCE_DIR}/src/rh_config.h.in"
    "generated/rh_config.h"
)


# Production sources (explicit list — new files require re-running CMake anyway,
# so spelling them out avoids the well-known file(GLOB) staleness problem).
set(RH_SOURCES
    src/blake2b.cpp
    src/FileIO.cpp
    src/DeltaViewer.cpp
    src/OutputTransaction.cpp
)

set(RH_TEST_SOURCES
    tests/Apply_tests.cpp
    tests/Blake_tests.cpp
    tests/ChunkIndex_tests.cpp
    tests/DeltaCodec_tests.cpp
    tests/Delta_tests.cpp
    tests/DeltaViewer_tests.cpp
    tests/FileIO_tests.cpp
    tests/HashConcepts_tests.cpp
    tests/RK_finger_tests.cpp
    tests/Signature_tests.cpp
    tests/main_tests.cpp
)

# ---- Targets --------------------------------------------------------------

# Find packages
find_package(Threads REQUIRED)
# OpenSSL 1.1.0+ exposes EVP_blake2b512(); modern distros ship 3.x.
find_package(OpenSSL 1.1.0 REQUIRED)

# Shared object library so the production sources are compiled once rather than
# once per executable, and so include paths and link dependencies are declared
# on a target instead of leaking to the whole directory via include_directories.
add_library(rh_core OBJECT ${RH_SOURCES})
target_compile_features(rh_core PUBLIC cxx_std_23)
target_include_directories(rh_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rh_core PUBLIC Threads::Threads OpenSSL::Crypto)

# Create executable
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE rh_core)

function(rh_project_warnings target)
    if (RH_WARNINGS_AS_ERRORS)
        if (MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
rh_project_warnings(rh_core)
rh_project_warnings(${PROJECT_NAME})

# ---- Install --------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- Fuzzing --------------------------------------------------------------
#
# The fuzz targets are built two ways. Where the compiler supports libFuzzer
# (Clang), they become real coverage-guided fuzzers. Everywhere else they build
# against a replay driver, so a committed corpus and any recorded crash input
# stay runnable as regression tests under the project's normal compiler.

option(RH_BUILD_FUZZERS "Build the fuzz targets" OFF)

if (RH_BUILD_FUZZERS)
    add_executable(fuzz_make_corpus fuzz/make_corpus.cpp)
    target_link_libraries(fuzz_make_corpus PRIVATE rh_core)
    target_include_directories(fuzz_make_corpus PRIVATE "${PROJECT_SOURCE_DIR}/fuzz")
    rh_project_warnings(fuzz_make_corpus)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    check_cxx_source_compiles("
        #include <cstdint>
        #include <cstddef>
        extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
    " RH_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)

    if (NOT RH_HAVE_LIBFUZZER)
        # Note for Clang 18: libFuzzer's runtime is built against libstdc++, but
        # this project needs libc++ there for std::expected, so the two cannot be
        # linked together. Clang 19+ works with libstdc++ and therefore with
        # libFuzzer. Replay drivers keep the corpus runnable meanwhile.
        message(STATUS "libFuzzer unavailable; building fuzz targets as replay drivers")
    endif()

    if (RH_HAVE_LIBFUZZER)
        add_library(rh_fuzz_core OBJECT ${RH_SOURCES})
        target_include_directories(rh_fuzz_core PUBLIC "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(rh_fuzz_core PUBLIC Threads::Threads OpenSSL::Crypto)
        target_compile_options(rh_fuzz_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
        rh_project_warnings(rh_fuzz_core)
    endif()

    foreach(_name fuzz_apply fuzz_roundtrip)
        if (RH_HAVE_LIBFUZZER)
            add_executable(${_name} fuzz/${_name}.cpp)
            target_compile_options(${_name} PRIVATE -fsanitize=fuzzer,address,undefined)
            target_link_options(${_name} PRIVATE -fsanitize=fuzzer,address,undefined)
            target_link_libraries(${_name} PRIVATE rh_fuzz_core)
            target_include_directories(${_name} PRIVATE "${PROJECT_SOURCE_DIR}/fuzz")
            rh_project_warnings(${_name})
        else()
            add_executable(${_name}_replay fuzz/${_name}.cpp fuzz/replay_main.cpp)
            target_link_libraries(${_name}_replay PRIVATE rh_core)
            target_include_directories(${_name}_replay PRIVATE "${PROJECT_SOURCE_DIR}/fuzz")
            rh_project_warnings(${_name}_replay)
        endif()
    endforeach()
endif()

# ---- Testing --------------------------------------------------------------
#
# Gated so a consumer who only wants the binary neither downloads GoogleTest nor
# needs network access at configure time. include(CTest) defines BUILD_TESTING
# (default ON) and is the convention other projects expect to be able to switch.

include(CTest)
option(RH_BUILD_TESTS "Build the unit test target" ${BUILD_TESTING})

if (RH_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "Do not install bundled test dependencies" FORCE)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
            URL_HASH SHA256=63b9c77751a5b8f492486005f67533fdc58682b67476fcb91650be5958d5195a
            DOWNLOAD_EXTRACT_TIMESTAMP false
    )
    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)
    add_executable(${PROJECT_NAME}_unit ${RH_TEST_SOURCES})
    target_link_libraries(${PROJECT_NAME}_unit PRIVATE rh_core gtest)
    rh_project_warnings(${PROJECT_NAME}_unit)
    gtest_discover_tests(${PROJECT_NAME}_unit)
    add_test(NAME CLI.contract COMMAND ${CMAKE_COMMAND}
        "-DRH_CLI=$<TARGET_FILE:rolling_hash>" -P "${PROJECT_SOURCE_DIR}/tests/cli_tests.cmake")
endif()
