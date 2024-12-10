cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)

project(udx_native C)

bare_target(target)

fetch_package("github:holepunchto/libudx#1b741187dbe66a3c6411946b5787bd7bb8f34022")

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

add_napi_module(udx_native_node)

target_sources(
  ${udx_native_node}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${udx_native_node}
  PRIVATE
    $<TARGET_OBJECTS:udx>
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
