#requires -Version 5.0
<#
.SYNOPSIS
    OsitoK x86-64 — arranque rapido en Windows (build + QEMU).

.DESCRIPTION
    Orquesta el build del kernel bare-metal x86-64 y lo corre en QEMU sobre
    Windows, apoyandose en el entorno msys2/mingw64 (donde viven make, mtools,
    qemu y bash). Hace de puente: chequea dependencias, instala las que estan
    en pacman, y delega el build+run a arch/x86/scripts/qemu-windows.sh.

    Equivalencias respecto al flujo macOS del repo:
      HVF           -> WHPX (Windows Hypervisor Platform)  [-Accel whpx|tcg]
      -display cocoa-> -display gtk                         [-Display gtk|sdl|none]
      toolchain bundleado en tools/ (Linux/Mac) -> NO sirve en Windows;
      hace falta un x86_64-elf-gcc + gnu-efi nativos (ver -Check).

.PARAMETER Check
    Solo reporta el estado de las dependencias y sale.

.PARAMETER Install
    Instala via pacman las dependencias disponibles en msys2
    (make, mtools, mingw-w64-x86_64-qemu).

.PARAMETER Rebuild
    Fuerza recompilacion aunque kernel.elf ya exista.

.PARAMETER BuildOnly
    Compila pero no lanza QEMU.

.PARAMETER Accel
    whpx (default) o tcg. WHPX requiere activar "Windows Hypervisor Platform"
    en "Caracteristicas de Windows".

.PARAMETER Display
    gtk (default), sdl o none.

.PARAMETER Msys2Root
    Raiz de msys2. Default: C:\msys64.

.EXAMPLE
    .\build-windows.ps1 -Check
.EXAMPLE
    .\build-windows.ps1 -Install
.EXAMPLE
    .\build-windows.ps1                 # build + run con WHPX + gtk
.EXAMPLE
    .\build-windows.ps1 -Accel tcg      # si no tienes WHPX
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Install,
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [ValidateSet('whpx','tcg')] [string]$Accel = 'whpx',
    [ValidateSet('gtk','sdl','none')] [string]$Display = 'gtk',
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

# Ruta msys ( /c/Users/... ) del repo, derivada de la ubicacion del script.
# Ej: C:\Users\xasko\osito-k  ->  /c/Users/xasko/osito-k
$RepoWin  = $PSScriptRoot
$drive    = $RepoWin.Substring(0,1).ToLower()
$RepoMsys = "/$drive" + ($RepoWin.Substring(2) -replace '\\','/')

# Ejecuta un comando en el shell de login mingw64 (PATH con /mingw64/bin).
function Invoke-Mingw([string]$Cmd) {
    $env:MSYSTEM      = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    & $Bash -lc $Cmd
    return $LASTEXITCODE
}

# --- Chequeo de dependencias -------------------------------------------------
$deps = @(
    @{ name='bash';            cmd='bash';            pacman='';                          note='msys2 base' },
    @{ name='make';            cmd='make';            pacman='make';                      note='' },
    @{ name='mtools (mcopy)';  cmd='mcopy';           pacman='mtools';                    note='arma la ESP image' },
    @{ name='qemu';            cmd='qemu-system-x86_64'; pacman='mingw-w64-x86_64-qemu';  note='emulador + firmware edk2' },
    @{ name='dd';              cmd='dd';              pacman='coreutils';                 note='' },
    @{ name='x86_64-elf-gcc';  cmd='x86_64-elf-gcc';  pacman='';                          note='CROSS toolchain bare-metal (NO esta en pacman)' },
    @{ name='gnu-efi';         cmd='';                file='/mingw64/include/efi/efi.h';  pacman='';  note='headers EFI (NO esta en pacman)' }
)

function Test-Deps {
    Info "Verificando dependencias en mingw64..."
    $missing = @()
    foreach ($d in $deps) {
        $present = $false
        if ($d.cmd) {
            Invoke-Mingw "command -v $($d.cmd) >/dev/null 2>&1" | Out-Null
            $present = ($LASTEXITCODE -eq 0)
        } elseif ($d.file) {
            Invoke-Mingw "test -f $($d.file)" | Out-Null
            $present = ($LASTEXITCODE -eq 0)
        }
        $tag = if ($present) { 'OK   ' } else { 'FALTA' }
        $col = if ($present) { 'Green' } else { 'Yellow' }
        $extra = if ($d.note) { "  ($($d.note))" } else { '' }
        Write-Host ("  [{0}] {1,-18}{2}" -f $tag, $d.name, $extra) -ForegroundColor $col
        if (-not $present) { $missing += $d }
    }
    return ,$missing
}

$missing = Test-Deps

if ($Check) {
    if ($missing.Count -eq 0) { Ok "Todas las dependencias presentes." }
    else {
        Warn "Faltan $($missing.Count). Para las de pacman corre:  .\build-windows.ps1 -Install"
        $crossMissing = $missing | Where-Object { -not $_.pacman }
        if ($crossMissing) {
            Warn "Sin paquete pacman (instalacion manual):"
            Warn "  - x86_64-elf-gcc/ld/objcopy: cross toolchain ELF bare-metal."
            Warn "      Opciones: build con crosstool-ng, o un prebuilt x86_64-elf de"
            Warn "      https://github.com/lordmilko/i686-elf-tools / equivalentes x86_64."
            Warn "      Luego agrega su bin al PATH de mingw64 (~/.bashrc o variable)."
            Warn "  - gnu-efi: clona https://github.com/ncroxon/gnu-efi y 'make' con el"
            Warn "      cross-gcc; instala headers/libs bajo /mingw64 (o ajusta EFI_PREFIX)."
            Warn "  NOTA: el toolchain bundleado en tools/ es Linux/Mac y NO corre en Windows."
        }
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
    Warn "Recorda: x86_64-elf-gcc y gnu-efi NO estan en pacman — ver  .\build-windows.ps1 -Check"
    exit 0
}

# --- Build + Run -------------------------------------------------------------
$cross = $missing | Where-Object { $_.name -eq 'x86_64-elf-gcc' -or $_.name -eq 'gnu-efi' }
if ($cross) {
    Fail "Falta el cross toolchain / gnu-efi. Corre primero:  .\build-windows.ps1 -Check"
}

$env:OK_ACCEL   = $Accel
$env:OK_DISPLAY = $Display
if ($Rebuild) { $env:OK_REBUILD = '1' } else { $env:OK_REBUILD = '0' }

if ($BuildOnly) {
    Info "Compilando (sin lanzar QEMU)..."
    $rc = Invoke-Mingw "cd '$RepoMsys' && make -C arch/x86"
    if ($rc -ne 0) { Fail "Build fallo (rc=$rc)." }
    Ok "Build OK: arch/x86/build/{boot.efi,kernel.elf}"
    exit 0
}

Info "Build + QEMU (accel=$Accel, display=$Display)..."
$runner = "$RepoMsys/arch/x86/scripts/qemu-windows.sh"
$rc = Invoke-Mingw "cd '$RepoMsys' && chmod +x '$runner' && OK_ACCEL='$Accel' OK_DISPLAY='$Display' OK_REBUILD='$($env:OK_REBUILD)' bash '$runner'"
if ($rc -ne 0) {
    if ($Accel -eq 'whpx') {
        Warn "QEMU salio con rc=$rc. Si fue WHPX, reintenta:  .\build-windows.ps1 -Accel tcg"
    }
    exit $rc
}
Ok "QEMU termino. Serial log: arch\x86\build\serial.log"
