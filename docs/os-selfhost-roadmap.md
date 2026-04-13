# Plan: OsitoK General-Purpose OS Roadmap — De AI Firmware a OS Self-Hosting

## Context

OsitoK x86-64 ha completado Tiers 0-6: un OS bare-metal con UEFI boot, NVMe R/W,
OsitoFS R/W, GPU compute pipeline (SASS SM75 + CB0 dispatch), CPU Llama 3.2 1B
inference con AVX2+BPE tokenizer, TCP/TLS/HTTPS, Claude API con tool use,
TCC compiler in-OS, multi-core SMP, pipes/signals, dynamic linking, QuickJS
JavaScript engine, y git nativo con SHA-1+zlib.

**Objetivos originales** (todos cumplidos):
1. ~~Ejecutar programas compilados (ELF binaries)~~ ✅ Tier 1
2. ~~Auto-hospedarse (compilar C en el propio OS)~~ ✅ Tier 3
3. ~~Hacer llamadas HTTPS a la API de Claude~~ ✅ Tier 4-5
4. ~~Correr un cliente Claude CLI nativo~~ ✅ Tier 5

**Nuevos objetivos**:
1. Madurar el OS — scheduler preemptivo, VFS, mmap, threads
2. Correr binarios Linux sin modificar (busybox, toybox, static Go)
3. Optimizar GPU inference — full VRAM pipeline, >10 tok/s
4. Self-hosting completo — compilar el propio kernel desde OsitoK

---

## Estado Actual del OS — Inventario (marzo 2026)

| Subsistema | Estado | Qué Falta |
|-----------|--------|-----------|
| **Memoria** | Page allocator + 4-level paging + kmalloc/kfree heap | No mmap, no per-process address space, no demand paging |
| **Procesos** | process_t, PID, FD table, exec/exit/waitpid (cooperative) | No scheduler preemptivo, no threads, no fork |
| **Filesystem** | OsitoFS v2 R/W, NVMe R/W, GPT parser | No directorios, no VFS, no /dev /proc |
| **Red** | I211 Ethernet + ARP + IPv4 + UDP + TCP + DNS + TLS 1.2 + HTTPS | No TCP server (listen/accept), no socket syscalls |
| **Consola** | Serial + GOP framebuffer + PS/2 keyboard + terminal line editor | No VT100 completo, no raw mode TTY |
| **libc** | CRT (_start, printf, malloc) + tcclib (FILE*, fprintf, qsort) | No musl, no POSIX completo |
| **ELF** | Full ELF64 loader (PT_LOAD, stack, argv) + dynamic linker | No PIE con ASLR, no lazy binding |
| **Interrupts** | IDT 256-entry, #PF/#GP/#UD handlers, APIC timer 100Hz, PIC remap | No preemptive scheduling via timer |
| **Syscalls** | 13 Linux-compat (read/write/open/close/fstat/lseek/brk/writev/access/unlink/ioctl/exit/arch_prctl) | No mmap, no clone, no signals, no sockets (~27 faltan para Phase 1) |
| **Compiler** | TCC 0.9.28rc in-OS (.c → .o → ELF) | No linker in-OS, no self-compile kernel |
| **GPU** | Completo: X1-X42 (boot chain + compute + SASS SM75), VRAM-resident weights/activations | GSP boot chain sin validar en HW real |
| **AI** | Llama 3.2 1B: CPU AVX2 + GPU dispatch + BPE tokenizer + Claude API streaming + 5 tools | No sampling (temp/top-p), no K-quants |
| **IPC** | Pipes (pipe/dup2), signals (SIGINT/SIGTERM/SIGKILL/SIGPIPE), shell redirection | No futex, no shared memory, no Unix domain sockets |
| **SMP** | 4-core AP startup via INIT-SIPI-SIPI, APIC init per core | No per-core scheduling, APs idle after boot |
| **Scripting** | QuickJS ES2020+ (bare-metal port, 7 builtins) | No fs/net bindings, no npm |
| **VCS** | Git nativo (SHA-1+zlib, init/add/commit/log/status/diff/branch/checkout) | No pack files, no merge, no remote |

---

## Roadmap de Features — Orden de Ejecución

### Tier 0: Completar Features Pendientes ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-CPU2** | NVMe write + OsitoFS v2 write | ✅ Done |
| **X-CPU3** | UDP prompt server (port 7777) | ✅ Done |

