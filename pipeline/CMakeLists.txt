cmake_minimum_required(VERSION 3.20)
project(ChaosFXPipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output DLL directly into the live plugin folder for immediate reload.
# Falls back to repo openplanet/ folder if USERPROFILE is not set.
if(DEFINED ENV{USERPROFILE})
    set(_plugin_dir "$ENV{USERPROFILE}/OpenplanetNext/Plugins/ChaosFX")
else()
    set(_plugin_dir "${CMAKE_SOURCE_DIR}/../openplanet/ChaosFX")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         "${_plugin_dir}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_plugin_dir}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${_plugin_dir}")

add_library(ChaosFXPipeline SHARED
    src/dllmain.cpp
    src/effects.cpp
    src/exports.cpp
    ChaosFXPipeline.def
)

target_include_directories(ChaosFXPipeline PRIVATE src)

target_link_libraries(ChaosFXPipeline PRIVATE
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(ChaosFXPipeline PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
)

# Suppress CRT secure warnings — we own every call site
target_compile_definitions(ChaosFXPipeline PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(ChaosFXPipeline PROPERTIES
    PREFIX ""
)
