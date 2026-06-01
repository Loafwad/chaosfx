# Build d3d11.dll proxy and deploy it to the Trackmania game directory.
# Run from any PowerShell window.

$ErrorActionPreference = "Stop"

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCmd) { $cmakeCmd.Source } else { $null }
if (-not $cmake) {
    $vsPaths = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    $cmake = if ($vsPaths) { $vsPaths.FullName } else {
        @("C:\Program Files\CMake\bin\cmake.exe","C:\Program Files (x86)\CMake\bin\cmake.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    }
}
if (-not $cmake) { throw "cmake.exe not found." }
Write-Host "Using cmake: $cmake"

$proxyDir = $PSScriptRoot
$buildDir  = Join-Path $proxyDir "build"
$gameDir   = "C:\Program Files (x86)\Steam\steamapps\common\Trackmania"

if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory $buildDir | Out-Null }
Push-Location $buildDir

& $cmake .. -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $cmake --build . --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Pop-Location

$dll = Join-Path $buildDir "Release\d3d11.dll"
Copy-Item $dll "$gameDir\" -Force
Write-Host ""
Write-Host "Build complete. Proxy deployed to: $gameDir\d3d11.dll"
