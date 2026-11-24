#requires -Version 5.0
<#
.SYNOPSIS
    OsitoK x86-64 en Windows: build con clang + QEMU con TCG.

.DESCRIPTION
    Puente PowerShell hacia el entorno msys2/mingw64. Compila boot.efi +
    kernel.elf con clang (make -C arch/x86 CLANG=1) y los bootea con
    arch/x86/scripts/run-windows.sh (QEMU TCG multi-thread, serial log en
    arch\x86\build\serial.log).

    No hay aceleracion por hardware: WHPX no puede bootear OVMF (#GP en
    PlatformPei antes de llegar a boot.efi; ver docs/windows-qemu-whpx.md).
    Para los tests de host (necesitan Linux) ver arch/x86/scripts/remote-test.sh.

.PARAMETER Check
    Solo reporta el estado de las dependencias y sale.

.PARAMETER Install
    Instala via pacman las dependencias disponibles (todas menos gnu-efi).

.PARAMETER BuildOnly
    Compila pero no lanza QEMU.

.PARAMETER NoBuild
    Lanza QEMU con el kernel.elf ya compilado, sin pasar por make.

.PARAMETER Display
    gtk (default), sdl o none (headless: todo queda en el serial log).

.PARAMETER Nvme
    Imagen NVMe a adjuntar, p.ej. .\nvme_ut99.img.

.PARAMETER Msys2Root
    Raiz de msys2. Default: C:\msys64.

.EXAMPLE
    .\build-windows.ps1 -Check
.EXAMPLE
    .\build-windows.ps1 -Install
.EXAMPLE
    .\build-windows.ps1                              # build + QEMU con ventana gtk
.EXAMPLE
    .\build-windows.ps1 -Display none -Nvme .\nvme_ut99.img
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Install,
    [switch]$BuildOnly,
    [switch]$NoBuild,
    [ValidateSet('gtk','sdl','none')] [string]$Display = 'gtk',
    [string]$Nvme = '',
    [string]$Msys2Root = 'C:\msys64'
)

$ErrorActionPreference = 'Stop'

function Info($m)  { Write-Host "[osito-win] $m" -ForegroundColor Cyan }
function Ok($m)    { Write-Host "[osito-win] $m" -ForegroundColor Green }
function Warn($m)  { Write-Host "[osito-win] $m" -ForegroundColor Yellow }
function Fail($m)  { Write-Host "[osito-win] ERROR: $m" -ForegroundColor Red; exit 1 }

# --- Localizar msys2/bash ----------------------------------------------------
$Bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path $Bash)) {
    Fail "No encuentro bash de msys2 en $Bash. Instala msys2 (https://www.msys2.org) o pasa -Msys2Root <ruta>."
}

# Ruta msys de un path de Windows. Ej: D:\projects\osito-k -> /d/projects/osito-k
function ConvertTo-MsysPath([string]$Path) {
    $full = (Resolve-Path -LiteralPath $Path).Path
    return "/" + $full.Substring(0,1).ToLower() + ($full.Substring(2) -replace '\\','/')
}
$RepoMsys = ConvertTo-MsysPath $PSScriptRoot

# Ejecuta un comando en el shell de login mingw64 (PATH con /mingw64/bin).
# La salida va directo a la consola: si no, se mezclaria con el exit code
# que devuelve la funcion.
function Invoke-Mingw([string]$Cmd) {
    $env:MSYSTEM        = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    & $Bash -lc $Cmd | Out-Host
    return $LASTEXITCODE
}

# --- Chequeo de dependencias -------------------------------------------------
$deps = @(
    @{ name='make';         cmd='make';               pacman='make';                        note='' },
    @{ name='mtools';       cmd='mcopy';              pacman='mingw-w64-x86_64-mtools';     note='arma la ESP image' },
    @{ name='qemu';         cmd='qemu-system-x86_64'; pacman='mingw-w64-x86_64-qemu';       note='emulador + firmware edk2' },
    @{ name='clang';        cmd='clang';              pacman='mingw-w64-x86_64-clang';      note='compila con --target=x86_64-elf' },
    @{ name='ld.lld';       cmd='ld.lld';             pacman='mingw-w64-x86_64-lld';        note='' },
    @{ name='llvm-objcopy'; cmd='llvm-objcopy';       pacman='mingw-w64-x86_64-llvm-tools'; note='' },
    @{ name='gnu-efi';      cmd=''; file='/usr/local/lib/elf_x86_64_efi.lds'; pacman='';    note='NO esta en pacman' }
)

