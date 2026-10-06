$ErrorActionPreference = 'Stop'
$toolsRoot = 'E:\SkyCraftKCD2Tools'
$release = Join-Path $toolsRoot 'releases\session-recovery-20261006'
$gameBin = 'D:\Steam\steamapps\common\KingdomComeDeliverance2\Bin\Win64MasterMasterSteamPGO'
$installedDll = Join-Path $gameBin 'd3d12.dll'
$runtimeRoot = Join-Path $toolsRoot 'fabric-src'
$verification = Get-Content -LiteralPath (Join-Path $release 'build-verification.json') -Raw | ConvertFrom-Json

function Assert-RuntimesClosed {
    if (Get-Process -Name KingdomCome -ErrorAction SilentlyContinue) {
        throw 'Game is running; do not replace the DLL.'
    }
    $clients = Get-CimInstance Win32_Process | Where-Object {
        $_.Name -match '^javaw?\.exe$' -and $_.CommandLine -like '*skycraft.startHidden=true*' -and
        $_.CommandLine -like '*SkyCraftKCD2Tools\fabric-src*'
    }
    if ($clients) { throw 'Hidden Minecraft is still running; save and quit it normally first.' }
}

Assert-RuntimesClosed
$candidateDll = Join-Path $release 'd3d12.dll'
$candidateJar = Join-Path $release 'skycraft-0.1.2.jar'
if ((Get-FileHash -LiteralPath $candidateDll).Hash -ne $verification.hostSha256) {
    throw 'Staged DLL differs from verified build.'
}
if ((Get-FileHash -LiteralPath $candidateJar).Hash -ne $verification.fabricJarSha256) {
    throw 'Staged JAR differs from verified build.'
}
$backup = Join-Path $toolsRoot ('backups\session-recovery-20261006-' + (Get-Date -Format HHmmss))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$previousHash = (Get-FileHash -LiteralPath $installedDll).Hash
Copy-Item -LiteralPath $installedDll -Destination (Join-Path $backup 'd3d12.dll')
$save = Join-Path $runtimeRoot 'run\saves\SkyCraft'
if (Test-Path -LiteralPath $save) {
    Copy-Item -LiteralPath $save -Destination (Join-Path $backup 'SkyCraft-save') -Recurse
}
Copy-Item -LiteralPath (Join-Path $runtimeRoot 'run\logs\latest.log') -Destination (Join-Path $backup 'minecraft-normal-shutdown.log')
Copy-Item -LiteralPath (Join-Path $gameBin 'ReturnOfModding\LogOutput.log') -Destination (Join-Path $backup 'previous-game.log')
Copy-Item -LiteralPath (Join-Path $toolsRoot 'releases\combat-shapes-20261006\skycraft-0.1.2.jar') -Destination (Join-Path $backup 'previous-skycraft.jar')
Assert-RuntimesClosed
Copy-Item -LiteralPath $candidateDll -Destination $installedDll -Force
$newHash = (Get-FileHash -LiteralPath $installedDll).Hash
if ($newHash -ne $verification.hostSha256) { throw 'Installed DLL failed hash verification.' }
$record = [ordered]@{
    candidate = 'session-recovery-20261006'
    installedAt = (Get-Date).ToString('o')
    status = 'installed-awaiting-user-gameplay'
    gameDll = $installedDll
    installedSha256 = $newHash
    previousSha256 = $previousHash
    backup = $backup
    staged = $release
    fabricJarSha256 = $verification.fabricJarSha256
    fabricRuntime = (Join-Path $runtimeRoot 'build\classes')
    runtimeClassesCheckedAgainstJar = $verification.runtimeClassesCheckedAgainstJar
    oldClientNormalShutdown = $true
    checks = @('Lua 5.1 host contracts', 'native input/session contracts',
        'collision-shape protocol contracts', 'surface contracts',
        'Fabric compileJava compileClientJava test jar: 30 tests passed',
        'MSVC Release KCD2ModLoader', 'release JAR matches active development classes')
    inGameVerified = $false
}
$json = $record | ConvertTo-Json -Depth 5
$json | Set-Content -LiteralPath (Join-Path $toolsRoot 'session-recovery-installation.json') -Encoding utf8
$json | Set-Content -LiteralPath (Join-Path $release 'installation.json') -Encoding utf8
$manifest = Get-ChildItem -LiteralPath $release -File | Where-Object { $_.Name -ne 'manifest.json' } |
    Sort-Object Name | ForEach-Object {
        [ordered]@{ file = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash; bytes = $_.Length }
    }
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
$json
