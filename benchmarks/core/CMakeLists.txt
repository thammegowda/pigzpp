# Benchmarks
add_executable(pigzpp_bench bench_compress.cpp)
target_link_libraries(pigzpp_bench PRIVATE pigzpp_lib)
target_compile_options(pigzpp_bench PRIVATE -Wall -Wextra -O3)
