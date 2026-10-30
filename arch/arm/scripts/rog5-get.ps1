#requires -Version 5.0
<#
.SYNOPSIS
    Copy a file from the ROG Phone 5's OsitoFS to this PC over its USB console.

.DESCRIPTION
    Talks to the running Osito-K through its CDC-ACM serial port
    (VID_1209&PID_0001): types `get <name>`, then takes "OKLD" + size +
    CRC-32 + the file (put's framing, the other way) and checks the CRC. The
    filesystem must be mounted (`ufs mount`). Device side:
    arch/arm/platform/sm8350/kload.c.

.PARAMETER Name
    The file's name on the phone.

.PARAMETER Out
    Where to write it. Default: the same name in the current directory.

.PARAMETER Port
    COM port. Default: the one whose PNP ID is VID_1209&PID_0001.

.EXAMPLE
    .\arch\arm\scripts\rog5-get.ps1 -Name screen.bmp
#>
param(
    [Parameter(Mandatory = $true)] [string]$Name,
    [string]$Out = '',
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[get] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[get] ERROR: $m" -ForegroundColor Red; exit 1 }

. (Join-Path $PSScriptRoot 'rog5-serial.ps1')

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Text.RegularExpressions;

public static class OsitoGet {
    public static uint Crc32(byte[] data) {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data) {
            crc ^= b;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    // After `get <name>`: skip the text up to "OKLD", then read the size,
    // the CRC and the bytes. Returns null with the device's answer in text
    // when it refuses, or on idleMs of silence.
    public static byte[] Receive(SerialPort sp, int idleMs, out string text, out uint crcSent) {
        sp.ReadTimeout = idleMs;
        var pre = new MemoryStream();
        byte[] chunk = new byte[65536];
        int pos = 0, len = 0, matched = 0;
        byte[] magic = { 0x4F, 0x4B, 0x4C, 0x44 };
        Regex refused = new Regex(@"(get: (?!sending)|usage: get)[^\r\n]*\r?\n");
        text = "";
        crcSent = 0;
        while (matched < 4) {
            if (pos == len) {
                text = Encoding.ASCII.GetString(pre.ToArray());
                if (refused.IsMatch(text)) return null;
                try { len = sp.Read(chunk, 0, chunk.Length); } catch (TimeoutException) { return null; }
                pos = 0;
            }
            byte b = chunk[pos++];
            pre.WriteByte(b);
            if (b == magic[matched]) matched++;
            else matched = (b == magic[0]) ? 1 : 0;
        }
        text = Encoding.ASCII.GetString(pre.ToArray(), 0, (int)pre.Length - 4);

        byte[] hdr = new byte[8];
        if (!Fill(sp, chunk, ref pos, ref len, hdr)) return null;
        uint size = BitConverter.ToUInt32(hdr, 0);
        crcSent = BitConverter.ToUInt32(hdr, 4);
        byte[] data = new byte[size];
        if (!Fill(sp, chunk, ref pos, ref len, data)) return null;
        return data;
    }

    // dst from what is left in chunk, then from the port
    static bool Fill(SerialPort sp, byte[] chunk, ref int pos, ref int len, byte[] dst) {
        int got = 0;
        while (got < dst.Length) {
            if (pos == len) {
                try { len = sp.Read(chunk, 0, chunk.Length); } catch (TimeoutException) { return false; }
                pos = 0;
            }
            int n = Math.Min(len - pos, dst.Length - got);
            Buffer.BlockCopy(chunk, pos, dst, got, n);
            pos += n;
            got += n;
        }
        return true;
    }
}
'@

if ($Name -match '\s') { Fail "the name can't contain spaces: '$Name'" }
if (-not $Out) { $Out = $Name }
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (-not $Port) { $Port = Find-OsitoPort }
if (-not $Port) { Fail 'Osito-K USB console not found (VID_1209&PID_0001): is it booted, cable in the side port?' }

$sp = Open-Port $Port
if (-not $sp) { Fail "could not open $Port" }
try {
    Start-Sleep -Milliseconds 200
    $null = $sp.ReadExisting()
    $t0 = Get-Date
    $sp.Write("get $Name`r")
    $text = ''
    $crcSent = [uint32]0
    $data = [OsitoGet]::Receive($sp, 5000, [ref]$text, [ref]$crcSent)
    $secs = ((Get-Date) - $t0).TotalSeconds
    if ($null -eq $data) {
        $answer = ($text -split "`r?`n" | Where-Object { $_ -match 'get' -and $_ -ne "get $Name" } | Select-Object -Last 1)
        if (-not $answer) { $answer = 'no answer (timeout)' }
        Fail "device: $("$answer".Trim())"
    }
    $null = Read-Until $sp 'bytes sent\r?\n' 3
} finally {
    Close-Port $sp
}

$crc = [OsitoGet]::Crc32($data)
if ($crc -ne $crcSent) { Fail ("crc mismatch: device {0:x8}, received {1:x8}" -f $crcSent, $crc) }
[IO.File]::WriteAllBytes($Out, $data)
$rate = $data.Length / 1KB / [Math]::Max($secs, 0.001)
Info ("{0}: {1} -> {2}, {3} bytes, crc ok, in {4:N1} s ({5:N0} KB/s)" -f $Port, $Name, $Out, $data.Length, $secs, $rate)
