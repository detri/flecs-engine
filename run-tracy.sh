#!/bin/bash
cmake -S . -B build/tracy -DFLECS_ENGINE_TRACY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/tracy
TRACY_DPI_SCALE=${TRACY_DPI_SCALE:-1.0} tracy-profiler &
./build/tracy/flecs "$@"
