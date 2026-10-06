#requires -RunAsAdministrator

$ErrorActionPreference = 'Stop'

if ((Get-CimInstance Win32_ComputerSystem).AutomaticManagedPagefile) {
    throw 'Automatic pagefile management is enabled; no settings were changed.'
}

$dBefore = Get-CimInstance Win32_PageFileSetting |
    Where-Object { $_.Name -ieq 'D:\pagefile.sys' }
if (-not $dBefore) {
    throw 'Expected D: pagefile setting was not found; no settings were changed.'
}

$cBefore = Get-CimInstance Win32_PageFileSetting |
    Where-Object { $_.Name -ieq 'C:\pagefile.sys' }
if ($cBefore) {
    if ($cBefore.InitialSize -eq 8192 -and $cBefore.MaximumSize -eq 8192) {
        Write-Output 'C: pagefile is already configured at 8192 MB.'
        exit 0
    }
    throw 'C: pagefile already has a different setting; no settings were changed.'
}

$cVolume = Get-Volume -DriveLetter C
if ($cVolume.SizeRemaining -lt 10GB) {
    throw 'C: needs at least 10 GB free before adding an 8 GB pagefile.'
}

New-CimInstance -ClassName Win32_PageFileSetting -Property @{
    Name = 'C:\pagefile.sys'
    InitialSize = [uint32]8192
    MaximumSize = [uint32]8192
} | Out-Null

$cAfter = Get-CimInstance Win32_PageFileSetting |
    Where-Object { $_.Name -ieq 'C:\pagefile.sys' }
$dAfter = Get-CimInstance Win32_PageFileSetting |
    Where-Object { $_.Name -ieq 'D:\pagefile.sys' }
if (-not $cAfter -or $cAfter.InitialSize -ne 8192 -or $cAfter.MaximumSize -ne 8192 -or
    -not $dAfter -or $dAfter.InitialSize -ne $dBefore.InitialSize -or
    $dAfter.MaximumSize -ne $dBefore.MaximumSize) {
    throw 'Pagefile settings did not match the requested values; inspect Windows settings.'
}

Write-Output 'C: pagefile configured at 8192 MB; D: setting preserved. Restart Windows to apply.'
