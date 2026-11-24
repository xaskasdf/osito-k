#requires -Version 5.0
<#
.SYNOPSIS
    Serial helpers for the Osito-K USB console, dot-sourced by rog5-kload.ps1
    and rog5-console.ps1.

.DESCRIPTION
    The console is the CDC-ACM gadget of arch/arm/platform/sm8350/dwc3_acm.c,
    VID_1209&PID_0001 (COM3 on the dev box).
#>

function Find-OsitoPort {
    $p = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
         Where-Object { $_.PNPDeviceID -match 'VID_1209&PID_0001' } | Select-Object -First 1
    if ($p) { return $p.DeviceID }
    return ''
}

# Wait up to $Seconds for the console to enumerate; '' if it doesn't
function Wait-OsitoPort([double]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        $port = Find-OsitoPort
        if ($port) { return $port }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    return ''
}

# $null if it can't be opened (usbser may need a moment after enumeration)
function Open-Port([string]$Name) {
    for ($i = 0; $i -lt 20; $i++) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort $Name, 115200, 'None', 8, 'One'
            $sp.DtrEnable = $true
            $sp.WriteTimeout = 20000
            $sp.Open()
            return $sp
        } catch { Start-Sleep -Milliseconds 250 }
    }
    return $null
}

# Read until $Pattern matches the accumulated text, $Seconds pass, or the
# device disconnects (it does right after accepting an Image, or rebooting)
function Read-Until($sp, [string]$Pattern, [double]$Seconds) {
    $buf = ''
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        try { $buf += $sp.ReadExisting() } catch { break }
        if ($buf -match $Pattern) { break }
        Start-Sleep -Milliseconds 20
    }
    return $buf
}

function Close-Port($sp) {
    try { if ($sp.IsOpen) { $sp.Close() } } catch { }
}
