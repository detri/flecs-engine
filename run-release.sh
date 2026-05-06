#!/usr/bin/env sh
set -eu

if [ ! -f build/release/CMakeCache.txt ]; then
  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build/release --parallel 8
./build/release/flecs "$@"
