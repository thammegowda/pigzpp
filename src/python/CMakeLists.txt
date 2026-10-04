find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
set(PIGZPP_NANOBIND_DIR "${PROJECT_SOURCE_DIR}/third_party/nanobind")
if(NOT EXISTS "${PIGZPP_NANOBIND_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "nanobind submodule is missing; run 'git submodule update --init --recursive'")
endif()
add_subdirectory("${PIGZPP_NANOBIND_DIR}" "${CMAKE_BINARY_DIR}/third_party/nanobind")

nanobind_add_module(pigzpp_py STABLE_ABI NB_STATIC pigzpp_pybind.cpp)
target_sources(pigzpp_py PRIVATE "${PROJECT_SOURCE_DIR}/src/pigzpp/main.cpp")
target_link_libraries(pigzpp_py PRIVATE pigzpp_lib)
target_compile_definitions(pigzpp_py PRIVATE
    PIGZPP_CLI_NO_MAIN
    PIGZPP_VERSION="${PROJECT_VERSION}"
)
set_target_properties(pigzpp_py PROPERTIES
    OUTPUT_NAME _pigzpp
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python/pigzpp"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python/pigzpp"
)
# Hide all symbols except PyInit__pigzpp on ELF platforms. This avoids PLT
# overhead for internal zlib-ng calls. Nanobind handles exports on macOS and
# Windows, whose linkers do not support GNU version scripts or -Bsymbolic.
if(UNIX AND NOT APPLE)
    target_link_options(pigzpp_py PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pigzpp.version"
        "LINKER:--no-undefined-version"
        "LINKER:-Bsymbolic"
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(pigzpp_py PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/python/pigzpp")
configure_file(
    pigzpp/__init__.py
    "${CMAKE_BINARY_DIR}/python/pigzpp/__init__.py"
    COPYONLY
)
configure_file(
    pigzpp/__main__.py
    "${CMAKE_BINARY_DIR}/python/pigzpp/__main__.py"
    COPYONLY
)

install(TARGETS pigzpp_py
    LIBRARY DESTINATION pigzpp
    RUNTIME DESTINATION pigzpp
)
