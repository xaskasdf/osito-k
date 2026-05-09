# In-browser C/C++ compilation in OsitoK-wasm

`cc` shell command compila código C/C++ directamente dentro de OsitoK
corriendo en navegador, sin servidor, sin tooling host. Pipeline:

```
osito> echo 'int main(){printf("hi"); return 0;}' > hello.c
osito> cc hello.c
> Untarring sysroot.tar... done.
> Fetching and compiling clang... done.
> clang -cc1 -emit-obj ... -o hello.o ...
> Fetching and compiling lld... done.
> wasm-ld ... -o hello.wasm
> hello.wasm
hi
```

## Componentes

```
shell.html  ──▶ window.__cc.compileLinkRun(src)
                    │
                    ▼
              [Web Worker — Blob-inlined]
                    │
       importScripts('shared.js')   ◀── R2: wasm.naranjositos.tech/toolchain/
                    │
            ┌───────┴────────┐
            ▼                ▼
       clang.wasm       lld.wasm        ◀── R2 (cached after first fetch)
        (30 MB)          (19 MB)
                    │
                    ▼
            sysroot.tar (9 MB) ── extracted into in-memory FS
            (libc, libc++, libc++abi, headers)
                    │
                    ▼
                  test.wasm (output) ── runs immediately via WASI shim;
                                       stdout streams back to terminal
```

