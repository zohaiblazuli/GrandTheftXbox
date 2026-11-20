cmake_minimum_required(VERSION 3.30)
project(GrandTheftXbox LANGUAGES CXX)

# D-004: C++20, MSVC, x64 Windows.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "GTX builds x64 only (Xbox titles are x86-64).")
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(src/common)
add_subdirectory(src/shims/xgameruntime)
add_subdirectory(src/launcher)

enable_testing()
add_subdirectory(tests)
