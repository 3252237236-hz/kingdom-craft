param(
    [switch]$MinecraftOnly
)

$ErrorActionPreference = 'Stop'
$toolsRoot = Split-Path -Parent $PSCommandPath
$fabricRoot = Join-Path $toolsRoot 'fabric-src'
$java = Join-Path $toolsRoot 'JDK25\jdk-25.0.4.1+1\bin\java.exe'
$launchConfig = Join-Path $fabricRoot '.gradle\loom-cache\launch.cfg'
$argumentFile = Join-Path $fabricRoot 'build\loom-cache\argFiles\runClient'
$gameDirectory = Join-Path $fabricRoot 'run'

foreach ($required in @($java, $launchConfig, $argumentFile, $gameDirectory)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "SkyCraft runtime file is missing: $required"
    }
}

$client = Get-CimInstance Win32_Process -Filter "Name = 'java.exe'" |
    Where-Object { $_.CommandLine -like "*fabric.dli.config=$launchConfig*" } |
    Select-Object -First 1

if ($client) {
    Write-Host "Minecraft client already running (PID $($client.ProcessId))."
} else {
    $javaArguments = @(
        '-Xmx2G',
        '-Dskycraft.startHidden=true',
        '-Dskycraft.quitWithSkyrim=false',
        '-Dskycraft.kcd2.passthrough=true',
        "-Dfabric.dli.config=$launchConfig",
        '-Dfabric.dli.env=client',
        '-Dfabric.dli.main=net.fabricmc.loader.impl.launch.knot.KnotClient',
        "@$argumentFile",
        '--enable-native-access=ALL-UNNAMED',
        '-XX:StackShadowPages=32',
        '--sun-misc-unsafe-memory-access=allow',
        '-Dfile.encoding=UTF-8',
        'net.fabricmc.devlaunchinjector.Main',
        '--username', 'Dovahkiin'
    )
    $startArgs = @{
        FilePath = $java
        ArgumentList = $javaArguments
        WorkingDirectory = $gameDirectory
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = Join-Path $toolsRoot 'minecraft-client.out.log'
        RedirectStandardError = Join-Path $toolsRoot 'minecraft-client.err.log'
    }
    $client = Start-Process @startArgs
    Write-Host "Minecraft client started (PID $($client.Id))."
}

if (-not $MinecraftOnly) {
    $steam = 'D:\Steam\steam.exe'
    if (-not (Test-Path -LiteralPath $steam)) {
        throw "Steam executable is missing: $steam"
    }
    Start-Process -FilePath $steam -ArgumentList '-applaunch', '1771300'
    Write-Host 'Started Kingdom Come: Deliverance II through Steam.'
}
