add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pybind11" "${CMAKE_CURRENT_BINARY_DIR}/pybind11")

pybind11_add_module(pigzpp_py pigzpp_pybind.cpp)
target_link_libraries(pigzpp_py PRIVATE pigzpp_lib)
set_target_properties(pigzpp_py PROPERTIES
    OUTPUT_NAME pigzpp
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# Hide all symbols except PyInit_pigzpp — avoids PLT overhead for internal
# zlib-ng calls (inflate, deflate, etc.), which gives ~2-3x decompression speedup.
target_link_options(pigzpp_py PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pigzpp.version"
    "LINKER:-Bsymbolic"
)

install(TARGETS pigzpp_py LIBRARY DESTINATION .)


