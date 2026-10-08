$ErrorActionPreference = 'Stop'
$toolsRoot = 'E:\SkyCraftKCD2Tools'
$name = 'shared-scene-20261009'
$release = Join-Path $toolsRoot ('releases\' + $name)
$gameBin = 'D:\Steam\steamapps\common\KingdomComeDeliverance2\Bin\Win64MasterMasterSteamPGO'
$runtimeRoot = Join-Path $toolsRoot 'fabric-src'
$runtimeBuild = Join-Path $runtimeRoot 'build'

function Assert-RuntimesClosed {
    if (Get-Process -Name KingdomCome -ErrorAction SilentlyContinue) {
        throw 'Game is running; installation must wait for a normal exit.'
    }
    $clients = Get-CimInstance Win32_Process | Where-Object {
        $_.Name -match '^javaw?\.exe$' -and
        ($_.CommandLine -like '*skycraft.startHidden=true*' -or $_.CommandLine -like '*SkyCraftKCD2Tools\fabric-src*')
    }
    if ($clients) { throw 'Minecraft is running; save and quit normally before installation.' }
}

function Assert-Contained([string]$path, [string]$root) {
    $resolved = [IO.Path]::GetFullPath($path)
    $boundary = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path outside intended root: $resolved"
    }
}

Assert-RuntimesClosed
$verification = Get-Content -LiteralPath (Join-Path $release 'build-verification.json') -Raw | ConvertFrom-Json
if ($verification.candidate -ne $name -or $verification.javaTests -lt 38 -or $verification.minecraftRuntimeChecks -lt 51 -or $verification.shaderRuntimeChecks -lt 13) {
    throw 'Candidate verification missing.'
}
$manifest = Get-Content -LiteralPath (Join-Path $release 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest) {
    $file = Join-Path $release $entry.file
    Assert-Contained $file $release
    if ((Get-FileHash -LiteralPath $file).Hash -ne $entry.sha256) { throw "Release file mismatch: $file" }
}
# The unchanged addon remains installed; the shader, host and MC runtime change.
foreach ($item in @(
    @{ path = (Join-Path $gameBin 'SkyCraftKCD2.addon64'); hash = $verification.addonSha256 }
)) {
    Assert-Contained $item.path $gameBin
    if ((Get-FileHash -LiteralPath $item.path).Hash -ne $item.hash) { throw 'Installed compositor differs from verified baseline.' }
}
$targets = @(
    @{ source = 'd3d12.dll'; target = (Join-Path $gameBin 'd3d12.dll'); root = $gameBin },
    @{ source = 'skycraft-0.1.2.jar'; target = (Join-Path $runtimeBuild 'libs\skycraft-0.1.2.jar'); root = $runtimeBuild },
    @{ source = 'SkyCraftKCD2.fx'; target = (Join-Path $gameBin 'reshade-shaders\Shaders\SkyCraftKCD2.fx'); root = $gameBin }
)
$directories = @(
    @{ source = 'runtime\classes'; current = (Join-Path $runtimeBuild 'classes'); root = $runtimeBuild; name = 'runtime-classes' },
    @{ source = 'runtime\resources'; current = (Join-Path $runtimeBuild 'resources'); root = $runtimeBuild; name = 'runtime-resources' },
    @{ source = 'runtime\src'; current = (Join-Path $runtimeRoot 'src'); root = $runtimeRoot; name = 'runtime-source' }
)
foreach ($target in $targets) {
    Assert-Contained $target.target $target.root
    if (-not (Test-Path -LiteralPath $target.target -PathType Leaf)) { throw "Installed file missing: $($target.target)" }
}
foreach ($directory in $directories) {
    Assert-Contained $directory.current $directory.root
    if (-not (Test-Path -LiteralPath $directory.current -PathType Container)) { throw "Installed directory missing: $($directory.current)" }
}
$backup = Join-Path $toolsRoot ('backups\' + $name + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
$fileBackups = @()
foreach ($target in $targets) {
    $saved = Join-Path $backup ([IO.Path]::GetFileName($target.target))
    Copy-Item -LiteralPath $target.target -Destination $saved
    $fileBackups += @{ target = $target.target; saved = $saved }
}
$save = Join-Path $runtimeRoot 'run\saves\SkyCraft'
if (Test-Path -LiteralPath $save) {
    Copy-Item -LiteralPath $save -Destination (Join-Path $backup 'SkyCraft-save') -Recurse
}
foreach ($log in @((Join-Path $runtimeRoot 'run\logs\latest.log'), (Join-Path $gameBin 'ReturnOfModding\LogOutput.log'))) {
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $backup ([IO.Path]::GetFileName($log))) }
}
$fileBackups | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'restoration-files.json') -Encoding utf8
$moved = @()
try {
    Assert-RuntimesClosed
    foreach ($directory in $directories) {
        $old = Join-Path $backup $directory.name
        Assert-Contained $directory.current $directory.root
        Assert-Contained $old $backup
        Move-Item -LiteralPath $directory.current -Destination $old
        $moved += @{ current = $directory.current; old = $old; root = $directory.root }
        $moved | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'restoration-directories.json') -Encoding utf8
        Copy-Item -LiteralPath (Join-Path $release $directory.source) -Destination $directory.current -Recurse
    }
    Assert-RuntimesClosed
    foreach ($target in $targets) { Copy-Item -LiteralPath (Join-Path $release $target.source) -Destination $target.target -Force }
    foreach ($target in $targets) {
        if ((Get-FileHash -LiteralPath $target.target).Hash -ne (Get-FileHash -LiteralPath (Join-Path $release $target.source)).Hash) {
            throw "Installed file mismatch: $($target.target)"
        }
    }
    foreach ($directory in $directories) {
        $sourceRoot = Join-Path $release $directory.source
        foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
            $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\')
            $installed = Join-Path $directory.current $relative
            Assert-Contained $installed $directory.current
            if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $installed).Hash) {
                throw "Installed directory mismatch: $installed"
            }
        }
    }
} catch {
    Assert-RuntimesClosed
    foreach ($file in $fileBackups) { Copy-Item -LiteralPath $file.saved -Destination $file.target -Force }
    foreach ($directory in $moved) {
        $failed = Join-Path $backup ('failed-' + [IO.Path]::GetFileName($directory.current))
        Assert-Contained $directory.current $directory.root
        Assert-Contained $failed $backup
        Assert-Contained $directory.old $backup
        if (Test-Path -LiteralPath $directory.current) { Move-Item -LiteralPath $directory.current -Destination $failed }
        Move-Item -LiteralPath $directory.old -Destination $directory.current
    }
    throw
}
$record = [ordered]@{
    candidate = $name
    installedAt = (Get-Date).ToString('o')
    status = 'installed-awaiting-gameplay-verification'
    gameBin = $gameBin
    backup = $backup
    release = $release
    installedHostSha256 = $verification.hostSha256
    fabricJarSha256 = $verification.fabricJarSha256
    addonSha256 = $verification.addonSha256
    shaderSha256 = $verification.shaderSha256
    activeFabricRuntime = (Join-Path $runtimeBuild 'classes')
    activeClassesCheckedAgainstJar = $verification.candidateClassesCheckedAgainstJar
    javaTests = $verification.javaTests
    minecraftIsolatedRuntimeChecks = $verification.minecraftRuntimeChecks
    minecraftRuntimeChecksBySystem = $verification.minecraftRuntimeChecksBySystem
    shaderRuntimeChecks = $verification.shaderRuntimeChecks
    inGameVerified = $false
    gameplayLimits = 'Shared native scene is bounded ray sampling. Native weapon clashes, NPC death/attacker recognition and actual camera displacement are unverified. Native NPC targeting Minecraft mobs, native mesh destruction, complete inventory/equipment and vertical physics remain incomplete.'
}
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $toolsRoot ($name + '-installation.json')) -Encoding utf8
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $release 'installation.json') -Encoding utf8
$updatedManifest = Get-ChildItem -LiteralPath $release -Recurse -File | Where-Object { $_.Name -ne 'manifest.json' } |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{ file = $_.FullName.Substring($release.Length).TrimStart('\').Replace('\', '/');
            sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash; bytes = $_.Length }
    }
$updatedManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
$record | ConvertTo-Json -Depth 6
