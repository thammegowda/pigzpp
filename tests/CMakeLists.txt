include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_MakeAvailable(googletest)
# Prevent gtest from being installed with the project
set_property(DIRECTORY "${googletest_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL YES)

# Unit tests
add_executable(pigzpp_tests
    test_crc.cpp
    test_pool.cpp
    test_compress_decompress.cpp
    test_cli.cpp
    test_format.cpp
    test_png_cpp.cpp
    test_zip.cpp
)
target_link_libraries(pigzpp_tests PRIVATE pigzpp_lib GTest::gtest_main)
target_compile_options(pigzpp_tests PRIVATE -Wall -Wextra)
target_compile_definitions(pigzpp_tests PRIVATE
    PIGZPP_BIN_PATH="$<TARGET_FILE:pigzpp_exe>"
)

include(GoogleTest)
gtest_discover_tests(pigzpp_tests)
