#!/bin/sh
# Builds libpigzppc.so from /src (read-only mount) in /build and copies it to /artifacts.
set -eu
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPIGZPP_BUILD_CAPI=ON \
    -DPIGZPP_BUILD_CLI=ON -DPIGZPP_BUILD_TESTS=OFF -DPIGZPP_BUILD_PYTHON=OFF \
    -DPIGZPP_BUILD_BENCHMARKS=OFF -DPIGZPP_INSTALL=OFF -DPIGZPP_STATIC=ON \
    -DCMAKE_SHARED_LINKER_FLAGS=-s -DCMAKE_EXE_LINKER_FLAGS=-s
cmake --build /build --target pigzppc pigzpp_exe pigzpp_lib --parallel "$(nproc)"
mkdir -p /artifacts
cp -f /build/libpigzppc.so /build/pigzpp /artifacts/
# Static libraries: pigzpp_lib plus its bundled dependencies (zlib-ng, zopfli, ISA-L).
mkdir -p /artifacts/lib
find /build -name '*.a' ! -name libfoo.a -exec cp -f {} /artifacts/lib/ \;
chmod -R a+rw /artifacts
ls -l /artifacts



