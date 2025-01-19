cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)
find_package(cmake-npm REQUIRED PATHS node_modules/cmake-npm)

project(udx_native C)

bare_target(target)

fetch_package("github:holepunchto/libudx#00d51c1aad0d3f2df0dd955dd3b8329e9d7b8fb5")

add_bare_module(udx_native_bare)

target_sources(
  ${udx_native_bare}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${udx_native_bare}
  PRIVATE
    $<TARGET_OBJECTS:udx>
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

add_napi_module(udx_native_napi)

target_sources(
  ${udx_native_napi}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${udx_native_napi}
  PRIVATE
    $<TARGET_OBJECTS:udx>
  PUBLIC
    udx
)

if(target MATCHES "win32")
  target_link_libraries(
    ${udx_native_napi}
    PUBLIC
      ws2_32
  )
endif()

resolve_node_module(bare-compat-napi compat)

target_include_directories(
  ${udx_native_napi}
  PRIVATE
    "${compat}/include"
)