### Tier 1: OS Fundaciones ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-OS1** | IDT + Exceptions + APIC timer | ✅ Done |
| **X-OS2** | Paging (4-level x86-64) | ✅ Done |
| **X-OS3** | Heap allocator (kmalloc/kfree) | ✅ Done |
| **X-OS4** | Syscall interface (SYSCALL/SYSRET) | ✅ Done |
| **X-OS5** | ELF64 loader | ✅ Done |
| **X-OS6** | Process subsystem (exec/exit/waitpid) | ✅ Done |

**Hito** ✅: `hello.elf` ejecuta `write(1, "Hello\n", 6); exit(0);`

### Tier 2: Shell + Filesystem Write ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-OS7** | Terminal line editor | ✅ Done |
| **X-OS8** | PS/2 keyboard driver | ✅ Done |
| **X-OS9** | Mini shell (20+ builtins) | ✅ Done |
| **X-OS10** | File I/O syscalls + GDT fix | ✅ Done |

**Hito** ✅: Shell interactivo `osito>` con keyboard, ELF exec, filesystem

### Tier 3: Compilador Self-Hosting ✅ (parcial)

| ID | Feature | Estado |
|----|---------|--------|
| **X-OS11** | TCC cross-compilation | ✅ Done |
| **X-OS12** | Minimal CRT (crt.c + syscall.S) | ✅ Done |
| **X-OS13** | TCC in-OS compilation (.c → .o) | ✅ Done |
| **X-OS14** | Port musl libc | Pendiente → Tier 7 |
| **X-OS15** | Port chibicc | Pendiente → Tier 7 |

**Hito** ✅: TCC compila C dentro de OsitoK, produce y ejecuta ELF binaries

### Tier 4: TCP/TLS + HTTP Client ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-NET1** | ICMP (ping) | ✅ Done |
| **X-NET2** | TCP stack (client-only) | ✅ Done |
| **X-NET3** | DNS resolver | ✅ Done |
| **X-NET4** | TLS 1.2 + crypto from scratch | ✅ Done |
| **X-NET5** | HTTP client (GET/POST, chunked, streaming) | ✅ Done |

**Hito** ✅: `curl api.anthropic.com/v1/messages` funcional desde OsitoK

### Tier 5: Claude CLI Nativo ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-CL1** | Claude API client (SSE streaming) | ✅ Done |
| **X-CL2** | REPL multi-turn | ✅ Done |
| **X-CL3** | Tool: file read/write/list | ✅ Done |
| **X-CL4** | Tool: exec (compile + run) | ✅ Done |
| **X-CL5** | Tool: search (grep) | ✅ Done |

**Hito** ✅: Claude agent nativo que lee código, escribe archivos, compila y ejecuta

### Tier 6: Avanzado ✅

| ID | Feature | Estado |
|----|---------|--------|
| **X-SMP** | Multi-core AP startup (INIT-SIPI-SIPI) | ✅ Done |
| **X-PIPE** | Pipes + dup2 + signals + shell redirection | ✅ Done |
| **X-DYN** | Dynamic linking (ld.so) | ✅ Done |
| **X-JS** | QuickJS ES2020+ JavaScript engine | ✅ Done |
| **X-GIT** | Git nativo (SHA-1 + zlib, 8 commands) | ✅ Done |

### Extras completados (fuera de roadmap original)

| ID | Feature | Estado |
|----|---------|--------|
| **X-TOK1** | BPE tokenizer (Llama 3, 128K vocab) | ✅ Done |
| **X-INF1** | GPU inference dispatch (6/7 ops en GPU) | ✅ Done |
| **X-INF2** | VRAM-resident activations | ✅ Done |
| **X-INF3** | VRAM-resident weights (~664MB) | ✅ Done |

---

### Tier 7: Madurez del OS — Siguiente Fase

Transformar OsitoK de un OS cooperative ring-0 a un OS preemptivo con las
abstracciones necesarias para correr software real sin modificar.