Toolchain bundle (basado en [binji/wasm-clang](https://github.com/binji/wasm-clang),
Apache-2.0) hosteado en `wasm.naranjositos.tech/toolchain/`:

| File | Tamaño | Rol |
|---|---|---|
| `clang.wasm` | 30 MB | Frontend C/C++ (clang 8.0.1 con `-cc1 -emit-obj`) |
| `lld.wasm` | 19 MB | `wasm-ld` linker |
| `sysroot.tar` | 9 MB | Headers + libs (`libc`, `libc++`, `libc++abi`, `libm`, `libpthread`, `librt`) |
| `memfs.wasm` | 337 KB | In-memory FS expuesto vía WASI shim |
| `shared.js` | 23 KB | API class: `compile/link/run/compileLinkRun/untar` |
| `worker.js` | 3 KB | (no usado — el worker está inlineado en `shell.html` como Blob) |

## Cómo funciona

### Lazy loading

`window.__cc` no carga nada hasta la primera llamada a `compileLinkRun`.
Al primer compilado:
1. Crea un `Worker` desde un Blob inlineado en `shell.html`.
2. El worker hace `importScripts(R2/shared.js)`.
3. Construye `new API({...})` apuntando a las URLs absolutas R2.
4. La instancia carga `memfs.wasm` y `untar(sysroot.tar)`.
5. Cachea `clang.wasm` y `lld.wasm` (compilados en `WebAssembly.Module`)
   para reusarlos en compilaciones siguientes.

Compilaciones siguientes son rápidas (~50-200ms para hello world).

### Bridge kernel ↔ JS

`hal/stubs.c::cmd_cc` corre en el kernel wasm:
- `osfs2_find` + `osfs2_read` levantan el archivo `.c` del FS.
- `EM_JS(js_cc_kick)` llama `window.__cc.compileLinkRun(src)`.
- Loop `while (!js_cc_done()) { drain stdout; emscripten_sleep(50); }`.
- Output se acumula en `window.__ccPending` (string), drenado en chunks
  vía `js_cc_drain()` y emitido por `serial_puts()` al terminal.

Asyncify permite que el kernel "bloquee" sin congelar la UI — el sleep
yielda al event loop mientras el worker sigue compilando en otro thread.

### Output del compilador

El binario que produce `wasm-ld` es **WASI standalone**, no Emscripten
side module. Diferencias clave:

| Aspecto | Emscripten side module (Quake 2) | WASI standalone (output de cc) |
|---|---|---|
| Magic adicional | sección custom `dylink.0` | sección `linking` (clang stdcall) |
| Entry point | `q2_main` / `app_main` (función explícita) | `_start` (WASI ABI) |
| Cómo se ejecuta | `dlopen` en main thread | Worker JS aparte con shim WASI |

Por eso `compileLinkRun` corre el output dentro del mismo worker que
ya tiene todo el shim listo (la API class de binji ya implementa
WASI). No requiere extender `proc_exec` para el caso `cc -run`.

## Casos de uso

| Comando | Qué hace |
|---|---|
| `cc hello.c` | Compila + linkea + corre inline (estilo `tcc -run`) |
| `cc hello.c -o hello.wasm` | Compila + linkea + guarda en OsitoFS, no ejecuta |
| `cc -E hello.c` | Solo preprocessor (`clang -cc1 -E`). |
| `cc -c hello.c` | Compile-only — produce `<base>.o` (sin `-o` toma basename del source). |
| `cc < hello.c` | Lee source desde redirect (o pipe `echo ... \| cc`). |
| `cc -o foo.wasm << END` ... `END` | Heredoc inline; multi-línea sin file en disco. |
| `exec hello.wasm` | `proc_exec` detecta WASI (no `dylink.0` section) y corre con shim. Output streamea al terminal. |
| `exec quake2.so` | `proc_exec` detecta `dylink.0` → carga via `dlopen` (Emscripten side module). |
| `edit <file>` | Editor multi-línea, terminator `.` en línea propia. Escribe a OsitoFS. |
| `make [target]` | Parsea Makefile (variables, reglas, deps, `$(VAR)`); ejecuta comandos via `shell_exec`. |

## IndexedDB cache

El worker cachea las descargas pesadas en IndexedDB (`osito-cc-v1` / store `toolchain`):

- **Primera visita**: ~50 MB descargados desde R2 + persistidos.
- **Visitas siguientes**: instantáneo. Cada uno emite `[cache] <file> (X MB)` para visibilidad.

Para limpiar el cache (forzar redownload), abrir DevTools → Application → IndexedDB → `osito-cc-v1` → Delete database.

## Flujo de prueba E2E

```
osito> echo '#include <stdio.h>
int main() { printf("hello from clang.wasm!\n"); return 0; }' > hello.c

osito> cc hello.c
> Untarring sysroot.tar... done.
> Fetching and compiling clang... done.
> clang -cc1 -emit-obj -disable-free -isysroot / ...
> Fetching and compiling lld... done.
> wasm-ld --no-threads --export-dynamic ... -o hello.wasm
> Compiling hello.wasm... done.
> hello.wasm
hello from clang.wasm!
```

Página standalone para sanity (sin kernel): `arch/wasm/build/cc-test.html`
(gitignored). Útil para debugging de WASI shim sin pasar por shell.

## Sysroot

Headers C/C++ disponibles bajo `/include/` y `/include/c++/v1/`:
- `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<errno.h>`, `<time.h>`
- `<vector>`, `<string>`, `<iostream>`, `<algorithm>`, `<map>`, `<unordered_map>`
- `<chrono>`, `<thread>` (limitado, no hay threads reales en wasm32)
- `<canvas.h>` — binji's custom Canvas binding (linkear con `-lcanvas`)

Libs en `/lib/wasm32-wasi/`:
- `libc.a`, `libc++.a`, `libc++abi.a`, `libm.a`, `libpthread.a`
- `librt.a`, `libdl.a`, `libcrypt.a`, `libresolv.a`, `libutil.a`
- `libcanvas.a` (Canvas binding)
- `libwasi-emulated-mman.a` (mmap emulation)
- `libclang_rt.builtins-wasm32.a` (compiler-rt)

## Limitaciones conocidas

- Clang 8.0.1 — soporta C17 y C++17 mostly, pero algunas features
  modernas (concepts, modules, std::format) no están.
- Sin threading real (no `pthread_create` funcional, `<thread>` linkea
  pero hace operaciones inválidas).
- No hay TCP/UDP/file I/O fuera de la MemFS — `fopen("hello.txt")` 
  abre desde MemFS, no desde OsitoFS.
- Output WASI necesita worker aparte para correr — `compileLinkRun`
  ya lo hace; `cc -o file.wasm + exec file.wasm` requiere extender
  `proc_exec` (TODO).
- Primera compilación: ~5-10 seg (download de ~50MB). Browser cache
  acelera todas las siguientes; no usamos IndexedDB todavía (TODO).

## Roadmap

- [x] ~~**`cc -o file.wasm`** + `exec file.wasm` con branch WASI en `proc_exec`~~ ✅ done
- [x] ~~**IndexedDB cache** para clang/lld/sysroot~~ ✅ done
- [x] ~~**`#include <ositok.h>`**~~ ✅ done — convenience header (oi_puts/oi_print/oi_log/OSITOK_VERSION) inyectado en MemFS al levantar el worker. Kernel-bridge imports (oi_chat, oi_dlopen) pendientes.
- [x] ~~**`make`** mínimo~~ ✅ done — `cmd_make` parsea variables `VAR = val`, reglas `target: deps`, comandos (cualquier indentación), `$(VAR)` expansion, ejecuta vía `shell_exec`.
- [x] ~~**`cc -E`** preprocessing only~~ ✅ done — patched shared.js's `preprocess()` que invoca `clang -cc1 -E`.
- [x] ~~**`<` redirect input**~~ ✅ done — wired en `parse_redirects` para cargar archivo en `sh_stdin_buf`. `cc < hello.c` o `cmd | cc` funciona.
- [x] ~~**`edit <file>`** multi-línea~~ ✅ done — terminator `.` en línea propia.
- [x] ~~**Kernel-bridge imports**~~ ✅ done — `osito_env` import module with `oi_random_u32`, `oi_now_us`, `oi_log_kernel`. App constructor adds it to the WebAssembly imports table; ositok.h declares them with `__attribute__((import_module/import_name))`. Compiled programs link via `wasm-ld --allow-undefined`; runtime resolves them in the worker's App instance.
- [x] ~~**Heredoc `<< EOF`**~~ ✅ done — shell main loop detects `<<TERM` in the command line, accumulates lines until terminator, feeds via `sh_stdin_buf`. Body cap 64KB.
- [x] ~~**`cc -c`** compile-only~~ ✅ done — produces `.o` (auto-named `<basename>.o` if no `-o`).
- [x] ~~**Real `oi_chat`**~~ done — poll-based bridge. Worker writes
      prompt to `SharedArrayBuffer`, sets state=1, `Atomics.wait`s. The
      kernel's `osito_kernel_poll()` runs from inside `kb_getchar` and
      `cc_drain_until_done` Asyncify-aware loops, reads the prompt via
      EM_JS, runs the shell `chat` command capturing output via the
      shell redirect, writes back to SAB, sets state=2, `Atomics.notify`.
      Worker wakes, copies result into user wasm memory, returns bytes
      written. The earlier `Module.ccall` path is left as a stub since
      it hits an Asyncify edge case under `MAIN_MODULE=1` (function-table
      indirect call to the llama callback fails when wasm is entered
      from a non-Asyncify JS frame). Routing through the kernel's
      ongoing execution avoids that entirely.
- [x] ~~**`cat`/`head`/`tail` reading from stdin**~~ ✅ done — `cat` reads
      from `sh_stdin_buf` when no filename arg; `head`/`tail`/`grep`
      already supported it.
- [ ] **Sandboxed `chat` REPL inside user wasm** — once oi_chat works,
      compose into a multi-turn loop.

## Servidor con cross-origin isolation

`oi_chat` requiere `SharedArrayBuffer` para el sync bridge entre worker
(donde corre el user wasm) y main thread (donde corre la inferencia
LLM). Eso requiere COOP+COEP headers que `python -m http.server` no
manda.

Use el wrapper en `arch/wasm/serve.py` (default puerto 8000):

```bash
cd arch/wasm/build
python3 ../serve.py [port]
```

Manda:
- `Cross-Origin-Opener-Policy: same-origin`
- `Cross-Origin-Embedder-Policy: credentialless`

`credentialless` permite fetches cross-origin sin cookies sin requerir
`Cross-Origin-Resource-Policy` en cada respuesta — útil para los assets
de R2.

Verificar en consola: `crossOriginIsolated === true` y
`typeof SharedArrayBuffer !== 'undefined'`.

## Configuración / referencias

- Toolchain bundle: <https://wasm.naranjositos.tech/toolchain/>
- Origen: <https://github.com/binji/wasm-clang>
- License: Apache 2.0 (clang/lld), MIT (wasi-libc), Apache 2.0 (libc++)
- CORS R2 rule (factory bucket): origins `https://*.naranjositos.tech`,
  `http://localhost:8000`, etc.
