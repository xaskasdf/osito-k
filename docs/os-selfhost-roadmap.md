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

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-SCHED** | **Scheduler preemptivo** | Timer-based context switch entre procesos. APIC timer ya corre a 100Hz — agregar per-process kernel stack, save/restore de registros completo, round-robin queue. Prerequisito para threads y procesos reales. | ~800 | Ninguna |
| **X-MMAP** | **mmap/munmap/mprotect** | Virtual memory real. MAP_ANONYMOUS (core), MAP_FIXED (ELF loader), MAP_PRIVATE (file-backed). Per-process VMA tracking. Bloqueante #1 para binarios Linux. | ~1200 | X-SCHED |
| **X-VFS** | **VFS layer** | Abstracción sobre OsitoFS. Mount points, `/dev` (null, zero, urandom, console), `/proc` (self/maps, self/status). Abrir la puerta a múltiples filesystems. | ~800 | Ninguna |
| **X-MUSL** | **Port musl libc** | Cross-compilar musl como libc estática para OsitoK. Syscall stubs, errno, TLS setup. Reemplaza CRT mínimo actual. Habilita portar apps POSIX reales. | ~500 glue | X-MMAP, X-VFS |
| **X-THREAD** | **Threads (clone/futex)** | clone(CLONE_VM\|CLONE_THREAD), futex(WAIT/WAKE), set_tid_address, gettid. Per-thread stacks, TLS via arch_prctl ARCH_SET_FS. Usar SMP cores para threads reales. | ~1000 | X-SCHED, X-MMAP |
| **X-EDIT** | **Port editor mínimo** | Portar un editor de texto (kilo ~1000LOC, o nano subset). Editar archivos desde OsitoK sin host. Necesita raw mode TTY + VT100 ANSI. | ~600 glue | X-MUSL |
| **X-HTTPD** | **TCP server (listen/accept)** | Completar TCP stack: listen(), accept(), server sockets. Implementar HTTP server mínimo. Exponer servicios desde OsitoK a la red. | ~600 | Ninguna |
| **X-SELF** | **Self-hosting completo** | Compilar el propio kernel x86 desde OsitoK. Requiere: TCC o GCC port, musl libc, gnu-efi headers, ld linker in-OS, make equivalent. Hito definitivo de un OS. | ~2000 | X-MUSL, X-EDIT |

**Hito**: `busybox sh` (musl-static, ~1MB) corre dentro de OsitoK. Editor funcional.

### Tier 8: GPU + Inference Optimization

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

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-SYSCALL40** | **40 syscalls POSIX** | Completar las ~27 syscalls faltantes para binarios musl-static (stat, getdents64, clock_gettime, nanosleep, signals, fork/execve/wait4, socket API). | ~2000 | X-MMAP, X-THREAD |
| **X-BUSYBOX** | **Run busybox** | Test target: `busybox sh`, `busybox ls`, `busybox cat`. Primer binario Linux no-trivial sin modificar. | ~200 test | X-SYSCALL40, X-MUSL |
| **X-SOCKET** | **Socket syscalls** | socket(AF_INET), connect, sendto, recvfrom, bind, listen, accept. Wrapper sobre TCP/UDP stack existente. Expone red via interfaz POSIX. | ~800 | X-HTTPD |
| **X-SIGNAL** | **Señales completas** | rt_sigaction, rt_sigprocmask, rt_sigreturn, sigframe en user stack. Signal delivery en syscall return + timer tick. | ~1000 | X-SCHED |
| **X-PE** | **Windows PE loader** | PE/COFF loader + NT syscall translation (Phase 3 del binary-compat-roadmap). Largo plazo. | ~5000+ | X-SYSCALL40 |

**Hito**: `busybox sh` interactivo. Luego: static Go binaries, toybox.

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
| **Tier 7** | **8** | **← Siguiente** |
| **Tier 8** | **5** | Paralelizable |
| **Tier 9** | **5** | Largo plazo |
| **Total** | **52** | 34 done, 18 pendientes |
