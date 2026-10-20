cmake_minimum_required(VERSION 3.20)
project(sb2modern LANGUAGES CXX)

# The game is a 32-bit process, so these replacement DLLs must be 32-bit.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with -A Win32: the game loads these DLLs into a 32-bit process.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLLs need no redistributable next to the game.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W3 /utf-8 /permissive-)

add_library(sb2common STATIC
  common/config.cpp
  common/log.cpp
  common/hook.cpp
)
target_include_directories(sb2common PUBLIC common)

add_library(ddraw SHARED
  ddraw/main.cpp
  ddraw/compat.cpp
  ddraw/renderer.cpp
  ddraw/dd.cpp
  ddraw/surface.cpp
  ddraw/palette.cpp
  ddraw/d3d.cpp
  ddraw/device.cpp
  ddraw/ddraw.def
)
target_link_libraries(ddraw PRIVATE sb2common d3d11 dxgi d3dcompiler dxguid winmm)
set_target_properties(ddraw PROPERTIES OUTPUT_NAME "ddraw")

add_library(dsound SHARED
  dsound/dsound.cpp
  dsound/mixer.cpp
  dsound/dsound.def
)
target_link_libraries(dsound PRIVATE sb2common ole32 winmm dxguid)
set_target_properties(dsound PROPERTIES OUTPUT_NAME "dsound")
