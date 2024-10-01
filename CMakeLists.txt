cmake_minimum_required(VERSION 3.25)

project(udx_native C)

include(bare)
include(napi)

bare_target(target)

if(NOT TARGET udx)
  add_subdirectory(vendor/libudx EXCLUDE_FROM_ALL)
endif()

add_bare_module(udx_native_bare)

target_sources(
  ${udx_native_bare}
  PRIVATE
    binding.c
  PUBLIC
    $<TARGET_OBJECTS:udx>
)

target_link_libraries(
  ${udx_native_bare}
  PUBLIC
    udx
)

if(target MATCHES "win32")
  target_link_libraries(
    ${udx_native_bare}
    PUBLIC
      ws2_32
  )
endif()

add_napi_module(udx_native_node)

target_sources(
  ${udx_native_node}
  PRIVATE
    binding.c
  PUBLIC
    $<TARGET_OBJECTS:udx>
)

target_link_libraries(
  ${udx_native_node}
  PUBLIC
    udx
)

if(target MATCHES "win32")
  target_link_libraries(
    ${udx_native_node}
    PUBLIC
      ws2_32
  )
endif()
