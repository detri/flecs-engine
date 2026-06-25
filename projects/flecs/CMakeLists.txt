add_executable(flecs src/main.c)

target_include_directories(flecs PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(flecs PRIVATE flecs_engine)

if(WIN32 AND NOT EMSCRIPTEN AND TARGET wgpu_native_lib)
  add_custom_command(TARGET flecs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:wgpu_native_lib>
      $<TARGET_FILE_DIR:flecs>
  )
endif()

if(EMSCRIPTEN)
  target_link_options(flecs PRIVATE
    -sALLOW_MEMORY_GROWTH=1
    -sASYNCIFY
    -sASYNCIFY_STACK_SIZE=65536
    -sSTACK_SIZE=1048576
    "-sASYNCIFY_IMPORTS=['emscripten_sleep']"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/etc/scenes/cube.flecs@etc/scenes/cube.flecs"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/etc/scenes/empty.flecs@etc/scenes/empty.flecs"
    "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/shell.html"
  )

  set_target_properties(flecs PROPERTIES
    SUFFIX ".html"
  )
endif()
