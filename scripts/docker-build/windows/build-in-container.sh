#!/bin/sh
# Cross-compiles pigzppc.dll (mingw-w64, x86_64) from /src (read-only mount) in /build
# and copies it to /artifacts.
set -eu
cat > /tmp/mingw-toolchain.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
# Static runtime so the DLL has no libgcc/libstdc++/winpthread dependencies.
cmake -S /src -B /build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DPIGZPP_BUILD_CAPI=ON \
    -DPIGZPP_BUILD_CLI=ON -DPIGZPP_BUILD_TESTS=OFF -DPIGZPP_BUILD_PYTHON=OFF \
    -DPIGZPP_BUILD_BENCHMARKS=OFF -DPIGZPP_INSTALL=OFF -DPIGZPP_STATIC=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-static -static-libgcc -static-libstdc++ -s" \
    -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++ -s"
cmake --build /build --target pigzppc pigzpp_exe pigzpp_lib --parallel "$(nproc)"
mkdir -p /artifacts
dll="$(find /build -name 'pigzppc.dll' -o -name 'libpigzppc.dll' | head -n 1)"
[ -n "$dll" ] || { echo "pigzppc.dll not found" >&2; exit 1; }
cp -f "$dll" /artifacts/pigzppc.dll
cp -f /build/pigzpp.exe /artifacts/
# Static libraries: pigzpp_lib plus its bundled dependencies (zlib-ng, zopfli, ISA-L).
mkdir -p /artifacts/lib
find /build -name '*.a' ! -name libfoo.a -exec cp -f {} /artifacts/lib/ \;
chmod -R a+rw /artifacts
ls -l /artifacts



