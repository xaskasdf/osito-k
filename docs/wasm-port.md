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

---

## Capabilities snapshot — May 2026

55/103 kernel modules compilados (53% del x86 kernel). Los 48 restantes
son arquitectónicamente excluidos (HW-bound, real-IP-net, host-OS). Todos
los items abajo están live en `https://factory.naranjositos.tech/wasm/osito.html`.

### LLM stack

- **Default model**: `brandon-tiny-10m-instruct.f16.gguf` (21 MB descarga
  inicial, cacheado por Service Worker). Custom block_sharing arch:
  12 unique blocks aliasados a 24 logical layers vía `layer_map`,
  DenseFormer DWA + Value Residual Learning + 4 register tokens. SPM
  tokenizer (no BPE byte-level). Speed: **64 ms/tok** (~15 tok/s).
- **Sampling auto-tuned** al detectar arch=brandon: temp 0.7, penalty
  1.2 0 0, ngram 3. User overrides (`temp`, `penalty`, `ngram`)
  persisten en localStorage.
- **Comandos**: `chat`, `rag <ctx> ::: <q>`, `bench [n]`, `claude` REPL
  (Anthropic API + SSE streaming), `bdebug`.

Ver [docs/brandon-tiny-integration.md](brandon-tiny-integration.md)
para guía completa de port a otros runtimes.

### Filesystem (12 formatos)

`mount-fs <type> <url>` + `ls /<type>/` + `cat /<type>/<file>`:

| Type | Driver | read_file | Notes |
|---|---|---|---|
| (primary) | ositofs2 | ✓ | persistido a IndexedDB; `git`/`cc -o`/edits sobreviven reload |
| iso | iso9660 | ✓ | callback-based; LBA 2048 |
| ext | ext2/3/4 | ✓ | callback-based; LBA 512 |
| fat | fat32 | ✓ | nvme_route swap; LFN |
| exfat | exfat | ✓ | nvme_route swap |
| ntfs | ntfs | ✓ | MFT, run-list parser |
| hfs | hfsplus | ls-only | catalog B-tree |
| btrfs | btrfs | ls-only | chunk tree |
| apfs | apfs | ls-only | container + volume superblock |
| udf | udf | ls-only | DVD/Blu-ray |
| sqfs | squashfs | ls-only | zlib decompress |

**Multi-aux real**: uno de cada tipo simultáneamente (10 slots, cada uno
con su propio backing buffer). `umount [type]` libera. VFS auto-mount
hace `cat /iso/foo` y `cat /fat/bar` en paralelo sin re-mount.

### Network bridges (browser-side)

| Comando | Bridge | Server-side |
|---|---|---|
| `curl <url>` | JS `fetch()` | (nada — CORS browser) |
| `claude` REPL | JS fetch + SSE | api.anthropic.com `/v1/messages` |
| `ws open <url>` | `new WebSocket` | cualquier wss:// |
| `tcp connect <host> <port>` | WebSocket → CF Worker proxy | [`tools/tcp-proxy-worker.js`](../tools/tcp-proxy-worker.js) |
| `https <host> [path]` | tls.c/tls13.c sobre net_tcp_send/recv → wasm_ws_* | TCP proxy + cert-validation OFF |
| `crypto sha256/sha512` | crypto.c/crypto2.c (linked, no transport) | (local) |

Linkeados también: `tls.c`, `tls13.c`, `wayland.c`, `fuse.c`, `evdev.c`,
`pty.c`, `sshd.c`, `initramfs.c`, `git.c`, `zlib.c`, `rcu.c` — pure C,
sin deps host.

### Persistencia

- **IndexedDB** (`osito-fs` database, `img/main` key): full FS image,
  ~56 MB. Se guarda al final de cada comando shell si está dirty.
  Restore al boot antes de mount.
- **localStorage** (`osito-cfg-*`): sampling tunables (temp/topp/rep/
  pres/freq/ngram) y proxy URL. Cargado al boot tras llama_init.

### PWA + offline

- `manifest.json` + Service Worker (`sw.js`): cache-first para
  osito.{html,js,wasm}, network-first con cache fallback para R2 GGUF
  e img. Browsers muestran prompt "install" en address bar tras primera
  carga. Offline tras primera carga.

### Comandos shell completos

```
chat <prompt>                            rag <ctx> ::: <q>
temp <t> [topp]                          penalty <rep> [pres] [freq]
ngram <n>                                bdebug <dwa|vr|reg|logits>
bench [n_tokens]                         time <command...>
claude (REPL)                            apikey sk-ant-...

cc <src.c> [-o out.wasm] [-c]            make
exec <out.wasm>                          edit <file>

git <init|add|commit|log|status|diff|branch|checkout>

ls [path]                                cat <file>
ls /iso/, /fat/, /ext/, /aux/, …         cat /iso/foo, /fat/bar, …
mount-fs <type> <url>                    fs-ls / fs-cat
umount [type]

curl <url>                               ws <open|send|recv|close|list>
tcp <proxy|connect|send|recv|close>      https <host> [path]
crypto sha256/sha512 <text>

info                                     help
```

### Diff vs x86 — qué falta y por qué

48 módulos kernel correctamente excluidos:

- **HW**: idt, paging, smp, smp_work, memory, heap, pci, pci_hotplug,
  nic, hwbp, perf, audio_sched, dma_sched, cpu_features, cpu_topology,
  dispatch, tensor_avx2, vdso_thunks, spec_*, self_optimize,
  pred_sched, io_predict, tensor_arena
- **Boot/diag**: main, serial, keyboard, panic, coredump, crash_report
- **Process model**: dynlink, elf, process, syscall, kmod, sys_inference
- **Power/display**: power, display
- **Real-IP net stack**: net.c, dhcp, ntp, mdns, ipv6, apipa,
  netfilter, http (browser no expone raw IP — bridge WS reemplaza)
- **claude.c** ya bridgeado vía wasm_http_request → JS fetch directo

Para cerrar gaps adicionales se necesita refactor profundo:
- Multi-aux DEL MISMO tipo (e.g. dos ISOs concurrentes) requiere
  context refactor en cada FS driver — todos usan globals.
- Server-side TCP/`accept()` necesita un protocolo over-WebSocket
  custom (httpd/sshd quedarían).
- WebGPU compute matvec no tiene ROI para modelos pequeños (256-dim
  matvec dispatch overhead ≈ scalar tiempo).
