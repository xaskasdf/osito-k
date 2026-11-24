# Paths to Claude on OsitoK — Technical Analysis

Two parallel paths to running AI on OsitoK: **(A)** native transformer inference
and **(B)** running the Claude Code binary. This document analyzes the current state,
gaps, and realistic roadmaps for both.

---

## Path A: Native Transformer Inference

### Current State (Working)

OsitoK runs Llama 3.2 1B inference from bare metal. Full stack implemented:

| Component | Status | Files |
|-----------|--------|-------|
| GGUF v2/v3 loader | Done | `fs/gguf.c` (456 LOC) |
| Tensor engine (Q4_0, Q8_0, F16, F32) | Done | `kernel/tensor.c` (834 LOC) |
| AVX2/FMA dispatch | Done | `kernel/tensor_avx2.c` (165 LOC) |
| Llama forward pass (GQA, SwiGLU, RoPE) | Done | `kernel/inference.c` (608 LOC) |
| GPU compute pipeline (X1-X42) | Done | `drivers/gpu_*.c`, `drivers/sass.c` |
| GPU dispatch wrappers + CB0 | Done | `drivers/gpu_tensor.c` |
| Claude API client (HTTPS/TLS/SSE) | Done | `kernel/claude.c`, `kernel/http.c`, `kernel/tls.c` |

**Performance**: ~5-10 s/tok CPU scalar, ~1-2 s/tok with AVX2 (Llama 1B Q4_0, 256 ctx)

### Bottleneck Analysis

Per-token compute breakdown (16 layers, Llama 1B):

```
matvec_q4_0       112 calls/tok    ~90% compute    BOTTLENECK
attention (QKV)   512 calls/tok    ~3%             O(n^2) with seq length
rmsnorm           49 calls/tok     ~2%
softmax           512 calls/tok    ~3%
rope/silu/vec_*   ~96 calls/tok    ~2%
```

90% del tiempo se gasta en `matvec_q4_0` (dequant Q4_0 + dot product).

### Missing for "Proper" Inference

| Feature | Impact | Effort | Notes |
|---------|--------|--------|-------|
| **Tokenizer (BPE)** | CRITICAL — sin esto no hay input/output de texto | ~2 semanas | Llama usa tiktoken, 128K vocab. Ref: llama2.c de Karpathy |
| **Compile SASS kernels** | 10-50x speedup en GPU | ~1 semana | PTX ya existe en `kernels/*.ptx`, falta `ptxas` → cubin |
| **Sampling (temp/top-p/top-k)** | Calidad de generacion | ~2 dias | ~200 LOC, trivial sobre logits |
| **KV cache float16** | 2x menos memoria KV | ~3 dias | Cambiar storage, convert on access |
| **K-quant support (Q4_K, Q6_K)** | Modelos modernos usan K-quants | ~2 semanas | 256-value blocks, mas complejo |
| **Sliding-window attention** | Contexto >256 sin O(n^2) | ~1 semana | Limitar inner loop a ultimos W tokens |
| **Copy Engine transfers** | 2-5x data transfer speed | ~3 dias | CE DMA ya implementado (X35), falta conectar a gpu_inference |
| **Layer streaming (NVMe)** | Modelos >RAM (8B, 70B) | ~2 semanas | Cargar 2 layers a la vez desde disco |

### Performance Roadmap

```
ACTUAL:  5-10 s/tok (CPU scalar)
         1-2 s/tok  (CPU AVX2)

CON SASS KERNELS COMPILADOS:
  GPU matvec_q4_0:     ~50-200 ms/tok  (10-50x)
  + CE DMA transfers:  ~30-100 ms/tok  (2-5x data)
  + All GPU kernels:   ~20-50 ms/tok   (cubrir el otro 10%)

TARGET:  >10 tok/s en RTX 2070+ (interactivo)
         >1 tok/s en CPU AVX2 moderno
```

### Modelos Soportados

