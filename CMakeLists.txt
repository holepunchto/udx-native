cmake_minimum_required(VERSION 3.25)

project(udx_native C)

include(bare)

bare_target(target)

if(NOT TARGET udx)
  add_subdirectory(vendor/libudx EXCLUDE_FROM_ALL)
endif()

add_bare_module(udx_native)

target_sources(
  ${udx_native}
  PRIVATE
    binding.c
  PUBLIC
    $<TARGET_OBJECTS:udx>
)

target_link_libraries(
  ${udx_native}
  PUBLIC
    udx
)

if(target MATCHES "win32")
  target_link_libraries(
    ${udx_native}
    PUBLIC
      ws2_32
  )
endif()
