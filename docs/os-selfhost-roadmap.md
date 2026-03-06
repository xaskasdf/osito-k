# Plan: OsitoK General-Purpose OS Roadmap — De AI Firmware a OS Self-Hosting

## Context

OsitoK x86-64 ha completado X1–X42 + X-CPU1: UEFI boot, NVMe read, OsitoFS,
Ethernet + UDP, GPU compute pipeline completo (SASS kernels + CB0 dispatch),
CPU Llama inference con AVX2. Pero es esencialmente un **firmware especializado**
que corre una secuencia monolítica en ring 0 sin aislamiento.

**Objetivo**: Transformar OsitoK en un OS general capaz de:
1. Ejecutar programas compilados (ELF binaries)
2. Auto-hospedarse (compilar C en el propio OS)
3. Hacer llamadas HTTPS a la API de Claude
4. Eventualmente correr un cliente Claude CLI nativo

**Próximos pasos inmediatos**: X-CPU2 (NVMe write) y X-CPU3 (UDP prompt server),
seguidos de las capas fundamentales del OS.

---

## Estado Actual del OS — Inventario

| Subsistema | Estado | Qué Falta |
|-----------|--------|-----------|
| **Memoria** | Bitmap page allocator (4KB), `mem_alloc_aligned` | No paging, no heap, no malloc/free, no VM |
| **Procesos** | Ninguno — secuencia monolítica ring 0 | No scheduler, no syscalls, no aislamiento |
| **Filesystem** | OsitoFS v2 read/write, NVMe read/write | No directorios, no permisos |
| **Red** | I211 Ethernet + ARP + IPv4 + UDP (polling) | No TCP, no DNS, no TLS, no ICMP |
| **Consola** | Serial COM1 + framebuffer GOP | No input por teclado, no ANSI, no terminal |
| **libc** | memset/memcpy/strlen/strcmp inline | No stdio, no stdlib, no math.h |
| **ELF** | Parser parcial (solo firmware GSP) | No loader user-space, no dynamic linking |
| **Interrupts** | UEFI-managed, polling en todo | No IDT propio, no APIC, no timer IRQ |
| **GPU** | Completo: X1-X42 (boot chain + compute + SASS) | Funcional pero GSP boot chain sin validar en HW |
| **AI** | Llama 3.2 1B CPU inference + AVX2 | ~5-10s/tok, single-token, solo Q4_0/Q8_0 |

---

## Roadmap de Features — Orden de Ejecución

### Tier 0: Completar Features Pendientes (X-CPU2, X-CPU3)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-CPU2** | **NVMe write** | IO command write + OsitoFS v2 write (create, append, truncate) | ~400 | Done ✓ |
| **X-CPU3** | **UDP prompt server** | Recibir prompt UDP:7777 → inference → devolver tokens | ~200 | Done ✓ |

### Tier 1: OS Fundaciones — Ejecutar Programas (~2-3 semanas)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-OS1** | **IDT + Exceptions** | Tabla de interrupciones propia, handlers de #PF/#GP/#UD, APIC timer | ~500 | Ninguna |
| **X-OS2** | **Paging (x86-64 4-level)** | PML4 setup, identity map kernel, per-process page tables, CR3 switch | ~600 | X-OS1 |
| **X-OS3** | **Heap allocator** | malloc/free sobre page allocator (first-fit o buddy), brk/sbrk | ~400 | X-OS2 |
| **X-OS4** | **Syscall interface** | MSR setup (LSTAR/STAR/FMASK), dispatch table, ABI: RAX=nr, RDI-R9=args | ~400 | X-OS2 |
| **X-OS5** | **ELF loader** | Cargar PT_LOAD segments, setup stack (argc/argv/envp), jump to e_entry | ~400 | X-OS2, X-OS4 |
| **X-OS6** | **Proceso mínimo** | process_t struct, file descriptor table, exec/exit/waitpid | ~500 | X-OS4, X-OS5 |

**Hito**: Ejecutar `hello.elf` compilado en Linux que hace `write(1, "Hello\n", 6); exit(0);`