| Modelo | Tamano Q4_0 | RAM Total | Status |
|--------|-------------|-----------|--------|
| Llama 3.2 1B | ~500 MB | ~520 MB | Funciona |
| Llama 3.2 3B | ~1.5 GB | ~1.6 GB | Deberia funcionar (mismo arch) |
| Llama 3 8B | ~4 GB | ~4.1 GB | Necesita >4GB RAM, mismo codigo |
| Llama 3 70B | ~40 GB | ~41 GB | Necesita layer streaming |

### Quick Wins (proximas 2 semanas)

1. **Tokenizer**: Implementar BPE para Llama 3 vocab — hace la inference interactiva
2. **Sampling**: temperature + top-p — 200 lineas, mejora dramática la calidad
3. **Compile SASS**: Correr `ptxas` en la maquina host, generar headers — GPU 10-50x

---

## Path B: Ejecutar el Binario de Claude Code

### Que es Claude Code

| Aspecto | Dato |
|---------|------|
| Runtime | Bun v1.3.11 (JavaScriptCore/WebKit, escrito en Zig) |
| Bundle | `cli.js` — 12.2 MB ESM minificado, codigo cerrado |
| Binario | 227 MB ELF dinamico (Bun + JSC embebido) |
| Deps nativas | libc.so.6, libpthread.so.0, libm.so.6, libdl.so.2 |
| APIs criticas | child_process (200+ calls), fs, net, tls, worker_threads, tty |
| UI | React + Ink (terminal TUI), tree-sitter (WASM), ripgrep (binario) |

### Gap Analysis: OsitoK vs Requisitos

```
TIENE OsitoK                         NECESITA Claude Code
---------------------------------    ----------------------------------
ELF64 loader (ET_EXEC statico)       Dynamic linker (ld-linux-x86-64.so.2)
13 syscalls basicos                  60+ syscalls Linux
brk heap                            mmap/mprotect (virtual memory)
Single process                      clone/pthread (threads)
Polling I/O                         epoll (async event loop)
No signals                          rt_sigaction/sigprocmask
Flat filesystem                     Directorios, permisos, symlinks
TCP/TLS 1.2                         Full POSIX sockets + TLS 1.3
Serial + framebuffer                TTY raw mode, ANSI, winsize
```

### Opciones Evaluadas

#### Opcion 1: Linux-Compatible Layer (6-12 meses)

Hacer OsitoK suficientemente Linux-compatible para correr el binario tal cual.

**Requiere:**
- Dynamic linker completo (~2000 LOC)
- mmap/mprotect con address space management (~1500 LOC)
- clone/pthreads infrastructure (~1500 LOC)
- epoll event loop (~800 LOC)
- Futex synchronization (~600 LOC)
- Signal handling (~1200 LOC)
- 40+ syscalls adicionales (~2000 LOC)
- Portar glibc/musl, libpthread, libm, libdl, libssl
- Per-process page tables y aislamiento

**Total estimado: ~12,000 LOC de kernel + port de libc**

**Veredicto**: Es basicamente construir un Linux-lite. Posible pero es un proyecto de 6-12 meses.

#### Opcion 2: Recompilar Bun Estatico para OsitoK (3-6 meses)

Tomar el source de Bun (github.com/oven-sh/bun) y recompilarlo.

**Requiere:**
- Entender codebase de Bun (Zig + C++) y JSC (~2 meses)
- Reemplazar pthreads con stubs/coroutines
- Reemplazar epoll con polling custom
- Reemplazar mmap con brk-based allocation
- Recompilar JavaScriptCore (WebKit) — proyecto enorme
- Claude Code source es cerrado — tendriamos Bun sin el cli.js real

**Veredicto**: Alto esfuerzo, y sin acceso al source de Claude Code solo tendriamos Bun (que podria correr otro JS).

#### Opcion 3: QuickJS como Runtime JS (2-4 semanas)

Portar QuickJS (Fabrice Bellard) a OsitoK.

| Aspecto | QuickJS | Bun |
|---------|---------|-----|
| Tamano | ~500 KB estatico | 227 MB |
| Lenguaje | C puro | Zig + C++ |
| Deps | Ninguna | libc, pthreads, libm, etc. |
| ES2020+ | Si | Si |
| JIT | No (interprete) | Si (JSC JIT) |
| Velocidad | ~100x mas lento | Referencia |
| npm | No | Si |
| Threads | No | Si |

