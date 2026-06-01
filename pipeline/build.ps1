# Build ChaosFXPipeline.dll using CMake + MSVC
# Run this from a normal PowerShell window (not Developer PS, not bash)

$ErrorActionPreference = "Stop"

# Find cmake.exe — check PATH first, then common install locations
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCmd) { $cmakeCmd.Source } else { $null }
if (-not $cmake) {
    $candidates = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe"
    )
    # Also search VS-bundled cmake
    $vsPaths = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vsPaths) { $candidates = @($vsPaths.FullName) + $candidates }
    $cmake = ($candidates | Where-Object { Test-Path $_ } | Select-Object -First 1)
}
if (-not $cmake) { throw "cmake.exe not found. Install CMake from https://cmake.org/download/ or install VS C++ workload." }
Write-Host "Using cmake: $cmake"

$root      = Split-Path $PSScriptRoot -Parent
$pipelineDir = Join-Path $root "pipeline"
$buildDir    = Join-Path $pipelineDir "build"
$pluginDir   = Join-Path $env:USERPROFILE "OpenplanetNext\Plugins\ChaosFX"

# CMake configure
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory $buildDir | Out-Null }
Push-Location $buildDir

& $cmake .. -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$pluginDir"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# Build
& $cmake --build . --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Pop-Location

# Copy AngelScript plugin files alongside the DLL
$opSrc = Join-Path $root "openplanet\ChaosFX"
Copy-Item "$opSrc\info.toml" "$pluginDir\" -Force
if (-not (Test-Path "$pluginDir\src")) { New-Item -ItemType Directory "$pluginDir\src" | Out-Null }
Copy-Item "$opSrc\src\Main.as" "$pluginDir\src\" -Force

Write-Host ""
Write-Host "Build complete. DLL placed at: $pluginDir\ChaosFXPipeline.dll"
Write-Host "Plugin files deployed: info.toml + src/Main.as"
