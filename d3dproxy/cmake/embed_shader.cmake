# embed_shader.cmake
# Wraps an .hlsl source file as a C++ raw string literal header.
# Called by add_custom_command — not intended to be included directly.
#
# Required -D arguments:
#   INPUT   — absolute path to the .hlsl file
#   OUTPUT  — absolute path to the .h file to write
#   VAR     — name of the const char* variable to declare

get_filename_component(outdir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${outdir}")
file(READ "${INPUT}" content)
file(WRITE "${OUTPUT}" "#pragma once\n\nstatic const char* ${VAR} = R\"hlsl(\n${content})hlsl\";\n")
