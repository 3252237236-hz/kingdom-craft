$ErrorActionPreference = 'Stop'
$toolsRoot = 'E:\SkyCraftKCD2Tools'
$name = 'world-combat-repair-20261007'
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

function Update-DepthConfig([string]$path) {
    $text = [IO.File]::ReadAllText($path)
    $values = "DepthCopyBeforeClears=1`r`nDepthCopyAtClearIndex=0`r`nUseAspectRatioHeuristics=1`r`n"
    $regex = [regex]'(?ms)^\[DEPTH\][^\r\n]*\r?\n(?<body>.*?)(?=^\[|\z)'
    $match = $regex.Match($text)
    if ($match.Success) {
        $body = [regex]::Replace($match.Groups['body'].Value,
            '(?m)^(DepthCopyBeforeClears|DepthCopyAtClearIndex|UseAspectRatioHeuristics)\s*=.*\r?\n?', '')
        $text = $text.Remove($match.Index, $match.Length).Insert($match.Index, "[DEPTH]`r`n" + $body.TrimEnd() + "`r`n" + $values + "`r`n")
    } else {
        $text += "`r`n[DEPTH]`r`n" + $values
    }
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}

Assert-RuntimesClosed
$verification = Get-Content -LiteralPath (Join-Path $release 'build-verification.json') -Raw | ConvertFrom-Json
$manifest = Get-Content -LiteralPath (Join-Path $release 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest) {
    $file = Join-Path $release $entry.file
    Assert-Contained $file $release
    if ((Get-FileHash -LiteralPath $file).Hash -ne $entry.sha256) { throw "Staged file mismatch: $file" }
}
$backup = Join-Path $toolsRoot ('backups\' + $name + '-' + (Get-Date -Format 'HHmmss'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$targets = @(
    @{ source = 'd3d12.dll'; target = (Join-Path $gameBin 'd3d12.dll'); root = $gameBin },
    @{ source = 'SkyCraftKCD2.addon64'; target = (Join-Path $gameBin 'SkyCraftKCD2.addon64'); root = $gameBin },
    @{ source = 'SkyCraftKCD2.fx'; target = (Join-Path $gameBin 'reshade-shaders\Shaders\SkyCraftKCD2.fx'); root = $gameBin },
    @{ source = 'skycraft-0.1.2.jar'; target = (Join-Path $runtimeBuild 'libs\skycraft-0.1.2.jar'); root = $runtimeBuild }
)
$fileBackups = @()
$index = 0
foreach ($target in $targets) {
    Assert-Contained $target.target $target.root
    if (-not (Test-Path -LiteralPath $target.target)) { throw "Installed component missing: $($target.target)" }
    $saved = Join-Path $backup ($index.ToString() + '-' + [IO.Path]::GetFileName($target.target))
    Copy-Item -LiteralPath $target.target -Destination $saved
    $fileBackups += @{ target = $target.target; saved = $saved }
    $index++
}
$configs = @(Get-ChildItem -LiteralPath $gameBin -Filter 'ReShade*.ini' -File)
if (-not ($configs.Name -contains 'ReShade.ini')) { throw 'ReShade global configuration missing.' }
foreach ($config in $configs) {
    $saved = Join-Path $backup $config.Name
    Copy-Item -LiteralPath $config.FullName -Destination $saved
    $fileBackups += @{ target = $config.FullName; saved = $saved }
}
$save = Join-Path $runtimeRoot 'run\saves\SkyCraft'
if (Test-Path -LiteralPath $save) {
    Copy-Item -LiteralPath $save -Destination (Join-Path $backup 'SkyCraft-save') -Recurse
}
foreach ($log in @((Join-Path $runtimeRoot 'run\logs\latest.log'), (Join-Path $gameBin 'ReturnOfModding\LogOutput.log'))) {
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $backup ([IO.Path]::GetFileName($log))) }
}
$moved = @()
try {
    Assert-RuntimesClosed
    foreach ($folder in @('classes', 'resources')) {
        $current = Join-Path $runtimeBuild $folder
        $old = Join-Path $backup ('runtime-' + $folder)
        Assert-Contained $current $runtimeBuild
        Assert-Contained $old $backup
        if (Test-Path -LiteralPath $current) {
            Move-Item -LiteralPath $current -Destination $old
            $moved += @{ current = $current; old = $old }
        }
        Copy-Item -LiteralPath (Join-Path $release ('runtime\' + $folder)) -Destination $current -Recurse
    }
    Assert-RuntimesClosed
    foreach ($target in $targets) { Copy-Item -LiteralPath (Join-Path $release $target.source) -Destination $target.target -Force }
    foreach ($config in $configs) { Update-DepthConfig $config.FullName }
    foreach ($target in $targets) {
        if ((Get-FileHash -LiteralPath $target.target).Hash -ne (Get-FileHash -LiteralPath (Join-Path $release $target.source)).Hash) {
            throw "Installed component hash failed: $($target.target)"
        }
    }
    foreach ($folder in @('classes', 'resources')) {
        $sourceRoot = Join-Path $release ('runtime\' + $folder)
        foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
            $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\')
            $installed = Join-Path (Join-Path $runtimeBuild $folder) $relative
            if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $installed).Hash) {
                throw "Installed runtime mismatch: $installed"
            }
        }
    }
} catch {
    Assert-RuntimesClosed
    foreach ($file in $fileBackups) { Copy-Item -LiteralPath $file.saved -Destination $file.target -Force }
    foreach ($folder in $moved) {
        $failed = Join-Path $backup ('failed-' + [IO.Path]::GetFileName($folder.current))
        Assert-Contained $folder.current $runtimeBuild
        Assert-Contained $failed $backup
        Assert-Contained $folder.old $backup
        if (Test-Path -LiteralPath $folder.current) { Move-Item -LiteralPath $folder.current -Destination $failed }
        Move-Item -LiteralPath $folder.old -Destination $folder.current
    }
    throw
}
$record = [ordered]@{
    candidate = $name
    installedAt = (Get-Date).ToString('o')
    status = 'installed-awaiting-user-gameplay'
    gameBin = $gameBin
    backup = $backup
    release = $release
    installedHostSha256 = $verification.hostSha256
    installedAddonSha256 = $verification.addonSha256
    installedShaderSha256 = $verification.shaderSha256
    fabricJarSha256 = $verification.fabricJarSha256
    activeFabricRuntime = (Join-Path $runtimeBuild 'classes')
    activeClassesCheckedAgainstJar = $verification.candidateClassesCheckedAgainstJar
    javaTests = $verification.javaTests
    minecraftIsolatedRuntimeChecks = $verification.minecraftRuntimeChecks
    reshadeIsolatedRuntimeChecks = $verification.reshadeRuntimeChecks
    updatedDepthConfigurations = @($configs.Name)
    inGameVerified = $false
    gameplayLimits = '当前伤害参数已按实际游戏绑定修正，但原生 NPC 死亡尚未实测；击退桥接为带障碍检查的水平位移，保留原生重力；真实相机校准、遮挡和粒子显示待用户存档验证。'
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $toolsRoot ($name + '-installation.json')) -Encoding utf8
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'installation.json') -Encoding utf8
$fileBackups | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'restoration-files.json') -Encoding utf8
$updatedManifest = Get-ChildItem -LiteralPath $release -Recurse -File | Where-Object { $_.Name -ne 'manifest.json' } |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{ file = $_.FullName.Substring($release.Length).TrimStart('\').Replace('\', '/');
            sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash; bytes = $_.Length }
    }
$updatedManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
$record | ConvertTo-Json -Depth 5