| ID | Feature | Descripción | ~Líneas | Deps | Estado |
|----|---------|-------------|---------|------|--------|
| **X-SCHED** | **Scheduler preemptivo** | Timer-based round-robin, fake ISR frame spawn, RSP-swap context switch. | ~800 | Ninguna | ✅ Done |
| **X-MMAP** | **mmap/munmap/mprotect** | MAP_ANONYMOUS identity-mapped, VMA tracking, page-level protection. | ~1200 | X-SCHED | ✅ Done |
| **X-VFS** | **VFS layer** | /dev, /proc, getcwd, readlink, getdents64. | ~800 | Ninguna | ✅ Done |
| **X-MUSL** | **Port musl libc** | musl 1.2.5 static + 20 new syscalls (TLS, time, signals). | ~500 glue | X-MMAP, X-VFS | ✅ Done |
| **X-FORK** | **fork/wait4/execve** | Preemptive fork, parent state save/restore (RW segments, brk, FS_BASE, fd/sig/vma). | ~1200 | X-SCHED, X-MUSL | ✅ Done |
| **X-THREAD** | **Threads (clone/futex)** | clone(CLONE_VM\|CLONE_THREAD), futex(WAIT/WAKE), set_tid_address, gettid. Per-thread stacks, TLS via arch_prctl ARCH_SET_FS. Usar SMP cores para threads reales. | ~1000 | X-SCHED, X-MMAP |
| **X-EDIT** | **Port editor mínimo** | Portar un editor de texto (kilo ~1000LOC, o nano subset). Editar archivos desde OsitoK sin host. Necesita raw mode TTY + VT100 ANSI. | ~600 glue | X-MUSL |
| **X-HTTPD** | **TCP server (listen/accept)** | Completar TCP stack: listen(), accept(), server sockets. Implementar HTTP server mínimo. Exponer servicios desde OsitoK a la red. | ~600 | Ninguna |
| **X-SELF** | **Self-hosting completo** | Compilar el propio kernel x86 desde OsitoK + kexec boot. **DONE**: Phase 1 (TCC cross-compile) → Phase 2 (build in-OS: 62 .c → 733KB ELF) → Phase 3 (kexec: load+boot self-built kernel) → Phase 4 (self-built kernel boots directly from UEFI). | ~2000 | X-MUSL, X-EDIT | **Done** ✅ |

**Hito** ✅: `busybox sh` (musl-static, ~1MB) corre dentro de OsitoK. Applets cat/echo/uname funcionan.

**Hito** ✅: **Kernel self-compile + kexec + standalone boot**. OsitoK compila su propio kernel (TCC 0.9.28rc, 62 fuentes, 733KB ELF), lo bootea via kexec, y el kernel auto-compilado bootea directamente desde UEFI. El sistema es auto-replicable.

### Tier 8: Hardware Boot — AMD Ryzen 7 5800X + RTX 3090

Target: Boot OsitoK on real hardware from WD SN740 512GB NVMe.

**Hardware inventory:**
| Device | Model | PCI ID | OsitoK status |
|--------|-------|--------|---------------|
| CPU | AMD Ryzen 7 5800X (8C/16T) | — | SMP OK |
| RAM | 48 GB DDR4 | — | Page alloc OK |
| GPU | NVIDIA RTX 3090 (GA102) | `10de:2204` | GSP/SASS driver exists |
| NVMe target | WD SN740 512GB | `15b7:5016` | NVMe driver exists (needs HW test) |
| NIC | Intel I211 | `8086:1539` | I211 driver exists |
| USB | AMD 400 xHCI + Matisse xHCI | `1022:43d5`, `1022:149c` | **X-XHCI needed** |
| SATA | AMD RAID | `1022:43bd` | X-AHCI WIP |
| Keyboard | SINO WEALTH Gaming KB (USB HID) | `258a:002a` | Needs xHCI |
| Mouse | HyperX Pulsefire Core (USB HID) | `0951:16de` | Needs xHCI |
| Serial | COM1 header on motherboard | — | Available for debug |

**Step-by-step boot plan:**

| Step | Task | Description | Deps |
|------|------|-------------|------|
| **H1** | xHCI USB driver | AMD 400/Matisse xHCI init, port enum, endpoint config. USB HID for keyboard+mouse | None |
| **H2** | USB HID input | Parse HID reports, scancode→ASCII, integrate with terminal editor | H1 |
| **H3** | NVMe SN740 bring-up | Test NVMe driver with SN740 (DRAM-less). May need CMB support | None |
| **H4** | Flash to NVMe | Create GPT on nvme1n1: ESP (boot.efi+kernel.elf) + OsitoFS partition | H3 |
| **H5** | UEFI GOP display | Verify framebuffer on RTX 3090 GOP output (already obtained by boot.efi) | None |
| **H6** | Real hardware boot | Boot from nvme1n1 via BIOS boot menu. COM1 serial for debug | H1-H5 |
| **H7** | Nouveau modesetting | Native display init for GA102. Resolution control, cursor | H6 |

**Critical path**: H1 → H2 → H6 (xHCI blocker: no keyboard = no interaction on real HW)

### Tier 9: GPU + Inference Optimization

