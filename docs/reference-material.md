# OsitoK — Material de Referencia

Fuentes de referencia extraidas en `~/research_material/` para el desarrollo del OS.

## Inventario

| Fuente | Path | Tamano | Contenido clave |
|--------|------|--------|-----------------|
| **WRK v1.2** | `wrk/` | 50MB | Windows Research Kernel — scheduler, MM, sync, SEH, handle table |
| **NT 3.5** | `nt_3.5/NT-782/PRIVATE/` | 896MB | kernel32, user32, gdi32, ntdll, csrss, winsock, NTVDM |
| **NT 4** | `nt_4/nt4/private/` | 1.1GB | DirectDraw, Win32k, Winsock2, GDI engine, display drivers |
| **NT5/XP** | `nt5_xp/` | ~40GB | Full Windows XP source — ntos, shell, base, multimedia |
| **MS-DOS 3.30** | `dos_3.30/` | 3MB | INT 21h, FCB, SFT, MCB, device drivers, FAT |
| **MS-DOS 6.0** | `dos_6.0/` | 70MB | Utilities source (fdisk, doskey, mode, tree, etc.) |
| **Win10 SDK** | `win10_sdk/` | 816MB | Shared source kit — drivers, samples |
| **Win31 DDK** | `win31_ddk/` | 136KB | 286/386 DDK diff — driver model reference |
| **16bit DDK** | `16bit_ddk/` | 603MB | Visual C + Win31 DDK full |
| **Undoc Formats** | `undoc_formats/` | 3MB | PDF + source para formatos no documentados |

---

## Mapa de Referencia por Feature de OsitoK

### 1. Per-Process Page Tables (Fase 1.1)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| WRK MM | `wrk/base/ntos/mm/procsup.c` | `MmCreateProcessAddressSpace()` — crea address space por proceso |
| WRK x86 | `wrk/base/ntos/mm/i386/procx86.c` | Page table setup para IA-32 |
| WRK amd64 | `wrk/base/ntos/mm/amd64/initamd.c` | Page table 4-level para AMD64 |
| WRK VAD | `wrk/base/ntos/mm/` | MMVAD (Virtual Address Descriptor) — AVL tree de regiones |
| WRK PTE | `wrk/base/ntos/mm/` | MMPTE structs — hardware/software page table entries |

**Algoritmo WRK**: Allocar PML4 page por proceso. Recursive mapping para hyperspace. Shared system space en upper half. CR3 switch en context swap.

### 2. SMP Scheduler (Fase 1.2)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| WRK dispatch | `wrk/base/ntos/ke/thredsup.c` | `KiSelectNextThread()`, `KiFindReadyThread()` |
| WRK queues | `wrk/base/ntos/ke/thredsup.c` | `KiQueueReadyThread()`, `KiDeferredReadyThread()` |
| WRK ctxswap | `wrk/base/ntos/ke/amd64/ctxswap.asm` | `SwapContext()` — AMD64 context switch |
| WRK quantum | `wrk/base/ntos/ke/` | `KiQuantumEnd()` — quantum expiry handler |

**Algoritmo WRK**: 32 priority levels. Per-processor ready queues. Bitmap para O(1) lookup del priority mas alto. Context switch guarda RSP/RBP/RBX/R12-R15, cambia CR3, carga nuevo stack.

### 3. Win32 Threading (Fase 1.3)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| NT 3.5 kernel32 | `nt_3.5/.../WINDOWS/BASE/CLIENT/THREAD.C` | `CreateThread()` wrapping NtCreateThread |
| NT 3.5 synch | `nt_3.5/.../WINDOWS/BASE/CLIENT/SYNCH.C` | `WaitForSingleObject()` wrapping KeWait |
| WRK thread | `wrk/base/ntos/ps/create.c` | Thread creation kernel-side |
| WRK wait | `wrk/base/ntos/ke/wait.c` | `KeWaitForSingleObject()` — dispatcher wait |

**Patron NT**: kernel32!CreateThread → NtCreateThread → PspCreateThread. Stack allocation, TEB init, start routine wrapper. WaitForSingleObject → NtWaitForSingleObject → KeWaitForSingleObject (dispatcher lock + wait block).

### 4. Section Objects / Memory-Mapped Files (Fase 2.1)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| WRK create | `wrk/base/ntos/mm/creasect.c` | `NtCreateSection()` — 5927 lineas |
| WRK map | `wrk/base/ntos/mm/mapview.c` | `NtMapViewOfSection()` — 6801 lineas |
| NT 3.5 user | `nt_3.5/.../WINDOWS/BASE/CLIENT/FILEMAP.C` | `CreateFileMapping()`, `MapViewOfFile()` |

**Algoritmo WRK**: Section → CONTROL_AREA → SEGMENT. MapView crea MMVAD en proceso. PTEs apuntan a control area. Page faults cargan paginas del archivo o pagefile.

### 5. DirectDraw (Fase 2.2)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| NT 4 ddraw | `nt_4/.../w32/ntgdi/direct/ddraw/` | `ddraw.c`, `ddsurf.c`, `ddsblto.c` |
| NT 4 HEL | `nt_4/.../w32/ntgdi/direct/ddhel/` | Hardware Emulation Layer (software fallback) |
| NT 4 vmem | `nt_4/.../w32/ntgdi/direct/ddraw/vmemmgr.c` | Video memory management |
| NT 4 blit | `nt_4/.../w32/ntgdi/direct/blitlib/` | Blit library (software blitting) |

