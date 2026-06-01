# OsitoK en Windows — arranque rápido

Guía para compilar y correr el kernel bare-metal x86-64 de OsitoK en Windows,
y continuar el debugging de UT99 (Win32 compat layer) en paralelo.

El proyecto se sincroniza desde el Mac vía `rsync` sobre SSH a
`C:\Users\xasko\osito-k`. El toolchain bundleado en `tools/` es Linux/Mac y
**no corre en Windows** — hay que tener un `x86_64-elf-gcc` + `gnu-efi`
nativos (ver paso 2).

---

## 0. Requisitos base

- **msys2** en `C:\msys64` (https://www.msys2.org). Trae `bash`, `pacman`,
  y la rama `mingw64` donde viven `make`, `mtools`, `qemu`.
- **PowerShell 5.1+** (el de Windows alcanza).
- (Opcional pero recomendado para acelerar QEMU) **Windows Hypervisor
  Platform** activado: *Activar o desactivar características de Windows* →
  marcar "Plataforma de hipervisor de Windows" → reiniciar. Sin esto, usar
  `-Accel tcg` (más lento).

---

## 1. Flujo con `build-windows.ps1`

Desde PowerShell, parado en `C:\Users\xasko\osito-k`:

```powershell
# 1) Ver qué falta
.\build-windows.ps1 -Check

# 2) Instalar lo que está en pacman (make, mtools, qemu + firmware edk2)
.\build-windows.ps1 -Install

# 3) (tras tener el cross-toolchain, ver paso 2 abajo) compilar y correr
.\build-windows.ps1                 # WHPX + ventana gtk
.\build-windows.ps1 -Accel tcg      # si no tenés WHPX
.\build-windows.ps1 -BuildOnly      # solo compilar
.\build-windows.ps1 -Rebuild        # forzar recompilación
```

El `.ps1` es un puente: hace todo el trabajo pesado a través del shell
`mingw64` de msys2, delegando build + QEMU en
`arch/x86/scripts/qemu-windows.sh`.

---

## 2. El cross-toolchain (lo único que pacman NO resuelve)

`build-windows.ps1 -Check` te marcará en amarillo:

- **`x86_64-elf-gcc` / `-ld` / `-objcopy`** — compilador cruzado ELF
  bare-metal. Opciones:
  - Prebuilt Windows (buscar binarios `x86_64-elf-tools` análogos a
    `lordmilko/i686-elf-tools`), o
  - construirlo con `crosstool-ng` dentro de msys2.
  - Dejá su `bin/` en el PATH de mingw64 (agregalo a `~/.bashrc` de msys2:
    `export PATH="/c/ruta/al/cross/bin:$PATH"`).
- **`gnu-efi`** — headers/libs EFI. Clonar https://github.com/ncroxon/gnu-efi
  y `make` con el cross-gcc; instalar bajo `/mingw64` (o exportar
  `EFI_PREFIX=/ruta` y pasarlo al make).

Una vez que `-Check` los muestre en verde, el paso 3 funciona.

---

## 3. Qué corre QEMU

`qemu-windows.sh` espeja la invocación validada en macOS (`qemu-cocoa.sh`),
con tres cambios para Windows:

| macOS            | Windows                         |
|------------------|---------------------------------|
| `-accel hvf`     | `-accel whpx,kernel-irqchip=off` (fallback TCG) |
| `-display cocoa` | `-display gtk`                  |
| firmware OVMF    | `edk2-x86_64-code.fd` del mingw-qemu |

Config: `q35`, `-m 512M`, `-smp 4`, virtio-net con `hostfwd udp:7778->7777`
y `tcp:50052->50052`, xHCI + teclado/tablet USB. Si existe
`arch/x86/build/nvme.img` (la imagen con UT99/DOOM) se adjunta como NVMe.

**Serial log** (clave para el debugging): `arch\x86\build\serial.log`.

---

## 4. Imagen NVMe con UT99

El runner adjunta `arch/x86/build/nvme.img` si existe. Para armarla en
Windows hay que portar el flujo de `tools/ositofs/` (mkfs/write) y meter
`Core.dll`, `Engine.u`, `UnrealTournament.exe` (ya sincronizados en la raíz
del repo). Mientras tanto, se puede copiar la `nvme.img` ya construida desde
el Mac vía el mismo rsync.

---

## 5. Contexto de debugging (memorias de Claude)

Todo el journey de UT99 (root causes, breakthroughs, patches quirúrgicos)
está en las memorias de Claude, sincronizadas a:

```
C:\Users\xasko\.claude\projects\C--Users-xasko-osito-k\memory\
```

`MEMORY.md` es el índice. Las más relevantes para seguir el debugging de
Win32/UT99:

- `project_ut99_processregistrants_throw.md` — cascade root en
  ProcessRegistrants Phase 2 (estado más reciente del Phase 7).
- `project_ut99_register_chain_decoded.md` / `_works.md` — cómo funciona la
  cadena GObjRegistrants.
- `project_ut99_fname_*` — saga FName::Names (root cause "package 0").
- `project_wdbg_toolkit.md` — primitivas de debug Win32 reutilizables.
- `project_ut99_binary_patch_methodology.md` — workflow de parches binarios
  vía `compat32_dispatch`.
- `project_compat32_jmpbuf_overflow.md` — fix del overflow de jmpbuf.

Al abrir el proyecto con Claude Code en Windows, esas memorias se
auto-cargan (el slug del proyecto es `C--Users-xasko-osito-k`).