Cerrar el gap entre "funciona" y "es rápido". Validar en hardware real.

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-GPU-OPT** | **Full VRAM pipeline** | Eliminar CPU fallback para attention loop. Compile SASS attention kernel (QKV + softmax + output en un dispatch). Meta: 0% CPU en forward pass (excepto logits argmax). | ~800 | Ninguna |
| **X-SAMPLE** | **Sampling avanzado** | temperature, top-p, top-k, repetition penalty sobre logits. ~200 LOC, mejora dramática en calidad de generación. | ~200 | Ninguna |
| **X-KQUANT** | **K-quant support** | Q4_K_M, Q6_K dequant para modelos modernos (Llama 3.1, Mistral, etc.). 256-value superblocks. | ~600 | Ninguna |
| **X-HW** | **Hardware testing** | Validar GSP boot chain + GPU compute en RTX 2070+ real. Correr tests en hardware no-QEMU. El momento de la verdad para X16-X42. | ~200 test | Ninguna |
| **X-STREAM** | **Layer streaming (NVMe)** | Cargar layers on-demand desde NVMe para modelos >RAM (8B, 70B). Double-buffer: layer N en GPU mientras layer N+1 lee de disco. | ~500 | X-GPU-OPT |

**Hito**: >10 tok/s en RTX 2070+ con Llama 3.2 1B. Modelos 8B viables con streaming.

### Tier 9: Linux Binary Compatibility (largo plazo)

Correr binarios Linux estáticos sin modificar. Ver `docs/binary-compat-roadmap.md`.

| ID | Feature | Descripción | ~Líneas | Deps | Estado |
|----|---------|-------------|---------|------|--------|
| **X-SYSCALL40** | **40 syscalls POSIX** | Completar las ~27 syscalls faltantes para binarios musl-static (stat, getdents64, clock_gettime, nanosleep, signals, fork/execve/wait4, socket API). | ~2000 | X-MMAP, X-THREAD | Parcial (~40 ya) |
| **X-BUSYBOX** | **Run busybox** | Test target: `busybox sh`, `busybox ls`, `busybox cat`. Primer binario Linux no-trivial sin modificar. | ~200 test | X-SYSCALL40, X-MUSL | ✅ Parcial (ash+cat+echo+uname) |
| **X-SOCKET** | **Socket syscalls** | socket(AF_INET), connect, sendto, recvfrom, bind, listen, accept. Wrapper sobre TCP/UDP stack existente. Expone red via interfaz POSIX. | ~800 | X-HTTPD |
| **X-SIGNAL** | **Señales completas** | rt_sigaction, rt_sigprocmask, rt_sigreturn, sigframe en user stack. Signal delivery en syscall return + timer tick. | ~1000 | X-SCHED |
| **X-PE** | **Windows PE loader** | PE/COFF loader + NT syscall translation (Phase 3 del binary-compat-roadmap). Largo plazo. | ~5000+ | X-SYSCALL40 |

**Hito** ✅ (parcial): `busybox sh` interactivo con cat/echo/uname. Falta: id, ls, más applets.

---

## Arquitectura MMU Virtual — Análisis y Diseño

### Contexto: por qué considerar MMU virtual

El modelo identity-mapped actual (virt == phys) funciona para inference y proceso
único, pero causa problemas crecientes con fork+execve:
- Parent state save/restore para RW segments, brk, FS_BASE, fd/sig/vma (~170 LOC de hacks)
- No soporta dos instancias del mismo binario en paralelo (un solo set de páginas físicas)
- Cada nueva pieza de estado per-proceso requiere agregar save/restore manual

### Ventajas del MMU virtual (per-process address spaces)

| Ventaja | Impacto |
|---------|---------|
| **Fork+execve trivial** | Child carga en 0x400000 → páginas físicas distintas. Parent intacto. Elimina TODO el save/restore. |
| **COW fork** | fork() solo clona page tables (~20KB), marca páginas read-only. #PF copia on-write. Más rápido que copiar 32KB de stack. |
| **brk/mmap aislados** | Cada proceso tiene su propio rango, naturalmente. No hay globals compartidos. |
| **Seguridad** | User process no puede leer kernel memory (bit supervisor en PTEs). |
| **Procesos concurrentes** | Múltiples binarios distintos pueden ejecutar en paralelo sin conflicto de VAs. |
| **Elimina hacks** | `saved_parent`, `elf_fork_restore`, `mem_reserve_range` fallback, frame pointer relocation — todo innecesario. |

### Desventajas del MMU virtual

