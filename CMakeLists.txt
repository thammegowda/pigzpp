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
option(PIGZPP_DISTRIBUTION_BUILD "Require self-contained distribution dependencies" OFF)
option(PIGZPP_NATIVE "Use -march=native for CPU-specific optimizations (not portable)" OFF)
option(PIGZPP_LTO "Enable Link-Time Optimization" ON)
option(PIGZPP_WASM_SIMD "Enable WASM 128-bit SIMD (Emscripten builds only)" ON)
option(PIGZPP_WASM_THREADS "Enable WASM threads/pthreads (Emscripten builds only)" OFF)

if(PIGZPP_DISTRIBUTION_BUILD)
    set(PIGZPP_USE_ZLIB_NG ON CACHE BOOL "Use vendored zlib-ng" FORCE)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-ng/CMakeLists.txt")
        message(FATAL_ERROR "Distribution builds require the vendored zlib-ng submodule")
    endif()
endif()

if(MSVC AND (PIGZPP_STATIC OR PIGZPP_DISTRIBUTION_BUILD))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(PIGZPP_GNU_LIKE OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(PIGZPP_GNU_LIKE ON)
endif()

# --- Emscripten / WebAssembly overrides ---
# ISA-L is x86 assembly and cannot target WASM; Python bindings, the CLI's static
# linking, and -march tuning are all native-only. Force portable defaults here so
# a plain `emcmake cmake ..` produces a working build.
if(EMSCRIPTEN)
    message(STATUS "Emscripten toolchain detected — configuring WebAssembly build")
    set(PIGZPP_USE_ISAL OFF CACHE BOOL "" FORCE)
    set(PIGZPP_NATIVE OFF CACHE BOOL "" FORCE)
    set(PIGZPP_STATIC OFF CACHE BOOL "" FORCE)
    set(PIGZPP_LTO OFF CACHE BOOL "" FORCE)
    set(PIGZPP_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
    set(PIGZPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PIGZPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
endif()

# --- Compiler flags ---
if(EMSCRIPTEN)
    # Use Emscripten's exception model (MVP-compatible) so binaryen/wasm-opt can
    # process the module. compress_buffer/decompress_buffer throw on error.
    add_compile_options(-fexceptions)
    add_link_options(-fexceptions)
    # WASM bytecode is portable; SIMD/threads are opt-in capabilities.
    if(PIGZPP_WASM_SIMD)
        add_compile_options(-msimd128)
        message(STATUS "WASM SIMD (128-bit) enabled")
    endif()
    if(PIGZPP_WASM_THREADS)
        add_compile_options(-pthread)
        add_link_options(-pthread)
        message(STATUS "WASM threads enabled (requires cross-origin isolation in browsers)")
    endif()
elseif(PIGZPP_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release" AND PIGZPP_GNU_LIKE)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-march=native>)
    message(STATUS "Using -march=native (NOT PORTABLE — binary requires this CPU or newer)")
else()
    if(PIGZPP_GNU_LIKE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mtune=generic>)
    endif()
    message(STATUS "Using portable CPU baseline. zlib-ng uses runtime CPU dispatch where available.")
endif()

if(PIGZPP_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PIGZPP_IPO_SUPPORTED OUTPUT PIGZPP_IPO_ERROR)
    string(REGEX MATCH "^[0-9]+" PIGZPP_C_COMPILER_MAJOR "${CMAKE_C_COMPILER_VERSION}")
    string(REGEX MATCH "^[0-9]+" PIGZPP_CXX_COMPILER_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
    if(PIGZPP_IPO_SUPPORTED AND
       CMAKE_C_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID AND
       PIGZPP_C_COMPILER_MAJOR STREQUAL PIGZPP_CXX_COMPILER_MAJOR)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled")
    else()
        message(WARNING "LTO disabled: C and C++ compilers do not have compatible IPO support (${PIGZPP_IPO_ERROR})")
    endif()
endif()

# --- Dependencies ---
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()
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
    if(EMSCRIPTEN)
        # wasm has no optimized arch; the runtime CPU-dispatch functable only adds
        # indirection and generates dispatch stubs that break under wasm's strict
        # call-signature validation. Call the generic C implementations directly.
        set(WITH_RUNTIME_CPU_DETECTION OFF CACHE BOOL "zlib-ng: no CPU dispatch on wasm" FORCE)
    endif()
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
    if(PIGZPP_DISTRIBUTION_BUILD)
        get_target_property(PIGZPP_ZLIB_TARGET_TYPE ${_zlibng_real} TYPE)
        if(NOT PIGZPP_ZLIB_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
            message(FATAL_ERROR "Distribution zlib-ng target must be static, got ${PIGZPP_ZLIB_TARGET_TYPE}")
        endif()
    endif()
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
if(PIGZPP_DISTRIBUTION_BUILD)
    get_target_property(PIGZPP_ZOPFLI_TARGET_TYPE zopfli TYPE)
    if(NOT PIGZPP_ZOPFLI_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Distribution Zopfli target must be static, got ${PIGZPP_ZOPFLI_TARGET_TYPE}")
    endif()
endif()
target_compile_options(zopfli PRIVATE
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>>:-O3;-DNDEBUG;-funroll-loops;-fomit-frame-pointer>
    $<$<AND:$<CONFIG:Release>,$<C_COMPILER_ID:MSVC>>:/O2;/DNDEBUG>
)

# ISA-L (Intel Intelligent Storage Acceleration Library)
# Provides hand-tuned assembly DEFLATE for ~2x throughput over zlib-ng.
# BSD-3-Clause license (Intel, 2011-2024). See third_party/isa-l/LICENSE.
option(PIGZPP_USE_ISAL "Use ISA-L for compression/decompression engine" ON)
if(APPLE AND PIGZPP_USE_ISAL)
    message(STATUS "Disabling ISA-L on macOS; using the portable zlib-ng backend")
    set(PIGZPP_USE_ISAL OFF CACHE BOOL "Use ISA-L for compression/decompression engine" FORCE)
elseif(WIN32 AND PIGZPP_USE_ISAL AND
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(STATUS "Disabling ISA-L on non-x86 Windows; using the portable zlib-ng backend")
    set(PIGZPP_USE_ISAL OFF CACHE BOOL "Use ISA-L for compression/decompression engine" FORCE)
endif()
if(PIGZPP_USE_ISAL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/isa-l/CMakeLists.txt")
    message(STATUS "Building ISA-L (hardware-accelerated DEFLATE) from third_party/isa-l")
    set(ISAL_BUILD_TESTS OFF CACHE BOOL "ISA-L: disable tests" FORCE)
    set(ISAL_BUILD_PERF_TESTS OFF CACHE BOOL "ISA-L: disable perf tests" FORCE)
    set(ISAL_BUILD_FUZZ_TESTS OFF CACHE BOOL "ISA-L: disable fuzz tests" FORCE)
    set(ISAL_BUILD_ISAL_SHIM OFF CACHE BOOL "ISA-L: disable shim library" FORCE)
    add_subdirectory(third_party/isa-l EXCLUDE_FROM_ALL)
    set(PIGZPP_HAS_ISAL ON)
    add_compile_definitions(PIGZPP_USE_ISAL)
    if(PIGZPP_DISTRIBUTION_BUILD)
        get_target_property(PIGZPP_ISAL_TARGET_TYPE isal TYPE)
        if(NOT PIGZPP_ISAL_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
            message(FATAL_ERROR "Distribution ISA-L target must be static, got ${PIGZPP_ISAL_TARGET_TYPE}")
        endif()
    endif()
else()
    message(STATUS "ISA-L disabled or not found; using zlib-ng only")
    set(PIGZPP_HAS_ISAL OFF)
endif()

# Library + CLI
add_subdirectory(src/pigzpp)

# WebAssembly (Embind) module — only under the Emscripten toolchain.
if(EMSCRIPTEN)
    add_subdirectory(src/wasm)
endif()

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
    add_subdirectory(benchmarks/core)
endif()

# Native release archives. Python wheel builds install only the extension and
# deliberately skip the CLI/docs package payload.
if(NOT SKBUILD AND NOT EMSCRIPTEN)
    install(FILES LICENSE README.md DESTINATION .)

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" PIGZPP_PACKAGE_PROCESSOR)
    if(PIGZPP_PACKAGE_PROCESSOR MATCHES "^(amd64|x86_64)$")
        set(PIGZPP_PACKAGE_PROCESSOR "x86_64")
    elseif(PIGZPP_PACKAGE_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(PIGZPP_PACKAGE_PROCESSOR "arm64")
    endif()

    if(WIN32)
        set(PIGZPP_PACKAGE_PLATFORM "windows")
        set(CPACK_GENERATOR "ZIP")
    elseif(APPLE)
        set(PIGZPP_PACKAGE_PLATFORM "macos")
        set(CPACK_GENERATOR "TGZ")
    else()
        set(PIGZPP_PACKAGE_PLATFORM "linux")
        set(CPACK_GENERATOR "TGZ")
    endif()

    set(CPACK_PACKAGE_NAME "pigzpp")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME
        "pigzpp-${PROJECT_VERSION}-${PIGZPP_PACKAGE_PLATFORM}-${PIGZPP_PACKAGE_PROCESSOR}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    include(CPack)
endif()
