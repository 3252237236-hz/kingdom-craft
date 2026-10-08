param(
    [string]$Output = 'E:\SkyCraftKCD2Tools\candidates\combat-handshake-20261009',
    [string]$CMake = 'E:\SkyCraftKCD2Tools\VSBuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = Join-Path $Output 'approval-contract-build'
New-Item -ItemType Directory -Path $Output -Force | Out-Null
& $CMake -S (Join-Path $repo 'kcd2-port\tests\native-contracts') -B $build -G 'Visual Studio 17 2022' -A x64 *> (Join-Path $Output 'attack-contract-build.log')
if ($LASTEXITCODE) { throw 'Contract CMake configure failed.' }
& $CMake --build $build --config Release >> (Join-Path $Output 'attack-contract-build.log')
if ($LASTEXITCODE) { throw 'Contract build failed.' }
$executable = Join-Path $build 'Release\attack_contract.exe'
& $executable *> (Join-Path $Output 'attack-contract.log')
if ($LASTEXITCODE) { throw 'Attack approval contract failed.' }
$text = Get-Content (Join-Path $Output 'attack-contract.log') -Raw
if ($text -notmatch 'native attack approval contracts passed \((\d+) checks\)') { throw 'Contract result missing.' }
$checks = [int]$Matches[1]
$sources = @{}
foreach ($file in @('kcd2-port/host/skycraft_attack_approvals.hpp','kcd2-port/bridge/skycraft_kcd2_protocol.hpp','kcd2-port/tests/attack_approvals_contract.cpp')) {
    $sources[$file] = (Get-FileHash (Join-Path $repo $file)).Hash
}
@{ checks=$checks; sourceSha256=$sources; executableSha256=(Get-FileHash $executable).Hash } |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Output 'attack-contract-evidence.json') -Encoding utf8
Write-Output $text.Trim()
