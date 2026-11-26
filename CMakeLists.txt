cmake_minimum_required(VERSION 4.0)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)

project(udx_native C CXX)

bare_target(target)

if(target MATCHES "win32")
  add_compile_options(/MT$<$<CONFIG:Debug>:d>)
endif()

fetch_package("github:holepunchto/libnapish#8cbfe78")
fetch_package("github:holepunchto/libudx#040a316")
fetch_package("github:holepunchto/libjstl#7e31e67")

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
    napish::static
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
