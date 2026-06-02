# ChaosFX — d3dproxy build recipes
# Run `just` from the repo root or d3dproxy/ directory.
#
# Prerequisites:
#   - Visual Studio 2022 with C++ workload
#   - CMake on PATH
#
# Usage:
#   just configure   — first-time CMake configure
#   just build       — incremental build (Release)
#   just deploy      — copy d3d11.dll to the game folder
#   just bd          — build + deploy in one step
#   just clean       — wipe the build directory
#   just run         — launch bridge + Twitch listener + open UI

set shell := ["powershell.exe", "-NoProfile", "-Command"]

build_dir := "d3dproxy\\build"
game_dir  := "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Trackmania"
cmake     := "& 'C:\\Program Files\\CMake\\bin\\cmake.exe'"

# Default: build and deploy
default: bd

# Configure the CMake project (run once, or after CMakeLists changes)
configure:
    {{cmake}} -S d3dproxy -B {{build_dir}} -G "Visual Studio 17 2022" -A x64

# Incremental Release build
build:
    {{cmake}} --build {{build_dir}} --config Release

# Copy the built DLL to the game folder
deploy:
    Copy-Item "{{build_dir}}\Release\d3d11.dll" "{{game_dir}}\d3d11.dll" -Force

# Build then deploy
bd: build deploy

# Wipe build directory (forces full reconfigure on next run)
clean:
    Remove-Item -Recurse -Force "{{build_dir}}"

# Launch the bridge, Twitch listener, and open the UI in the browser
run:
    .\start.bat
