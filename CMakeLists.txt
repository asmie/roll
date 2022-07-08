# CMakeLists file written by Piotr "asmie" Olszewski
# Project: rolling_hash

cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(rolling_hash)

# Versioning
set (RH_VERSION_MAJOR 0)
set (RH_VERSION_MINOR 0)
set (RH_VERSION_REV 1)
set (RH_VERSION "${RH_VERSION_MAJOR}.${RH_VERSION_MINOR}.${RH_VERSION_REV}" )

# Configuration
enable_language(CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Compilation options
if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pipe)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -g3 -ggdb")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -g0 -Os")

 #   set(CMAKE_EXE_LINKER_FLAGS "-z now -s")
endif()

# Checking time
include (CheckIncludeFileCXX)
include (CheckCXXSymbolExists)

check_include_file_cxx (cstdlib RH_SYSTEM_HAVE_CSTDLIB)
check_include_file_cxx (cstdio RH_SYSTEM_HAVE_CSTDIO)
check_include_file_cxx (iostream RH_SYSTEM_HAVE_IOSTREAM)
check_include_file_cxx (cstring RH_SYSTEM_HAVE_CSTRING)


# Configuration file
configure_file (
  "${PROJECT_SOURCE_DIR}/src/rh_config.h.in"
  "generated/rh_config.h"
  )


# Find source files
file(GLOB SOURCES src/blake512.cpp src/FileIO.cpp )

# Include header files
include_directories(src ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Find packages
# # Threads
find_package(Threads REQUIRED)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} src/main.cpp)
target_link_libraries(${PROJECT_NAME} Threads::Threads ${ADDITIONAL_LIBRARIES})

# Testing time
enable_testing()

# Try to find GTest
find_package(GTest)

if(GTEST_FOUND)
	include_directories(src ${CMAKE_CURRENT_BINARY_DIR}/generated ${GTEST_INCLUDE_DIR})
	file(GLOB TEST_SOURCES tests/*.cpp ${SOURCES} )
	add_executable(${PROJECT_NAME}_unit ${TEST_SOURCES})
	target_link_libraries(${PROJECT_NAME}_unit PUBLIC gtest pthread ${CMAKE_DL_LIBS} ${ADDITIONAL_LIBRARIES})
	add_test(${PROJECT_NAME}_unit_test ${PROJECT_NAME}_unit)
endif(GTEST_FOUND)
