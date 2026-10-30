[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Kernel,
    [Parameter(Mandatory=$true)][string]$KernelSha256,
    [Parameter(Mandatory=$true)][string]$PublicKey,
    [Parameter(Mandatory=$true)][string]$Python,
    [Parameter(Mandatory=$true)][string]$Output,
    [int]$InterfaceIndex = 14,
    [string]$InterfaceMac = 'F0-2F-74-1F-4F-51',
    [string]$Source = '169.254.167.94',
    [string]$Peer = '169.254.167.93',
    [ValidateRange(60,14400)][int]$Seconds = 7200,
    [switch]$Serve
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Explicit, Size = 28)]
public struct OsitoTransferAddress {
    [FieldOffset(0)] public ushort Family;
    [FieldOffset(4)] public uint IPv4;
}
[StructLayout(LayoutKind.Sequential)]
public struct OsitoTransferRow {
    public OsitoTransferAddress Address;
    public ulong InterfaceLuid;
    public uint InterfaceIndex;
    public int PrefixOrigin, SuffixOrigin;
    public uint ValidLifetime, PreferredLifetime;
    public byte OnLinkPrefixLength, SkipAsSource;
    public int DadState;
    public uint ScopeId;
    public long CreationTimeStamp;
}
public static class OsitoTransferIp {
    [DllImport("iphlpapi.dll", ExactSpelling = true)]
    public static extern void InitializeUnicastIpAddressEntry(ref OsitoTransferRow row);
    [DllImport("iphlpapi.dll", ExactSpelling = true)]
    public static extern uint CreateUnicastIpAddressEntry(ref OsitoTransferRow row);
    [DllImport("iphlpapi.dll", ExactSpelling = true)]
    public static extern uint GetUnicastIpAddressEntry(ref OsitoTransferRow row);
    [DllImport("iphlpapi.dll", ExactSpelling = true)]
    public static extern uint DeleteUnicastIpAddressEntry(ref OsitoTransferRow row);
}
'@
if ([Runtime.InteropServices.Marshal]::SizeOf([type][OsitoTransferRow]) -ne 80 -or
    [Runtime.InteropServices.Marshal]::OffsetOf([type][OsitoTransferRow], 'InterfaceIndex').ToInt32() -ne 40 -or
    [Runtime.InteropServices.Marshal]::OffsetOf([type][OsitoTransferRow], 'DadState').ToInt32() -ne 64) {
    throw 'IP Helper ABI mismatch'
}
$kernelPath = (Resolve-Path -LiteralPath $Kernel).Path
$publicPath = (Resolve-Path -LiteralPath $PublicKey).Path
$pythonPath = (Resolve-Path -LiteralPath $Python).Path
$serverPath = Join-Path $PSScriptRoot 'oftp-server.py'
if ((Get-FileHash -LiteralPath $kernelPath -Algorithm SHA256).Hash -ine $KernelSha256) {
    throw 'Kernel hash mismatch'
}
$publicText = [IO.File]::ReadAllText($publicPath)
if ($publicText -notmatch '^ssh-ed25519 [A-Za-z0-9+/]{68}(\s|$)' -or $publicText -match 'PRIVATE KEY') {
    throw 'Expected an Ed25519 public key, not a private key'
}
$adapter = Get-NetAdapter -InterfaceIndex $InterfaceIndex
if ($adapter.Status -ne 'Up' -or $adapter.MacAddress -ne $InterfaceMac) {
    throw 'Expected Ethernet adapter is not up'
}
if (@(Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -eq $Source).Count -or
    @(Get-NetRoute -AddressFamily IPv4 | Where-Object DestinationPrefix -eq "$Peer/32").Count) {
    throw 'Temporary address/route already exists; refusing to overwrite it'
}
if (@(Get-NetUDPEndpoint -LocalPort 7781 -ErrorAction SilentlyContinue).Count) {
    throw 'UDP 7781 already in use'
}
if (-not $Serve) {
    Write-Output 'Validated files, kernel hash, IP Helper ABI and Ethernet. No changes made.'
    exit 0
}
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator token required for temporary IP/route/firewall'
}
if (Test-Path -LiteralPath $Output) { throw 'Output directory must be new' }
[void][IO.Directory]::CreateDirectory($Output)
$Output = (Resolve-Path -LiteralPath $Output).Path
$root = Join-Path $Output 'serve'
[void][IO.Directory]::CreateDirectory($root)
Copy-Item -LiteralPath $kernelPath -Destination (Join-Path $root 'kernel.elf')
Copy-Item -LiteralPath $publicPath -Destination (Join-Path $root 'osito-dev.pub')
if ((Get-FileHash -LiteralPath (Join-Path $root 'kernel.elf') -Algorithm SHA256).Hash -ine $KernelSha256) {
    throw 'Copied kernel hash mismatch'
}
Start-Transcript -Path (Join-Path $Output 'setup.log') | Out-Null
$before = Get-NetIPInterface -InterfaceIndex $InterfaceIndex -AddressFamily IPv4
$row = New-Object OsitoTransferRow
[OsitoTransferIp]::InitializeUnicastIpAddressEntry([ref]$row)
$address = New-Object OsitoTransferAddress
$address.Family = 2
$address.IPv4 = [BitConverter]::ToUInt32(([Net.IPAddress]::Parse($Source)).GetAddressBytes(), 0)
$row.Address = $address
$row.InterfaceLuid = 0
$row.InterfaceIndex = $InterfaceIndex
$row.PrefixOrigin = 1
$row.SuffixOrigin = 1
$row.ValidLifetime = $Seconds + 60
$row.PreferredLifetime = $Seconds + 60
$row.OnLinkPrefixLength = 32
$row.SkipAsSource = 1
$created = $false
$route = $null
$firewall = $null
$server = $null
$result = [ordered]@{State='starting'; Source=$Source; Peer=$Peer; Port=7781; KernelSha256=$KernelSha256; DhcpBefore=[string]$before.Dhcp; HelperPid=$PID}
try {
    $rc = [OsitoTransferIp]::CreateUnicastIpAddressEntry([ref]$row)
    if ($rc) { throw "CreateUnicastIpAddressEntry failed: $rc" }
    $created = $true
    $dadDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 250
        $rc = [OsitoTransferIp]::GetUnicastIpAddressEntry([ref]$row)
        if ($rc -or $row.DadState -eq 2) { throw 'IP address conflict or query failure' }
    } while ($row.DadState -ne 4 -and [DateTime]::UtcNow -lt $dadDeadline)
    if ($row.DadState -ne 4) { throw 'Address duplicate detection did not finish' }
    $route = New-NetRoute -DestinationPrefix "$Peer/32" -InterfaceIndex $InterfaceIndex -NextHop '0.0.0.0' `
        -RouteMetric 1 -PolicyStore ActiveStore -ValidLifetime ([TimeSpan]::FromSeconds($Seconds + 60)) `
        -PreferredLifetime ([TimeSpan]::FromSeconds($Seconds + 60))
    if ((Get-NetIPInterface -InterfaceIndex $InterfaceIndex -AddressFamily IPv4).Dhcp -ne $before.Dhcp) {
        throw 'DHCP state changed unexpectedly'
    }
    $firewall = New-NetFirewallRule -Name ('OsitoK-OFTP-' + [Guid]::NewGuid().ToString('N')) `
        -DisplayName 'OsitoK temporary development transfer' -Direction Inbound -Action Allow `
        -Protocol UDP -LocalAddress $Source -LocalPort 7781 -RemoteAddress $Peer -Program $pythonPath -Profile Any
    $arguments = '-u "{0}" {1} 7781 "{2}" --read-only' -f $serverPath, $Source, $root
    $server = Start-Process -FilePath $pythonPath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Output 'oftp.log') -RedirectStandardError (Join-Path $Output 'oftp-error.log')
    Start-Sleep -Seconds 1
    if ($server.HasExited) { throw "OFTP server exited: $($server.ExitCode)" }
    if (-not @(Get-NetUDPEndpoint -LocalAddress $Source -LocalPort 7781 | Where-Object OwningProcess -eq $server.Id).Count) {
        throw 'OFTP socket not owned by the new server'
    }
    & ping.exe -4 -S $Source -n 2 -w 1000 $Peer
    $result.PingExit = $LASTEXITCODE
    $result.State = 'ready'
    $result.ServerPid = $server.Id
    $result.ExpiresUtc = [DateTime]::UtcNow.AddSeconds($Seconds).ToString('o')
    $result.FirewallRule = $firewall.Name
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'status.json') -Encoding UTF8
    $end = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $end -and -not (Test-Path -LiteralPath (Join-Path $Output 'stop'))) {
        if ($server.HasExited) { throw 'OFTP server terminated early' }
        Start-Sleep -Seconds 1
    }
} catch {
    $result.Error = [string]$_
    Write-Output $result.Error
} finally {
    $cleanupErrors = @()
    try {
        if ($server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id
            if (-not $server.WaitForExit(10000)) { throw 'OFTP process did not exit' }
        }
    } catch { $cleanupErrors += [string]$_ }
    try { if ($firewall) { $firewall | Remove-NetFirewallRule } }
    catch { $cleanupErrors += [string]$_ }
    try { if ($route) { $route | Remove-NetRoute -Confirm:$false } }
    catch { $cleanupErrors += [string]$_ }
    try {
        if ($created) { $result.AddressCleanup = [OsitoTransferIp]::DeleteUnicastIpAddressEntry([ref]$row) }
        $result.DhcpAfter = [string](Get-NetIPInterface -InterfaceIndex $InterfaceIndex -AddressFamily IPv4).Dhcp
    } catch { $cleanupErrors += [string]$_ }
    $result.CleanupErrors = $cleanupErrors
    $result.State = 'stopped'
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'status.json') -Encoding UTF8
    Stop-Transcript | Out-Null
}