**Requiere para OsitoK:**
- Compilar QuickJS con TCC o GCC cross → ET_EXEC statico
- Agregar bindings C para syscalls de OsitoK (fs, net)
- ~500 LOC de glue code

**Veredicto**: Muy viable. No corre Claude Code real, pero permite ejecutar JavaScript custom que hable con la API de Claude.

#### Opcion 4: Cliente C Nativo con Tool Use (2-3 semanas)

Extender lo que ya tenemos (X-CL1) con soporte de tools.

**Ya funciona:**
- `claude_chat()` → POST /v1/messages → SSE streaming
- Verificado: Claude responde desde bare metal

**Agregar:**
- Multi-turn conversation history (X-CL2, ~200 LOC)
- Tool use protocol: file read/write, exec, search (X-CL3-5, ~1000 LOC)
- REPL interactivo con prompt persistente

**Veredicto**: Path mas rapido a un "Claude agent" funcional en OsitoK. No es Claude Code, pero hace lo mismo: lee archivos, escribe codigo, ejecuta comandos.

---

## Comparacion de Paths

```
                    Esfuerzo    Resultado                          Viable?
                    --------    ---------                          -------
Path A: Inference   2-4 sem     Llama local >1 tok/s, interactivo   SI
Path B1: Linux-compat 6-12 mes  Claude Code binario real             Dificil
Path B2: Bun statico  3-6 mes   Bun runtime (sin cli.js)             Medio
Path B3: QuickJS      2-4 sem   JS runtime, scripts custom           SI
Path B4: C nativo     2-3 sem   Claude agent con tools               SI (ya empezado)
```

---

## Recomendacion: Roadmap Combinado

### Fase 1: Inference Interactiva ✅
- ~~Tokenizer BPE para Llama 3~~ ✅ X-TOK1
- ~~Compilar SASS kernels desde PTX existente~~ ✅ X41-X42
- ~~GPU inference dispatch con VRAM-resident weights~~ ✅ X-INF1/2/3
- Sampling (temperature, top-p) — pendiente (X-SAMPLE)
- Resultado: Llama 3.2 1B funciona con BPE tokenizer + GPU dispatch

### Fase 2: Claude Agent Nativo ✅
- ~~X-CL2: REPL multi-turn con historial~~ ✅
- ~~X-CL3: Tool use — file read/write~~ ✅
- ~~X-CL4: Tool use — exec (compilar y ejecutar C)~~ ✅
- ~~X-CL5: Tool use — search (grep en codebase)~~ ✅
- Resultado: Claude agent funcional que lee, escribe, compila y ejecuta desde OsitoK

### Fase 3: JavaScript + Git ✅
- ~~QuickJS ES2020+ engine en bare-metal~~ ✅ X-JS
- ~~Git nativo con SHA-1 + zlib~~ ✅ X-GIT
- QuickJS bindings a fs/net — pendiente
- Resultado: JavaScript runtime + version control nativos

### Fase 4: OS Maturity — SIGUIENTE
Madurar el kernel para soportar software real. Ver `docs/os-selfhost-roadmap.md` Tier 7.
- **Scheduler preemptivo** (X-SCHED) — timer-based context switch, prerequisito para todo
- **mmap/munmap/mprotect** (X-MMAP) — virtual memory real, bloqueante #1 para Linux compat
- **VFS layer** (X-VFS) — mount points, /dev, /proc
- **Port musl libc** (X-MUSL) — libc POSIX para portar apps reales
- **Threads** (X-THREAD) — clone/futex, usar SMP cores
- **Port editor** (X-EDIT) — editar archivos desde OsitoK (kilo ~1000LOC)
- **TCP server** (X-HTTPD) — listen/accept, servir HTTP
- **Self-hosting** (X-SELF) — compilar el propio kernel desde OsitoK
- Resultado: busybox sh funcional, editor, TCP server, self-compile

