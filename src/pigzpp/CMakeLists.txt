# pigzpp library + CLI executable

set(PIGZPP_LIB_SOURCES
    crc.cpp
    io.cpp
    format.cpp
    compress.cpp
    decompress.cpp
    png.cpp
    zip.cpp
)

add_library(pigzpp_lib STATIC ${PIGZPP_LIB_SOURCES})
target_include_directories(pigzpp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pigzpp_lib PUBLIC zlib_target zopfli)
if(NOT EMSCRIPTEN)
    # Under Emscripten pthreads come from -pthread (added globally), not this target.
    target_link_libraries(pigzpp_lib PUBLIC Threads::Threads)
endif()
if(PIGZPP_HAS_ISAL)
    target_link_libraries(pigzpp_lib PUBLIC isal)
endif()
set_target_properties(pigzpp_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MATH_LIBRARY)
    target_link_libraries(pigzpp_lib PUBLIC ${MATH_LIBRARY})
endif()

target_compile_options(pigzpp_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wcast-qual>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O3;-DNDEBUG;-funroll-loops;-fomit-frame-pointer;-falign-functions=32;-falign-loops=32>
    $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang>>:-g;-fsanitize=address,undefined>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2;/DNDEBUG>
)
target_link_options(pigzpp_lib PUBLIC
    $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang>>:-fsanitize=address,undefined>
)

# CLI executable (native only — the WASM build ships the Embind module instead).
if(NOT EMSCRIPTEN)
add_executable(pigzpp_exe main.cpp)
set_target_properties(pigzpp_exe PROPERTIES
    OUTPUT_NAME pigzpp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(pigzpp_exe PRIVATE pigzpp_lib)
target_compile_definitions(pigzpp_exe PRIVATE PIGZPP_VERSION="${PROJECT_VERSION}")

target_compile_options(pigzpp_exe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O3;-DNDEBUG;-funroll-loops;-fomit-frame-pointer;-falign-functions=32;-falign-loops=32>
    $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang>>:-g;-fsanitize=address,undefined>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2;/DNDEBUG>
)

if(PIGZPP_STATIC AND CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(pigzpp_exe PRIVATE -static -static-libgcc -static-libstdc++)
    message(STATUS "Static linking enabled for pigzpp executable")
elseif(PIGZPP_STATIC AND MSVC)
    set_property(TARGET pigzpp_exe PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    message(STATUS "Static MSVC runtime enabled for pigzpp executable")
endif()
if(NOT SKBUILD)
    install(TARGETS pigzpp_exe RUNTIME DESTINATION bin)
endif()
endif()

# Shared C-ABI library for in-process language bindings (Go/cgo, Rust, etc.).
# Bundles pigzpp_lib and its static deps into one shared object so consumers
# link a single -lpigzppc without knowing the internal dependency graph.
option(PIGZPP_BUILD_CAPI "Build the shared C-ABI library (libpigzppc)" OFF)
if(PIGZPP_BUILD_CAPI AND NOT EMSCRIPTEN)
    add_library(pigzppc SHARED capi.cpp)
    set_target_properties(pigzppc PROPERTIES
        OUTPUT_NAME pigzppc
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(pigzppc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pigzppc PRIVATE pigzpp_lib)
    target_compile_options(pigzppc PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O3;-DNDEBUG>
        $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2;/DNDEBUG>
    )
    message(STATUS "Building shared C-ABI library libpigzppc")
endif()
