# OsitoK: Separación Kernel/Bootloader + Road to Self-Compiling

## El problema: somos una aplicación EFI

OsitoK actualmente es un **PE32+ EFI binary** (`ositok.efi`). UEFI lo carga, le da
control, llamamos `ExitBootServices()`, y seguimos corriendo el mismo .efi como kernel.

Esto es como si Linux fuera GRUB — funciona, pero impone restricciones que no
debería tener un kernel:

```
Actual:   UEFI → ositok.efi (PE32+ shared object, -fPIE, gnu-efi linker script)
                  ↑ bootloader y kernel son el mismo binario

Correcto: UEFI → boot.efi (bootloader, ~300 líneas)
                    → carga kernel.bin (flat binary o ELF)
                    → jump a kernel entry
                  kernel.bin: compilado sin restricciones EFI
```

### Restricciones que impone el formato EFI

| Restricción | Causa | Impacto |
|-------------|-------|---------|
| `-fPIE` obligatorio | OVMF puede relocar el .efi a cualquier dirección | Código más lento (GOT indirection en cada global) |
| `-fno-jump-tables` | Jump tables generan offsets sin relocación, fallan post-reloc | switch/case no optimizado, if-else chains en su lugar |
| No asm con globals | `"m"(var)` genera R_X86_64_PC32 que falla en .so PIE | Workarounds en inline asm (leer CS/SS via mov, etc.) |
| objcopy PE32+ | Hay que convertir ELF .so → PE32+ con secciones explícitas | Frágil, perdemos info, .rodata DEBE incluirse explícito |
| gnu-efi linker script | `elf_x86_64_efi.lds` controla el layout | No podemos usar higher-half mapping ni linker script propio |
| gnu-efi headers | `<efi.h>`, `<efilib.h>` requeridos para boot | Mezcla boot y kernel en un solo compilation unit |
| TCC no puede generar PE32+ | TCC genera ELF, no PE+reloc+objcopy | **Self-hosting kernel imposible con formato actual** |

### Cómo bootean los OS reales

```
DOS:        BIOS → MBR → boot sector → IO.SYS → MSDOS.SYS → COMMAND.COM
Linux:      UEFI → GRUB/systemd-boot → vmlinuz (bzImage propio, descomprime, long mode)
            Alternativa: UEFI → EFISTUB (vmlinuz es PE32+ wrapper de un kernel ELF)
Windows:    UEFI → Windows Boot Manager → winload.efi → ntoskrnl.exe (PE)
FreeBSD:    UEFI → loader.efi → kernel (ELF64)
OsitoK:     UEFI → ositok.efi → (sigue corriendo como PE32+)  ← esto
```

Ningún OS serio corre su kernel como EFI application. El bootloader es EFI; el
kernel es formato propio.

---

## Diseño: boot.efi + kernel.bin

### boot.efi — Bootloader UEFI (~300 líneas)

Responsabilidades:
1. Inicializar GOP (framebuffer)
2. Inicializar serial (COM1 para debug)
3. Obtener memory map de UEFI
4. Encontrar ACPI RSDP en EFI ConfigurationTable
5. Cargar `kernel.bin` desde la ESP (FAT) o desde OsitoFS (NVMe)
6. `ExitBootServices()`
7. Pasar boot_info struct al kernel
8. Jump al entry point del kernel

```c
/* boot_info: todo lo que el kernel necesita saber del mundo UEFI */
typedef struct {
    /* Framebuffer */
    uint32_t *fb_base;
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;

    /* Memory map */
    void     *mmap_buf;
    uint64_t  mmap_size;
    uint64_t  mmap_desc_size;
    uint64_t  mmap_desc_ver;

    /* ACPI */
    void     *acpi_rsdp;

    /* Kernel load address */
    uint64_t  kernel_phys_base;
    uint64_t  kernel_size;
} boot_info_t;
```

**Formato**: PE32+ EFI (sigue necesitando gnu-efi, -fPIE — pero son ~300 líneas,
no 50+ archivos de kernel).

