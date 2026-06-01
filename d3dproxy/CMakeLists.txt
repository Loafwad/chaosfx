cmake_minimum_required(VERSION 3.20)
project(ChaosFXD3DProxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(d3d11 SHARED
    src/dllmain.cpp
    src/proxy_globals.cpp
    src/proxy_exports.cpp
    src/proxy_swapchain.cpp
    d3d11.def
)

target_include_directories(d3d11 PRIVATE src)

# dxgi.lib for DXGI types/GUIDs. Do NOT link d3d11.lib — that imports from ourselves.
target_link_libraries(d3d11 PRIVATE dxgi.lib dxguid.lib)

target_compile_definitions(d3d11 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(d3d11 PROPERTIES
    OUTPUT_NAME "d3d11"
    PREFIX ""
)
