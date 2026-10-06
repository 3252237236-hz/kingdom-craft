param(
    [string]$ZigPath = 'E:\SkyCraftKCD2Tools\zig-windows-x86_64-0.13.0\zig.exe'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ZigPath)) {
    throw "Zig compiler not found: $ZigPath"
}

$output = Join-Path $PSScriptRoot '.build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$source = Join-Path $PSScriptRoot 'skycraft_bridge.cpp'
$test = Join-Path $PSScriptRoot 'bridge_selftest.cpp'
$dll = Join-Path $output 'skycraft_bridge.dll'
$importLibrary = Join-Path $output 'skycraft_bridge.lib'
$exe = Join-Path $output 'bridge_selftest.exe'

& $ZigPath c++ -std=c++20 -O2 -shared -o $dll $source
if ($LASTEXITCODE -ne 0) { throw 'Bridge DLL build failed' }
& $ZigPath c++ -std=c++20 -O2 -o $exe $test $importLibrary
if ($LASTEXITCODE -ne 0) { throw 'Bridge self-test build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Bridge self-test failed' }

Write-Output $dll
