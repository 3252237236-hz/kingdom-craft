param(
    [string]$OutputDirectory = "$PSScriptRoot\dist"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$modRoot = Join-Path $OutputDirectory 'SkyCraftKCD2'
$dataRoot = Join-Path $modRoot 'Data'
$pakPath = Join-Path $dataRoot 'skycraft_kcd2.pak'
$manifestPath = Join-Path $PSScriptRoot 'mod.manifest'
[xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
$modId = [string]$manifest.kcd_mod.info.modid
if ($modId -cnotmatch '^[a-z_]+$') {
    throw "Invalid KCD2 modid: $modId"
}
$startupEntry = "Scripts/Startup/$modId.lua"
$startupPath = Join-Path $PSScriptRoot "content\Scripts\Startup\$modId.lua"
if (-not (Test-Path -LiteralPath $startupPath -PathType Leaf)) {
    throw "Missing KCD2 startup script: $startupPath"
}

New-Item -ItemType Directory -Force -Path $dataRoot | Out-Null
Copy-Item -LiteralPath $manifestPath -Destination $modRoot -Force
if (Test-Path -LiteralPath $pakPath) {
    Remove-Item -LiteralPath $pakPath
}
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    (Join-Path $PSScriptRoot 'content'),
    $pakPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)
$archive = [System.IO.Compression.ZipFile]::OpenRead($pakPath)
try {
    if (-not $archive.GetEntry($startupEntry)) {
        throw "Packaged PAK is missing $startupEntry"
    }
} finally {
    $archive.Dispose()
}
Write-Output $modRoot