### Tier 2: Shell + Filesystem Write (~2 semanas)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-OS7** | **Terminal + line editor** | Line discipline, backspace, Ctrl+C, VT100 básico (ANSI colors/cursor) | ~400 | X-OS1 |
| **X-OS8** | **Keyboard driver** | PS/2 o USB HID keyboard (scancode → ASCII), ring buffer | ~300 | X-OS1 |
| **X-OS9** | **Mini shell** | Parse cmdline, fork+exec, pipes, redirection (<, >, >>), builtins (cd, echo, exit) | ~600 | X-OS6, X-OS7, X-OS8 |
| **X-OS10** | **OsitoFS v2 write** | Crear archivos, append, truncate. File descriptor write syscall | ~500 | X-CPU2, X-OS4 |

**Hito**: Prompt interactivo `osito>` que ejecuta programas del disco

### Tier 3: Compilador Self-Hosting (~3 semanas)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-OS11** | **Cross-compile TCC** | Portar TCC (Tiny C Compiler, ~15KB bin) para target OsitoK | ~200 glue | X-OS6, X-OS3 |
| **X-OS12** | **Newlib stubs** | _read/_write/_sbrk/_exit → syscalls de OsitoK. libc mínima | ~300 | X-OS4, X-OS3 |
| **X-OS13** | **TCC self-hosting** | `tcc -c tcc.c && tcc -o tcc tcc.o` en OsitoK | ~100 test | X-OS11, X-OS12 |
| **X-OS14** | **Port musl libc** | libc POSIX-compliant. Compilar con TCC en OsitoK | ~500 glue | X-OS13 |
| **X-OS15** | **Port chibicc** | Compilador C11 completo. Mejor que TCC para apps reales | ~300 glue | X-OS14 |

**Hito**: Escribir, compilar y ejecutar un programa C **dentro de OsitoK**

### Tier 4: TCP/TLS + HTTP Client (~3-4 semanas)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-NET1** | **ICMP** | Ping request/reply, unreachable reporting | ~200 | Ninguna |
| **X-NET2** | **TCP stack** | Portar lwIP (o implementar minimal: 3-way handshake, send/recv, close) | ~1500 | X-NET1 |
| **X-NET3** | **DNS resolver** | UDP query a 8.8.8.8, parse A records, cache | ~300 | X-NET2 |
| **X-NET4** | **TLS 1.2/1.3** | Portar BearSSL (30-50KB, no malloc, ideal bare-metal) | ~500 glue | X-NET2 |
| **X-NET5** | **HTTP client** | GET/POST sobre TLS, JSON parse mínimo, streaming response | ~600 | X-NET3, X-NET4 |

**Hito**: `curl api.anthropic.com/v1/messages` funcional desde OsitoK

### Tier 5: Claude CLI Nativo (~2-3 semanas)

| ID | Feature | Descripción | ~Líneas | Deps |
|----|---------|-------------|---------|------|
| **X-CL1** | **Claude API client (C)** | HTTP POST a /v1/messages, streaming SSE, tool_use parse | ~800 | X-NET5, X-OS3 |
| **X-CL2** | **REPL interactivo** | Prompt → API → print response, historial, multi-turn | ~400 | X-CL1, X-OS7 |
| **X-CL3** | **Tool: file read/write** | Claude puede leer/escribir archivos del FS | ~300 | X-CL2, X-OS10 |
| **X-CL4** | **Tool: exec** | Claude puede compilar y ejecutar código | ~300 | X-CL3, X-OS11 |
| **X-CL5** | **Tool: search** | Grep-like búsqueda en codebase (simple C, no ripgrep) | ~400 | X-CL3 |

**Hito**: Claude CLI nativo en C que lee código, escribe archivos y ejecuta programas

### Tier 6: Avanzado (futuro)

| ID | Feature | Descripción |
|----|---------|-------------|
| **X-SMP** | Multi-core (AP startup, spinlocks) | Paralelizar inference y OS tasks |
| **X-PIPE** | IPC pipes + señales POSIX | Comunicación entre procesos |
| **X-DYN** | Dynamic linking (ld.so) | Shared libraries |
| **X-JS** | Port QuickJS (~367KB) | JavaScript runtime para Claude Code real |
| **X-GIT** | Port git (o mini-vcs) | Version control nativo |

---

## Cadena de Dependencias

