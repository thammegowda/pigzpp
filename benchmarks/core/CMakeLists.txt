# Benchmarks
add_executable(pigzpp_bench bench_compress.cpp)
target_link_libraries(pigzpp_bench PRIVATE pigzpp_lib)
target_compile_options(pigzpp_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-O3>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
