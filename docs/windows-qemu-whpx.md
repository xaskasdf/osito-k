# QEMU en Windows: WHPX vs TCG para OsitoK

> Resumen: **WHPX no puede bootear OsitoK** en este Windows (QEMU 11.0 + edk2).
> Corremos **TCG multi-thread**. Este doc deja la investigación y un plan de
> patch por si en algún momento queremos aceleración por hardware.

## TL;DR — cómo correr

Desde un shell **MSYS2 MINGW64** (no PowerShell — `build-windows.ps1` usa `&&`,
que no parsea en PS 5.1):

```bash
# build + run headless
OK_BUILD=1 ./arch/x86/scripts/run-windows.sh

# run de UT99 (ventana + NVMe), captura [NAMEHASH] en build/serial.log
OK_DISPLAY=gtk OK_NVME=/c/Users/xasko/osito-k/nvme_ut99.img ./arch/x86/scripts/run-windows.sh
```

Build del kernel/boot con clang: `make -C arch/x86 CLANG=1 build/boot.efi build/kernel.elf`
(ver [project_windows_clang_x86_build] en memoria / commits `def8254`, `9a0f046`).

## El problema con WHPX

`-accel whpx` **inicializa bien** (no hay error de partición), pero **OVMF
crashea en `PlatformPei`** con `#GP (General Protection)` a los ~1.3 KB de
serial, **antes** de llegar a `boot.efi`:

```
!!!! X64 Exception Type - 0D(#GP - General Protection)  CPU Apic ID - 00000000 !!!!
RIP  - 0000000000834EEE ... PlatformPei.dll
```

### Causa raíz
WHPX tiene emulación de CPU incompleta. OVMF hace un **`WRMSR`** temprano (setup
de MTRR / APIC en PlatformPei) que **WHPX rechaza → inyecta `#GP`** en el guest.
Es la combinación de dos limitaciones upstream conocidas, **ambas abiertas**:

- [qemu#2461](https://gitlab.com/qemu-project/qemu/-/issues/2461) — WHPX no setea permisos de `WRMSR`.
- [qemu#934](https://gitlab.com/qemu-project/qemu/-/issues/934) / [qemu#2887](https://gitlab.com/qemu-project/qemu/-/work_items/2887) — WHPX + OVMF (ROMD pflash + APIC emulation).

### Qué se probó (todo da el mismo `#GP`)
| Variante | Resultado |
|---|---|
| `-drive if=pflash` (code+vars) | #GP |
| `-bios ovmf-unified.fd` (vars+code concatenados, 4 MB) | #GP |
| `-smp 1` y `-smp 4` | #GP (smp4 además crashea en los APs) |
| `-machine q35,smm=off` | #GP |
| `-global ICH9-LPC.disable_s3=1` | #GP |
| `-cpu host,-x2apic,-pmu,-rdrand` | #GP |

El workaround famoso **`-bios` en vez de `-pflash`** ([qemu#513](https://gitlab.com/qemu-project/qemu/-/issues/513)) arregla un
síntoma **distinto** (fallo de MMIO/ROMD del pflash, con error en stderr). El
nuestro es el `WRMSR` → `#GP` del guest, **sin** error en stderr. No aplica.

El workaround de "legacy BIOS + grub chainload" tampoco sirve: **OsitoK
requiere UEFI** (boot.efi usa BootServices: AllocatePages, GOP, ExitBootServices).

### Validación de que es WHPX (no firmware malo)
La **misma `ovmf-unified.fd` bootea perfecto con `-accel tcg`** (`OsitoK boot.efi
→ kernel.elf → HEAP/HKDF/RSA self-tests PASS`). Por lo tanto el firmware es
válido y el `#GP` es 100% culpa de WHPX.

## Workaround actual: TCG multi-thread

```
-accel tcg,thread=multi -cpu max -machine q35 -m 512M -smp 4 -bios ovmf-unified.fd
```

`thread=multi` corre los 4 vCPU del guest en 4 threads del host (en vez de
serializar todo en uno). No es hardware-accel, pero para capturar el `[NAMEHASH]`
de UT99 (es init del engine, no gameplay) alcanza. Validado: bootea completo.

## Plan de patch (DIFERIDO — primero vemos cómo va UT99 en TCG)

Para que WHPX bootee OVMF habría que **parchear el backend WHPX de QEMU** y
recompilar desde fuente.

1. **Diagnóstico fino**: el QEMU de msys2 viene **sin backend de tracing**
   (`--trace 'whpx*'` da 0 líneas), así que el MSR exacto no es capturable acá.
   Para identificarlo: compilar QEMU con `--enable-trace-backends=log` y correr
   con `--trace 'whpx_unsupported_msr_access'` (evento que loguea el MSR
   rechazado), o instrumentar `whpx-all.c`.
2. **Fix**: en `target/i386/whpx/whpx-all.c`, el handler de salidas por `WRMSR`
   debe **emular** los MSR que OVMF escribe en boot (rango MTRR `0x200`–`0x2FF`,
   `IA32_MTRR_DEF_TYPE 0x2FF`, posible `IA32_APIC_BASE 0x1B`) en lugar de dejar
   que WHPX inyecte `#GP`. Referencia: cómo lo maneja el backend KVM en
   `target/i386/kvm/kvm.c` (`kvm_put_msrs`/MTRR) — portar esa lógica a WHPX.
   Relacionado con el permiso `WRMSR` de #2461.
3. **Build en Windows**: QEMU desde fuente en MSYS2 (meson + ninja + glib2 +
   pixman + un montón de `mingw-w64-x86_64-*` dev). Reemplazar `qemu-system-x86_64`.
4. **Riesgo**: aunque se arreglen los MTRR MSR, WHPX podría rechazar **otras**
   operaciones de OVMF después (el backend es ampliamente incompleto). Payoff
   incierto — validar con un build de prueba antes de invertir.

**Esfuerzo: alto.** Solo vale la pena si la velocidad de TCG resulta
bloqueante para el debugging de UT99.
