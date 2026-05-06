#!/usr/bin/env sh
set -eu

if [ ! -f build/asan/CMakeCache.txt ]; then
  cmake -S . -B build/asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
fi
cmake --build build/asan --parallel 8
./build/asan/flecs "$@"
