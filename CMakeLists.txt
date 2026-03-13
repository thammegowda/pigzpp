cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
project(pigzpp VERSION 1.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "No build type selected, default to Release")
endif()

# --- Build options ---
option(PIGZPP_USE_ZLIB_NG "Use zlib-ng instead of system zlib (hardware-accelerated CRC)" ON)
option(PIGZPP_STATIC "Build fully static binary" ON)
option(PIGZPP_NATIVE "Use -march=native for CPU-specific optimizations (not portable)" OFF)
option(PIGZPP_LTO "Enable Link-Time Optimization" ON)

# --- Compiler flags ---
if(PIGZPP_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-march=native>)
    message(STATUS "Using -march=native (NOT PORTABLE — binary requires this CPU or newer)")
else()
    # Baseline x86-64: runs on any 64-bit x86 CPU. zlib-ng handles
    # hardware acceleration via runtime CPUID dispatch.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mtune=generic>)
    message(STATUS "Using portable x86-64 baseline (-mtune=generic). zlib-ng uses runtime CPU dispatch for HW accel.")
endif()

if(PIGZPP_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "LTO enabled")
endif()

# --- Dependencies ---
find_package(Threads REQUIRED)
find_library(MATH_LIBRARY m)

# All third-party libraries are built as static archives.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# zlib: prefer zlib-ng, built as part of the project
if(PIGZPP_USE_ZLIB_NG AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-ng/CMakeLists.txt")
    message(STATUS "Building zlib-ng (hardware-accelerated) from third_party/zlib-ng")
    set(ZLIB_COMPAT ON CACHE BOOL "zlib-ng: zlib-compatible API" FORCE)
    set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "zlib-ng: disable tests" FORCE)
    set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "zlib-ng: disable tests" FORCE)
    set(WITH_GTEST OFF CACHE BOOL "zlib-ng: disable gtest" FORCE)
    set(WITH_BENCHMARKS OFF CACHE BOOL "zlib-ng: disable benchmarks" FORCE)
    set(WITH_NEW_STRATEGIES ON CACHE BOOL "zlib-ng: new strategies" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory(third_party/zlib-ng EXCLUDE_FROM_ALL)
    # With BUILD_SHARED_LIBS=OFF and ZLIB_COMPAT, zlib-ng creates the real
    # target as "zlib-ng" (static) and aliases zlibstatic/zlib-ng-static to it.
    # We need the real (non-alias) target to set properties on.
    set(_zlibng_real zlib-ng)
    add_library(zlib_target ALIAS ${_zlibng_real})
    # Override include dirs: only expose generated public headers (zlib.h, zconf.h)
    # from the build dir, not the source dir (which has an internal deflate.h
    # that would shadow zopfli's deflate.h).
    set_property(TARGET ${_zlibng_real} PROPERTY INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib-ng>"
    )
    # Propagate ZLIB_COMPAT so consumers including internal headers (deflate.h)
    # get the correct include chain (zlib.h, not zlib-ng.h).
    add_compile_definitions(ZLIB_COMPAT)
else()
    message(STATUS "Using system zlib")
    find_package(ZLIB REQUIRED)
    add_library(zlib_target ALIAS ZLIB::ZLIB)
endif()

# Zopfli as a static library
set(ZOPFLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zopfli/src/zopfli")
file(GLOB ZOPFLI_SOURCES "${ZOPFLI_DIR}/*.c")
list(FILTER ZOPFLI_SOURCES EXCLUDE REGEX "zopfli_bin\\.c$")
add_library(zopfli STATIC ${ZOPFLI_SOURCES})
target_include_directories(zopfli PUBLIC ${ZOPFLI_DIR})
set_target_properties(zopfli PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(zopfli PRIVATE
    $<$<CONFIG:Release>:-O3 -DNDEBUG -funroll-loops -fomit-frame-pointer>
)

# ISA-L (Intel Intelligent Storage Acceleration Library)
# Provides hand-tuned assembly DEFLATE for ~2x throughput over zlib-ng.
# BSD-3-Clause license (Intel, 2011-2024). See third_party/isa-l/LICENSE.
option(PIGZPP_USE_ISAL "Use ISA-L for compression/decompression engine" ON)
if(PIGZPP_USE_ISAL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/isa-l/CMakeLists.txt")
    message(STATUS "Building ISA-L (hardware-accelerated DEFLATE) from third_party/isa-l")
    set(ISAL_BUILD_TESTS OFF CACHE BOOL "ISA-L: disable tests" FORCE)
    set(ISAL_BUILD_PERF_TESTS OFF CACHE BOOL "ISA-L: disable perf tests" FORCE)
    set(ISAL_BUILD_FUZZ_TESTS OFF CACHE BOOL "ISA-L: disable fuzz tests" FORCE)
    set(ISAL_BUILD_ISAL_SHIM OFF CACHE BOOL "ISA-L: disable shim library" FORCE)
    add_subdirectory(third_party/isa-l EXCLUDE_FROM_ALL)
    set(PIGZPP_HAS_ISAL ON)
    add_compile_definitions(PIGZPP_USE_ISAL)
else()
    message(STATUS "ISA-L disabled or not found; using zlib-ng only")
    set(PIGZPP_HAS_ISAL OFF)
endif()

# Library + CLI
add_subdirectory(src/pigzpp)

# Python bindings
option(PIGZPP_BUILD_PYTHON "Build Python bindings" ON)
if(PIGZPP_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()

# Testing
option(PIGZPP_BUILD_TESTS "Build tests" ON)
if(PIGZPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks
option(PIGZPP_BUILD_BENCHMARKS "Build benchmarks" ON)
if(PIGZPP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