### 6. GDI (Fase 2.3)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| NT 3.5 client | `nt_3.5/.../WINDOWS/GDI/CLIENT/` | `BITMAP.C` (BitBlt), `DCQUERY.C` (GetDC) |
| NT 3.5 engine | `nt_3.5/.../WINDOWS/GDI/GRE/` | Graphics Rendering Engine |
| NT 3.5 framebuf | `nt_3.5/.../WINDOWS/GDI/DISPLAYS/FRAMEBUF/` | Framebuffer display driver |

### 7. Winsock (Fase 2.4)

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| NT 3.5 winsock | `nt_3.5/.../NET/SOCKETS/WINSOCK/` | `SOCKET.C`, `SEND.C`, `RECV.C`, `SELECT.C` |
| NT 4 winsock2 | `nt_4/.../net/sockets/winsock2/dll/winsock2/` | Winsock 2 con SPI |

**Patron NT**: Winsock DLL → AFD driver (kernel) → TCPIP.SYS. Para OsitoK: Winsock shim → bridge directo a `net.c` (TCP/IP stack ya existe).

### 8. SEH / Exception Handling

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| WRK dispatch | `wrk/base/ntos/ke/i386/exceptn.c` | `KiDispatchException()` — 1591 lineas |
| WRK frame | `wrk/base/ntos/ke/` | Frame-based exception handling |

**Algoritmo WRK**: Save state → KeContextFromKframes. Kernel mode: RtlDispatchException walks handler chain. User mode: copy to user stack, dispatch via ntdll.

### 9. Handle Table / Object Manager

| Referencia | Path | Funciones clave |
|-----------|------|-----------------|
| WRK handle | `wrk/base/ntos/ob/obhandle.c` | `ObpCreateHandle()` — 3-level hierarchical table |
| WRK ref | `wrk/base/ntos/ob/obref.c` | `ObReferenceObjectByHandle()` |

**Algoritmo WRK**: 3-level table (table → block → page). Per-process + kernel tables. Fast handle→object via index lookup. Reference counting per-handle.

### 10. DOS Emulation

| Referencia | Path | Contenido |
|-----------|------|-----------|
| DOS 3.30 includes | `dos_3.30/.../SRC/DOS/*.INC` | FCB, SFT, DPB, MCB, EXE structs |
| DOS 3.30 API ref | `dos_3.30/.../PROGREF/1A_CALLS.A` | INT 21h complete reference (76KB) |
| DOS 3.30 drivers | `dos_3.30/.../PROGREF/2_DEVDR.A` | Device driver architecture (87KB) |
| DOS 3.30 BIOS | `dos_3.30/.../SRC/BIOS/` | CON, AUX, CLOCK, disk drivers |
| NT 3.5 NTVDM | `nt_3.5/.../MVDM/` | V86 monitor, SOFTPC, WOW thunking |

---

---

## NT5/XP Source Paths (XPSP1)

Base: `~/research_material/nt5_xp/nt5src/Source/XPSP1/NT/`

| Componente | Path | Contenido |
|-----------|------|-----------|
| **Kernel** | `base/ntos/ke/` | Scheduler, context switch, DPC, APC, interrupts |
| **Memory Manager** | `base/ntos/mm/` | Paging, sections, VAD, working set, page fault |
| **Object Manager** | `base/ntos/ob/` | Handle table, naming, security descriptors |
| **Process/Thread** | `base/ntos/ps/` | Process/thread creation, termination, PEB/TEB |
| **I/O Manager** | `base/ntos/io/` | IRP dispatch, driver model, file system |
| **Security** | `base/ntos/se/` | Tokens, ACLs, privileges |
| **Power** | `base/ntos/po/` | ACPI, sleep states, idle detection |
| **Executive** | `base/ntos/ex/` | Mutexes, events, semaphores, resources |
| **RTL** | `base/ntos/rtl/` | Runtime library (strings, heaps, compression) |
| **VDM** | `base/ntos/vdm/` | Virtual DOS Machine kernel support |
| **Win32 subsystem** | `windows/` | user32, gdi32, kernel32, win32k |
| **DirectX** | `multimedia/` | DirectDraw, Direct3D, DirectSound, DirectInput |
| **Networking** | `net/` | TCP/IP, Winsock, NDIS, AFD |
| **Drivers** | `drivers/` | Storage, video, USB, audio, input |
| **Shell** | `shell/` | Explorer, taskbar, common dialogs |

Tambien disponible: `Win2K3/NT/` con la misma estructura (Server 2003).

## Notas de Uso

- **WRK** es la referencia mas limpia — codigo de produccion con licencia de investigacion, bien documentado.
- **NT 3.5** tiene las implementaciones user-mode mas claras (kernel32, user32, gdi32) — antes de la complejidad de NT 4+.
- **NT 4** agrega DirectDraw y Win32k (kernel-mode graphics) — necesario para DirectX compat.
- **NT5/XP** es la referencia mas completa pero es enorme (~40GB). Usar para busquedas puntuales.
- **DOS 3.30** tiene la mejor documentacion del modelo DOS — ideal para el emulador de OsitoK.
