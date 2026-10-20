cmake_minimum_required(VERSION 3.20)
project(grn_dump LANGUAGES CXX)

# granny.dll is 32-bit, so the extractor must be too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with -A Win32: granny.dll is a 32-bit DLL.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
add_executable(grn_dump grn_dump.cpp)
