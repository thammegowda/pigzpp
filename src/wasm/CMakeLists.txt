# pigzpp WebAssembly (Embind) module. Only configured under the Emscripten toolchain.

add_executable(pigzpp_wasm pigzpp_wasm.cpp)
target_link_libraries(pigzpp_wasm PRIVATE pigzpp_lib)
target_compile_definitions(pigzpp_wasm PRIVATE PIGZPP_VERSION="${PROJECT_VERSION}")

target_compile_options(pigzpp_wasm PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# Emit an ES module (pigzpp_wasm.mjs) alongside the .wasm.
set_target_properties(pigzpp_wasm PROPERTIES
    OUTPUT_NAME pigzpp_wasm
    SUFFIX ".mjs"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm"
)

# Base Emscripten link options shared by all variants.
target_link_options(pigzpp_wasm PRIVATE
    -lembind
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createPigzppModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS
    # pigzpp uses sizable stack frames (e.g. a 64KB PNG scratch buffer and the
    # DEFLATE pipeline); the default 64KB wasm stack overflows. Give it room.
    -sSTACK_SIZE=8MB
    $<$<CONFIG:Release>:-O3>
)

if(PIGZPP_WASM_THREADS)
    # Threaded variant: real pthreads via Web Workers + SharedArrayBuffer.
    # Browsers require cross-origin isolation (COOP/COEP) and blocking calls must
    # run off the main thread (invoke from a Web Worker).
    target_compile_definitions(pigzpp_wasm PRIVATE PIGZPP_WASM_THREADS_ENABLED)
    target_link_options(pigzpp_wasm PRIVATE
        -pthread
        -sPTHREAD_POOL_SIZE=8
        -sPTHREAD_POOL_SIZE_STRICT=0
    )
endif()

# Benchmark-only: a thin direct zlib-ng wrapper (same engine, no facade/threads),
# used to isolate pigzpp's overhead vs a minimal binding. Off by default.
option(PIGZPP_WASM_BUILD_ZLIBNG_BASELINE "Build the thin zlib-ng WASM baseline" OFF)
if(PIGZPP_WASM_BUILD_ZLIBNG_BASELINE)
    add_executable(zlibng_wasm zlibng_wasm.cpp)
    target_link_libraries(zlibng_wasm PRIVATE zlib_target)
    target_compile_options(zlibng_wasm PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O3 -DNDEBUG>)
    set_target_properties(zlibng_wasm PROPERTIES
        OUTPUT_NAME zlibng_wasm
        SUFFIX ".mjs"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm"
    )
    target_link_options(zlibng_wasm PRIVATE
        -lembind
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createZlibngModule
        -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=8MB
        $<$<CONFIG:Release>:-O3>
    )
endif()

