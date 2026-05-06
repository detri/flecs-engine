#!/usr/bin/env sh
set -eu

if [ ! -f build/debug/CMakeCache.txt ]; then
  cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
fi
cmake --build build/debug --parallel 8
./build/debug/flecs "$@"