```
Tier 0 (inmediato):
  X-CPU2 (NVMe write) ──────────────────────────────────────────→ X-OS10
  X-CPU3 (UDP prompt) ──────────────────────────────────────────→ (standalone)

Tier 1 (OS fundaciones):
  X-OS1 (IDT) → X-OS2 (Paging) → X-OS3 (Heap) ─┐
                     │                             ├→ X-OS6 (Process) → Tier 2
                     └→ X-OS4 (Syscall) → X-OS5 (ELF) ┘

Tier 2 (shell):
  X-OS7 (Terminal) ─┐
  X-OS8 (Keyboard) ─┼→ X-OS9 (Shell) → Tier 3
  X-OS6 (Process)  ─┘
  X-CPU2 → X-OS10 (FS write) → Tier 3

Tier 3 (compiler):
  X-OS11 (TCC port) → X-OS13 (self-host) → X-OS14 (musl) → X-OS15 (chibicc)
  X-OS12 (Newlib) ──→ X-OS13

Tier 4 (network):
  X-NET1 (ICMP) → X-NET2 (TCP) → X-NET3 (DNS) ─┐
                                   X-NET4 (TLS) ─┼→ X-NET5 (HTTP)
                                                  └→ Tier 5

Tier 5 (Claude CLI):
  X-NET5 + X-OS3 → X-CL1 (API) → X-CL2 (REPL) → X-CL3-5 (tools)
```

---

## Opciones Tecnológicas Clave

### Compilador: TCC → chibicc
- **TCC**: 15-20KB binario, self-hosting, single-pass. Ideal para bootstrap.
- **chibicc**: C11 completo, multi-pass, compila Git/SQLite/libpng. Para producción.
- **Ambos**: Cross-compilar desde Linux inicialmente, luego self-host en OsitoK.

### libc: Newlib stubs → musl
- **Newlib**: Diseñada para bare-metal, solo necesita _read/_write/_sbrk stubs.
- **musl**: POSIX-compliant, 500KB static core. Para apps reales.
- **Path**: Newlib para bootstrap TCC, musl para chibicc y apps.

### TCP: lwIP (port) o custom minimal
- **lwIP**: ~100KB, maduro, probado en embedded. Port = driver glue code.
- **Custom**: Más control, menos código, pero muchos edge cases (RFC 793).
- **Recomendación**: lwIP (confiable) o TCP mínimo si solo necesitamos HTTP client.

### TLS: BearSSL
- **BearSSL**: 30-50KB, no necesita malloc, diseñado para bare-metal.
- **Alternativas**: wolfSSL (más features, más grande), mbedTLS (necesita malloc).
- **BearSSL es ideal** para OsitoK por su diseño sin heap.

### Claude CLI: C nativo (no Node.js)
- Claude Code real es Node.js/TypeScript (~10.5MB bundle). **No viable** a corto plazo.
- **Path real**: Cliente C que habla directo con la API REST de Claude.
- **Largo plazo**: Port QuickJS (~367KB JS runtime) para JavaScript.

---

## Primer Paso: X-CPU2 + X-CPU3

### X-CPU2: NVMe Write (~400 líneas)
**Archivos**: `arch/x86/drivers/nvme.c`, `arch/x86/fs/ositofs2.c`
- Agregar NVMe IO write command (opcode 0x01, misma infraestructura que read)
- `nvme_write(lba, count, buf)` — write blocks
- `nvme_write_bytes(offset, buf, len)` — byte-level con chunking
- OsitoFS v2: `osfs2_create(name, size)`, `osfs2_write(file, offset, buf, len)`
- Actualizar file table + superblock en disco

### X-CPU3: UDP Prompt Server (~200 líneas)
**Archivos**: `arch/x86/kernel/net.c` (o nuevo `kernel/prompt_server.c`)
- UDP listener en port 7777
- Formato: raw text prompt → inference → raw text response
- Loop: `net_poll() → parse prompt → inference_generate() → net_udp_send()`
- Integrar con GPU dispatch table (usa GPU kernels si disponible)

### Verificación
```bash
make -C arch/x86 clean && make -C arch/x86
# QEMU: enviar prompt por UDP → recibir tokens
echo "Hello" | nc -u localhost 7777
```

---

## Referencia: OSes Hobby que Lograron Self-Hosting

| OS | Path | Tiempo al Self-Hosting |
|----|------|----------------------|
| **SerenityOS** | Kernel→FS→Shell→Port GCC | ~6 meses |
| **Sortix** | Kernel POSIX→Port GCC→Self-host | ~4 meses |
| **Managarm** | Microkernel→IPC→Port GCC | ~8 meses |

**Lección clave**: Todos portaron GCC/TCC en vez de escribir compilador propio.
Cross-compilar primero, luego self-host. TCC es el path más rápido al bootstrap.