**Carga del kernel**: Lee `kernel.bin` de la ESP via `EFI_FILE_PROTOCOL`:
```
ESP:/EFI/BOOT/BOOTX64.EFI   → boot.efi
ESP:/EFI/BOOT/kernel.bin     → kernel (cargado a 0x100000 o donde haya RAM libre)
```

### kernel.bin — Kernel sin restricciones EFI

```
Compilación:
  gcc -ffreestanding -fno-pie -mcmodel=large -O2 -c kernel/*.c
  ld -T kernel.ld -o kernel.elf [todos los .o]
  objcopy -O binary kernel.elf kernel.bin    (flat binary)

Alternativa ELF (más flexible):
  ld -T kernel.ld -o kernel.elf [todos los .o]
  boot.efi carga kernel.elf, parsea PT_LOAD, jump a e_entry
```

**Linker script** (`kernel.ld`):
```ld
ENTRY(kernel_entry)

SECTIONS {
    /* Kernel cargado a 1MB (classic load address) */
    . = 0x100000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
```

### Beneficios inmediatos

| Antes (ositok.efi) | Después (boot.efi + kernel.bin) |
|---|---|
| `-fPIE` en 50+ archivos | `-fno-pie` — acceso directo a globals |
| `-fno-jump-tables` obligatorio | Jump tables normales — switch/case optimizado |
| No inline asm con globals | Sin restricciones |
| objcopy PE32+ frágil | `objcopy -O binary` o ELF directo |
| gnu-efi contamina el build | Solo en boot.efi (~1 archivo) |
| ~1.1MB .efi con overhead | Kernel más compacto |
| **TCC no puede generar el kernel** | **TCC puede generar kernel.bin** ← clave |

---

## Road to Self-Compiling Kernel

### Fase 0: Separar boot.efi de kernel (prerequisito)

**Input**: `ositok.efi` monolítico
**Output**: `boot.efi` (~300 LOC) + `kernel.bin` (~50 archivos)

Pasos:
1. Crear `arch/x86/boot/boot.efi.c` — bootloader standalone
2. Crear `arch/x86/kernel.ld` — linker script del kernel
3. Mover todo `efi_main.c` excepto el boot flow a `kernel_entry(boot_info_t *)`
4. Makefile genera `boot.efi` (con gnu-efi) y `kernel.bin` (sin gnu-efi)
5. `qemu-test.sh` carga ambos desde ESP
6. Verificar: boot idéntico, shell funcional, todos los tests pasan

**Estimación**: ~400 líneas nuevas, ~200 líneas refactoreadas.

**Riesgo**: bajo. El boot flow ya está separado conceptualmente (efi_main → kernel_entry).
Solo hay que cortar el cordón umbilical de gnu-efi.

### Fase 1: Kernel compila con TCC desde el host

**Input**: `kernel.bin` generado por GCC
**Output**: `kernel.bin` generado por TCC (cross-compilando desde Linux)

Pasos:
1. Compilar cada .c del kernel con `tcc -c -nostdlib -nostdinc` desde Linux
2. Linkear con `tcc -nostdlib -static` o con `ld -T kernel.ld`
3. Verificar binario funcional en QEMU
4. Documentar las incompatibilidades TCC vs GCC (si las hay)

**Bloqueantes potenciales**:
- TCC no soporta `-mcmodel=large` → verificar que acceso a globals funcione sin PIE
- TCC puede no manejar todo el inline asm (lo hace mayormente bien en x86-64)
- TCC no soporta `__attribute__((optimize("O0")))` → buscar alternativas
- TCC no tiene `-mavx2` → tensor_avx2.c necesita ifdef o separación

**Estimación**: ~1 día de prueba y ajuste.

### Fase 2: Kernel compila con TCC dentro de OsitoK

**Input**: archivos .c del kernel en OsitoFS + TCC corriendo en OsitoK
**Output**: `kernel.bin` generado enteramente dentro de OsitoK

