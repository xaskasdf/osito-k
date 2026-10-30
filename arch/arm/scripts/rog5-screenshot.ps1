#requires -Version 5.0
<#
.SYNOPSIS
    Take a screenshot of the ROG Phone 5's panel and save it here as a PNG.

.DESCRIPTION
    Runs `screenshot` on the running Osito-K (the panel as screen.bmp in
    OsitoFS), fetches it with rog5-get.ps1 and converts it to PNG.

.PARAMETER Out
    The PNG to write. Default: rog5-screen-<date>-<time>.png in the current
    directory.

.PARAMETER Port
    COM port. Default: the one whose PNP ID is VID_1209&PID_0001.

.EXAMPLE
    .\arch\arm\scripts\rog5-screenshot.ps1 -Out desk.png
#>
param(
    [string]$Out = '',
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[screenshot] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[screenshot] ERROR: $m" -ForegroundColor Red; exit 1 }

if (-not $Out) { $Out = 'rog5-screen-{0:yyyyMMdd-HHmmss}.png' -f (Get-Date) }
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$portArgs = @{}
if ($Port) { $portArgs.Port = $Port }

$answer = & (Join-Path $PSScriptRoot 'rog5-console.ps1') -Command 'screenshot' `
    -Expect 'screenshot: .*(bytes|failed|no [a-z]+.*|memory)\r?\n' -Timeout 30 @portArgs
if ($LASTEXITCODE -ne 0 -or $answer -notmatch 'bytes') { Fail "device: $("$answer".Trim())" }
Info "$("$answer".Trim())"

$bmp = Join-Path ([IO.Path]::GetTempPath()) 'rog5-screen.bmp'
& (Join-Path $PSScriptRoot 'rog5-get.ps1') -Name screen.bmp -Out $bmp @portArgs
if ($LASTEXITCODE -ne 0) { exit 1 }

Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile($bmp)
try {
    $img.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $img.Dispose()
}
Remove-Item -LiteralPath $bmp
Info ("{0} ({1:N0} KB)" -f $Out, ((Get-Item -LiteralPath $Out).Length / 1KB))
