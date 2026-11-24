#requires -Version 5.0
<#
.SYNOPSIS
    Run one shell command on the Osito-K USB console and print its output.

.DESCRIPTION
    Types -Command on the running Osito-K's console (VID_1209&PID_0001) and
    reads until -Expect matches, -Timeout passes, or the device disconnects
    (`reboot bootloader` does). Prints what came back without the echo.
    Exit code 0 if -Expect matched, 1 if not, 2 if no console showed up
    within -WaitPort seconds.

    rog5-boot.sh --flash uses it to send `reboot bootloader` and, after
    flashing, to check the new kernel's build stamp with `uname`.

.EXAMPLE
    .\arch\arm\scripts\rog5-console.ps1 -Command 'cpus'
    .\arch\arm\scripts\rog5-console.ps1 -WaitPort 120 -Command uname -Expect 'built .*'
#>
param(
    [Parameter(Mandatory = $true)] [string]$Command,
    [string]$Expect = 'shell> $',
    [double]$Timeout = 10,
    [double]$WaitPort = 0,
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rog5-serial.ps1')

if (-not $Port) { $Port = Wait-OsitoPort $WaitPort }
if (-not $Port) { exit 2 }
$sp = Open-Port $Port
if (-not $sp) { exit 2 }
try {
    Start-Sleep -Milliseconds 300
    $null = $sp.ReadExisting()
    $sp.Write("$Command`r")
    $out = Read-Until $sp $Expect $Timeout
} finally {
    Close-Port $sp
}
# Drop the echoed command line and the trailing prompt
$lines = $out -split "`r?`n" | Where-Object { $_ -ne $Command -and $_ -notmatch '^shell> ?$' }
Write-Output (($lines -join "`n").Trim())
if ($out -match $Expect) { exit 0 }
exit 1
