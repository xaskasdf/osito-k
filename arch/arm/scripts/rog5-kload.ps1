#requires -Version 5.0
<#
.SYNOPSIS
    Boot an Osito-K Image on the ROG Phone 5 over its USB console, without flashing.

.DESCRIPTION
    Talks to the running Osito-K through its CDC-ACM serial port
    (VID_1209&PID_0001): types `kload`, sends "OKLD" + size + CRC-32 + the Image,
    waits for the device to accept it and disconnect, then waits for the new
    kernel's console to come back and runs `uptime` on it.
    Device side and protocol: arch/arm/platform/sm8350/kload.c.

.PARAMETER Image
    Raw kernel Image (arch/arm/build/Image), not boot.img.

.PARAMETER Port
    COM port. Default: the one whose PNP ID is VID_1209&PID_0001.

.EXAMPLE
    .\arch\arm\scripts\rog5-kload.ps1 -Image .\arch\arm\build\rog5\Image-sm8350
#>
param(
    [Parameter(Mandatory = $true)] [string]$Image,
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[kload] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[kload] ERROR: $m" -ForegroundColor Red; exit 1 }

. (Join-Path $PSScriptRoot 'rog5-serial.ps1')

Add-Type -TypeDefinition @'
public static class OsitoCrc32 {
    public static uint Compute(byte[] data) {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data) {
            crc ^= b;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }
}
'@

$data = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Image).Path)
$crc = [OsitoCrc32]::Compute($data)
if (-not $Port) { $Port = Find-OsitoPort }
if (-not $Port) { Fail 'Osito-K USB console not found (VID_1209&PID_0001): is it booted, cable in the side port?' }
Info ("{0}: {1} bytes, crc32 {2:x8}" -f $Port, $data.Length, $crc)

# --- Send ---------------------------------------------------------------------
$sp = Open-Port $Port
if (-not $sp) { Fail "could not open $Port" }
try {
    Start-Sleep -Milliseconds 200
    $null = $sp.ReadExisting()
    $sp.Write("kload`r")
    $out = Read-Until $sp 'kload: ready' 5
    if ($out -notmatch 'kload: ready') { Fail "no 'kload: ready' from the device. Got: $out" }

    [byte[]]$hdr = @(0x4F, 0x4B, 0x4C, 0x44) +
                   [BitConverter]::GetBytes([uint32]$data.Length) +
                   [BitConverter]::GetBytes([uint32]$crc)
    $t0 = Get-Date
    $sp.Write($hdr, 0, $hdr.Length)
    for ($off = 0; $off -lt $data.Length; $off += 65536) {
        $sp.Write($data, $off, [Math]::Min(65536, $data.Length - $off))
    }
    $out = Read-Until $sp 'kload: (\d+ bytes, crc ok|timeout|crc mismatch|bad size|out of memory|not an ARM64)' 30
    $secs = ((Get-Date) - $t0).TotalSeconds
    $last = "$($out -split "`n" | Where-Object { $_ -match 'kload: ' -and $_ -notmatch 'ready' } | Select-Object -Last 1)".Trim()
    if ($out -match 'kload: (timeout|crc mismatch|bad size|out of memory|not an ARM64)') {
        Fail "device rejected the Image: $last"
    }
    if ($out -match 'crc ok') { Info ("sent in {0:N1} s; device: {1}" -f $secs, $last) }
    else { Info ("sent in {0:N1} s; the device disconnected before confirming" -f $secs) }
} finally {
    Close-Port $sp
}

# --- Wait for the new kernel ------------------------------------------------------
Info 'waiting for the new kernel on USB...'
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $deadline -and (Find-OsitoPort)) { Start-Sleep -Milliseconds 200 }
$deadline = (Get-Date).AddSeconds(60)
$newPort = ''
while ((Get-Date) -lt $deadline -and -not $newPort) {
    Start-Sleep -Milliseconds 500
    $newPort = Find-OsitoPort
}
if (-not $newPort) { Fail 'the new kernel did not come back on USB within 60 s (check the screen)' }

$sp = Open-Port $newPort
if (-not $sp) { Fail "could not open $newPort" }
try {
    Start-Sleep -Milliseconds 300
    $null = $sp.ReadExisting()
    $sp.Write("uptime`r")
    $out = Read-Until $sp 'shell> $' 5
    if ($out -notmatch 'Up ') { Fail "no answer from the new kernel. Got: $out" }
    Info ("new kernel up on {0}: {1}" -f $newPort, (($out -split "`n" | Where-Object { $_ -match '^Up ' }) -join '').Trim())
} finally {
    Close-Port $sp
}
