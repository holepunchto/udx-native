cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)

project(udx_native C)

bare_target(target)

if(target MATCHES "win32")
  add_compile_options(/MT$<$<CONFIG:Debug>:d>)
endif()

fetch_package("github:holepunchto/libudx#deb8a6a072e3ace21eca8b77e51d59322b081825")

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