Pasos:
1. Subir todos los kernel .c/.h y kernel.ld a OsitoFS
2. Escribir un build script (o comando `build` en el shell) que:
   - Compila cada .c → .o con `tcc.elf -c -nostdlib -nostdinc`
   - Linkea todos los .o → kernel.elf (TCC linker o minimal ld)
   - `objcopy -O binary kernel.elf kernel.bin` (necesitamos objcopy in-OS o equivalente)
3. El kernel.bin resultante es idéntico (o equivalente) al compilado desde host
4. Rebootear con el kernel auto-compilado

**Bloqueantes**:
- **Linker script**: TCC's built-in linker tiene soporte limitado para linker scripts.
  Solución: usar `-Wl,-Ttext,0x100000` como hacemos en X-SELF.
- **objcopy**: No existe in-OS. Solución: (a) el bootloader carga ELF directamente
  (ya tenemos ELF loader), o (b) escribir un minimal strip (~100 LOC).
- **Espacio en disco**: ~50 archivos .c × ~10-60KB = ~1.5MB source + ~1MB de .o's.
  OsitoFS soporta esto sin problema (508 bloques de 1MB).
- **Compilación secuencial**: Sin `make`, compilar ~50 archivos toma ~30-60s en QEMU.
  Aceptable para proof of concept.

**Estimación**: ~600 líneas (build command + glue).

### Fase 3: Compilar, instalar, y rebootear desde OsitoK

**Input**: Kernel auto-compilado dentro de OsitoK
**Output**: OsitoK reboota con el nuevo kernel

Pasos:
1. Después de `build`, el kernel.bin/.elf queda en OsitoFS
2. Comando `install` copia kernel.bin a la ESP (via NVMe write o EFI variable)
3. Comando `reboot` → UEFI → boot.efi → carga el nuevo kernel
4. **Ciclo completo**: editar → compilar → instalar → rebootear → verificar

**Alternativa sin reboot**: kexec-style — cargar kernel.bin en RAM, saltar directo.
Más complejo pero evita el ciclo UEFI.

---

## Formato del kernel: flat binary vs ELF

### Opción A: Flat binary (`objcopy -O binary`)

```
Ventajas:
  - Bootloader trivial: cargar N bytes a dirección fija, jump
  - No parsing necesario
  - Más pequeño (sin headers)

Desventajas:
  - BSS no es explícito (bootloader debe saber el tamaño total y zerear)
  - Sin metadata (no sabemos entry point, tamaño, etc.)
  - Necesita objcopy para generar (no existe in-OS)
```

### Opción B: ELF (kernel.elf)

```
Ventajas:
  - Ya tenemos ELF loader completo en el kernel (y en boot.efi es ~50 líneas)
  - BSS size explícito en program headers
  - Entry point en e_entry
  - TCC genera ELF nativamente — no necesita objcopy
  - Metadata útil (secciones, símbolos para debug)

Desventajas:
  - Bootloader necesita parsear ELF (pero ya lo hacemos, ~50 LOC)
  - Ligeramente más grande que flat binary
```

**Recomendación: ELF**. TCC genera ELF. El bootloader ya sabe parsear ELF.
No necesitamos objcopy. Es la opción natural para self-hosting.

---

## Build actual vs build futuro

### Actual (todo-en-uno EFI)

```
arch/x86/boot/efi_main.c    ──┐
arch/x86/kernel/*.c          ──┤── gcc -fPIE → .o
arch/x86/drivers/*.c         ──┤        ↓
arch/x86/fs/*.c              ──┘   ld -shared → ositok.so
                                        ↓
                                   objcopy → ositok.efi (PE32+)
```

### Futuro (boot separado de kernel)

```
arch/x86/boot/boot_efi.c    ── gcc -fPIE → boot.o
                                    ↓
                               ld -shared → boot.so → objcopy → boot.efi

arch/x86/kernel/*.c          ──┐
arch/x86/drivers/*.c         ──┤── gcc -fno-pie → .o  (O tcc -c → .o)
arch/x86/fs/*.c              ──┘        ↓
                                   ld -T kernel.ld → kernel.elf

ESP image:
  /EFI/BOOT/BOOTX64.EFI = boot.efi
  /EFI/BOOT/kernel.elf  = kernel
```

