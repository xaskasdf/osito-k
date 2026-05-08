# OsitoK WebAssembly Port

`arch/wasm/` compila el kernel x86-64 a WebAssembly via Emscripten, reemplazando lo dependiente de hardware con stubs y drivers de browser. Permite correr el shell, el LLM (Llama via GGUF), OsitoFS v2, el compositor GUI y módulos `.so` cargados con `dlopen` (probado: Quake 2 wasm side-module).

## Build

```bash
cd arch/wasm
make                # → build/osito.{html,js,wasm}
make run            # emrun build/osito.html
```

Requiere `emcc` (Emscripten 3.x+). Output: `osito.wasm` ~2 MB, `osito.js` ~550 KB, `osito.html` ~5 KB.

## Estrategia: compartir + stub

| Capa | Approach |
|---|---|
| Kernel core (shell, terminal, tensor, inference, tokenizer, GUI, compositor, OsitoFS, FS subsystems portables) | Compilados directo desde `arch/x86/`. |
| HW dependent (paging, IDT, SMP, syscall, NVMe, GPU/GSP, xHCI, HDA, NIC, VirtIO) | No-op stubs en `hal/stubs.c`. |
| Inline asm x86 (CMOS RTC, RDTSC, RDRAND, CPUID, MSR) | Guardado con `#ifdef __EMSCRIPTEN__` en cada origen. |
| Network (DHCP, NTP, IPv6, mDNS, sshd, TLS13, OFTP) | Stubs (devuelven -1 / no-op). Browser usa `fetch()` para I/O real. |
| AVX2 / tensor_avx2 | Removido del build; `cpu_has_avx2() = false` → ruta scalar. Stub `matvec_q4_0_avx2` por si linker lo necesita. |
| Display | Canvas 2D drivers (`drivers/display.c`, `drivers/fb.c`). |
| Input | `drivers/kb.c` ring buffer alimentado por JS keyboard events. |
| Serial | `drivers/serial.c` → `terminal` div del HTML. |
| Memoria | `hal/mem.c` backed por libc malloc (sin paging). |
| Process / exec | `proc_exec` carga side modules vía `dlopen` con **validación de arquitectura** (rechaza ELF nativo). |

## Arquitectura validation en `proc_exec`

Antes de `dlopen`, lee los primeros 4 bytes y rechaza con mensaje claro si no es wasm:

```
[EXEC] Not a wasm side module: ELF (x86-64) — wasm runtime cannot dlopen native binaries.
[EXEC] Tip: rebuild with 'emcc -sSIDE_MODULE=2'.
[exec] exited with code 1
osito>
```

Sin esta validación, `dlopen` lanza una promise rejection sin handler que suspende Asyncify para siempre y cuelga el shell.

## Asset hosting (Cloudflare R2)

Modelo GGUF + imagen OsitoFS se descargan desde:

- `https://wasm.naranjositos.tech/models/smollm2-135m-instruct-q4_0.gguf`
- `https://wasm.naranjositos.tech/images/q2_wasm.img`

`wasm.naranjositos.tech` es un **R2 custom domain nativo** sobre el bucket `factory` (sin worker en medio). El bucket tiene CORS rule que acepta:

```
origins: ["https://*.naranjositos.tech", "https://naranjositos.tech",
         "http://localhost:8000", "http://localhost:3000", "http://127.0.0.1:8000"]
methods: ["GET", "HEAD"]
exposeHeaders: ["Content-Length", "Content-Type", "Content-Range", "Accept-Ranges", "ETag"]
```

> **Nota**: `factory.naranjositos.tech` (otro custom domain del mismo bucket) está interceptado por un Worker `red-night-c3d4` que NO añade headers CORS. No lo uses para fetches cross-origin.

### Override de URLs (testing local)

```
http://localhost:8000/osito.html?fs=./local.img&model=./local.gguf
```

Los query params `fs=` y `model=` reemplazan los URLs por defecto.

## Subsistemas nuevos del x86 ya integrados

(2026-04-01 → 2026-05-08 sweep — todos compilan en wasm)

```
klog, kallsyms, usym, sysctl, sysv_ipc, vt, blkdev, dm, sysfs, kobject,
inotify, random, oom, psi, ns, caps, strace, lockdep, seccomp, kprof,
kstate, trace, io_uring, bpf, msync, memcompress, timers, crypto, crypto2,
sched_rt, workqueue, kthread, slab, socket, tokenizer_trie, vfs, tmpfs
```

Excluidos por dependencias x86-only (perf MSRs, RDPMC, CPUID parsing): `perf, power, cpu_features, cpu_topology, dispatch, rcu, panic`. Se proveen como stubs en `hal/stubs.c`.

## Verificación end-to-end

1. `make && make run`
2. Esperar boot:
   - Modelo GGUF parseado, LLaMA inicializado.
   - OsitoFS montado (label `Quake2-WASM`).
3. Probar:
   - `help` — lista de comandos.
   - `echo hi | grep h` — pipes (X-PIPES).
   - `uname` / `mem` / `ps` / `cpus` — sysinfo.
   - `ls` — lista archivos en `/`.
   - `chat hello` — inferencia local (SmolLM2-135M).
   - `exec quake2.so` — carga side module → menú de Quake 2.
4. Console del navegador no debe tener errores `panic`/`abort` ni unhandled promise rejections.

## Layout de `arch/wasm/`

```
arch/wasm/
├── boot/wasm_init.c       Entry: serial → fb → heap → fetch model+FS → shell
├── hal/
│   ├── mem.c              malloc-backed memory subsystem
│   └── stubs.c            ~150 stubs para subsystems excluidos
├── drivers/
│   ├── serial.c           print → JS terminal
│   ├── kb.c               JS events → kb ring buffer
│   ├── display.c          Canvas 2D framebuffer
│   └── fb.c               framebuffer abstraction
├── include/types.h        Re-exporta arch/x86/include/types.h con guards
├── Makefile               emcc + ASYNCIFY + MAIN_MODULE + 512MB heap
└── shell.html             Template HTML (fetch model+FS + Module config)
```

## Limitaciones conocidas

- Sin red real — `fetch()` solo desde JS (model + FS image). No TCP/UDP/HTTPS.
- Sin paging — un solo address space, `fork`/`exec` real no disponibles.
- Inferencia LLM scalar (no AVX2) → ~5× más lento que x86 nativo.
- `dlopen` solo acepta wasm side modules (`-sSIDE_MODULE=2`), no ELF nativos.

## Configuración R2 CORS — referencia rápida

```bash
curl -X PUT https://api.cloudflare.com/client/v4/accounts/$ACCOUNT/r2/buckets/$BUCKET/cors \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{
    "rules": [{
      "allowed": {
        "origins": ["https://*.naranjositos.tech","http://localhost:8000"],
        "methods": ["GET","HEAD"],
        "headers": ["*"]
      },
      "exposeHeaders": ["Content-Length","Content-Type","Content-Range","Accept-Ranges","ETag"],
      "maxAgeSeconds": 86400
    }]
  }'
```

Token requiere scope **`Workers R2 Storage:Edit`**. Para purgar cache CDN: añadir **`Cache Purge`** en el token.