| Desventaja | Impacto |
|------------|---------|
| **Refactor extenso** | El kernel necesita higher-half mapping (0xFFFF800000000000+). ~30 archivos asumen phys==virt. |
| **TLB flush** | CR3 switch en context switch invalida TLB. PCID mitiga pero es complejo. |
| **Page tables por proceso** | ~20KB por proceso mínimo (PML4 + PDPT + PD + PT para mapear 0-8MB). |
| **DMA/MMIO** | NVMe, GPU, I211 necesitan direcciones físicas. Hay que distinguir kernel VA vs phys addr. |
| **Kernel↔user copies** | Necesita `copy_from_user`/`copy_to_user` explícitos (o mapear kernel en todos los address spaces). |
| **GPU inference perf** | PRAMIN writes, CE DMA, tensor buffers — todo trabaja con direcciones físicas. Agregar traducción penaliza hot paths. |

### Diseño propuesto: Split Address Space (sin afectar kernel hot paths)

La clave es que el **kernel y los drivers se quedan identity-mapped** — solo el
user-space vive en un address space virtual separado.

```
Per-process page tables (un CR3 por proceso):

  0xFFFF800000000000+  Kernel identity map (compartido entre todos los procesos)
  │                    Mismas PDPT entries, solo se clonan los primeros 2 niveles.
  │                    phys == virt - KERNEL_VBASE  →  todos los drivers, DMA, GPU
  │                    funcionan EXACTAMENTE igual. Solo cambian por un offset constante.
  │
  0x0000000000400000   User process A  →  mapea a phys pages libres (ej. 0x2000000)
  0x0000000000400000   User process B  →  mapea a phys pages libres (ej. 0x2200000)
                       Misma VA, distintas PA. CR3 switch en scheduler.
```

### Implementación en fases (sin romper nada existente)

> **Status update (2026-04-12 pm)**: Fase 2 (per-process page tables) is
> **DONE** as the X-PGTBL commit (`199ba97`). Each process now has its
> own CR3 with cloned PDPT[0] + PDPT[1] PDs, scheduler switches on
> context change, proc_exec creates + activates, proc_free releases.
> VMA `owner` filtering keeps sibling processes' mmap regions isolated.
> Also: per-process fd_table (`5876fe0`) moves `fd_table[MAX_FDS]`
> inline into process_t with pipe_buf_t refcounting — fork clones
> parent's table, zsh fork+exec of external commands works. **Still
> pending**: Fase 0 (VBASE macros no-op) and Fase 1 (higher-half kernel
> mapping with AT() linker script + bootstrap stub) — the kernel itself
> still lives at physical 0x02000000 identity-mapped, and drivers still
> use direct PA == VA pointers. Higher-half is the next refactor.

**Fase 0: Preparación (no-op funcional) ✅ DONE (2026-04-12)**
- `arch/x86/include/paging.h`: `KERNEL_VBASE 0xFFFF800000000000ULL`,
  `PHYS_TO_VIRT(p)` = `p + KERNEL_VBASE`, `VIRT_TO_PHYS(v)` = `v - KERNEL_VBASE`.
- Drivers migran a las macros uno a uno. Callers migrados hasta ahora:
  - `heap.c` — commit `65820fc`: todo el heap kernel vive en upper-half
    (`[HEAP] Heap at 0xFFFF800001401000`).
  - `paging.c` — commit `14b1d5b`: NULL guard y COW copy usan la mirror.
  - `syscall.c` — commit `e8ecea7`: `sys_mprotect` PROT_NONE→RW y
    `demand_page_fault` hacen memset/vfs_read vía upper-half; el phys
    sigue siendo lo que se instala en el PTE user.
  - `main.c` — commit `40fcbb9`: shadow framebuffer (4 MB) en upper-half,
    `fb_enable_shadow(PHYS_TO_VIRT(shadow_phys))`.
  - `inference.c` — commit `5fedcef`: LLaMA state (layer table, kv
    cache, scratch) en upper-half. Tensors puramente CPU-only.
  - `tensor.c` — commit `7c0b9e0`: 17 allocations del benchmark suite
    (Q4_0 matvec, Q8_0 matvec, perf 2048x2048, AVX2 vs scalar). Valida
    al boot: Q4_0/Q8_0 matvec OK, perf corre.
  - `win32/dllloader.c` — commit `1f29896`: buffer temporal para leer
    PE de disco; `dll_load` parsea headers y mapea segmentos PE32
    aparte, así que el buffer sólo vive kernel-side.
  - **`syscall.c sys_mmap` — commit `74b074d`**: deja de devolver
    phys-as-VA. Reserva una VA de un pool compartido (≥ 20 GB) y crea
    una VMA sin páginas; `demand_page_fault` aloca la phys en el
    primer acceso y la instala en el PML4 del proceso via
    `paging_map_page_in_cr3`. Unifica PROT_NONE y PROT_READ|WRITE en
    un único path lazy. Último caller que usaba la identity map como
    "VA allocator" para user space.
  - `drivers/gpu.c` VBIOS — commit `894bae4`: 256 KB scratch buffer
    parseado CPU-side. Único sitio migrable de todos los drivers
    (ver abajo).

