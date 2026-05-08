# pigzpp library + CLI executable

set(PIGZPP_LIB_SOURCES
    crc.cpp
    io.cpp
    format.cpp
    compress.cpp
    decompress.cpp
)

add_library(pigzpp_lib STATIC ${PIGZPP_LIB_SOURCES})
target_include_directories(pigzpp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pigzpp_lib PUBLIC zlib_target Threads::Threads zopfli)
if(PIGZPP_HAS_ISAL)
    target_link_libraries(pigzpp_lib PUBLIC isal)
endif()
set_target_properties(pigzpp_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MATH_LIBRARY)
    target_link_libraries(pigzpp_lib PUBLIC ${MATH_LIBRARY})
endif()

target_compile_options(pigzpp_lib PRIVATE
    -Wall -Wextra -Wpedantic -Wcast-qual
    $<$<CONFIG:Release>:-O3 -DNDEBUG -funroll-loops -fomit-frame-pointer
        -falign-functions=32 -falign-loops=32>
    $<$<CONFIG:Debug>:-g -fsanitize=address,undefined>
)
target_link_options(pigzpp_lib PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# CLI executable
add_executable(pigzpp_exe main.cpp)
set_target_properties(pigzpp_exe PROPERTIES
    OUTPUT_NAME pigzpp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(pigzpp_exe PRIVATE pigzpp_lib)

target_compile_options(pigzpp_exe PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3 -DNDEBUG -funroll-loops -fomit-frame-pointer
        -falign-functions=32 -falign-loops=32>
    $<$<CONFIG:Debug>:-g -fsanitize=address,undefined>
)

if(PIGZPP_STATIC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(pigzpp_exe PRIVATE -static -static-libgcc -static-libstdc++)
    message(STATUS "Static linking enabled for pigzpp executable")
endif()