### Fase 5: GPU Optimization (paralela)
Ver `docs/os-selfhost-roadmap.md` Tier 8.
- **Full VRAM pipeline** (X-GPU-OPT) — eliminar CPU fallback en forward pass
- **Sampling** (X-SAMPLE) — temperature, top-p, top-k
- **K-quant support** (X-KQUANT) — Q4_K_M, Q6_K para modelos modernos
- **Hardware testing** (X-HW) — validar GPU pipeline en RTX 2070+ real
- **Layer streaming** (X-STREAM) — modelos >RAM desde NVMe
- Resultado: >10 tok/s en GPU real, modelos 8B viables

### Fase 6: Linux Binary Compatibility (largo plazo)
Ver `docs/binary-compat-roadmap.md`.
- 40+ syscalls POSIX (X-SYSCALL40)
- Socket syscalls (X-SOCKET)
- Señales completas (X-SIGNAL)
- Objetivo: correr binarios Linux estáticos sin modificar
- Eventualmente: dynamic linker → Bun → Claude Code binario

---

## Dependencias entre Paths (actualizado marzo 2026)

```
COMPLETADO                           SIGUIENTE
──────────                           ─────────
Path A: Inference ✅                 Fase 5: GPU Optimization
  Tokenizer ✅                         X-SAMPLE (sampling)
  SASS Kernels ✅                      X-KQUANT (K-quants)
  VRAM weights ✅                      X-GPU-OPT (full VRAM pipeline)
  GPU dispatch ✅                      X-HW (hardware test!)
                                       X-STREAM (layer streaming)
Path B4: C Agent ✅
  Claude API ✅                      Fase 4: OS Maturity
  Tool Use ✅                          X-SCHED (preemptive scheduler)
  REPL multi-turn ✅                       ↓
                                       X-MMAP (virtual memory)
Path B3: QuickJS ✅                        ↓        ↓
  JS engine ✅                         X-VFS ──→ X-MUSL (musl libc)
  Git nativo ✅                                      ↓
                                       X-THREAD     X-EDIT (editor)
                                           ↓             ↓
                                       X-HTTPD      X-SELF (self-host kernel)

                                     Fase 6: Linux Compat
                                       X-SYSCALL40 (40 syscalls)
                                       X-SOCKET (POSIX sockets)
                                       X-SIGNAL (full signals)
                                           ↓
                                       X-BUSYBOX (busybox sh)
                                           ↓
                                       Dynamic Linker
                                           ↓
                                       Claude Code Binary (B1)
```

---

## Appendix: Syscall Gap (OsitoK vs Linux)

### Implementados (15+)
```
read(0)  write(1)  open(2)  close(3)  fstat(5)  lseek(8)
brk(12)  ioctl(16)  writev(20)  access(21)  unlink(87)
exit(60)  arch_prctl(158)
+ pipe/dup2/kill via X-PIPE subsystem (kernel-level, no via SYSCALL nr yet)
+ signal handlers (SIGINT/SIGTERM/SIGKILL/SIGPIPE) via X-PIPE
```

### Criticos para binarios Linux (faltan ~25)
```
mmap(9)  mprotect(10)  munmap(11)       Virtual memory
clone(56)  futex(202)  set_tid_address   Threading
epoll_create(213)  epoll_ctl  epoll_wait  Async I/O
rt_sigaction(13)  rt_sigprocmask(14)     Signals
getpid(39)  getuid(102)  gettid(186)     Identity
stat(4)  lstat(6)  readlink(89)          Filesystem
getdents64(217)  mkdir(83)  rmdir(84)    Directories
pipe2(293)  dup2(33)  fcntl(72)          FD management
clock_gettime(228)  nanosleep(35)        Time
socket(41)  connect(42)  sendto(44)      Sockets (vs raw TCP)
```

### Nice-to-have para compatibilidad completa (~35 mas)
```
poll  select  pread64  pwrite64  sendfile
getcwd  chdir  rename  chmod  chown
kill  tgkill  wait4  execve
setsockopt  getsockopt  bind  listen  accept
prctl  getrandom  memfd_create  copy_file_range
sched_yield  sched_getaffinity  set_robust_list
```