**Driver audit (2026-04-12)**: 50+ sitios `mem_alloc_pages`/
`mem_alloc_aligned` en `drivers/` (NVMe, xHCI, i211, GSP, virtio,
hda, usb_storage, gpu_tensor, sass, gmmu). **Todos son DMA** y
deben permanecer en phys para que los dispositivos puedan
dereferenciarlos (descriptor rings, command queues, packet buffers,
GPU MMU radix3, firmware pushbuffers, etc.). Único caller kernel-CPU-
only: el buffer de parseo de VBIOS en `gpu.c` — migrado. Los drivers
NO bloquean la eventual eliminación del identity map del PML4 de
user processes.

**Pendientes aún en identity map (no-driver)**: Win32 compat thunk
pool (`compat32.c`), thread stacks (`ntprocess.c`), ddraw framebuffer
y surfaces (`ddraw_shim.c` — el proxy COM PE32 NO debe migrar). Sin
regression test de PE binaries, quedan diferidos.

- El resto del kernel sigue funcionando via identity map lower-half;
  los drivers todavía acceden sus DMA buffers por su phys vía identity.

**Kernel text relocation (Option C) ✅ DONE (2026-04-13)**

- `boot.efi` (commit `ab9d59d`): antes de `ExitBootServices` clona la
  PML4 de UEFI (que es read-only en sus propias tablas), añade
  `PML4[256] -> PDPT` con páginas 1GB cubriendo 0..16 GB del upper-half
  direct map, y switchea CR3 a la nueva PML4. El kernel ahora puede
  saltar a una entry high desde UEFI.
- `kernel.ld` + `Makefile` (commit `cd1c2a1`): kernel text/rodata/
  data/bss linkados a `VMA = KERNEL_VBASE + 0x2000000` con
  `AT(ADDR(...) - KERNEL_VBASE)` para que `p_paddr` siga siendo low.
  ELF entry pasa a `0xFFFF8000020007F0`. Compilación con
  `-mcmodel=large` (32-bit displacements no caben en VAs upper-half;
  ~17% slower hot loops, aceptable para esta fase).
- El kernel sigue corriendo el lower-half identity map en `kernel_pml4`
  por compatibilidad con paging code que dereferencia phys-as-virt y
  con drivers que tocan DMA buffers via phys. La eliminación final del
  identity map es un paso futuro independiente.

**Pendientes aún en identity map (no-driver)**: Win32 compat thunk
pool (`compat32.c`), thread stacks (`ntprocess.c`), ddraw framebuffer
y surfaces (`ddraw_shim.c` — el proxy COM PE32 NO debe migrar). Sin
regression test de PE binaries, quedan diferidos.

**Fase 1: Higher-half kernel mapping ✅ DONE (2026-04-12)**
- `paging_init()` instala un segundo mapeo del total de RAM a
  `VA = phys + KERNEL_VBASE` (PML4[256]) además del identity map bajo.
- Helper interno generalizado: `paging_map_range_at(phys_s, phys_e, virt_offset, flags)`.
  `paging_identity_map_range` pasa a ser wrapper con offset 0.
- Linker script sin tocar; el kernel text corre identity-mapped como siempre.
- Overhead del mirror: ~5 page-table pages adicionales (PDPT + PDs),
  `[PAGE] Page tables built: 11 pages (44 KB)`.
- Commits: `fdb93e3` (mirror), `c80aa0e` (macros activas), `65820fc` (heap migrated).

**Fase 2: Per-process page tables para user space ✅ DONE (2026-04-12)**
- `proc_create_address_space()`: crea PML4 con upper-half del kernel copiado
- `elf_load_segments()`: alloca páginas libres, las mapea en el PML4 del proceso
- `sched_tick()`: hace CR3 switch al PML4 del proceso siguiente
- **El kernel code sigue usando identity-mapped addresses** — zero impact en inference

**Fase 3: Fork con COW**
- `proc_fork()`: clona page tables del lower half, marca pages read-only
- #PF handler: si la página es COW, alloca nueva página, copia, remapea writable
- Eliminar todo el save/restore de X-FORK

### Invariante de rendimiento: kernel hot paths intactos

El diseño garantiza que:
- `nvme_read(lba, count, buf)` — `buf` es dirección física directa (identity map en upper half)
- `gpu_write(reg, val)` — BAR0 MMIO sigue siendo phys addr directa
- PRAMIN window reads/writes — idéntico
- CE DMA transfers — src/dst son phys addr, no se traducen
- tensor buffer allocation — `mem_alloc_pages()` devuelve phys, GPU ve la misma addr

