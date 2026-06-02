cmake_minimum_required(VERSION 3.20)
project(ChaosFXD3DProxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Shader embedding ──────────────────────────────────────────────────────────
# Each .hlsl is read at build time and wrapped into a generated .h as a C++
# raw string literal.  Editing an .hlsl triggers only that file's regeneration
# and a C++ recompile — no full reconfigure needed.
function(embed_shader hlsl_base var_name)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/src/effects/${hlsl_base}.hlsl")
    set(dst "${CMAKE_CURRENT_BINARY_DIR}/effects/${hlsl_base}.h")
    add_custom_command(
        OUTPUT  "${dst}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT="${src}"
            -DOUTPUT="${dst}"
            -DVAR="${var_name}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake"
        DEPENDS "${src}"
        COMMENT "Embedding shader ${hlsl_base}.hlsl"
    )
    set(EMBEDDED_SHADERS ${EMBEDDED_SHADERS} "${dst}" PARENT_SCOPE)
endfunction()

set(EMBEDDED_SHADERS "")
embed_shader(pink_mode        k_PinkShaderSrc)
embed_shader(kaleidoscope     k_KaleidoscopeShaderSrc)
embed_shader(mirror           k_MirrorShaderSrc)
embed_shader(flipped_screen   k_FlipShaderSrc)

add_library(d3d11 SHARED
    src/dllmain.cpp
    src/proxy_globals.cpp
    src/proxy_exports.cpp
    src/proxy_swapchain.cpp
    src/effects.cpp
    src/http_poll.cpp
    ${EMBEDDED_SHADERS}
    d3d11.def
)

target_include_directories(d3d11 PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# dxgi.lib for DXGI types/GUIDs. d3dcompiler.lib for runtime shader compilation.
# winhttp.lib for the reward bridge polling thread.
# Do NOT link d3d11.lib — that imports from ourselves.
target_link_libraries(d3d11 PRIVATE dxgi.lib dxguid.lib d3dcompiler.lib winhttp.lib)

target_compile_definitions(d3d11 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(d3d11 PROPERTIES
    OUTPUT_NAME "d3d11"
    PREFIX ""
)
