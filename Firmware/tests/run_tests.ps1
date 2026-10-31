# Builds and runs the host tests for the portable firmware code (no board needed).
param(
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$build = Join-Path $PSScriptRoot "build"

cmake -S $PSScriptRoot -B $build -G $Generator | Out-Null
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "Test build failed." }

foreach ($name in @("test_hpgl", "test_interp")) {
    $exe = Join-Path $build "Release\$name.exe"
    if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $build "$name.exe" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($name)." }
}
