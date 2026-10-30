#requires -Version 5.0
<#
.SYNOPSIS
    Copy a file onto the ROG Phone 5's OsitoFS over its USB console.

.DESCRIPTION
    Talks to the running Osito-K through its CDC-ACM serial port
    (VID_1209&PID_0001): types `put <name>`, sends "OKLD" + size + CRC-32 + the
    file (the kload framing), and waits for the device to store it. The
    filesystem must be mounted (`ufs mount`). Device side:
    arch/arm/platform/sm8350/kload.c.

.PARAMETER File
    The file to send (up to 64 MB).

.PARAMETER Name
    Its name on the phone. Default: the file's own name.

.PARAMETER Port
    COM port. Default: the one whose PNP ID is VID_1209&PID_0001.

.EXAMPLE
    .\arch\arm\scripts\rog5-put.ps1 -File .\hello.elf
#>
param(
    [Parameter(Mandatory = $true)] [string]$File,
    [string]$Name = '',
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[put] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[put] ERROR: $m" -ForegroundColor Red; exit 1 }

. (Join-Path $PSScriptRoot 'rog5-serial.ps1')

Add-Type -TypeDefinition @'
public static class OsitoPutCrc32 {
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

$path = (Resolve-Path -LiteralPath $File).Path
if (-not $Name) { $Name = [IO.Path]::GetFileName($path) }
if ($Name -match '\s') { Fail "the name can't contain spaces: '$Name'" }
$data = [IO.File]::ReadAllBytes($path)
if ($data.Length -eq 0) { Fail 'empty file' }
$crc = [OsitoPutCrc32]::Compute($data)
if (-not $Port) { $Port = Find-OsitoPort }
if (-not $Port) { Fail 'Osito-K USB console not found (VID_1209&PID_0001): is it booted, cable in the side port?' }
Info ("{0}: {1} -> {2}, {3} bytes, crc32 {4:x8}" -f $Port, $path, $Name, $data.Length, $crc)

$sp = Open-Port $Port
if (-not $sp) { Fail "could not open $Port" }
try {
    Start-Sleep -Milliseconds 200
    $null = $sp.ReadExisting()
    $sp.Write("put $Name`r")
    $out = Read-Until $sp 'put: (ready|no filesystem)' 5
    if ($out -notmatch 'put: ready') { Fail "the device isn't ready: $($out.Trim())" }

    [byte[]]$hdr = @(0x4F, 0x4B, 0x4C, 0x44) +
                   [BitConverter]::GetBytes([uint32]$data.Length) +
                   [BitConverter]::GetBytes([uint32]$crc)
    $t0 = Get-Date
    $sp.Write($hdr, 0, $hdr.Length)
    for ($off = 0; $off -lt $data.Length; $off += 65536) {
        $sp.Write($data, $off, [Math]::Min(65536, $data.Length - $off))
    }
    $wait = 30 + [int]($data.Length / 1MB) * 2
    $out = Read-Until $sp 'put: (.+, crc ok, .+|timeout.*|crc mismatch.*|bad size|out of memory)\r?\n' $wait
    $secs = ((Get-Date) - $t0).TotalSeconds
    $last = "$($out -split "`n" | Where-Object { $_ -match 'put: ' -and $_ -notmatch 'ready' } | Select-Object -Last 1)".Trim()
    if ($last -notmatch 'verified') { Fail "device: $last" }
    $rate = $data.Length / 1KB / [Math]::Max($secs, 0.001)
    Info ("{0} in {1:N1} s ({2:N0} KB/s)" -f $last, $secs, $rate)
} finally {
    Close-Port $sp
}
