#ifndef FLECS_ENGINE_VENDOR_H
#define FLECS_ENGINE_VENDOR_H

#ifdef __EMSCRIPTEN__
  #include <emscripten/emscripten.h>
  #include <emscripten/html5.h>
  #include <GLFW/glfw3.h>
  #include <webgpu/webgpu.h>
#else
  #ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
  #endif
  #include <GLFW/glfw3.h>
  #include <GLFW/glfw3native.h>
  #include <webgpu.h>
  #ifdef _WIN32
    #ifdef near
      #undef near
    #endif
    #ifdef far
      #undef far
    #endif
  #endif
#endif

#include <flecs.h>
#include "wgpu_compat.h"

#endif