function Test-Deps {
    Info "Verificando dependencias en mingw64..."
    $missing = @()
    foreach ($d in $deps) {
        if ($d.cmd) { $probe = "command -v $($d.cmd) >/dev/null 2>&1" }
        else        { $probe = "test -f $($d.file)" }
        Invoke-Mingw $probe | Out-Null
        $present = ($LASTEXITCODE -eq 0)
        $tag = if ($present) { 'OK   ' } else { 'FALTA' }
        $col = if ($present) { 'Green' } else { 'Yellow' }
        $extra = if ($d.note) { "  ($($d.note))" } else { '' }
        Write-Host ("  [{0}] {1,-14}{2}" -f $tag, $d.name, $extra) -ForegroundColor $col
        if (-not $present) { $missing += $d }
    }
    return ,$missing
}

function Show-GnuEfiHelp {
    Warn "gnu-efi se instala a mano: compilar https://github.com/ncroxon/gnu-efi e"
    Warn "  instalar headers/libs bajo /usr/local de msys2 (C:\msys64\usr\local)."
    Warn "  arch/x86/Makefile lo detecta ahi; si no, pasar EFI_PREFIX=<ruta> a make."
}

$missing = Test-Deps

if ($Check) {
    if ($missing.Count -eq 0) { Ok "Todas las dependencias presentes." }
    else {
        if ($missing | Where-Object { $_.pacman }) {
            Warn "Para las que estan en pacman corre:  .\build-windows.ps1 -Install"
        }
        if ($missing | Where-Object { $_.name -eq 'gnu-efi' }) { Show-GnuEfiHelp }
    }
    exit 0
}

# --- Install (solo paquetes pacman) ------------------------------------------
if ($Install) {
    $pkgs = ($deps | Where-Object { $_.pacman } | ForEach-Object { $_.pacman }) -join ' '
    Info "pacman -S --needed $pkgs"
    $rc = Invoke-Mingw "pacman -S --needed --noconfirm $pkgs"
    if ($rc -ne 0) { Fail "pacman fallo (rc=$rc). Abre 'MSYS2 MINGW64' y reintenta manualmente." }
    Ok "Paquetes pacman listos."
    if ($missing | Where-Object { $_.name -eq 'gnu-efi' }) { Show-GnuEfiHelp }
    exit 0
}

# --- Build + Run -------------------------------------------------------------
if ($missing.Count -gt 0) {
    Fail "Faltan dependencias. Corre primero:  .\build-windows.ps1 -Check"
}

if (-not $NoBuild) {
    Info "Compilando boot.efi + kernel.elf (clang)..."
    $rc = Invoke-Mingw "cd '$RepoMsys' && make -C arch/x86 CLANG=1 -j`$(nproc) build/boot.efi build/kernel.elf"
    if ($rc -ne 0) { Fail "Build fallo (rc=$rc)." }
    Ok "Build OK: arch\x86\build\{boot.efi,kernel.elf}"
}
if ($BuildOnly) { exit 0 }

$nvmeMsys = ''
if ($Nvme) {
    if (-not (Test-Path -LiteralPath $Nvme)) { Fail "No existe la imagen NVMe: $Nvme" }
    $nvmeMsys = ConvertTo-MsysPath $Nvme
}

Info "QEMU TCG (display=$Display). Serial log: arch\x86\build\serial.log"
$rc = Invoke-Mingw "cd '$RepoMsys' && OK_DISPLAY='$Display' OK_NVME='$nvmeMsys' bash arch/x86/scripts/run-windows.sh"
if ($rc -ne 0) { Warn "QEMU salio con rc=$rc."; exit $rc }
Ok "QEMU termino. Serial log: arch\x86\build\serial.log"
