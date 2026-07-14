find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(pigzpp_py STABLE_ABI NB_STATIC pigzpp_pybind.cpp)
target_link_libraries(pigzpp_py PRIVATE pigzpp_lib)
set_target_properties(pigzpp_py PROPERTIES
    OUTPUT_NAME pigzpp
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# Hide all symbols except PyInit_pigzpp on ELF platforms. This avoids PLT
# overhead for internal zlib-ng calls. Nanobind handles exports on macOS and
# Windows, whose linkers do not support GNU version scripts or -Bsymbolic.
if(UNIX AND NOT APPLE)
    target_link_options(pigzpp_py PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pigzpp.version"
        "LINKER:-Bsymbolic"
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(pigzpp_py PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

install(TARGETS pigzpp_py
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)