El único cambio es que el kernel accede a esas direcciones como `phys + KERNEL_VBASE`
en vez de `phys`, pero el compilador optimiza `phys + constante` a una sola instrucción.
No hay table walk, no hay TLB miss, no hay overhead medible.

### Prioridad

**No es urgente.** El save/restore actual funciona para busybox ash applets.
Implementar cuando:
1. Se necesiten dos binarios distintos ejecutando en paralelo con preemption
2. El save/restore se vuelva insostenible (cada nuevo global = nuevo bug)
3. Se implemente `clone(CLONE_VM)` para threads reales (requiere address space propio)

**Estimación**: ~800 líneas total (Fase 0-2). ~400 líneas más para COW (Fase 3).

---

## Cadena de Dependencias (Tier 7+)

```
Tier 7 (OS maturity):
  X-SCHED (preemptive) ──→ X-MMAP (virtual memory) ──→ X-MUSL (musl libc) ──→ X-EDIT (editor)
       │                        │                            │                       │
       └──→ X-THREAD ──────────┘                            └──→ X-SELF (self-host) ┘
                                                                      │
  X-VFS (mount/dev/proc) ──→ X-MUSL                                  │
  X-HTTPD (TCP server) ──→ (standalone)                               │
                                                                      v
                                                              Compilar kernel
                                                              desde OsitoK

Tier 8 (GPU optimization):
  X-SAMPLE (sampling) ──→ (standalone)
  X-KQUANT (K-quants) ──→ (standalone)
  X-GPU-OPT (full VRAM) ──→ X-STREAM (layer streaming NVMe)
  X-HW (hardware test) ──→ (standalone, critical validation)

Tier 9 (Linux compat):
  X-MMAP + X-THREAD ──→ X-SYSCALL40 ──→ X-BUSYBOX
  X-HTTPD ──→ X-SOCKET (POSIX sockets)
  X-SCHED ──→ X-SIGNAL (full signals)
  (all above) ──→ X-PE (Windows PE, largo plazo)
```

### Camino crítico para `busybox sh`:

```
X-SCHED → X-MMAP → X-MUSL → X-SYSCALL40 → X-BUSYBOX
                       ↑
                    X-VFS
```

### Camino crítico para self-hosting:

```
X-SCHED → X-MMAP → X-MUSL → X-EDIT → X-SELF
                       ↑
                    X-VFS
```

### Paralelizable (sin deps):

```
X-HTTPD (TCP server)      — se puede hacer ya
X-SAMPLE (sampling)       — se puede hacer ya
X-KQUANT (K-quants)       — se puede hacer ya
X-HW (hardware test)      — se puede hacer ya
X-VFS (VFS layer)         — se puede hacer ya
```

---

## Opciones Tecnológicas Clave (actualizado)

### Scheduler: APIC timer-driven preemption
- APIC timer ya corre a 100Hz. Solo falta: save registers → pick next → restore → IRET.
- Per-process kernel stack + TSS update (RSP0 para ring 3→0 transitions).
- Referencia: xv6's `swtch()` — 20 líneas de asm, el scheduler más simple que funciona.

### Virtual Memory: mmap con VMA tracking
- Per-process VMA list (start, end, flags, file). mmap = alloc pages + map PTEs + record VMA.
- Demand paging opcional (page fault → alloc on access). Simplificar: pre-alloc all pages.
- mprotect = update PTE bits + INVLPG.

### libc: musl (cross-compiled)
- Cross-compilar musl desde Linux host como libc.a estática para OsitoK target.
- Syscall stubs: musl usa `__syscall` inline asm → nuestro SYSCALL entry.
- Path: `x86_64-ositok-musl-gcc` cross-compiler o parche mínimo sobre musl configure.

### VFS: minimal, OsitoFS como root
- Superblock table con mount points. OsitoFS monta como `/`.
- `/dev` in-memory: null, zero, urandom (RDRAND), console (tty), fd/N.
- `/proc` in-memory: self/maps (VMA dump), self/status (PID/name).
- No es necesario un VFS completo — solo lo suficiente para que musl funcione.

### Editor: kilo (antirez)
- ~1000 LOC de C, un solo archivo, sin deps. MIT license.
- Necesita: raw mode TTY (tcgetattr/tcsetattr → ioctl stubs), ANSI escape sequences.
- Alternativa: escribir uno propio (~500 LOC si es solo ed-style line editor).

