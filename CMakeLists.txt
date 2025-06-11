cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)
find_package(cmake-npm REQUIRED PATHS node_modules/cmake-npm)

project(udx_native C CXX)

bare_target(target)

if(target MATCHES "win32")
  add_compile_options(/MT$<$<CONFIG:Debug>:d>)
endif()

fetch_package("github:holepunchto/libudx#9e2f1166bcb1664f14eeaaee5ba298a530d65fb9")
fetch_package("github:holepunchto/libjstl#122cbdd")

add_bare_module(udx_native_bare)

target_sources(
  ${udx_native_bare}
  PRIVATE
    binding.cc
)

target_link_libraries(
  ${udx_native_bare}
  PRIVATE
    $<TARGET_OBJECTS:udx>
    jstl
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
    binding.cc
)

target_link_libraries(
  ${udx_native_napi}
  PRIVATE
    $<TARGET_OBJECTS:udx>
    jstl
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