### Futuro self-hosted (dentro de OsitoK)

```
osito> build
  [1/52] Compiling kernel/main.c...
  [2/52] Compiling kernel/serial.c...
  ...
  [52/52] Linking kernel.elf...
  kernel.elf: 847296 bytes

osito> install
  Writing kernel.elf to ESP...
  Done.

osito> reboot
  [rebooting into self-compiled kernel]
```

---

## Resumen de fases y estimaciones

| Fase | Descripción | ~LOC | Deps | Resultado | Estado |
|------|-------------|------|------|-----------|--------|
| **0** | Separar boot.efi de kernel.elf | ~680 | Ninguna | Kernel sin restricciones EFI | **Done** |
| **1** | TCC cross-compila kernel desde host | ~50 test | Fase 0 | Verificar TCC genera kernel válido | **Next** |
| **2** | TCC compila kernel dentro de OsitoK | ~600 | Fase 1 | Self-compiling proof of concept | Planned |
| **3** | Instalar + rebootear con kernel propio | ~200 | Fase 2 | Ciclo completo edit→build→boot | Planned |

### Fase 0 — Completada (2026-03-10)

Commit `aca7071`: `boot.efi` (57KB) + `kernel.elf` (543KB ELF64 at 32MB).
- boot.efi: standalone UEFI bootloader using LocateHandleBuffer (not LOADED_IMAGE_PROTOCOL)
- kernel.elf: `-fno-pie`, custom `kernel.ld`, BSS zeroing, `boot_info_t` protocol
- Verificado en QEMU: IDT, paging, SMP 4-core, SYSCALL, Win32, crypto, NIC, shell prompt
- Restricciones EFI eliminadas del kernel: `-fPIE` y `-fno-jump-tables` ya no necesarios

El hito definitivo: OsitoK se compila a sí mismo, se instala, y reboota
con el kernel que acaba de compilar. Un OS que se auto-reproduce.

---

## Dependencias y compatibilidad TCC

### Lo que TCC 0.9.28rc soporta (verificado en OsitoK)

- C99/C11 con extensiones GNU
- Inline asm con AT&T syntax
- Static linking (produce ET_EXEC ELF64)
- `-nostdlib -nostdinc -static`
- `-Wl,-Ttext,ADDR` y `-Wl,-section-alignment,ALIGN`
- Linking múltiples .o files
- Forward declarations y weak symbols (parcial)

### Lo que TCC NO soporta (necesita workaround)

| Feature GCC | Alternativa TCC |
|---|---|
| `-fPIE` / `-fpic` | No necesario con kernel separado ✓ |
| `-mcmodel=large` | No necesario si kernel < 2GB ✓ |
| `-mavx2 -mfma` | ifdef: compilar tensor_avx2.c solo con GCC, o excluir del self-build |
| `__attribute__((optimize("O0")))` | Reescribir como volatile o separar en archivo propio |
| `-fno-jump-tables` | No necesario sin EFI ✓ |
| Linker scripts complejos | `-Wl,-Ttext,0x100000` suficiente para flat layout |
| `__builtin_expect` | Ignorado por TCC (compilará pero sin optimización) |
| `asm volatile ("..." ::: "memory")` | Soportado ✓ |

### Archivos que necesitan ajuste para TCC

1. `kernel/tensor_avx2.c` — usa instrinsics AVX2. **Excluir del self-build** (CPU fallback existe).
2. `kernel/process.c` — usa `__attribute__((optimize("O0")))`. Reemplazar por `volatile`.
3. `kernel/isr_stubs.S` — assembly puro. TCC puede ensamblar x86-64 asm.
4. `kernel/syscall_entry.S` — assembly puro. Ídem.
5. `boot/efi_main.c` — **no se compila con TCC** (se queda como boot.efi con GCC).