### TCP server: extensión de net.c
- TCP stack existente es client-only. Agregar: listen state, SYN backlog, accept().
- Reusa 90% del código TCP existente (handshake, state machine, checksums).
- HTTP server mínimo: serve static files + API endpoints.

### Self-hosting: path a compilar el kernel
- **Opción A**: Port GCC cross-compiler a OsitoK (pesado pero completo)
- **Opción B**: Usar TCC in-OS + linker script + gnu-efi headers (más viable)
- **Opción C**: Port chibicc (C11 completo, ~10K LOC, más simple que GCC)
- **Bloqueantes**: EFI binary format (shared object with objcopy), gnu-efi headers,
  linker script support. TCC puede no manejar todo el Makefile.

---

## Referencia: OSes Hobby que Lograron Self-Hosting

| OS | Path | Tiempo al Self-Hosting |
|----|------|----------------------|
| **SerenityOS** | Kernel→FS→Shell→Port GCC | ~6 meses |
| **Sortix** | Kernel POSIX→Port GCC→Self-host | ~4 meses |
| **Managarm** | Microkernel→IPC→Port GCC | ~8 meses |

**Lección clave**: Todos portaron GCC/TCC en vez de escribir compilador propio.
Cross-compilar primero, luego self-host. TCC es el path más rápido al bootstrap.

---

## OsitoFS v2 Block Reclamation + Futuro v3

### v2 con reclamación (implementado)

OsitoFS v2 originalmente era append-only: `osfs2_delete()` marcaba archivos como
inválidos pero no liberaba los bloques en disco. Cada ciclo delete+create consumía
espacio permanentemente — después de ~8 rebuilds en una partición de 512MB, se
acababa el espacio.

**Solución implementada**: bitmap de bloques en memoria (32KB), reconstruido al mount
desde la file table. `delete()` libera bloques en el bitmap; `create()` busca
huecos libres (first-fit) antes de hacer append. Sin cambio de formato en disco.

Esto permite ciclos ilimitados de build (delete .o → create .o → link) sin leak.

### v3 (futuro, no bloqueante)

Limitaciones de v2 que v3 resolvería:

| Limitación v2 | Impacto | Solución v3 |
|---|---|---|
| **Bloques de 1MB** | 38x desperdicio en archivos chicos (headers 2KB → 1MB) | Bloques de 4KB con extents |
| **Sin directorios** | Flat namespace, basename stripping como workaround | Directorio como archivo especial (inode-like) |
| **Bitmap solo en RAM** | Si crash antes de persist, se reconstruye al mount (OK) | Bitmap persistido en disco (bloque dedicado) |
| **File table fija 1MB** | 4096 max files siempre, no crece | Tabla dinámica con overflow blocks |
| **Sin timestamps** | No se sabe cuándo se modificó un archivo | mtime/ctime en file entry |
| **Sin permisos** | Todo es root RW | uid/gid/mode en file entry |

**Formato propuesto v3**:
```
Block 0:     Superblock (magic=OSF3, block_size=4096)
Block 1-N:   Block bitmap (1 bit per 4KB block)
Block N+1:   Root directory inode
Block N+2+:  Data blocks (files, directories, indirect blocks)

Inode (128 bytes):
  - type (file/dir/symlink)
  - size, uid, gid, mode
  - mtime, ctime
  - 12 direct block pointers
  - 1 indirect, 1 double-indirect
  - extent list (start_block, count) × 4 for contiguous files
```

**Prioridad**: Baja. v2 con reclamación es suficiente para self-build y operación
normal. v3 sería necesario para:
- Port de software que asume directorios (make, git, etc.)
- Filesystem con >4096 archivos
- Eficiencia de espacio para muchos archivos chicos

**Estimación**: ~1000-1500 LOC kernel driver + ~500 LOC host tools (mkfs, fsck).

---

## Resumen: Feature Count por Tier

| Tier | Features | Estado |
|------|----------|--------|
| Tier 0 | 2 | ✅ Completo |
| Tier 1 | 6 | ✅ Completo |
| Tier 2 | 4 | ✅ Completo |
| Tier 3 | 3/5 | ✅ Parcial (musl/chibicc pendientes) |
| Tier 4 | 5 | ✅ Completo |
| Tier 5 | 5 | ✅ Completo |
| Tier 6 | 5 | ✅ Completo |
| Extras | 4 | ✅ Completo |
| Tier 7 | 5/9 | ✅ Parcial (SCHED+MMAP+VFS+MUSL+FORK done) |
| **Tier 8** | **5** | **Paralelizable** |
| Tier 9 | 1/5 | Parcial (BUSYBOX ash parcial) |
| **Total** | **53** | 39 done, 14 pendientes |
