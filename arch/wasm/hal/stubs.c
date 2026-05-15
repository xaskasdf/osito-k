/*
 * OsitoK WASM — Hardware stubs
 *
 * No-op / safe-default implementations of all hardware subsystems
 * not applicable in a browser context.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <emscripten.h>

/* ── Override printf/fprintf for dlopen compatibility ─────────── */
/*
 * Emscripten libc's vfprintf uses internal function pointers for FILE
 * stream I/O that crash with "function signature mismatch" when called
 * from dlopen'd side modules. Override printf/fprintf/fputs/puts in the
 * kernel (MAIN_MODULE) to use serial_puts directly — this bypasses the
 * broken vfprintf entirely. vsnprintf is safe (no function pointers).
 *
 * This override applies to ALL code (kernel + side modules) since the
 * MAIN_MODULE's symbols take precedence.
 */
extern void serial_puts(const char *s);

/* Safe printf that bypasses vfprintf — used by side modules */
void safe_printf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(buf);
}

/* ── Tick counter (replaces APIC timer in idt.c) ─────────────── */

static volatile uint64_t wasm_ticks = 0;

void idt_init(void) {}
/* Use performance.now() → 100 Hz synthetic ticks (1 tick = 10ms) */
uint64_t idt_get_ticks(void) {
    double ms = EM_ASM_DOUBLE({ return performance.now(); });
    wasm_ticks = (uint64_t)(ms / 10.0);
    return wasm_ticks;
}
void wasm_tick_advance(void) { wasm_ticks++; }

/* ── Paging ──────────────────────────────────────────────────── */

void paging_init(void) {}
uint64_t paging_get_kernel_cr3(void) { return 0; }
int  paging_map_mmio(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
void paging_setup_pat(void) {}
int  paging_map_wc(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }

/* ── SMP ─────────────────────────────────────────────────────── */

void smp_init(void) {}

/* ── Syscall ─────────────────────────────────────────────────── */

void syscall_init(void) {}

/* ── Process subsystem ───────────────────────────────────────── */

void proc_init(void) {}
void proc_list(void) { extern void serial_puts(const char *); serial_puts("  (no processes in WASM mode)\n"); }

/* ── Helper: extract file from OsitoFS to Emscripten MEMFS ───── */

static void extract_to_memfs(const char *osfs_name, const char *memfs_path)
{
    extern void serial_puts(const char *);
    extern void serial_putdec(uint64_t);
    extern void *osfs2_find(const char *);
    extern int   osfs2_read(void *, uint64_t, void *, uint64_t);
    extern uint64_t osfs2_file_size(void *);

    void *file = osfs2_find(osfs_name);
    if (!file) return;

    uint64_t size = osfs2_file_size(file);
    serial_puts("[WASM] Extracting ");
    serial_puts(osfs_name);
    serial_puts(" (");
    serial_putdec(size / (1024 * 1024));
    serial_puts(" MB)...\n");

    void *buf = malloc((size_t)size);
    if (!buf) { serial_puts("[WASM] malloc failed\n"); return; }
    osfs2_read(file, 0, buf, size);

    EM_ASM({
        try {
            /* Create parent directories */
            var path = UTF8ToString($2);
            var parts = path.split('/').filter(function(p){ return p; });
            var dir = '';
            for (var i = 0; i < parts.length - 1; i++) {
                dir += '/' + parts[i];
                try { FS.mkdir(dir); } catch(e) {}
            }
            FS.writeFile(path, HEAPU8.subarray($0, $0 + $1));
        } catch(e) { console.error('[WASM] extract failed:', e); }
    }, buf, (uint32_t)size, memfs_path);
    free(buf);
}

/* Forward decls for helpers used by proc_exec; bodies are further down. */
static void cc_drain_until_done(void);
extern void js_cc_run_wasi(const uint8_t *src, int size, const char *name,
                            const char *argv_joined,
                            const uint8_t *stdin_buf, int stdin_len);

/* ── proc_exec: load and run WASM side modules via dlopen ────── */

int proc_exec(const char *filename, int argc, const char **argv)
{
    extern void serial_puts(const char *);
    extern void *osfs2_find(const char *);
    extern uint64_t osfs2_file_size(void *);
    extern int osfs2_read(void *, uint64_t, void *, uint64_t);

    if (!filename) { serial_puts("[EXEC] No filename\n"); return -1; }

    /* Find executable in OsitoFS */
    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[EXEC] Not found: ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    uint64_t size = osfs2_file_size(file);
    serial_puts("[EXEC] Loading ");
    serial_puts(filename);
    serial_puts("...\n");

    if (size < 16) {
        serial_puts("[EXEC] File too small to be an executable\n");
        return -1;
    }

    /* Extract .so from OsitoFS to a heap buffer for arch validation */
    void *buf = malloc((size_t)size);
    if (!buf) { serial_puts("[EXEC] malloc failed\n"); return -1; }
    osfs2_read(file, 0, buf, size);

    /* ── Architecture validation ────────────────────────────────────
     * Emscripten's dlopen accepts WebAssembly side modules only
     * (magic `\x00asm`, version 0x01). Native x86-64 / aarch64 ELF
     * binaries cannot be loaded — and emcc's loader throws inside an
     * unhandled promise that suspends Asyncify forever, hanging the
     * shell. Reject upfront with a clear message. */
    const uint8_t *hdr = (const uint8_t *)buf;
    bool is_wasm  = (hdr[0]==0x00 && hdr[1]=='a' && hdr[2]=='s' && hdr[3]=='m');
    bool is_elf   = (hdr[0]==0x7F && hdr[1]=='E' && hdr[2]=='L' && hdr[3]=='F');

    if (!is_wasm) {
        serial_puts("[EXEC] Not a wasm side module: ");
        if (is_elf) {
            uint16_t e_machine = (uint16_t)hdr[18] | ((uint16_t)hdr[19] << 8);
            const char *arch = "unknown";
            switch (e_machine) {
                case 0x003E: arch = "x86-64"; break;
                case 0x0003: arch = "i386";   break;
                case 0x00B7: arch = "aarch64";break;
                case 0x0028: arch = "ARM";    break;
                case 0x00F3: arch = "RISC-V"; break;
            }
            serial_puts("ELF (");
            serial_puts(arch);
            serial_puts(") — wasm runtime cannot dlopen native binaries.\n");
            serial_puts("[EXEC] Tip: rebuild with 'emcc -sSIDE_MODULE=2'.\n");
        } else {
            serial_puts("unknown magic ");
            const char hex[] = "0123456789ABCDEF";
            char m[12] = "0x00 0x00 0x00 0x00";
            for (int i = 0; i < 4; i++) {
                m[2 + i*5]     = hex[(hdr[i] >> 4) & 0xF];
                m[2 + i*5 + 1] = hex[hdr[i] & 0xF];
            }
            serial_puts(m);
            serial_puts("\n");
        }
        free(buf);
        return -1;
    }

    /* Detect Emscripten side module (has `dylink.0` custom section) vs
     * WASI standalone module. We just scan the first ~256 bytes for the
     * literal "dylink" — fast and unique enough since custom section
     * names appear early in the wasm file layout. */
    bool is_dylink = false;
    {
        size_t scan = size < 256 ? (size_t)size : 256;
        for (size_t i = 8; i + 6 < scan; i++) {
            if (hdr[i]=='d' && hdr[i+1]=='y' && hdr[i+2]=='l' &&
                hdr[i+3]=='i' && hdr[i+4]=='n' && hdr[i+5]=='k') {
                is_dylink = true;
                break;
            }
        }
    }

    if (!is_dylink) {
        /* WASI standalone — route to clang.wasm worker's WASI runtime
         * (App class with the same wasi_unstable shim that runs `cc`'s
         * compileLinkRun outputs). Streams stdout via __ccPending. */
        serial_puts("[EXEC] WASI module ");
        serial_puts(filename);
        serial_puts(" via clang.wasm runtime\n");
        /* Pack argv[1..argc-1] into a NUL-separated string for the worker. */
        char joined[512]; joined[0] = 0; int jp = 0;
        for (int i = 1; i < argc && jp < (int)sizeof(joined) - 32; i++) {
            int al = strlen(argv[i]);
            if (al > 100) al = 100;
            memcpy(joined + jp, argv[i], al);
            joined[jp + al] = '\x1f';   /* unit separator */
            jp += al + 1;
        }
        if (jp > 0) joined[jp - 1] = 0; else joined[0] = 0;
        /* Forward shell pipe / `<file` stdin to the WASI shim. */
        extern const char *sh_stdin_buf;
        extern uint32_t sh_stdin_len;
        const uint8_t *sin = (const uint8_t *)sh_stdin_buf;
        int sin_len = sh_stdin_buf ? (int)sh_stdin_len : 0;
        js_cc_run_wasi((const uint8_t *)buf, (int)size, filename, joined,
                       sin, sin_len);
        free(buf);
        cc_drain_until_done();
        serial_puts("\n");
        return 0;
    }

    /* Emscripten side module — dlopen path */
    EM_ASM({
        try { FS.mkdir('/tmp'); } catch(e) {}
        FS.writeFile('/tmp/app.so', HEAPU8.subarray($0, $0 + $1));
    }, buf, (uint32_t)size);
    free(buf);

    /* Extract game data if this is Q2 */
    if (strstr(filename, "quake2")) {
        extract_to_memfs("baseq2/pak0.pak", "/baseq2/pak0.pak");
    }

    /* dlopen: load the side module */
    void *handle = dlopen("/tmp/app.so", RTLD_NOW);
    if (!handle) {
        serial_puts("[EXEC] dlopen failed: ");
        const char *err = dlerror();
        serial_puts(err ? err : "unknown error");
        serial_puts("\n");
        return -1;
    }

    /* Try known entry points */
    typedef int (*entry_fn)(int, char **);
    entry_fn entry = NULL;

    /* Q2 entry */
    entry = (entry_fn)dlsym(handle, "q2_main");
    /* Generic entry */
    if (!entry) entry = (entry_fn)dlsym(handle, "app_main");

    if (!entry) {
        serial_puts("[EXEC] No entry point found (tried q2_main, app_main)\n");
        dlclose(handle);
        return -1;
    }

    serial_puts("[EXEC] Running entry...\n");
    int ret = entry(argc, (char **)argv);
    serial_puts("[EXEC] entry returned\n");

    /* Check if the app exported a frame function for rAF scheduling.
     * Side modules can't use emscripten_set_main_loop — the kernel
     * must schedule their frame callback via JS requestAnimationFrame. */
    typedef void (*frame_fn)(void);
    frame_fn frame = (frame_fn)dlsym(handle, "q2_frame");
    if (frame) {
        serial_puts("[EXEC] Setting up frame loop + input via rAF\n");

        /* Get input function pointers from side module */
        void *pushkey = dlsym(handle, "q2_push_key");
        void *pushmouse = dlsym(handle, "q2_push_mouse");

        /* Set up frame loop + keyboard/mouse input in JS */
        EM_ASM({
            var framePtr = $0;
            var keyPtr = $1;
            var mousePtr = $2;

            /* Frame loop */
            var __frameErrors = 0;
            function __appFrame() {
                try { dynCall('v', framePtr); } catch(e) {
                    __frameErrors++;
                    if (__frameErrors <= 3) console.error('[EXEC] frame error #' + __frameErrors + ':', e.stack || e);
                }
                requestAnimationFrame(__appFrame);
            }
            requestAnimationFrame(__appFrame);

            /* Key mapping (replicates Q2's js_key_to_q2) */
            function keyToQ2(e) {
                var k = e.key;
                if (k.length === 1) {
                    var c = k.charCodeAt(0);
                    if (c >= 65 && c <= 90) return c + 32;
                    if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) return c;
                    if (c === 32) return 32;
                    var punc = "-=[]\\;',./`~";
                    if (punc.indexOf(k) >= 0) return (k === '~') ? 96 : c;
                    return 0;
                }
                if (k==="Enter") return 13;
                if (k==="Escape") return 27;
                if (k==="Backspace") return 127;
                if (k==="Tab") return 9;
                if (k==="ArrowUp") return 128;
                if (k==="ArrowDown") return 129;
                if (k==="ArrowLeft") return 130;
                if (k==="ArrowRight") return 131;
                if (k==="Alt") return 132;
                if (k==="Control") return 133;
                if (k==="Shift") return 134;
                if (k.length===2 && k[0]==="F") return 159+parseInt(k[1]);
                if (k.length===3 && k[0]==="F") return 159+parseInt(k.substring(1));
                if (k==="Insert") return 141;
                if (k==="Delete") return 148;
                if (k==="Home") return 143;
                if (k==="End") return 145;
                if (k==="PageUp") return 147;
                if (k==="PageDown") return 149;
                return 0;
            }

            /* Keyboard */
            if (keyPtr) {
                var held = {};
                document.addEventListener('keydown', function(e) {
                    if (e.repeat) { e.preventDefault(); return; }
                    var k = keyToQ2(e);
                    if (k > 0 && !held[k]) {
                        held[k] = true;
                        dynCall('vii', keyPtr, [k, 1]);
                    }
                    e.preventDefault();
                });
                document.addEventListener('keyup', function(e) {
                    var k = keyToQ2(e);
                    if (k > 0) {
                        held[k] = false;
                        dynCall('vii', keyPtr, [k, 0]);
                    }
                    e.preventDefault();
                });
            }

            /* Mouse: pointer lock + movement + clicks */
            var c = document.getElementById('q2-canvas');
            if (c && mousePtr) {
                c.addEventListener('click', function() { c.requestPointerLock(); });
                document.addEventListener('mousemove', function(e) {
                    if (document.pointerLockElement === c)
                        dynCall('vii', mousePtr, [e.movementX, e.movementY]);
                });
            }
            if (c && keyPtr) {
                document.addEventListener('mousedown', function(e) {
                    if (document.pointerLockElement === c)
                        dynCall('vii', keyPtr, [200 + e.button, 1]);
                });
                document.addEventListener('mouseup', function(e) {
                    if (document.pointerLockElement === c)
                        dynCall('vii', keyPtr, [200 + e.button, 0]);
                });
            }
        }, frame, pushkey, pushmouse);

        /* Don't dlclose — the module must stay loaded while frames run */
        return ret;
    }

    dlclose(handle);
    serial_puts("[EXEC] Exited with code ");
    extern void serial_putdec(uint64_t);
    serial_putdec((uint64_t)ret);
    serial_puts("\n");
    return ret;
}

/* ── Dynamic linker ──────────────────────────────────────────── */

void dl_init(void) {}

/* ── Win32 compat ────────────────────────────────────────────── */

void win32_init(void) {}

/* ── PCI ─────────────────────────────────────────────────────── */

void pci_scan(void) {}
void *pci_get_gpu(void)          { return NULL; }
void *pci_get_nvme(void)         { return NULL; }
int   pci_get_nvme_count(void)   { return 0; }
void *pci_get_nvme_idx(int i)    { (void)i; return NULL; }
void *pci_get_nic(void)          { return NULL; }
void *pci_get_xhci(void)         { return NULL; }
void *pci_get_hda(void)          { return NULL; }
int   pci_get_device_count(void) { return 0; }

/* ── NVMe ────────────────────────────────────────────────────── */

int  nvme_init(uint64_t bar0) { (void)bar0; return -1; }
bool nvme_is_ready(void)      { return false; }

/* ── NVMe memory backend for OsitoFS ─────────────────────────── */
/* OsitoFS calls nvme_read_bytes/nvme_write_bytes for all I/O.
 * On WASM, the .img file is fetched from R2 into this buffer. */
static uint8_t *wasm_nvme_buf = NULL;
static uint64_t wasm_nvme_size = 0;

void wasm_nvme_set_buffer(void *buf, uint64_t size) {
    wasm_nvme_buf = (uint8_t *)buf;
    wasm_nvme_size = size;
}

int nvme_read_bytes(uint64_t offset, void *buf, uint64_t len) {
    if (!wasm_nvme_buf || offset + len > wasm_nvme_size) return -1;
    memcpy(buf, wasm_nvme_buf + offset, (size_t)len);
    return 0;
}

/* Persistence — every write marks the image dirty; a coalesced flush
 * pushes the whole buffer to IndexedDB. We rely on the kernel's
 * cooperative scheduler + Asyncify yields to call wasm_persist_flush
 * periodically (kicked from the shell main loop after each command). */
static bool g_nvme_dirty = false;

EM_JS(void, js_persist_save, (const uint8_t *buf, int len), {
    var u8 = HEAPU8.slice(buf, buf + len).slice();
    var req = indexedDB.open('osito-fs', 1);
    req.onupgradeneeded = function() {
        req.result.createObjectStore('img');
    };
    req.onsuccess = function() {
        var db = req.result;
        var tx = db.transaction('img', 'readwrite');
        tx.objectStore('img').put(u8, 'main');
        tx.oncomplete = function() { db.close(); };
        tx.onerror = function() { db.close(); };
    };
});

EM_JS(int, js_persist_load_kick, (), {
    window.__persistDone = false;
    window.__persistBuf = null;
    var req = indexedDB.open('osito-fs', 1);
    req.onupgradeneeded = function() {
        req.result.createObjectStore('img');
    };
    req.onsuccess = function() {
        var db = req.result;
        var tx = db.transaction('img');
        var g = tx.objectStore('img').get('main');
        g.onsuccess = function() {
            window.__persistBuf = g.result || null;
            window.__persistDone = true;
            db.close();
        };
        g.onerror = function() { window.__persistDone = true; db.close(); };
    };
    req.onerror = function() { window.__persistDone = true; };
    return 0;
});

EM_JS(int, js_persist_done, (), { return window.__persistDone ? 1 : 0; });
EM_JS(int, js_persist_size, (), {
    return window.__persistBuf ? window.__persistBuf.byteLength : 0;
});
EM_JS(void, js_persist_copy, (uint8_t *dst, int max), {
    var src = window.__persistBuf;
    if (!src) return;
    var n = src.byteLength < max ? src.byteLength : max;
    HEAPU8.set(src.subarray(0, n), dst);
});

/* Public API: try to load the persisted image into wasm_nvme_buf.
 * Returns the size loaded, or 0 if no snapshot or buffer too small. */
int wasm_persist_load(void)
{
    js_persist_load_kick();
    while (!js_persist_done()) emscripten_sleep(20);
    int sz = js_persist_size();
    if (sz <= 0 || !wasm_nvme_buf) return 0;
    if ((uint64_t)sz > wasm_nvme_size) return 0;
    js_persist_copy(wasm_nvme_buf, sz);
    serial_puts("[persist] restored ");
    serial_putdec((uint64_t)sz);
    serial_puts(" bytes from IndexedDB\n");
    return sz;
}

/* Push the dirty image back to IndexedDB. Async (no Asyncify wait —
 * we don't need to block on writes). */
void wasm_persist_flush(void)
{
    if (!g_nvme_dirty || !wasm_nvme_buf) return;
    js_persist_save(wasm_nvme_buf, (int)wasm_nvme_size);
    g_nvme_dirty = false;
}

/* ── localStorage-backed config persistence ──────────────────────
 * Sampling tunables (temp, penalty, ngram, proxy URL) are tiny so
 * we keep them in localStorage as a single JSON blob. Read at boot,
 * written whenever a `temp/penalty/ngram/tcp proxy` command runs.
 * ──────────────────────────────────────────────────────────────── */

EM_JS(void, js_config_save, (const char *key, const char *value), {
    try {
        var k = UTF8ToString(key);
        var v = UTF8ToString(value);
        localStorage.setItem('osito-cfg-' + k, v);
    } catch (e) {}
});

EM_JS(int, js_config_load, (const char *key, char *dst, int max), {
    try {
        var k = UTF8ToString(key);
        var v = localStorage.getItem('osito-cfg-' + k) || '';
        var bytes = lengthBytesUTF8(v) + 1;
        if (bytes > max) bytes = max;
        stringToUTF8(v, dst, bytes);
        return v.length;
    } catch (e) { return -1; }
});

void wasm_config_save(const char *key, const char *value)
{ js_config_save(key, value); }

int wasm_config_load(const char *key, char *dst, int max)
{ return js_config_load(key, dst, max); }

/* Raw localStorage set — for keys that other JS code reads directly
 * (e.g. 'osito-model' is read by shell.html before WASM loads). */
EM_JS(void, js_localstorage_set_raw, (const char *key, const char *value), {
    try {
        var k = UTF8ToString(key);
        var v = UTF8ToString(value);
        if (v) localStorage.setItem(k, v);
        else   localStorage.removeItem(k);
    } catch (e) {}
});

void wasm_localstorage_set(const char *key, const char *value)
{ js_localstorage_set_raw(key, value); }

EM_JS(void, js_reload_page, (), {
    try { location.reload(); } catch (e) {}
});

void wasm_reload_page(void) { js_reload_page(); }

/* Format current wall-clock as ISO 8601 UTC. JS owns the formatting
 * so DST/locale bugs aren't ours. Returns bytes written. */
EM_JS(int, js_iso_now, (char *dst, int max), {
    var s = new Date().toISOString();
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return bytes - 1;
});

int wasm_iso_now(char *dst, int max) { return js_iso_now(dst, max); }

/* Background-fetch the cc toolchain into the browser HTTP cache so
 * the first `cc` invocation doesn't wait 30s for clang+lld+sysroot. */
EM_JS(void, js_precache_toolchain, (), {
    var T = 'https://wasm.naranjositos.tech/toolchain';
    var assets = [T + '/clang.wasm', T + '/lld.wasm', T + '/sysroot.tar', T + '/memfs.wasm'];
    var done = 0;
    assets.forEach(function(u) {
        fetch(u).then(function(r) {
            if (r.ok) { done++; }
        }).catch(function() {});
    });
});

void wasm_precache_toolchain(void) { js_precache_toolchain(); }

/* ══════════════════════════════════════════════════════════════
 *  WebGPU compute matvec — F32 weights, dispatch via EM_JS.
 *  Falls back to scalar when navigator.gpu is missing or init fails.
 * ══════════════════════════════════════════════════════════════ */

EM_JS(int, js_wgpu_init_kick, (), {
    window.__gpuReady = false;
    window.__gpuPending = true;
    window.__gpuError = '';
    if (!navigator.gpu) {
        window.__gpuError = 'navigator.gpu missing (WebGPU unsupported)';
        window.__gpuPending = false;
        return 0;
    }
    (async function() {
        try {
            const adapter = await navigator.gpu.requestAdapter();
            if (!adapter) throw new Error('no adapter');
            const device = await adapter.requestDevice();
            const code = `
                struct Dims { rows: u32, cols: u32 };
                @group(0) @binding(0) var<storage, read> weights: array<f32>;
                @group(0) @binding(1) var<storage, read> inp: array<f32>;
                @group(0) @binding(2) var<storage, read_write> outp: array<f32>;
                @group(0) @binding(3) var<uniform> dims: Dims;
                @compute @workgroup_size(64)
                fn matvec(@builtin(global_invocation_id) gid: vec3<u32>) {
                    let row = gid.x;
                    if (row >= dims.rows) { return; }
                    var sum: f32 = 0.0;
                    let base = row * dims.cols;
                    for (var c: u32 = 0u; c < dims.cols; c = c + 1u) {
                        sum = sum + weights[base + c] * inp[c];
                    }
                    outp[row] = sum;
                }
            `;
            /* RMS norm: y[i] = (x[i] / rms(x)) * weight[i] where
             * rms(x) = sqrt(mean(x^2) + eps). One workgroup of 64
             * threads cooperates to compute the mean via shared memory
             * reduction, then writes the output in parallel. */
            const rmsCode = `
                struct NormDims { dim: u32, eps_bits: u32 };
                @group(0) @binding(0) var<storage, read> weight: array<f32>;
                @group(0) @binding(1) var<storage, read> inp: array<f32>;
                @group(0) @binding(2) var<storage, read_write> outp: array<f32>;
                @group(0) @binding(3) var<uniform> nd: NormDims;
                var<workgroup> sh: array<f32, 64>;
                @compute @workgroup_size(64)
                fn rms_norm(@builtin(local_invocation_id) lid: vec3<u32>) {
                    let tid = lid.x;
                    let dim = nd.dim;
                    var partial: f32 = 0.0;
                    var i: u32 = tid;
                    loop {
                        if (i >= dim) { break; }
                        partial = partial + inp[i] * inp[i];
                        i = i + 64u;
                    }
                    sh[tid] = partial;
                    workgroupBarrier();
                    if (tid == 0u) {
                        var s: f32 = 0.0;
                        for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                            s = s + sh[k];
                        }
                        let eps = bitcast<f32>(nd.eps_bits);
                        sh[0] = inverseSqrt(s / f32(dim) + eps);
                    }
                    workgroupBarrier();
                    let scale = sh[0];
                    i = tid;
                    loop {
                        if (i >= dim) { break; }
                        outp[i] = inp[i] * scale * weight[i];
                        i = i + 64u;
                    }
                }
            `;
            /* Numerically-stable softmax: subtract max, exp, normalize.
             * Same single-workgroup pattern. */
            const softCode = `
                struct SDims { len: u32 };
                @group(0) @binding(0) var<storage, read> inp: array<f32>;
                @group(0) @binding(1) var<storage, read_write> outp: array<f32>;
                @group(0) @binding(2) var<uniform> sd: SDims;
                var<workgroup> sh: array<f32, 64>;
                @compute @workgroup_size(64)
                fn softmax(@builtin(local_invocation_id) lid: vec3<u32>) {
                    let tid = lid.x;
                    let len = sd.len;
                    var lmax: f32 = -3.402823e38;
                    var i: u32 = tid;
                    loop { if (i >= len) { break; }
                        lmax = max(lmax, inp[i]); i = i + 64u; }
                    sh[tid] = lmax;
                    workgroupBarrier();
                    if (tid == 0u) {
                        var m: f32 = -3.402823e38;
                        for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                            m = max(m, sh[k]);
                        }
                        sh[0] = m;
                    }
                    workgroupBarrier();
                    let gmax = sh[0];
                    var lsum: f32 = 0.0;
                    i = tid;
                    loop { if (i >= len) { break; }
                        let e = exp(inp[i] - gmax);
                        outp[i] = e; lsum = lsum + e;
                        i = i + 64u; }
                    sh[tid] = lsum;
                    workgroupBarrier();
                    if (tid == 0u) {
                        var s: f32 = 0.0;
                        for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                            s = s + sh[k];
                        }
                        sh[0] = s;
                    }
                    workgroupBarrier();
                    let inv = 1.0 / sh[0];
                    i = tid;
                    loop { if (i >= len) { break; }
                        outp[i] = outp[i] * inv; i = i + 64u; }
                }
            `;
            const matvecMod = device.createShaderModule({ code });
            const rmsMod = device.createShaderModule({ code: rmsCode });
            const softMod = device.createShaderModule({ code: softCode });
            window.__gpuDevice = device;
            window.__gpuPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: matvecMod, entryPoint: 'matvec' },
            });
            window.__gpuRmsPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: rmsMod, entryPoint: 'rms_norm' },
            });
            window.__gpuSoftPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: softMod, entryPoint: 'softmax' },
            });
            /* Fused QKV: compute Q (dim rows) + K (kv_dim rows) + V
             * (kv_dim rows) from the same input vector in one dispatch.
             * One global thread per output row, dispatched as ceil(total/64). */
            const qkvCode = `
                struct QKVDims { dim: u32, q_rows: u32, kv_rows: u32, _pad: u32 };
                @group(0) @binding(0) var<storage, read> wq: array<f32>;
                @group(0) @binding(1) var<storage, read> wk: array<f32>;
                @group(0) @binding(2) var<storage, read> wv: array<f32>;
                @group(0) @binding(3) var<storage, read> inp: array<f32>;
                @group(0) @binding(4) var<storage, read_write> outq: array<f32>;
                @group(0) @binding(5) var<storage, read_write> outk: array<f32>;
                @group(0) @binding(6) var<storage, read_write> outv: array<f32>;
                @group(0) @binding(7) var<uniform> qd: QKVDims;
                @compute @workgroup_size(64)
                fn qkv(@builtin(global_invocation_id) gid: vec3<u32>) {
                    let idx = gid.x;
                    let total = qd.q_rows + 2u * qd.kv_rows;
                    if (idx >= total) { return; }
                    var sum: f32 = 0.0;
                    if (idx < qd.q_rows) {
                        let row = idx;
                        let base = row * qd.dim;
                        for (var c: u32 = 0u; c < qd.dim; c = c + 1u) {
                            sum = sum + wq[base + c] * inp[c];
                        }
                        outq[row] = sum;
                    } else if (idx < qd.q_rows + qd.kv_rows) {
                        let row = idx - qd.q_rows;
                        let base = row * qd.dim;
                        for (var c: u32 = 0u; c < qd.dim; c = c + 1u) {
                            sum = sum + wk[base + c] * inp[c];
                        }
                        outk[row] = sum;
                    } else {
                        let row = idx - qd.q_rows - qd.kv_rows;
                        let base = row * qd.dim;
                        for (var c: u32 = 0u; c < qd.dim; c = c + 1u) {
                            sum = sum + wv[base + c] * inp[c];
                        }
                        outv[row] = sum;
                    }
                }
            `;
            const qkvMod = device.createShaderModule({ code: qkvCode });
            window.__gpuQkvPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: qkvMod, entryPoint: 'qkv' },
            });
            /* Fused attention block: QKV proj + RoPE + KV-cache write +
             * scaled-dot-product attention + output projection, all in
             * one dispatch. Shared workgroup memory holds Q scratch,
             * attention scores, and attn_out. Sized for brandon-tiny
             * (dim<=256, max_seq<=512). One mapAsync per layer per
             * token instead of 5+. */
            const attnCode = `
                const MAX_DIM: u32 = 256u;
                const MAX_SEQ: u32 = 512u;
                struct AttnDims {
                    dim: u32, kv_dim: u32,
                    head_dim: u32, n_heads: u32,
                    n_kv_heads: u32, gqa_ratio: u32,
                    pos: u32, max_seq: u32,
                    scale_bits: u32, rope_base_bits: u32,
                    _pad0: u32, _pad1: u32,
                };
                @group(0) @binding(0) var<storage, read> wq: array<f32>;
                @group(0) @binding(1) var<storage, read> wk: array<f32>;
                @group(0) @binding(2) var<storage, read> wv: array<f32>;
                @group(0) @binding(3) var<storage, read> wo: array<f32>;
                @group(0) @binding(4) var<storage, read> inp: array<f32>;
                @group(0) @binding(5) var<storage, read_write> kv_k: array<f32>;
                @group(0) @binding(6) var<storage, read_write> kv_v: array<f32>;
                @group(0) @binding(7) var<storage, read_write> outp: array<f32>;
                @group(0) @binding(8) var<uniform> ad: AttnDims;

                var<workgroup> q_scratch: array<f32, MAX_DIM>;
                var<workgroup> att: array<f32, MAX_SEQ>;
                var<workgroup> attn_out: array<f32, MAX_DIM>;
                var<workgroup> partial: array<f32, 64>;

                @compute @workgroup_size(64)
                fn fused_attn(@builtin(local_invocation_id) lid: vec3<u32>) {
                    let tid = lid.x;
                    let dim = ad.dim;
                    let kv_dim = ad.kv_dim;
                    let head_dim = ad.head_dim;
                    let n_heads = ad.n_heads;
                    let n_kv_heads = ad.n_kv_heads;
                    let gqa_ratio = ad.gqa_ratio;
                    let pos = ad.pos;
                    let scale = bitcast<f32>(ad.scale_bits);
                    let rope_base = bitcast<f32>(ad.rope_base_bits);

                    /* Stage 1: QKV projections. Each thread handles
                     * rows striped by 64 across the combined Q|K|V
                     * row space. K and V rows go straight into the
                     * persistent KV cache at row[pos]. */
                    var i: u32 = tid;
                    let total = dim + 2u * kv_dim;
                    loop {
                        if (i >= total) { break; }
                        var sum: f32 = 0.0;
                        if (i < dim) {
                            let row = i;
                            let base = row * dim;
                            for (var c: u32 = 0u; c < dim; c = c + 1u) {
                                sum = sum + wq[base + c] * inp[c];
                            }
                            q_scratch[row] = sum;
                        } else if (i < dim + kv_dim) {
                            let row = i - dim;
                            let base = row * dim;
                            for (var c: u32 = 0u; c < dim; c = c + 1u) {
                                sum = sum + wk[base + c] * inp[c];
                            }
                            kv_k[pos * kv_dim + row] = sum;
                        } else {
                            let row = i - dim - kv_dim;
                            let base = row * dim;
                            for (var c: u32 = 0u; c < dim; c = c + 1u) {
                                sum = sum + wv[base + c] * inp[c];
                            }
                            kv_v[pos * kv_dim + row] = sum;
                        }
                        i = i + 64u;
                    }
                    workgroupBarrier();

                    /* Stage 2: RoPE on Q (in q_scratch) and current K
                     * (just-written kv_k[pos*kv_dim..]). Standard llama
                     * pair rotation. */
                    let pairs_per_head = head_dim / 2u;
                    let total_q_pairs  = n_heads    * pairs_per_head;
                    let total_k_pairs  = n_kv_heads * pairs_per_head;

                    i = tid;
                    loop {
                        if (i >= total_q_pairs) { break; }
                        let h = i / pairs_per_head;
                        let p = i % pairs_per_head;
                        let exponent = f32(2u * p) / f32(head_dim);
                        var inv_freq = pow(rope_base, -exponent);
                        /* Llama 3 NTK-aware scaling — auto-detect via
                         * rope_base. Constants match the CPU rope()
                         * (factor=32, lo=1, hi=4, orig_ctx=8192). */
                        if (rope_base >= 100000.0) {
                            let TWO_PI  = 6.28318530717958647692;
                            let wavelen = TWO_PI / inv_freq;
                            let LO_WAVE = 8192.0;
                            let HI_WAVE = 2048.0;
                            let FACTOR  = 32.0;
                            if (wavelen > LO_WAVE) {
                                inv_freq = inv_freq / FACTOR;
                            } else if (wavelen >= HI_WAVE) {
                                let smooth = (8192.0 / wavelen - 1.0) / 3.0;
                                inv_freq = (1.0 - smooth) * (inv_freq / FACTOR)
                                         + smooth * inv_freq;
                            }
                        }
                        let theta = f32(pos) * inv_freq;
                        let c = cos(theta);
                        let s = sin(theta);
                        let idx = h * head_dim + 2u * p;
                        let x0 = q_scratch[idx];
                        let x1 = q_scratch[idx + 1u];
                        q_scratch[idx]      = x0 * c - x1 * s;
                        q_scratch[idx + 1u] = x0 * s + x1 * c;
                        i = i + 64u;
                    }
                    i = tid;
                    loop {
                        if (i >= total_k_pairs) { break; }
                        let h = i / pairs_per_head;
                        let p = i % pairs_per_head;
                        let exponent = f32(2u * p) / f32(head_dim);
                        var inv_freq = pow(rope_base, -exponent);
                        /* Llama 3 NTK-aware scaling — auto-detect via
                         * rope_base. Constants match the CPU rope()
                         * (factor=32, lo=1, hi=4, orig_ctx=8192). */
                        if (rope_base >= 100000.0) {
                            let TWO_PI  = 6.28318530717958647692;
                            let wavelen = TWO_PI / inv_freq;
                            let LO_WAVE = 8192.0;
                            let HI_WAVE = 2048.0;
                            let FACTOR  = 32.0;
                            if (wavelen > LO_WAVE) {
                                inv_freq = inv_freq / FACTOR;
                            } else if (wavelen >= HI_WAVE) {
                                let smooth = (8192.0 / wavelen - 1.0) / 3.0;
                                inv_freq = (1.0 - smooth) * (inv_freq / FACTOR)
                                         + smooth * inv_freq;
                            }
                        }
                        let theta = f32(pos) * inv_freq;
                        let c = cos(theta);
                        let s = sin(theta);
                        let idx = pos * kv_dim + h * head_dim + 2u * p;
                        let x0 = kv_k[idx];
                        let x1 = kv_k[idx + 1u];
                        kv_k[idx]      = x0 * c - x1 * s;
                        kv_k[idx + 1u] = x0 * s + x1 * c;
                        i = i + 64u;
                    }
                    workgroupBarrier();

                    /* Stage 3: For each query head h, compute attention
                     * scores against K[0..pos+1], softmax, weighted V
                     * sum -> attn_out[h*head_dim..(h+1)*head_dim]. */
                    for (var h: u32 = 0u; h < n_heads; h = h + 1u) {
                        let kv_h = h / gqa_ratio;
                        /* 3a: dot products att[p] = q_h . k[p][kv_h] * scale */
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            var dot: f32 = 0.0;
                            for (var d: u32 = 0u; d < head_dim; d = d + 1u) {
                                dot = dot + q_scratch[h * head_dim + d]
                                          * kv_k[i * kv_dim + kv_h * head_dim + d];
                            }
                            att[i] = dot * scale;
                            i = i + 64u;
                        }
                        workgroupBarrier();
                        /* 3b: softmax — max, exp, sum, normalize */
                        var lmax: f32 = -3.402823e38;
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            lmax = max(lmax, att[i]);
                            i = i + 64u;
                        }
                        partial[tid] = lmax;
                        workgroupBarrier();
                        if (tid == 0u) {
                            var m: f32 = -3.402823e38;
                            for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                                m = max(m, partial[k]);
                            }
                            partial[0] = m;
                        }
                        workgroupBarrier();
                        let gmax = partial[0];
                        var lsum: f32 = 0.0;
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            let e = exp(att[i] - gmax);
                            att[i] = e;
                            lsum = lsum + e;
                            i = i + 64u;
                        }
                        partial[tid] = lsum;
                        workgroupBarrier();
                        if (tid == 0u) {
                            var s: f32 = 0.0;
                            for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                                s = s + partial[k];
                            }
                            partial[0] = s;
                        }
                        workgroupBarrier();
                        let inv_sum = 1.0 / partial[0];
                        /* 3c: attn_out[h*head_dim+d] = sum(att[p]*inv_sum * v[p][kv_h][d]) */
                        var d: u32 = tid;
                        loop {
                            if (d >= head_dim) { break; }
                            var v_acc: f32 = 0.0;
                            for (var p: u32 = 0u; p <= pos; p = p + 1u) {
                                v_acc = v_acc + att[p] * inv_sum
                                              * kv_v[p * kv_dim + kv_h * head_dim + d];
                            }
                            attn_out[h * head_dim + d] = v_acc;
                            d = d + 64u;
                        }
                        workgroupBarrier();
                    }

                    /* Stage 4: Output projection W_o . attn_out -> outp.
                     * One thread per output row, striped by 64. */
                    i = tid;
                    loop {
                        if (i >= dim) { break; }
                        var sum: f32 = 0.0;
                        let base = i * dim;
                        for (var c: u32 = 0u; c < dim; c = c + 1u) {
                            sum = sum + wo[base + c] * attn_out[c];
                        }
                        outp[i] = sum;
                        i = i + 64u;
                    }
                }
            `;
            const attnMod = device.createShaderModule({ code: attnCode });
            window.__gpuAttnPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: attnMod, entryPoint: 'fused_attn' },
            });

            /* Q4_K dequant test pipeline. Single-workgroup compute that
             * reads a 144-byte Q4_K block from binding 0 (as array<u32>)
             * and writes 256 f32s to binding 1. Validates that the WGSL
             * port of dequant_q4_k_block matches the CPU implementation
             * bit-exact; precondition for porting fused-attn matvecs
             * to native Q4_K (eliminates the +608 MB predequant_attn
             * cost for Llama 1B Q4_K_M). */
            const q4kCode = `
                @group(0) @binding(0) var<storage, read> blk: array<u32>;
                @group(0) @binding(1) var<storage, read_write> outp: array<f32>;

                fn sb(idx: u32) -> u32 {
                    let w = blk[1u + (idx / 4u)];
                    return (w >> (8u * (idx % 4u))) & 0xFFu;
                }
                fn q4k_d(j: u32) -> u32 {
                    if (j < 4u) { return sb(j) & 63u; }
                    let lo = sb(j + 4u) & 0xFu;
                    let hi = (sb(j - 4u) >> 6u) & 3u;
                    return lo | (hi << 4u);
                }
                fn q4k_m(j: u32) -> u32 {
                    if (j < 4u) { return sb(j + 4u) & 63u; }
                    let lo = sb(j + 4u) >> 4u;
                    let hi = (sb(j) >> 6u) & 3u;
                    return lo | (hi << 4u);
                }
                fn qb(idx: u32) -> u32 {
                    let w = blk[4u + (idx / 4u)];
                    return (w >> (8u * (idx % 4u))) & 0xFFu;
                }

                @compute @workgroup_size(64)
                fn dequant_q4k(@builtin(local_invocation_id) lid: vec3<u32>) {
                    let tid = lid.x;
                    let h0 = blk[0u];
                    let dd   = unpack2x16float(h0).x;
                    let dmin = unpack2x16float(h0).y;
                    var e: u32 = tid;
                    loop {
                        if (e >= 256u) { break; }
                        let pair = e / 64u;
                        let off  = e % 64u;
                        let is_high = off >= 32u;
                        let sub_idx = pair * 2u + select(0u, 1u, is_high);
                        let sc = f32(q4k_d(sub_idx));
                        let mn = f32(q4k_m(sub_idx));
                        let qbyte = qb(pair * 32u + (off % 32u));
                        let nibble = select(qbyte & 0xFu, qbyte >> 4u, is_high);
                        outp[e] = dd * sc * f32(nibble) - dmin * mn;
                        e = e + 64u;
                    }
                }
            `;
            const q4kMod = device.createShaderModule({ code: q4kCode });
            window.__gpuQ4KPipeline = device.createComputePipeline({
                layout: 'auto',
                compute: { module: q4kMod, entryPoint: 'dequant_q4k' },
            });

            /* Q4_K-native fused-attn variant for Llama 1B Q4_K_M without
             * predequant. wq/wk/wv/wo are array<u32> (raw Q4_K bytes,
             * 36 u32 per 256-elem block). Matvecs inline the dequant
             * via q4k_dequant_one helper. Eliminates the +608 MB heap
             * cost of predequant_attn.
             *
             * Workgroup memory still uses storage q_scratch + attn_out
             * (bindings 9, 10) — same layout as attnCodeLarge so the
             * KV cache + scratch allocs are reusable. */
            const attnCodeQ4K = `
                const MAX_SEQ: u32 = 2048u;
                struct AttnDims {
                    dim: u32, kv_dim: u32,
                    head_dim: u32, n_heads: u32,
                    n_kv_heads: u32, gqa_ratio: u32,
                    pos: u32, max_seq: u32,
                    scale_bits: u32, rope_base_bits: u32,
                    _pad0: u32, _pad1: u32,
                };
                @group(0) @binding(0) var<storage, read> wq: array<u32>;
                @group(0) @binding(1) var<storage, read> wk: array<u32>;
                @group(0) @binding(2) var<storage, read> wv: array<u32>;
                @group(0) @binding(3) var<storage, read> wo: array<u32>;
                @group(0) @binding(4) var<storage, read> inp: array<f32>;
                @group(0) @binding(5) var<storage, read_write> kv_k: array<f32>;
                @group(0) @binding(6) var<storage, read_write> kv_v: array<f32>;
                @group(0) @binding(7) var<storage, read_write> outp: array<f32>;
                @group(0) @binding(8) var<uniform> ad: AttnDims;
                @group(0) @binding(9)  var<storage, read_write> q_scratch: array<f32>;
                @group(0) @binding(10) var<storage, read_write> attn_out: array<f32>;

                var<workgroup> att: array<f32, MAX_SEQ>;
                var<workgroup> partial: array<f32, 64>;

                /* Read byte at offset (4..15) into the block's scales region. */
                fn sb_at(buf: ptr<storage, array<u32>, read>, base: u32, idx: u32) -> u32 {
                    let w = (*buf)[base + 1u + (idx / 4u)];
                    return (w >> (8u * (idx % 4u))) & 0xFFu;
                }
                fn qb_at(buf: ptr<storage, array<u32>, read>, base: u32, idx: u32) -> u32 {
                    let w = (*buf)[base + 4u + (idx / 4u)];
                    return (w >> (8u * (idx % 4u))) & 0xFFu;
                }
                fn q4k_d_at(buf: ptr<storage, array<u32>, read>, base: u32, j: u32) -> u32 {
                    if (j < 4u) { return sb_at(buf, base, j) & 63u; }
                    let lo = sb_at(buf, base, j + 4u) & 0xFu;
                    let hi = (sb_at(buf, base, j - 4u) >> 6u) & 3u;
                    return lo | (hi << 4u);
                }
                fn q4k_m_at(buf: ptr<storage, array<u32>, read>, base: u32, j: u32) -> u32 {
                    if (j < 4u) { return sb_at(buf, base, j + 4u) & 63u; }
                    let lo = sb_at(buf, base, j + 4u) >> 4u;
                    let hi = (sb_at(buf, base, j) >> 6u) & 3u;
                    return lo | (hi << 4u);
                }
                fn q4k_one(buf: ptr<storage, array<u32>, read>, base: u32, e: u32) -> f32 {
                    let h0 = (*buf)[base];
                    let dd   = unpack2x16float(h0).x;
                    let dmin = unpack2x16float(h0).y;
                    let pair = e / 64u;
                    let off = e % 64u;
                    let is_high = off >= 32u;
                    let sub_idx = pair * 2u + select(0u, 1u, is_high);
                    let sc = f32(q4k_d_at(buf, base, sub_idx));
                    let mn = f32(q4k_m_at(buf, base, sub_idx));
                    let qbyte = qb_at(buf, base, pair * 32u + (off % 32u));
                    let nibble = select(qbyte & 0xFu, qbyte >> 4u, is_high);
                    return dd * sc * f32(nibble) - dmin * mn;
                }

                @compute @workgroup_size(64)
                fn fused_attn(@builtin(local_invocation_id) lid: vec3<u32>) {
                    let tid = lid.x;
                    let dim = ad.dim;
                    let kv_dim = ad.kv_dim;
                    let head_dim = ad.head_dim;
                    let n_heads = ad.n_heads;
                    let n_kv_heads = ad.n_kv_heads;
                    let gqa_ratio = ad.gqa_ratio;
                    let pos = ad.pos;
                    let scale = bitcast<f32>(ad.scale_bits);
                    let rope_base = bitcast<f32>(ad.rope_base_bits);
                    let blocks_per_row = dim / 256u;
                    let words_per_row  = blocks_per_row * 36u;

                    /* Stage 1: Q,K,V via inline Q4_K dot product. */
                    var i: u32 = tid;
                    let total = dim + 2u * kv_dim;
                    loop {
                        if (i >= total) { break; }
                        var sum: f32 = 0.0;
                        if (i < dim) {
                            let row_base = i * words_per_row;
                            for (var b: u32 = 0u; b < blocks_per_row; b = b + 1u) {
                                let blk = row_base + b * 36u;
                                for (var e: u32 = 0u; e < 256u; e = e + 1u) {
                                    sum = sum + q4k_one(&wq, blk, e) * inp[b * 256u + e];
                                }
                            }
                            q_scratch[i] = sum;
                        } else if (i < dim + kv_dim) {
                            let row = i - dim;
                            let row_base = row * words_per_row;
                            for (var b: u32 = 0u; b < blocks_per_row; b = b + 1u) {
                                let blk = row_base + b * 36u;
                                for (var e: u32 = 0u; e < 256u; e = e + 1u) {
                                    sum = sum + q4k_one(&wk, blk, e) * inp[b * 256u + e];
                                }
                            }
                            kv_k[pos * kv_dim + row] = sum;
                        } else {
                            let row = i - dim - kv_dim;
                            let row_base = row * words_per_row;
                            for (var b: u32 = 0u; b < blocks_per_row; b = b + 1u) {
                                let blk = row_base + b * 36u;
                                for (var e: u32 = 0u; e < 256u; e = e + 1u) {
                                    sum = sum + q4k_one(&wv, blk, e) * inp[b * 256u + e];
                                }
                            }
                            kv_v[pos * kv_dim + row] = sum;
                        }
                        i = i + 64u;
                    }
                    workgroupBarrier();

                    /* Stage 2: RoPE on Q (q_scratch) and current K
                     * (just-written kv_k[pos*kv_dim..]). Same NTK
                     * scaling math as attnCodeLarge. */
                    let pairs_per_head = head_dim / 2u;
                    let total_q_pairs  = n_heads    * pairs_per_head;
                    let total_k_pairs  = n_kv_heads * pairs_per_head;
                    i = tid;
                    loop {
                        if (i >= total_q_pairs) { break; }
                        let h = i / pairs_per_head;
                        let p = i % pairs_per_head;
                        let exponent = f32(2u * p) / f32(head_dim);
                        var inv_freq = pow(rope_base, -exponent);
                        if (rope_base >= 100000.0) {
                            let TWO_PI = 6.28318530717958647692;
                            let wavelen = TWO_PI / inv_freq;
                            if (wavelen > 8192.0) {
                                inv_freq = inv_freq / 32.0;
                            } else if (wavelen >= 2048.0) {
                                let smooth = (8192.0 / wavelen - 1.0) / 3.0;
                                inv_freq = (1.0 - smooth) * (inv_freq / 32.0) + smooth * inv_freq;
                            }
                        }
                        let theta = f32(pos) * inv_freq;
                        let c = cos(theta); let s = sin(theta);
                        let idx = h * head_dim + 2u * p;
                        let x0 = q_scratch[idx]; let x1 = q_scratch[idx + 1u];
                        q_scratch[idx]      = x0 * c - x1 * s;
                        q_scratch[idx + 1u] = x0 * s + x1 * c;
                        i = i + 64u;
                    }
                    i = tid;
                    loop {
                        if (i >= total_k_pairs) { break; }
                        let h = i / pairs_per_head;
                        let p = i % pairs_per_head;
                        let exponent = f32(2u * p) / f32(head_dim);
                        var inv_freq = pow(rope_base, -exponent);
                        if (rope_base >= 100000.0) {
                            let TWO_PI = 6.28318530717958647692;
                            let wavelen = TWO_PI / inv_freq;
                            if (wavelen > 8192.0) {
                                inv_freq = inv_freq / 32.0;
                            } else if (wavelen >= 2048.0) {
                                let smooth = (8192.0 / wavelen - 1.0) / 3.0;
                                inv_freq = (1.0 - smooth) * (inv_freq / 32.0) + smooth * inv_freq;
                            }
                        }
                        let theta = f32(pos) * inv_freq;
                        let c = cos(theta); let s = sin(theta);
                        let idx = pos * kv_dim + h * head_dim + 2u * p;
                        let x0 = kv_k[idx]; let x1 = kv_k[idx + 1u];
                        kv_k[idx]      = x0 * c - x1 * s;
                        kv_k[idx + 1u] = x0 * s + x1 * c;
                        i = i + 64u;
                    }
                    workgroupBarrier();

                    /* Stage 3: per-head attention. Identical to large F32. */
                    for (var h: u32 = 0u; h < n_heads; h = h + 1u) {
                        let kv_h = h / gqa_ratio;
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            var dot: f32 = 0.0;
                            for (var d: u32 = 0u; d < head_dim; d = d + 1u) {
                                dot = dot + q_scratch[h * head_dim + d]
                                          * kv_k[i * kv_dim + kv_h * head_dim + d];
                            }
                            att[i] = dot * scale;
                            i = i + 64u;
                        }
                        workgroupBarrier();
                        var lmax: f32 = -3.402823e38;
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            lmax = max(lmax, att[i]);
                            i = i + 64u;
                        }
                        partial[tid] = lmax;
                        workgroupBarrier();
                        if (tid == 0u) {
                            var m: f32 = -3.402823e38;
                            for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                                m = max(m, partial[k]);
                            }
                            partial[0] = m;
                        }
                        workgroupBarrier();
                        let gmax = partial[0];
                        var lsum: f32 = 0.0;
                        i = tid;
                        loop {
                            if (i > pos) { break; }
                            let e = exp(att[i] - gmax);
                            att[i] = e;
                            lsum = lsum + e;
                            i = i + 64u;
                        }
                        partial[tid] = lsum;
                        workgroupBarrier();
                        if (tid == 0u) {
                            var s: f32 = 0.0;
                            for (var k: u32 = 0u; k < 64u; k = k + 1u) {
                                s = s + partial[k];
                            }
                            partial[0] = s;
                        }
                        workgroupBarrier();
                        let inv_sum = 1.0 / partial[0];
                        var d: u32 = tid;
                        loop {
                            if (d >= head_dim) { break; }
                            var v_acc: f32 = 0.0;
                            for (var p: u32 = 0u; p <= pos; p = p + 1u) {
                                v_acc = v_acc + att[p] * inv_sum
                                              * kv_v[p * kv_dim + kv_h * head_dim + d];
                            }
                            attn_out[h * head_dim + d] = v_acc;
                            d = d + 64u;
                        }
                        workgroupBarrier();
                    }

                    /* Stage 4: W_o · attn_out via inline Q4_K dot. */
                    i = tid;
                    loop {
                        if (i >= dim) { break; }
                        let row_base = i * words_per_row;
                        var sum: f32 = 0.0;
                        for (var b: u32 = 0u; b < blocks_per_row; b = b + 1u) {
                            let blk = row_base + b * 36u;
                            for (var e: u32 = 0u; e < 256u; e = e + 1u) {
                                sum = sum + q4k_one(&wo, blk, e) * attn_out[b * 256u + e];
                            }
                        }
                        outp[i] = sum;
                        i = i + 64u;
                    }
                }
            `;
            try {
                const attnModQ4K = device.createShaderModule({ code: attnCodeQ4K });
                window.__gpuAttnPipelineQ4K = device.createComputePipeline({
                    layout: 'auto',
                    compute: { module: attnModQ4K, entryPoint: 'fused_attn' },
                });
            } catch (e) {
                console.warn('[wgpu] Q4_K attn pipeline failed:', String(e));
                window.__gpuAttnPipelineQ4K = null;
            }

            /* Large-dim variant for Llama 1B (dim=2048, kv_dim=512).
             * Promotes q_scratch and attn_out from workgroup arrays to
             * storage buffers (bindings 9, 10) so the per-workgroup
             * memory budget no longer caps dim. MAX_SEQ bumped to 2048
             * (8 KB workgroup mem for att[], still well under 16 KB
             * limit; partial[] adds 256 B). */
            const attnCodeLarge = attnCode
                .replace('const MAX_DIM: u32 = 256u;', 'const MAX_DIM: u32 = 256u;\n                const _UNUSED_LG: u32 = 1u;')
                .replace('const MAX_SEQ: u32 = 512u;', 'const MAX_SEQ: u32 = 2048u;')
                .replace('var<workgroup> q_scratch: array<f32, MAX_DIM>;',
                         '@group(0) @binding(9) var<storage, read_write> q_scratch: array<f32>;')
                .replace('var<workgroup> attn_out: array<f32, MAX_DIM>;',
                         '@group(0) @binding(10) var<storage, read_write> attn_out: array<f32>;');
            const attnModL = device.createShaderModule({ code: attnCodeLarge });
            window.__gpuAttnPipelineLarge = device.createComputePipeline({
                layout: 'auto',
                compute: { module: attnModL, entryPoint: 'fused_attn' },
            });

            window.__gpuReady = true;
        } catch (e) {
            window.__gpuError = String(e);
        }
        window.__gpuPending = false;
    })();
    return 1;
});

EM_JS(int, js_wgpu_pending, (), { return window.__gpuPending ? 1 : 0; });
EM_JS(int, js_wgpu_ready,   (), { return window.__gpuReady ? 1 : 0; });
EM_JS(int, js_wgpu_error,   (char *dst, int max), {
    var s = window.__gpuError || '';
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return s ? bytes - 1 : 0;
});

/* Synchronous init wrapper. Returns 1 if WebGPU is ready, 0 if
 * unsupported / init failed. Idempotent — repeat calls return cached
 * state. */
static int g_wgpu_init_done = 0;
int wasm_wgpu_init(void)
{
    if (g_wgpu_init_done) return js_wgpu_ready();
    js_wgpu_init_kick();
    while (js_wgpu_pending()) emscripten_sleep(20);
    g_wgpu_init_done = 1;
    return js_wgpu_ready();
}

int wasm_wgpu_error(char *dst, int max) { return js_wgpu_error(dst, max); }

/* Dispatch one matvec on the GPU. Allocates+destroys buffers per call
 * (no cache yet). Async via Asyncify suspend. Returns 0 on success,
 * -1 on failure (caller falls back to scalar). */
EM_JS(int, js_wgpu_matvec_kick, (const float *w, const float *vinp, float *out,
                                  int rows, int cols), {
    window.__gpuMatvecDone = false;
    window.__gpuMatvecOK = false;
    if (!window.__gpuReady) { window.__gpuMatvecDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuPipeline;
            const wBytes = rows * cols * 4;
            const iBytes = cols * 4;
            const oBytes = rows * 4;

            /* Buffer cache keyed by shape "rowsxcols". The brandon
             * forward pass calls with a small set of recurring shapes
             * (Q/K/V/FFN per layer); creating fresh buffers each call
             * dominated runtime. Cap the cache so unique shapes don't
             * leak GPU memory unbounded. */
            if (!window.__gpuBufCache) window.__gpuBufCache = new Map();
            const key = rows + 'x' + cols;
            let slot = window.__gpuBufCache.get(key);
            if (!slot) {
                if (window.__gpuBufCache.size >= 16) {
                    /* Evict oldest entry. */
                    const evictKey = window.__gpuBufCache.keys().next().value;
                    const evict = window.__gpuBufCache.get(evictKey);
                    evict.wBuf.destroy(); evict.iBuf.destroy();
                    evict.oBuf.destroy(); evict.dBuf.destroy();
                    evict.rBuf.destroy();
                    window.__gpuBufCache.delete(evictKey);
                }
                slot = {
                    wBuf: dev.createBuffer({ size: wBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    iBuf: dev.createBuffer({ size: iBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    oBuf: dev.createBuffer({ size: oBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dBuf: dev.createBuffer({ size: 8,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rBuf: dev.createBuffer({ size: oBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    bg: null,
                };
                slot.bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: slot.wBuf } },
                        { binding: 1, resource: { buffer: slot.iBuf } },
                        { binding: 2, resource: { buffer: slot.oBuf } },
                        { binding: 3, resource: { buffer: slot.dBuf } },
                    ],
                });
                /* Dims are constant per shape — write once. */
                dev.queue.writeBuffer(slot.dBuf, 0, new Uint32Array([rows, cols]));
                window.__gpuBufCache.set(key, slot);
            }

            dev.queue.writeBuffer(slot.wBuf, 0, HEAPU8.slice(w, w + wBytes));
            dev.queue.writeBuffer(slot.iBuf, 0, HEAPU8.slice(vinp, vinp + iBytes));

            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(Math.ceil(rows / 64));
            pass.end();
            enc.copyBufferToBuffer(slot.oBuf, 0, slot.rBuf, 0, oBytes);
            dev.queue.submit([enc.finish()]);

            await slot.rBuf.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(slot.rBuf.getMappedRange()), out);
            slot.rBuf.unmap();
            window.__gpuMatvecOK = true;
        } catch (e) {
            window.__gpuError = String(e);
        }
        window.__gpuMatvecDone = true;
    })();
    return 1;
});

EM_JS(int, js_wgpu_matvec_done, (), { return window.__gpuMatvecDone ? 1 : 0; });
EM_JS(int, js_wgpu_matvec_ok,   (), { return window.__gpuMatvecOK ? 1 : 0; });

int wasm_wgpu_matvec(const float *w, const float *vin, float *out,
                     int rows, int cols)
{
    if (!js_wgpu_ready()) return -1;
    js_wgpu_matvec_kick(w, vin, out, rows, cols);
    while (!js_wgpu_matvec_done()) emscripten_sleep(1);
    return js_wgpu_matvec_ok() ? 0 : -1;
}

/* ── RMS norm: y[i] = (x[i] / sqrt(mean(x^2) + eps)) * weight[i] ── */
EM_JS(int, js_wgpu_rmsnorm_kick, (const float *weight, const float *inp,
                                   float *out, int dim, int eps_bits), {
    window.__gpuOpDone = false;
    window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuRmsPipeline;
            const dimBytes = dim * 4;
            if (!window.__gpuRmsCache) window.__gpuRmsCache = new Map();
            const key = 'r' + dim;
            let slot = window.__gpuRmsCache.get(key);
            if (!slot) {
                if (window.__gpuRmsCache.size >= 8) {
                    const k0 = window.__gpuRmsCache.keys().next().value;
                    const old = window.__gpuRmsCache.get(k0);
                    old.wBuf.destroy(); old.iBuf.destroy();
                    old.oBuf.destroy(); old.dBuf.destroy(); old.rBuf.destroy();
                    window.__gpuRmsCache.delete(k0);
                }
                slot = {
                    wBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    iBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    oBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dBuf: dev.createBuffer({ size: 8,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    bg: null
                };
                slot.bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: slot.wBuf } },
                        { binding: 1, resource: { buffer: slot.iBuf } },
                        { binding: 2, resource: { buffer: slot.oBuf } },
                        { binding: 3, resource: { buffer: slot.dBuf } },
                    ],
                });
                window.__gpuRmsCache.set(key, slot);
            }
            dev.queue.writeBuffer(slot.wBuf, 0, HEAPU8.slice(weight, weight + dimBytes));
            dev.queue.writeBuffer(slot.iBuf, 0, HEAPU8.slice(inp, inp + dimBytes));
            dev.queue.writeBuffer(slot.dBuf, 0, new Uint32Array([dim, eps_bits]));
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(1);  /* one workgroup of 64 threads */
            pass.end();
            enc.copyBufferToBuffer(slot.oBuf, 0, slot.rBuf, 0, dimBytes);
            dev.queue.submit([enc.finish()]);
            await slot.rBuf.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(slot.rBuf.getMappedRange()), out);
            slot.rBuf.unmap();
            window.__gpuOpOK = true;
        } catch (e) {
            window.__gpuError = String(e);
        }
        window.__gpuOpDone = true;
    })();
    return 1;
});
EM_JS(int, js_wgpu_op_done, (), { return window.__gpuOpDone ? 1 : 0; });
EM_JS(int, js_wgpu_op_ok,   (), { return window.__gpuOpOK ? 1 : 0; });

int wasm_wgpu_rmsnorm(const float *weight, const float *in, float *out,
                       int dim, float eps)
{
    if (!js_wgpu_ready()) return -1;
    /* Pass eps as raw IEEE 754 bits — WGSL bitcasts back. Avoids
     * stuffing a float into a uniform via Uint32Array trickery. */
    union { float f; uint32_t u; } u; u.f = eps;
    js_wgpu_rmsnorm_kick(weight, in, out, dim, (int)u.u);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

/* ── Softmax: numerically-stable, in-place semantics ─────────── */
EM_JS(int, js_wgpu_softmax_kick, (const float *inp, float *out, int len), {
    window.__gpuOpDone = false;
    window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuSoftPipeline;
            const lenBytes = len * 4;
            if (!window.__gpuSoftCache) window.__gpuSoftCache = new Map();
            const key = 's' + len;
            let slot = window.__gpuSoftCache.get(key);
            if (!slot) {
                if (window.__gpuSoftCache.size >= 8) {
                    const k0 = window.__gpuSoftCache.keys().next().value;
                    const old = window.__gpuSoftCache.get(k0);
                    old.iBuf.destroy(); old.oBuf.destroy();
                    old.dBuf.destroy(); old.rBuf.destroy();
                    window.__gpuSoftCache.delete(k0);
                }
                slot = {
                    iBuf: dev.createBuffer({ size: lenBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    oBuf: dev.createBuffer({ size: lenBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dBuf: dev.createBuffer({ size: 4,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rBuf: dev.createBuffer({ size: lenBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    bg: null
                };
                slot.bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: slot.iBuf } },
                        { binding: 1, resource: { buffer: slot.oBuf } },
                        { binding: 2, resource: { buffer: slot.dBuf } },
                    ],
                });
                dev.queue.writeBuffer(slot.dBuf, 0, new Uint32Array([len]));
                window.__gpuSoftCache.set(key, slot);
            }
            dev.queue.writeBuffer(slot.iBuf, 0, HEAPU8.slice(inp, inp + lenBytes));
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(1);
            pass.end();
            enc.copyBufferToBuffer(slot.oBuf, 0, slot.rBuf, 0, lenBytes);
            dev.queue.submit([enc.finish()]);
            await slot.rBuf.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(slot.rBuf.getMappedRange()), out);
            slot.rBuf.unmap();
            window.__gpuOpOK = true;
        } catch (e) {
            window.__gpuError = String(e);
        }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_softmax(const float *in, float *out, int len)
{
    if (!js_wgpu_ready()) return -1;
    js_wgpu_softmax_kick(in, out, len);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

/* ── GPU-resident buffer handles ────────────────────────────────
 * Activations don't have to bounce CPU<->GPU each op. The handle API
 * lets callers allocate a persistent VRAM buffer once, run a chain of
 * ops that read/write it, then download the final result.
 *
 *   wgpu_alloc(n_floats)           -> int handle id
 *   wgpu_upload(h, src_ptr, n)
 *   wgpu_download(h, dst_ptr, n)
 *   wgpu_free(h)
 *
 *   wasm_wgpu_matvec_h(hw, hin, hout, rows, cols)
 *   wasm_wgpu_rmsnorm_h(hw, hin, hout, dim, eps)
 *   wasm_wgpu_softmax_h(hin, hout, len)
 *
 * Pure dispatch — no buffer copy on the hot path. */

EM_JS(int, js_wgpu_alloc, (int n_floats), {
    if (!window.__gpuReady) return -1;
    if (!window.__gpuHandles) {
        window.__gpuHandles = [];
        window.__gpuHandleNext = 1;
    }
    const dev = window.__gpuDevice;
    const buf = dev.createBuffer({
        size: n_floats * 4,
        usage: GPUBufferUsage.STORAGE
             | GPUBufferUsage.COPY_DST
             | GPUBufferUsage.COPY_SRC,
    });
    const id = window.__gpuHandleNext++;
    window.__gpuHandles[id] = { buf, n: n_floats };
    return id;
});
EM_JS(void, js_wgpu_free, (int id), {
    if (!window.__gpuHandles) return;
    const h = window.__gpuHandles[id];
    if (!h) return;
    h.buf.destroy();
    window.__gpuHandles[id] = null;
});
EM_JS(void, js_wgpu_upload, (int id, const float *src, int n), {
    const h = window.__gpuHandles && window.__gpuHandles[id];
    if (!h) return;
    window.__gpuDevice.queue.writeBuffer(h.buf, 0, HEAPU8.slice(src, src + n*4));
});
EM_JS(int, js_wgpu_download_kick, (int id, float *dst, int n), {
    window.__gpuDlDone = false;
    const h = window.__gpuHandles && window.__gpuHandles[id];
    if (!h) { window.__gpuDlDone = true; return -1; }
    (async function() {
        const dev = window.__gpuDevice;
        const r = dev.createBuffer({
            size: n * 4,
            usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
        });
        const enc = dev.createCommandEncoder();
        enc.copyBufferToBuffer(h.buf, 0, r, 0, n * 4);
        dev.queue.submit([enc.finish()]);
        await r.mapAsync(GPUMapMode.READ);
        HEAPU8.set(new Uint8Array(r.getMappedRange()), dst);
        r.unmap();
        r.destroy();
        window.__gpuDlDone = true;
    })();
    return 0;
});
EM_JS(int, js_wgpu_dl_done, (), { return window.__gpuDlDone ? 1 : 0; });

int wgpu_alloc(int n_floats)         { return js_wgpu_ready() ? js_wgpu_alloc(n_floats) : -1; }
void wgpu_free(int id)               { js_wgpu_free(id); }
void wgpu_upload(int id, const float *src, int n) { js_wgpu_upload(id, src, n); }
void wgpu_download(int id, float *dst, int n)
{
    if (js_wgpu_download_kick(id, dst, n) != 0) return;
    while (!js_wgpu_dl_done()) emscripten_sleep(1);
}

/* Handle-based matvec: zero copy on the hot path. The pipeline + bind
 * group are still cached per-shape so dispatching is fast. */
EM_JS(int, js_wgpu_matvec_h_kick, (int hw, int hin, int hout, int rows, int cols), {
    window.__gpuOpDone = false;
    window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuPipeline;
            if (!window.__gpuMatvecHCache) window.__gpuMatvecHCache = new Map();
            const key = rows + 'x' + cols + ':' + hw + ',' + hin + ',' + hout;
            let slot = window.__gpuMatvecHCache.get(key);
            if (!slot) {
                if (window.__gpuMatvecHCache.size >= 64) {
                    const k0 = window.__gpuMatvecHCache.keys().next().value;
                    const old = window.__gpuMatvecHCache.get(k0);
                    if (old.dBuf) old.dBuf.destroy();
                    window.__gpuMatvecHCache.delete(k0);
                }
                const w = window.__gpuHandles[hw];
                const i = window.__gpuHandles[hin];
                const o = window.__gpuHandles[hout];
                if (!w || !i || !o) {
                    window.__gpuOpOK = false; window.__gpuOpDone = true; return;
                }
                const dBuf = dev.createBuffer({
                    size: 8,
                    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
                });
                dev.queue.writeBuffer(dBuf, 0, new Uint32Array([rows, cols]));
                const bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: w.buf } },
                        { binding: 1, resource: { buffer: i.buf } },
                        { binding: 2, resource: { buffer: o.buf } },
                        { binding: 3, resource: { buffer: dBuf } },
                    ],
                });
                slot = { bg, dBuf };
                window.__gpuMatvecHCache.set(key, slot);
            }
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(Math.ceil(rows / 64));
            pass.end();
            dev.queue.submit([enc.finish()]);
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_matvec_h(int hw, int hin, int hout, int rows, int cols)
{
    if (!js_wgpu_ready()) return -1;
    js_wgpu_matvec_h_kick(hw, hin, hout, rows, cols);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

EM_JS(int, js_wgpu_rmsnorm_h_kick, (int hw, int hin, int hout,
                                     int dim, int eps_bits), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuRmsPipeline;
            if (!window.__gpuRmsHCache) window.__gpuRmsHCache = new Map();
            const key = dim + ':' + hw + ',' + hin + ',' + hout;
            let slot = window.__gpuRmsHCache.get(key);
            if (!slot) {
                if (window.__gpuRmsHCache.size >= 64) {
                    const k0 = window.__gpuRmsHCache.keys().next().value;
                    const old = window.__gpuRmsHCache.get(k0);
                    if (old.dBuf) old.dBuf.destroy();
                    window.__gpuRmsHCache.delete(k0);
                }
                const w = window.__gpuHandles[hw];
                const i = window.__gpuHandles[hin];
                const o = window.__gpuHandles[hout];
                if (!w || !i || !o) {
                    window.__gpuOpOK = false; window.__gpuOpDone = true; return;
                }
                const dBuf = dev.createBuffer({ size: 8,
                    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
                const bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: w.buf } },
                        { binding: 1, resource: { buffer: i.buf } },
                        { binding: 2, resource: { buffer: o.buf } },
                        { binding: 3, resource: { buffer: dBuf } },
                    ],
                });
                slot = { bg, dBuf, last_eps: 0 };
                window.__gpuRmsHCache.set(key, slot);
            }
            /* Re-write dims uniform if eps changed (rare). */
            if (slot.last_eps !== eps_bits) {
                dev.queue.writeBuffer(slot.dBuf, 0, new Uint32Array([dim, eps_bits]));
                slot.last_eps = eps_bits;
            }
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe); pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(1);
            pass.end();
            dev.queue.submit([enc.finish()]);
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_rmsnorm_h(int hw, int hin, int hout, int dim, float eps)
{
    if (!js_wgpu_ready()) return -1;
    union { float f; uint32_t u; } u; u.f = eps;
    js_wgpu_rmsnorm_h_kick(hw, hin, hout, dim, (int)u.u);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

EM_JS(int, js_wgpu_softmax_h_kick, (int hin, int hout, int len), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuSoftPipeline;
            if (!window.__gpuSoftHCache) window.__gpuSoftHCache = new Map();
            const key = len + ':' + hin + ',' + hout;
            let slot = window.__gpuSoftHCache.get(key);
            if (!slot) {
                if (window.__gpuSoftHCache.size >= 64) {
                    const k0 = window.__gpuSoftHCache.keys().next().value;
                    const old = window.__gpuSoftHCache.get(k0);
                    if (old.dBuf) old.dBuf.destroy();
                    window.__gpuSoftHCache.delete(k0);
                }
                const i = window.__gpuHandles[hin];
                const o = window.__gpuHandles[hout];
                if (!i || !o) {
                    window.__gpuOpOK = false; window.__gpuOpDone = true; return;
                }
                const dBuf = dev.createBuffer({ size: 4,
                    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
                dev.queue.writeBuffer(dBuf, 0, new Uint32Array([len]));
                const bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: i.buf } },
                        { binding: 1, resource: { buffer: o.buf } },
                        { binding: 2, resource: { buffer: dBuf } },
                    ],
                });
                slot = { bg, dBuf };
                window.__gpuSoftHCache.set(key, slot);
            }
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe); pass.setBindGroup(0, slot.bg);
            pass.dispatchWorkgroups(1);
            pass.end();
            dev.queue.submit([enc.finish()]);
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_softmax_h(int hin, int hout, int len)
{
    if (!js_wgpu_ready()) return -1;
    js_wgpu_softmax_h_kick(hin, hout, len);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

/* ── Fused brandon LM head: rmsnorm(x, w_norm) -> matvec(W_lm, .) -> logits.
 * Two GPU dispatches in one command buffer, single mapAsync at the end.
 * Weight buffers are cached by C pointer so repeat calls only re-upload x. */
EM_JS(int, js_wgpu_lm_head_kick, (const float *xin, const float *w_norm,
                                   const float *w_lm, float *logits,
                                   int dim, int vocab, int eps_bits,
                                   int wn_id, int wlm_id), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const rmsP = window.__gpuRmsPipeline;
            const mvP = window.__gpuPipeline;
            if (!window.__gpuLMHCache) window.__gpuLMHCache = new Map();
            const key = dim + 'x' + vocab + ':' + wn_id + ',' + wlm_id;
            let s = window.__gpuLMHCache.get(key);
            if (!s) {
                if (window.__gpuLMHCache.size >= 4) {
                    const k0 = window.__gpuLMHCache.keys().next().value;
                    const old = window.__gpuLMHCache.get(k0);
                    old.wnBuf.destroy(); old.wlmBuf.destroy();
                    old.xBuf.destroy(); old.midBuf.destroy(); old.outBuf.destroy();
                    old.dimsRms.destroy(); old.dimsMv.destroy(); old.rBuf.destroy();
                    window.__gpuLMHCache.delete(k0);
                }
                s = {
                    wnBuf: dev.createBuffer({ size: dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    wlmBuf: dev.createBuffer({ size: vocab * dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    xBuf: dev.createBuffer({ size: dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    midBuf: dev.createBuffer({ size: dim * 4,
                        usage: GPUBufferUsage.STORAGE }),
                    outBuf: dev.createBuffer({ size: vocab * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dimsRms: dev.createBuffer({ size: 8,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    dimsMv: dev.createBuffer({ size: 8,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rBuf: dev.createBuffer({ size: vocab * 4,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    bgRms: null, bgMv: null,
                    last_wn: 0, last_wlm: 0,
                };
                s.bgRms = dev.createBindGroup({
                    layout: rmsP.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: s.wnBuf } },
                        { binding: 1, resource: { buffer: s.xBuf } },
                        { binding: 2, resource: { buffer: s.midBuf } },
                        { binding: 3, resource: { buffer: s.dimsRms } },
                    ],
                });
                s.bgMv = dev.createBindGroup({
                    layout: mvP.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: s.wlmBuf } },
                        { binding: 1, resource: { buffer: s.midBuf } },
                        { binding: 2, resource: { buffer: s.outBuf } },
                        { binding: 3, resource: { buffer: s.dimsMv } },
                    ],
                });
                dev.queue.writeBuffer(s.dimsRms, 0, new Uint32Array([dim, eps_bits]));
                dev.queue.writeBuffer(s.dimsMv, 0, new Uint32Array([vocab, dim]));
                window.__gpuLMHCache.set(key, s);
            }
            /* Re-upload weights only when the C pointer changes (model reload). */
            if (s.last_wn !== wn_id) {
                dev.queue.writeBuffer(s.wnBuf, 0, HEAPU8.slice(w_norm, w_norm + dim*4));
                s.last_wn = wn_id;
            }
            if (s.last_wlm !== wlm_id) {
                dev.queue.writeBuffer(s.wlmBuf, 0,
                    HEAPU8.slice(w_lm, w_lm + vocab*dim*4));
                s.last_wlm = wlm_id;
            }
            /* x changes every token. */
            dev.queue.writeBuffer(s.xBuf, 0, HEAPU8.slice(xin, xin + dim*4));
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(rmsP); pass.setBindGroup(0, s.bgRms);
            pass.dispatchWorkgroups(1);
            pass.setPipeline(mvP);  pass.setBindGroup(0, s.bgMv);
            pass.dispatchWorkgroups(Math.ceil(vocab / 64));
            pass.end();
            enc.copyBufferToBuffer(s.outBuf, 0, s.rBuf, 0, vocab * 4);
            dev.queue.submit([enc.finish()]);
            await s.rBuf.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(s.rBuf.getMappedRange()), logits);
            s.rBuf.unmap();
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

/* ── Fused QKV: 3 matvecs sharing the same input in one dispatch.
 * Caches weight buffers by (W_q, W_k, W_v) pointer triple so a
 * brandon layer's per-token call only re-uploads x. */
EM_JS(int, js_wgpu_qkv_kick, (const float *x, const float *wq, const float *wk,
                               const float *wv, float *outq, float *outk,
                               float *outv, int dim, int q_rows, int kv_rows,
                               int wq_id, int wk_id, int wv_id), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            const pipe = window.__gpuQkvPipeline;
            if (!window.__gpuQkvCache) window.__gpuQkvCache = new Map();
            const key = dim + ':' + q_rows + ':' + kv_rows + ':' +
                        wq_id + ',' + wk_id + ',' + wv_id;
            let s = window.__gpuQkvCache.get(key);
            if (!s) {
                /* Cap at 32 entries × per layer (12) = ~384 layer-shape
                 * combos; brandon reuses 1 shape across 12 layers so
                 * the limit is loose. */
                if (window.__gpuQkvCache.size >= 32) {
                    const k0 = window.__gpuQkvCache.keys().next().value;
                    const old = window.__gpuQkvCache.get(k0);
                    old.wqBuf.destroy(); old.wkBuf.destroy(); old.wvBuf.destroy();
                    old.xBuf.destroy(); old.qBuf.destroy(); old.kBuf.destroy();
                    old.vBuf.destroy(); old.dBuf.destroy(); old.rqBuf.destroy();
                    old.rkBuf.destroy(); old.rvBuf.destroy();
                    window.__gpuQkvCache.delete(k0);
                }
                const dimBytes = dim * 4;
                const qBytes = q_rows * 4;
                const kvBytes = kv_rows * 4;
                s = {
                    wqBuf: dev.createBuffer({ size: q_rows * dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    wkBuf: dev.createBuffer({ size: kv_rows * dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    wvBuf: dev.createBuffer({ size: kv_rows * dim * 4,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    xBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    qBuf: dev.createBuffer({ size: qBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    kBuf: dev.createBuffer({ size: kvBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    vBuf: dev.createBuffer({ size: kvBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dBuf: dev.createBuffer({ size: 16,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rqBuf: dev.createBuffer({ size: qBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    rkBuf: dev.createBuffer({ size: kvBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    rvBuf: dev.createBuffer({ size: kvBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    bg: null, last_wq: 0, last_wk: 0, last_wv: 0
                };
                s.bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: [
                        { binding: 0, resource: { buffer: s.wqBuf } },
                        { binding: 1, resource: { buffer: s.wkBuf } },
                        { binding: 2, resource: { buffer: s.wvBuf } },
                        { binding: 3, resource: { buffer: s.xBuf } },
                        { binding: 4, resource: { buffer: s.qBuf } },
                        { binding: 5, resource: { buffer: s.kBuf } },
                        { binding: 6, resource: { buffer: s.vBuf } },
                        { binding: 7, resource: { buffer: s.dBuf } },
                    ],
                });
                dev.queue.writeBuffer(s.dBuf, 0,
                    new Uint32Array([dim, q_rows, kv_rows, 0]));
                window.__gpuQkvCache.set(key, s);
            }
            /* Re-upload weights only when the C pointer changes. */
            if (s.last_wq !== wq_id) {
                dev.queue.writeBuffer(s.wqBuf, 0, HEAPU8.slice(wq, wq + q_rows*dim*4));
                s.last_wq = wq_id;
            }
            if (s.last_wk !== wk_id) {
                dev.queue.writeBuffer(s.wkBuf, 0, HEAPU8.slice(wk, wk + kv_rows*dim*4));
                s.last_wk = wk_id;
            }
            if (s.last_wv !== wv_id) {
                dev.queue.writeBuffer(s.wvBuf, 0, HEAPU8.slice(wv, wv + kv_rows*dim*4));
                s.last_wv = wv_id;
            }
            dev.queue.writeBuffer(s.xBuf, 0, HEAPU8.slice(x, x + dim*4));
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, s.bg);
            const total = q_rows + 2 * kv_rows;
            pass.dispatchWorkgroups(Math.ceil(total / 64));
            pass.end();
            enc.copyBufferToBuffer(s.qBuf, 0, s.rqBuf, 0, q_rows * 4);
            enc.copyBufferToBuffer(s.kBuf, 0, s.rkBuf, 0, kv_rows * 4);
            enc.copyBufferToBuffer(s.vBuf, 0, s.rvBuf, 0, kv_rows * 4);
            dev.queue.submit([enc.finish()]);
            /* Map all three readbacks; await the last one — the others
             * resolve concurrently so this is a single round-trip. */
            await Promise.all([
                s.rqBuf.mapAsync(GPUMapMode.READ),
                s.rkBuf.mapAsync(GPUMapMode.READ),
                s.rvBuf.mapAsync(GPUMapMode.READ),
            ]);
            HEAPU8.set(new Uint8Array(s.rqBuf.getMappedRange()), outq);
            HEAPU8.set(new Uint8Array(s.rkBuf.getMappedRange()), outk);
            HEAPU8.set(new Uint8Array(s.rvBuf.getMappedRange()), outv);
            s.rqBuf.unmap(); s.rkBuf.unmap(); s.rvBuf.unmap();
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_qkv(const float *x, const float *wq, const float *wk, const float *wv,
                   float *outq, float *outk, float *outv,
                   int dim, int q_rows, int kv_rows)
{
    if (!js_wgpu_ready()) return -1;
    int wq_id = (int)((uint64_t)wq & 0x7FFFFFFF);
    int wk_id = (int)((uint64_t)wk & 0x7FFFFFFF);
    int wv_id = (int)((uint64_t)wv & 0x7FFFFFFF);
    js_wgpu_qkv_kick(x, wq, wk, wv, outq, outk, outv, dim, q_rows, kv_rows,
                      wq_id, wk_id, wv_id);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

/* ── Persistent KV cache buffer pool ─────────────────────────────
 * For Phase-4 fused attention: each layer reserves a GPU storage
 * buffer for its K and V cache, sized once for max_seq × kv_dim.
 * Pool lookup by layer index — caller supplies a stable layer id.
 * Returns 0 on success, sets per-layer handle ids out_hk/out_hv. */
/* ── Q4_K dequant test dispatcher ────────────────────────────────
 *
 * Reads a 144-byte block from `blk_ptr`, dispatches the GPU dequant
 * shader, writes 256 f32s to `out_ptr`. Blocking on Asyncify. Used by
 * the `wgpu q4k_gpu` shell command to compare against CPU dequant. */
EM_JS(int, js_wgpu_q4k_dequant_kick, (const void *blk_ptr, float *out_ptr), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev  = window.__gpuDevice;
            const pipe = window.__gpuQ4KPipeline;
            const bIn  = dev.createBuffer({ size: 144,
                usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
            const bOut = dev.createBuffer({ size: 256 * 4,
                usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
            const bRd  = dev.createBuffer({ size: 256 * 4,
                usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
            dev.queue.writeBuffer(bIn, 0, HEAPU8.slice(blk_ptr, blk_ptr + 144));
            const bg = dev.createBindGroup({
                layout: pipe.getBindGroupLayout(0),
                entries: [
                    { binding: 0, resource: { buffer: bIn  } },
                    { binding: 1, resource: { buffer: bOut } },
                ],
            });
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, bg);
            pass.dispatchWorkgroups(1);
            pass.end();
            enc.copyBufferToBuffer(bOut, 0, bRd, 0, 256 * 4);
            dev.queue.submit([enc.finish()]);
            await bRd.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(bRd.getMappedRange()), out_ptr);
            bRd.unmap();
            bIn.destroy(); bOut.destroy(); bRd.destroy();
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_q4k_dequant(const void *blk, float *out)
{
    if (!js_wgpu_ready()) return -1;
    js_wgpu_q4k_dequant_kick(blk, out);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

EM_JS(int, js_wgpu_kvcache_alloc, (int layer, int max_seq, int kv_dim), {
    if (!window.__gpuReady) return -1;
    if (!window.__gpuKVPool) window.__gpuKVPool = {};
    var slot = window.__gpuKVPool[layer];
    if (slot && slot.max_seq === max_seq && slot.kv_dim === kv_dim) return 0;
    if (slot) { slot.kBuf.destroy(); slot.vBuf.destroy(); }
    var dev = window.__gpuDevice;
    var bytes = max_seq * kv_dim * 4;
    window.__gpuKVPool[layer] = {
        max_seq: max_seq, kv_dim: kv_dim,
        kBuf: dev.createBuffer({ size: bytes,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC }),
        vBuf: dev.createBuffer({ size: bytes,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC }),
    };
    return 0;
});

int wasm_wgpu_kvcache_alloc(int layer, int max_seq, int kv_dim)
{
    if (!js_wgpu_ready()) return -1;
    return js_wgpu_kvcache_alloc(layer, max_seq, kv_dim);
}

/* ── Fused attention block dispatch ──────────────────────────────
 * Runs QKV projection, RoPE, attention scoring, softmax, value
 * mixing, and output projection in ONE compute dispatch. KV cache
 * lives in the per-layer pool (js_wgpu_kvcache_alloc); only x and
 * the layer's weights cross the bus per-token after warm-up. */
EM_JS(int, js_wgpu_fused_attn_kick, (
    int layer,
    const float *x, const float *wq, const float *wk, const float *wv, const float *wo,
    float *outp,
    int dim, int kv_dim, int head_dim, int n_heads, int n_kv_heads,
    int gqa_ratio, int pos, int max_seq,
    int scale_bits, int rope_base_bits,
    int wq_id, int wk_id, int wv_id, int wo_id,
    int weight_dtype), {
    window.__gpuOpDone = false; window.__gpuOpOK = false;
    if (!window.__gpuReady) { window.__gpuOpDone = true; return 0; }
    (async function() {
        try {
            const dev = window.__gpuDevice;
            /* Pipeline select:
             *   weight_dtype=2 (Q4_K) → Q4K pipeline (zero predequant cost)
             *   dim > 256             → Large F32 pipeline (storage q/o)
             *   else                   → small F32 pipeline (workgroup q/o)
             *
             * Q4_K path falls back if the pipeline compile failed (older
             * WebGPU implementations may not support pointer-to-storage
             * function params used by the inline dequant). */
            const isQ4K = (weight_dtype === 2) && !!window.__gpuAttnPipelineQ4K;
            const isLarge = !isQ4K && dim > 256;
            const pipe = isQ4K  ? window.__gpuAttnPipelineQ4K
                       : isLarge ? window.__gpuAttnPipelineLarge
                                 : window.__gpuAttnPipeline;
            const kv = window.__gpuKVPool && window.__gpuKVPool[layer];
            if (!kv) {
                window.__gpuError = 'KV cache not allocated for layer ' + layer;
                window.__gpuOpDone = true; return;
            }
            if (!window.__gpuAttnCache) window.__gpuAttnCache = new Map();
            /* Per-layer attn cache key now also discriminates on dtype so
             * we don't reuse F32-sized weight buffers when the model is
             * Q4_K (or vice versa). */
            const key = layer + ':' + dim + ':' + kv_dim + ':' + head_dim + ':' + weight_dtype;
            let s = window.__gpuAttnCache.get(key);
            if (!s) {
                if (window.__gpuAttnCache.size >= 32) {
                    const k0 = window.__gpuAttnCache.keys().next().value;
                    const old = window.__gpuAttnCache.get(k0);
                    old.wqBuf.destroy(); old.wkBuf.destroy(); old.wvBuf.destroy();
                    old.woBuf.destroy(); old.xBuf.destroy(); old.outBuf.destroy();
                    old.dBuf.destroy(); old.rBuf.destroy();
                    if (old.qBuf) old.qBuf.destroy();
                    if (old.oBuf) old.oBuf.destroy();
                    window.__gpuAttnCache.delete(k0);
                }
                const dimBytes = dim * 4;
                /* Q4_K byte size per weight row = (dim/256) * 144. */
                const blocksPerRow = (dim / 256) | 0;
                const q4kRowBytes = blocksPerRow * 144;
                const wqBytes = isQ4K ? dim    * q4kRowBytes : dim    * dim * 4;
                const wkBytes = isQ4K ? kv_dim * q4kRowBytes : kv_dim * dim * 4;
                const wvBytes = isQ4K ? kv_dim * q4kRowBytes : kv_dim * dim * 4;
                const woBytes = isQ4K ? dim    * q4kRowBytes : dim    * dim * 4;
                s = {
                    wqBuf: dev.createBuffer({ size: wqBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    wkBuf: dev.createBuffer({ size: wkBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    wvBuf: dev.createBuffer({ size: wvBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    woBuf: dev.createBuffer({ size: woBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    _wqBytes: wqBytes, _wkBytes: wkBytes,
                    _wvBytes: wvBytes, _woBytes: woBytes,
                    xBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }),
                    outBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC }),
                    dBuf: dev.createBuffer({ size: 48,
                        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
                    rBuf: dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }),
                    qBuf: null, oBuf: null,
                    bg: null,
                    last_wq: 0, last_wk: 0, last_wv: 0, last_wo: 0
                };
                /* Large-dim path: persistent q + o scratch (per layer
                 * config). Small path uses workgroup arrays inside the
                 * shader and skips these buffers entirely. */
                const entries = [
                    { binding: 0, resource: { buffer: s.wqBuf } },
                    { binding: 1, resource: { buffer: s.wkBuf } },
                    { binding: 2, resource: { buffer: s.wvBuf } },
                    { binding: 3, resource: { buffer: s.woBuf } },
                    { binding: 4, resource: { buffer: s.xBuf } },
                    { binding: 5, resource: { buffer: kv.kBuf } },
                    { binding: 6, resource: { buffer: kv.vBuf } },
                    { binding: 7, resource: { buffer: s.outBuf } },
                    { binding: 8, resource: { buffer: s.dBuf } },
                ];
                if (isLarge || isQ4K) {
                    s.qBuf = dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE });
                    s.oBuf = dev.createBuffer({ size: dimBytes,
                        usage: GPUBufferUsage.STORAGE });
                    entries.push({ binding: 9,  resource: { buffer: s.qBuf } });
                    entries.push({ binding: 10, resource: { buffer: s.oBuf } });
                }
                s.bg = dev.createBindGroup({
                    layout: pipe.getBindGroupLayout(0),
                    entries: entries,
                });
                window.__gpuAttnCache.set(key, s);
            }
            if (s.last_wq !== wq_id) {
                dev.queue.writeBuffer(s.wqBuf, 0, HEAPU8.slice(wq, wq + s._wqBytes));
                s.last_wq = wq_id;
            }
            if (s.last_wk !== wk_id) {
                dev.queue.writeBuffer(s.wkBuf, 0, HEAPU8.slice(wk, wk + s._wkBytes));
                s.last_wk = wk_id;
            }
            if (s.last_wv !== wv_id) {
                dev.queue.writeBuffer(s.wvBuf, 0, HEAPU8.slice(wv, wv + s._wvBytes));
                s.last_wv = wv_id;
            }
            if (s.last_wo !== wo_id) {
                dev.queue.writeBuffer(s.woBuf, 0, HEAPU8.slice(wo, wo + s._woBytes));
                s.last_wo = wo_id;
            }
            dev.queue.writeBuffer(s.xBuf, 0, HEAPU8.slice(x, x + dim*4));
            /* Pack uniform: 12 u32s = 48 bytes. */
            dev.queue.writeBuffer(s.dBuf, 0, new Uint32Array([
                dim, kv_dim, head_dim, n_heads, n_kv_heads, gqa_ratio,
                pos, max_seq, scale_bits, rope_base_bits, 0, 0
            ]));
            const enc = dev.createCommandEncoder();
            const pass = enc.beginComputePass();
            pass.setPipeline(pipe);
            pass.setBindGroup(0, s.bg);
            pass.dispatchWorkgroups(1);   /* single workgroup of 64 threads */
            pass.end();
            enc.copyBufferToBuffer(s.outBuf, 0, s.rBuf, 0, dim * 4);
            dev.queue.submit([enc.finish()]);
            await s.rBuf.mapAsync(GPUMapMode.READ);
            HEAPU8.set(new Uint8Array(s.rBuf.getMappedRange()), outp);
            s.rBuf.unmap();
            window.__gpuOpOK = true;
        } catch (e) { window.__gpuError = String(e); }
        window.__gpuOpDone = true;
    })();
    return 1;
});

int wasm_wgpu_fused_attn(int layer,
    const float *x, const void *wq, const void *wk, const void *wv, const void *wo,
    float *out,
    int dim, int kv_dim, int head_dim, int n_heads, int n_kv_heads,
    int gqa_ratio, int pos, int max_seq, float scale, float rope_base,
    int weight_dtype)
{
    if (!js_wgpu_ready()) return -1;
    union { float f; uint32_t u; } sb, rb;
    sb.f = scale; rb.f = rope_base;
    int wq_id = (int)((uint64_t)wq & 0x7FFFFFFF);
    int wk_id = (int)((uint64_t)wk & 0x7FFFFFFF);
    int wv_id = (int)((uint64_t)wv & 0x7FFFFFFF);
    int wo_id = (int)((uint64_t)wo & 0x7FFFFFFF);
    js_wgpu_fused_attn_kick(layer, x, wq, wk, wv, wo, out,
        dim, kv_dim, head_dim, n_heads, n_kv_heads, gqa_ratio,
        pos, max_seq, (int)sb.u, (int)rb.u,
        wq_id, wk_id, wv_id, wo_id, weight_dtype);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

int wasm_wgpu_brandon_lm_head(const float *x, const float *w_norm,
                               const float *w_lm, float *logits,
                               int dim, int vocab, float eps)
{
    if (!js_wgpu_ready()) return -1;
    union { float f; uint32_t u; } u; u.f = eps;
    /* Use the low 31 bits of the data pointer as a stable cache key. */
    int wn_id  = (int)((uint64_t)w_norm & 0x7FFFFFFF);
    int wlm_id = (int)((uint64_t)w_lm   & 0x7FFFFFFF);
    js_wgpu_lm_head_kick(x, w_norm, w_lm, logits, dim, vocab, (int)u.u,
                          wn_id, wlm_id);
    while (!js_wgpu_op_done()) emscripten_sleep(1);
    return js_wgpu_op_ok() ? 0 : -1;
}

/* Status accessors for the bottom-bar live update. Returns pointers
 * into kernel memory — JS reads them with UTF8ToString. */
extern bool osfs2_is_mounted(void);
extern const char *osfs2_label(void);
extern uint32_t osfs2_file_count(void);
extern uint32_t osfs2_free_blocks(void);
extern uint32_t osfs2_get_block_size(void);

const char *wasm_status_fs_label(void)
{ return osfs2_is_mounted() ? osfs2_label() : ""; }

int wasm_status_fs_files(void)
{ return osfs2_is_mounted() ? (int)osfs2_file_count() : 0; }

int wasm_status_fs_free_mb(void)
{
    if (!osfs2_is_mounted()) return 0;
    uint64_t b = (uint64_t)osfs2_free_blocks() * (uint64_t)osfs2_get_block_size();
    return (int)(b / (1024 * 1024));
}

int nvme_write_bytes(uint64_t offset, const void *buf, uint64_t len) {
    if (!wasm_nvme_buf || offset + len > wasm_nvme_size) return -1;
    memcpy(wasm_nvme_buf + offset, buf, (size_t)len);
    g_nvme_dirty = true;
    return 0;
}

int nvme_flush(void) { return 0; }

/* disk_read_bytes / disk_write_bytes — bypass blkdev layer in WASM:
 * blkdev.c's version dispatches to a registered block device, but we
 * never register one. Override with direct access to the wasm_nvme_buf. */
int disk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len)
{ return nvme_read_bytes(byte_offset, buf, len); }
int disk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len)
{ return nvme_write_bytes(byte_offset, buf, len); }

/* ── Network ─────────────────────────────────────────────────── */

int  i211_init(uint64_t bar0) { (void)bar0; return -1; }
bool i211_link_up(void)       { return false; }

void net_init(const uint8_t ip[4]) { (void)ip; }
void net_poll(void) {}
int net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port,
                 const void *data, uint32_t len)
{
    (void)dst_ip; (void)dst_port; (void)src_port; (void)data; (void)len;
    return -1;
}
void net_udp_listen(uint16_t port, void *handler) { (void)port; (void)handler; }
void net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq) { (void)dst_ip; (void)seq; }
uint32_t net_icmp_get_rx_count(void) { return 0; }

/* ── xHCI USB ────────────────────────────────────────────────── */

int  xhci_init(uint64_t bar0, uint8_t bus, uint8_t dev, uint8_t func)
{
    (void)bar0; (void)bus; (void)dev; (void)func; return -1;
}
void xhci_poll(void) {}
bool xhci_is_ready(void) { return false; }

/* ── GPU / GSP ───────────────────────────────────────────────── */

typedef struct { uint64_t bar0_base; int gsp_present; } gpu_device_t;
typedef struct { int gsp_present; } gpu_probe_t;
static gpu_probe_t wasm_gpu_probe = { 0 };

void gpu_init(uint64_t bar0) { (void)bar0; }
gpu_probe_t *gpu_get_probe(void)  { return &wasm_gpu_probe; }
int  gsp_probe(void)              { return -1; }
int  gsp_load_firmware(void)      { return -1; }
int  gsp_queue_init(void)         { return -1; }
int  gsp_boot(void)               { return -1; }
void gpu_llama_benchmark_standalone(void) {}

/* ── Scheduler stubs ─────────────────────────────────────────── */

/* WASM sched_spawn: for the compositor, set up a JS requestAnimationFrame
 * loop that calls wasm_compositor_frame() (one frame per callback).
 * This avoids Asyncify conflicts with the shell's blocking loop. */
int  sched_spawn(const char *name, void (*entry)(void))
{
    (void)entry;
    /* Check if this is the compositor thread */
    bool is_comp = false;
    if (name) {
        const char *c = "compositor";
        const char *n = name;
        while (*c && *n && *c == *n) { c++; n++; }
        is_comp = (*c == '\0');
    }
    if (is_comp) {
        /* Initialize compositor state (the part before the while loop) */
        extern void compositor_init(void);
        /* Set compositor_running = true by calling the thread entry briefly?
         * No — compositor_thread sets it. Instead, call the init via extern. */
        /* Actually compositor_init was already called by the shell command.
         * Just start the rAF loop for rendering. */
        /* Start rAF loop — wasm_compositor_frame sets compositor_running
         * internally on first call via compositor_init's state. */
        extern void compositor_start_wasm(void);
        compositor_start_wasm();
        EM_ASM({
            function __compFrame() {
                Module.ccall('wasm_compositor_frame', null, [], []);
                requestAnimationFrame(__compFrame);
            }
            requestAnimationFrame(__compFrame);
        });
    }
    return 0;
}
int  sched_yield(void) { emscripten_sleep(0); return 0; }
uint64_t sched_get_switches(void) { return 0; }
bool sched_is_enabled(void)       { return false; }

/* ── GPT ─────────────────────────────────────────────────────── */

int gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size)
{
    (void)part_offset; (void)part_size; return -1;
}

/* ── Crypto ─────────────────────────────────────────────────── */

int crypto_selftest(void) { return 0; }

/* tensor_benchmark is defined in arch/x86/kernel/tensor.c */

/* ── Git stubs ───────────────────────────────────────────────── */

int git_init(void)           { return -1; }
int git_add(const char *f)   { (void)f; return -1; }
int git_commit(const char *m){ (void)m; return -1; }
int git_log(void)            { return -1; }
int git_status(void)         { return -1; }
int git_diff(void)           { return -1; }
int git_branch(const char *n){ (void)n; return -1; }
int git_checkout(const char *b){ (void)b; return -1; }

/* ══════════════════════════════════════════════════════════════
 *  Tier 2 #8 — Web Workers scaffolding
 *
 *  Spawns N dedicated Workers (one per logical core) with a tiny JS
 *  bundle that handles 'job' messages: dot product of a weight slice
 *  and an input vector. Used by future SIMD matvec parallelization
 *  for Llama-1B-class models where single-threaded dequant+dot is
 *  the bottleneck. For brandon-tiny (dim=128) the overhead dominates;
 *  this scaffolding only earns its keep on larger shapes.
 *
 *  C surface:
 *    int  wasm_workers_init(int n)      → spawn n workers (caps at 8)
 *    int  wasm_workers_count(void)      → number alive
 *    int  wasm_workers_dot_f32(const float *W, const float *x,
 *                              int rows, int cols, float *out)
 *                                       → row-parallel dot product
 *  ════════════════════════════════════════════════════════════ */
EM_JS(int, js_workers_init, (int n), {
    if (window.__pool) {
        for (var i = 0; i < window.__pool.length; i++)
            window.__pool[i].w.terminate();
    }
    var src = `
        onmessage = function(e) {
            var d = e.data;
            if (d.kind === 'dot_f32') {
                var W = new Float32Array(d.wbuf);
                var x = new Float32Array(d.xbuf);
                var r0 = d.r0, r1 = d.r1, cols = d.cols;
                var out = new Float32Array(r1 - r0);
                for (var r = r0; r < r1; r++) {
                    var s = 0;
                    var base = (r - r0) * cols;
                    for (var c = 0; c < cols; c++) s += W[base + c] * x[c];
                    out[r - r0] = s;
                }
                postMessage({ kind: 'done', r0: r0, r1: r1, out: out.buffer },
                            [out.buffer]);
            }
        };`;
    var blob = new Blob([src], { type: 'application/javascript' });
    var url = URL.createObjectURL(blob);
    window.__pool = [];
    for (var i = 0; i < n && i < 8; i++) {
        var w = new Worker(url);
        window.__pool.push({ w: w, busy: false, lastOut: null });
    }
    return window.__pool.length;
});
EM_JS(int, js_workers_count, (), {
    return window.__pool ? window.__pool.length : 0;
});

/* Synchronous parallel dot — caller blocks via Asyncify until all
 * workers report back. The weight matrix is row-major [rows, cols]
 * f32; we slice contiguous row blocks and ship one slice per worker. */
EM_JS(int, js_workers_dot_f32_kick, (const float *W, const float *x,
                                      int rows, int cols, float *out), {
    var pool = window.__pool;
    if (!pool || pool.length === 0) return -1;
    window.__poolPending = pool.length;
    window.__poolErr = 0;
    var xbuf = HEAPU8.slice(x, x + cols * 4).buffer;
    var n = pool.length;
    var per = Math.ceil(rows / n);
    for (var i = 0; i < n; i++) {
        var r0 = i * per;
        var r1 = Math.min(r0 + per, rows);
        if (r0 >= rows) {
            window.__poolPending--;
            continue;
        }
        var wptr = W + r0 * cols * 4;
        var wbuf = HEAPU8.slice(wptr, wptr + (r1 - r0) * cols * 4).buffer;
        /* Local xbuf clone so each worker has its own copy. */
        var xb = HEAPU8.slice(x, x + cols * 4).buffer;
        pool[i].busy = true;
        (function(idx, r0_, r1_) {
            pool[idx].w.onmessage = function(e) {
                if (e.data.kind === 'done') {
                    var part = new Float32Array(e.data.out);
                    HEAPU8.set(new Uint8Array(part.buffer),
                               out + r0_ * 4);
                    pool[idx].busy = false;
                    window.__poolPending--;
                }
            };
        })(i, r0, r1);
        pool[i].w.postMessage({ kind: 'dot_f32', wbuf: wbuf, xbuf: xb,
                                 r0: r0, r1: r1, cols: cols },
                                [wbuf, xb]);
    }
    return 0;
});
EM_JS(int, js_workers_pending, (), {
    return window.__poolPending ? window.__poolPending : 0;
});

int wasm_workers_init(int n)   { return js_workers_init(n); }
int wasm_workers_count(void)   { return js_workers_count(); }

int wasm_workers_dot_f32(const float *W, const float *x,
                          int rows, int cols, float *out)
{
    if (wasm_workers_count() <= 0) return -1;
    if (js_workers_dot_f32_kick(W, x, rows, cols, out) != 0) return -1;
    while (js_workers_pending() > 0) emscripten_sleep(1);
    return 0;
}

/* ── HDA audio ───────────────────────────────────────────────── */

void hda_init(void) {}

/* ── OsitoFS: in-memory virtual filesystem ───────────────────── */

/* ── OsitoFS v2: now compiled from arch/x86/fs/ositofs2.c ────── */
/* nvme_read_bytes above provides memory-backed I/O.
 * osfs2_find_gguf and osfs2_read_layer_index are in ositofs2.c */

#if 0  /* OLD mini VFS — replaced by real OsitoFS */
typedef struct {
    char          name[64];
    uint64_t      size;
    const uint8_t *data;
} wasm_vfile_t;

static const char vfs_readme[] =
    "OsitoK WebAssembly Shell\n"
    "========================\n"
    "Bare-metal AI OS compiled to wasm32 via Emscripten.\n"
    "\n"
    "Try: chat <prompt>   — local Llama inference\n"
    "     mem             — heap stats\n"
    "     cpus            — CPU info\n"
    "     uname           — system info\n"
    "     uptime          — time since load\n";

static const char vfs_hello_c[] =
    "#include <stdio.h>\n"
    "\n"
    "int main(void) {\n"
    "    printf(\"Hello from OsitoK!\\n\");\n"
    "    return 0;\n"
    "}\n";

static const char vfs_motd[] =
    "Welcome to OsitoK (wasm32 build).\n"
    "Type 'help' for available commands.\n";

static wasm_vfile_t wasm_vfs[] = {
    { "README.txt", 0, (const uint8_t *)vfs_readme  },
    { "hello.c",   0, (const uint8_t *)vfs_hello_c  },
    { "motd.txt",  0, (const uint8_t *)vfs_motd     },
    { "",          0, NULL }
};

/* Fill sizes at first use */
static void vfs_init_sizes(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; wasm_vfs[i].data; i++)
        wasm_vfs[i].size = strlen((const char *)wasm_vfs[i].data);
}

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

bool osfs2_is_mounted(void) { vfs_init_sizes(); return true; }

void osfs2_list(void)
{
    vfs_init_sizes();
    for (int i = 0; wasm_vfs[i].data; i++) {
        serial_puts("  ");
        serial_puts(wasm_vfs[i].name);
        serial_puts("\t");
        serial_putdec(wasm_vfs[i].size);
        serial_puts(" bytes\n");
    }
}

void *osfs2_find(const char *n)
{
    vfs_init_sizes();
    if (!n) return NULL;
    for (int i = 0; wasm_vfs[i].data; i++) {
        if (strcmp(wasm_vfs[i].name, n) == 0)
            return (void *)&wasm_vfs[i];
    }
    return NULL;
}

int osfs2_read(void *f, uint64_t off, void *buf, uint64_t len)
{
    wasm_vfile_t *vf = (wasm_vfile_t *)f;
    if (!vf || off >= vf->size) return 0;
    uint64_t avail = vf->size - off;
    if (len > avail) len = avail;
    memcpy(buf, vf->data + off, (size_t)len);
    return (int)len;
}

int  osfs2_mount(uint64_t off)  { (void)off; return 0; }
int  osfs2_delete(const char *n){ (void)n; return -1; }
uint32_t osfs2_free_blocks(void){ return 0; }
void osfs2_list_files(void)     {}
uint64_t osfs2_file_size(void *f)
{
    wasm_vfile_t *vf = (wasm_vfile_t *)f;
    return vf ? vf->size : 0;
}

#endif  /* OLD mini VFS */

/* ── sys_caps (hardware-derived resource limits) ─────────────── */

typedef struct {
    uint64_t total_ram;     uint64_t available_ram;
    uint32_t cpu_count;     uint32_t page_count;
    uint64_t heap_init_size; uint64_t heap_grow_size;
    uint64_t brk_heap_size;  uint64_t user_stack_size;
    uint32_t max_processes;  uint32_t max_fds_global;
    uint64_t elf_max_size;   uint64_t elf_max_alloc;
    uint64_t win32_heap_size; uint64_t crt_pool_size;
    uint64_t win32_va_limit;  uint32_t tcp_max_conns;
} sys_caps_t;

sys_caps_t g_sys_caps;

void sys_caps_init(void)
{
    g_sys_caps.total_ram       = 256ULL * 1024 * 1024;
    g_sys_caps.available_ram   = 240ULL * 1024 * 1024;
    g_sys_caps.cpu_count       = 1;
    g_sys_caps.page_count      = (uint32_t)(256 * 1024 * 1024 / 4096);
    g_sys_caps.heap_init_size  = 4 * 1024 * 1024;
    g_sys_caps.heap_grow_size  = 1 * 1024 * 1024;
    g_sys_caps.brk_heap_size   = 16 * 1024 * 1024;
    g_sys_caps.user_stack_size = 1 * 1024 * 1024;
    g_sys_caps.max_processes   = 4;
    g_sys_caps.max_fds_global  = 128;
    g_sys_caps.elf_max_size    = 64 * 1024 * 1024;
    g_sys_caps.elf_max_alloc   = 64 * 1024 * 1024;
    g_sys_caps.win32_heap_size = 16 * 1024 * 1024;
    g_sys_caps.crt_pool_size   = 4 * 1024 * 1024;
    g_sys_caps.win32_va_limit  = 256ULL * 1024 * 1024;
    g_sys_caps.tcp_max_conns   = 8;
}

int sys_caps_check_alloc(uint64_t bytes, const char *what)
{
    (void)bytes; (void)what; return 1;
}

/* mem_get_used / mem_free_pages / kmalloc / kfree / kcalloc / krealloc
   are provided by hal/mem.c */

/* TCP state constants from net.h — duplicated here since net.h pulls
 * in the full kernel network typedefs we don't want. */
#define TCP_CLOSED      0
#define TCP_ESTABLISHED 2

/* ── Network TCP — WS-tunneled bridge ────────────────────────────
 *
 * The native net.c speaks raw TCP through nic.c (I211 hardware).
 * In WASM there's no raw IP access, so we tunnel through a Cloudflare
 * Worker proxy (tools/tcp-proxy-worker.js) that bridges WebSocket
 * frames ↔ TCP bytes. Conventions:
 *   - conn_id IS the WebSocket handle (int) returned by wasm_ws_open
 *   - net_tcp_connect takes IPv4 which is useless here — use
 *     wasm_tcp_connect_host(host, port) instead
 *   - state translates ws state → TCP_* constants from net.h
 * ──────────────────────────────────────────────────────────────── */

extern int wasm_ws_open(const char *url);
extern int wasm_ws_state(int handle);
extern int wasm_ws_wait_open(int handle, int timeout_ms);
extern int wasm_ws_send(int handle, const void *data, int len);
extern int wasm_ws_recv_wait(int handle, void *dst, int max, int timeout_ms);
extern void wasm_ws_close(int handle);

/* Mutable proxy URL template — set by `tcp proxy <url>` shell.
 * Default empty so a kernel without a deployed proxy fails clean. */
static char g_tcp_proxy_url[256] =
    "wss://tcp-proxy.naranjositos.tech/?host={host}&port={port}";

void wasm_tcp_set_proxy(const char *url) {
    int n = 0;
    while (url && url[n] && n < (int)sizeof(g_tcp_proxy_url) - 1) {
        g_tcp_proxy_url[n] = url[n]; n++;
    }
    g_tcp_proxy_url[n] = '\0';
}

const char *wasm_tcp_get_proxy(void) { return g_tcp_proxy_url; }

/* Substitute {host}/{port} into the proxy template, then open WS. */
int wasm_tcp_connect_host(const char *host, uint16_t port)
{
    if (!host || !*host) return -1;

    char url[512];
    int up = 0;
    char portbuf[8];
    int pn = 0;
    {
        uint16_t v = port;
        if (v == 0) portbuf[pn++] = '0';
        else {
            char tmp[8]; int t = 0;
            while (v) { tmp[t++] = '0' + v % 10; v /= 10; }
            while (t) portbuf[pn++] = tmp[--t];
        }
        portbuf[pn] = '\0';
    }

    const char *t = g_tcp_proxy_url;
    while (*t && up < (int)sizeof(url) - 1) {
        if (t[0] == '{' && t[1] == 'h' && t[2] == 'o' &&
            t[3] == 's' && t[4] == 't' && t[5] == '}') {
            const char *h = host;
            while (*h && up < (int)sizeof(url) - 1) url[up++] = *h++;
            t += 6;
        } else if (t[0] == '{' && t[1] == 'p' && t[2] == 'o' &&
                   t[3] == 'r' && t[4] == 't' && t[5] == '}') {
            for (int i = 0; i < pn && up < (int)sizeof(url) - 1; i++)
                url[up++] = portbuf[i];
            t += 6;
        } else {
            url[up++] = *t++;
        }
    }
    url[up] = '\0';

    int h = wasm_ws_open(url);
    if (h <= 0) return -1;
    if (wasm_ws_wait_open(h, 5000) < 0) {
        wasm_ws_close(h);
        return -1;
    }
    return h;
}

/* IPv4 connect — not supported in WASM (no raw IP). Returns -1 so
 * native code that wraps DNS+connect either falls back to JS fetch
 * or fails cleanly. */
int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port)
     { (void)dst_ip; (void)dst_port; (void)src_port; return -1; }

/* The conn handle IS the WS handle. */
int  net_tcp_send(int conn, const void *data, uint32_t len)
     { return conn > 0 ? wasm_ws_send(conn, data, (int)len) : -1; }

int  net_tcp_recv(int conn, void *buf, uint32_t buf_size)
     {
        if (conn <= 0) return -1;
        /* Non-blocking poll — match the native semantics: 0 if nothing,
         * else bytes copied, -1 if connection died. */
        int s = wasm_ws_state(conn);
        if (s >= 3) return -1;
        return wasm_ws_recv_wait(conn, buf, (int)buf_size, 0);
     }

int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size, uint32_t timeout_ticks)
     {
        if (conn <= 0) return -1;
        /* Native uses ticks (~10 ms each); we treat as ms directly. */
        int rc = wasm_ws_recv_wait(conn, buf, (int)buf_size, (int)timeout_ticks * 10);
        if (rc < 0) return -1;
        return rc;
     }

void net_tcp_close(int conn) { if (conn > 0) wasm_ws_close(conn); }

int  net_tcp_state(int conn) {
    if (conn <= 0) return TCP_CLOSED;
    int s = wasm_ws_state(conn);
    if (s == 1) return TCP_ESTABLISHED;
    return TCP_CLOSED;
}

int  net_dns_resolve(const char *hostname, uint8_t ip_out[4])
     { (void)hostname; (void)ip_out; return -1; }
int  net_tcp_listen(uint16_t port) { (void)port; return -1; }
int  net_tcp_accept(int listener, uint32_t timeout_ticks)
     { (void)listener; (void)timeout_ticks; return -1; }
void net_tcp_stop_listen(int listener) { (void)listener; }

/* ── HTTP ────────────────────────────────────────────────────── */

int  http_open(void *session, const char *hostname)
     { (void)session; (void)hostname; return -1; }
int  http_request(void *session, const char *method, const char *path,
                  const char *hostname, const char *const *req_headers,
                  const void *body, uint32_t body_len, void *resp)
     { (void)session; (void)method; (void)path; (void)hostname;
       (void)req_headers; (void)body; (void)body_len; (void)resp; return -1; }
int  http_read_body_full(void *session, const void *resp, void *buf, uint32_t buf_size)
     { (void)session; (void)resp; (void)buf; (void)buf_size; return -1; }
void http_close(void *session) { (void)session; }
const char *http_get_header(const void *resp, const char *name)
     { (void)resp; (void)name; return NULL; }
uint32_t http_session_size(void)  { return 256; }
uint32_t http_response_size(void) { return 64; }

/* ── TLS ─────────────────────────────────────────────────────── */

uint32_t tls_conn_size(void) { return 256; }
int  tls_connect(void *tls, int tcp_conn, const char *hostname)
     { (void)tls; (void)tcp_conn; (void)hostname; return -1; }
int  tls_send(void *tls, const void *data, uint32_t len)
     { (void)tls; (void)data; (void)len; return -1; }
int  tls_recv(void *tls, void *buf, uint32_t buf_size, uint32_t timeout_ticks)
     { (void)tls; (void)buf; (void)buf_size; (void)timeout_ticks; return -1; }
void tls_close(void *tls) { (void)tls; }

/* ── Claude API (WASM = JS fetch bridge) ─────────────────────────
 *
 * The native build has a full TLS 1.2/1.3 client + HTTP framing in
 * tls.c/tls13.c/http.c/claude.c. WASM lifts those out and delegates
 * the network leg to fetch(). Conversation history is kept in a
 * session struct as a single accumulating JSON `messages` array.
 *
 * No streaming yet — we POST and call the user callback once with
 * the full assistant text. Multi-turn works because we re-send the
 * whole history each turn (cheap at typical chat lengths).
 * ──────────────────────────────────────────────────────────────── */

static char g_claude_key[256] = {0};

void claude_set_api_key(const char *key)
{
    int i = 0;
    while (key && key[i] && i < (int)sizeof(g_claude_key) - 1) {
        g_claude_key[i] = key[i]; i++;
    }
    g_claude_key[i] = '\0';
}

const char *claude_get_api_key(void)
{
    return g_claude_key[0] ? g_claude_key : NULL;
}

#define CLAUDE_HISTORY_MAX (32 * 1024)

typedef struct {
    /* Accumulating JSON: "{\"role\":\"user\",\"content\":\"…\"},…"
     * We assemble the final messages array body at send time. */
    char buf[CLAUDE_HISTORY_MAX];
    int  len;
    int  n_msgs;
} claude_session_t;

void *claude_session_new(void)
{
    extern void *malloc(size_t);
    claude_session_t *s = (claude_session_t *)malloc(sizeof(*s));
    if (!s) return NULL;
    s->buf[0] = '\0';
    s->len = 0;
    s->n_msgs = 0;
    return s;
}

void claude_session_free(void *s)
{
    extern void free(void *);
    if (s) free(s);
}

void claude_session_clear(void *s)
{
    claude_session_t *cs = (claude_session_t *)s;
    if (cs) { cs->buf[0] = '\0'; cs->len = 0; cs->n_msgs = 0; }
}

/* Append a JSON-escaped string into dst[*pos], advance *pos.
 * Escapes: " \ \n \r \t and control chars as \uXXXX. */
static void json_escape_into(char *dst, int max, int *pos, const char *src)
{
    int p = *pos;
    while (*src && p < max - 8) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
        case '"':  dst[p++]='\\'; dst[p++]='"';  break;
        case '\\': dst[p++]='\\'; dst[p++]='\\'; break;
        case '\n': dst[p++]='\\'; dst[p++]='n';  break;
        case '\r': dst[p++]='\\'; dst[p++]='r';  break;
        case '\t': dst[p++]='\\'; dst[p++]='t';  break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                dst[p++]='\\'; dst[p++]='u'; dst[p++]='0'; dst[p++]='0';
                dst[p++]=hex[(c>>4)&0xF]; dst[p++]=hex[c&0xF];
            } else {
                dst[p++] = (char)c;
            }
        }
    }
    *pos = p;
}

extern int wasm_http_request(const char *url, const char *method,
                              const char *headers_json, const char *body,
                              uint8_t **out_buf, int *out_len);

/* Forward decls for the streaming bridge defined later in this file. */
extern int js_stream_kick(const char *url, const char *headers_json,
                           const char *body);
extern int js_stream_done(void);
extern int js_stream_status(void);
extern int js_stream_q_size(void);
extern int js_stream_q_pop(char *dst, int max);

/* Pull the assistant text out of Anthropic's JSON response. Looks for
 * "text":"…" inside the first content[] object with type=text. */
static int extract_assistant_text(const char *json, char *out, int max)
{
    const char *p = json;
    const char *needle = "\"text\":\"";
    int nlen = 8;
    while (*p) {
        int match = 1;
        for (int i = 0; i < nlen; i++) if (p[i] != needle[i]) { match = 0; break; }
        if (match) { p += nlen; break; }
        p++;
    }
    if (!*p) return -1;
    int n = 0;
    while (*p && n < max - 1) {
        if (*p == '\\') {
            char e = p[1];
            if (e == 'n') { out[n++] = '\n'; p += 2; }
            else if (e == 't') { out[n++] = '\t'; p += 2; }
            else if (e == 'r') { out[n++] = '\r'; p += 2; }
            else if (e == '"') { out[n++] = '"';  p += 2; }
            else if (e == '\\') { out[n++] = '\\'; p += 2; }
            else if (e == 'u') {
                /* \uXXXX — naive: emit as ASCII if codepoint < 128 */
                if (p[2] && p[3] && p[4] && p[5]) {
                    unsigned cp = 0;
                    for (int k = 2; k < 6; k++) {
                        char c = p[k]; cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= c - '0';
                        else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                    }
                    if (cp < 0x80) out[n++] = (char)cp;
                    p += 6;
                } else { p++; }
            } else { out[n++] = e; p += 2; }
        } else if (*p == '"') {
            break;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n;
}

int claude_session_send_with_tools(void *s, const char *user_msg,
                                    int (*callback)(const char *, uint32_t, void *),
                                    void *ctx)
{
    if (!s || !user_msg || !callback) return -1;
    if (!claude_get_api_key()) {
        const char *m = "[claude] no API key set — run: apikey sk-ant-...\n";
        callback(m, 50, ctx);
        return -1;
    }

    claude_session_t *cs = (claude_session_t *)s;

    /* 1. Append the user turn to history (JSON message form). */
    if (cs->n_msgs > 0 && cs->len < CLAUDE_HISTORY_MAX - 1)
        cs->buf[cs->len++] = ',';
    {
        const char *prefix = "{\"role\":\"user\",\"content\":\"";
        for (const char *p = prefix; *p; p++)
            if (cs->len < CLAUDE_HISTORY_MAX - 1) cs->buf[cs->len++] = *p;
        json_escape_into(cs->buf, CLAUDE_HISTORY_MAX, &cs->len, user_msg);
        const char *suffix = "\"}";
        for (const char *p = suffix; *p; p++)
            if (cs->len < CLAUDE_HISTORY_MAX - 1) cs->buf[cs->len++] = *p;
        cs->buf[cs->len] = '\0';
    }
    cs->n_msgs++;

    /* 2. Build the request body: messages array wrapped in JSON.
     *    stream=true so we get SSE chunks instead of one blob. */
    static char body[CLAUDE_HISTORY_MAX + 1024];
    int bp = 0;
    const char *header =
        "{\"model\":\"claude-haiku-4-5-20251001\","
        "\"max_tokens\":1024,"
        "\"stream\":true,"
        "\"messages\":[";
    for (const char *p = header; *p; p++) body[bp++] = *p;
    for (int i = 0; i < cs->len; i++) body[bp++] = cs->buf[i];
    body[bp++] = ']';
    body[bp++] = '}';
    body[bp]   = '\0';

    /* 3. Build headers JSON. */
    static char headers[1024];
    {
        int p = 0;
        const char *prefix =
            "{\"content-type\":\"application/json\","
            "\"anthropic-version\":\"2023-06-01\","
            "\"x-api-key\":\"";
        for (const char *q = prefix; *q; q++) headers[p++] = *q;
        for (const char *q = g_claude_key; *q && p < (int)sizeof(headers) - 4; q++)
            headers[p++] = *q;
        headers[p++] = '"';
        headers[p++] = '}';
        headers[p]   = '\0';
    }

    /* 4. POST + drain SSE chunks as they arrive. */
    js_stream_kick("https://api.anthropic.com/v1/messages", headers, body);

    /* Accumulate the assistant text for history while streaming. */
    static char text[16 * 1024];
    int tlen = 0;

    while (!js_stream_done() || js_stream_q_size() > 0) {
        if (js_stream_q_size() == 0) {
            emscripten_sleep(20);
            continue;
        }
        char ev[4096];
        int n = js_stream_q_pop(ev, sizeof(ev));
        if (n <= 0) continue;

        /* Each event payload is a JSON object like:
         *   {"type":"content_block_delta","delta":{"text":"..."}}
         * Extract the inner text via the same naive parser used for
         * the non-streaming response. */
        if (js_stream_status() && js_stream_status() / 100 != 2) continue;

        char chunk[2048];
        int clen = extract_assistant_text(ev, chunk, sizeof(chunk));
        if (clen > 0) {
            callback(chunk, (uint32_t)clen, ctx);
            for (int i = 0; i < clen && tlen < (int)sizeof(text) - 1; i++)
                text[tlen++] = chunk[i];
            text[tlen] = '\0';
        }
    }

    int status = js_stream_status();
    if (status / 100 != 2) {
        char codebuf[64];
        int n = 0;
        const char *p = "[claude] HTTP ";
        while (*p && n < 60) codebuf[n++] = *p++;
        int v = status, t = 0; char tmp[8];
        if (v == 0) tmp[t++] = '0';
        else while (v && t < 8) { tmp[t++] = '0' + v % 10; v /= 10; }
        while (t) codebuf[n++] = tmp[--t];
        codebuf[n++] = '\n';
        codebuf[n] = '\0';
        callback(codebuf, n, ctx);
        return -1;
    }

    if (tlen == 0) {
        const char *m = "[claude] empty response\n";
        callback(m, 24, ctx);
        return -1;
    }

    /* 7. Append assistant turn to history for next call. */
    if (cs->len < CLAUDE_HISTORY_MAX - 1) cs->buf[cs->len++] = ',';
    {
        const char *prefix = "{\"role\":\"assistant\",\"content\":\"";
        for (const char *p = prefix; *p; p++)
            if (cs->len < CLAUDE_HISTORY_MAX - 1) cs->buf[cs->len++] = *p;
        json_escape_into(cs->buf, CLAUDE_HISTORY_MAX, &cs->len, text);
        const char *suffix = "\"}";
        for (const char *p = suffix; *p; p++)
            if (cs->len < CLAUDE_HISTORY_MAX - 1) cs->buf[cs->len++] = *p;
        cs->buf[cs->len] = '\0';
    }
    cs->n_msgs++;
    return 0;
}

int claude_session_send(void *s, const char *user_msg,
                         int (*callback)(const char *, uint32_t, void *), void *ctx)
{
    return claude_session_send_with_tools(s, user_msg, callback, ctx);
}

int claude_chat(const void *messages, int msg_count, const char *model,
                int max_tokens, int (*callback)(const char *, uint32_t, void *),
                void *ctx)
{
    /* Single-shot: stand up a temp session, send, free. messages is an
     * array of {role, content} pairs; we use only the last user msg. */
    (void)model; (void)max_tokens;
    if (msg_count <= 0 || !messages) return -1;
    struct cmsg { const char *role; const char *content; };
    const struct cmsg *m = (const struct cmsg *)messages;
    const char *last_user = NULL;
    for (int i = msg_count - 1; i >= 0; i--) {
        if (m[i].role && m[i].role[0] == 'u') { last_user = m[i].content; break; }
    }
    if (!last_user) return -1;
    void *sess = claude_session_new();
    if (!sess) return -1;
    int rc = claude_session_send(sess, last_user, callback, ctx);
    claude_session_free(sess);
    return rc;
}

/* ── SMP CPU info ────────────────────────────────────────────── */

uint32_t smp_cpu_count(void) { return 1; }

typedef struct {
    uint32_t apic_id;
    uint32_t cpu_index;
    bool     online;
    bool     bsp;
    uint64_t stack_top;
} cpu_info_t;
static cpu_info_t wasm_cpu_info = { 0, 0, true, true, 0 };
cpu_info_t *smp_cpu_info(uint32_t index) { (void)index; return &wasm_cpu_info; }

/* ── Scheduler test threads ──────────────────────────────────── */

void sched_test_a(void) {}
void sched_test_b(void) {}

/* ── Win32 + setjmp ──────────────────────────────────────────── */

int  win32_exec(const char *filename) { (void)filename; return -1; }
int  kern_setjmp(uint64_t *buf) { (void)buf; return 0; }  /* never restores */
uint64_t *compat32_crash_jmpbuf = NULL;

/* ── saved_boot_info (normally set by UEFI bootloader) ────────── */

typedef struct { uint32_t width; uint32_t height; uint32_t pitch; uint32_t pixel_format; } boot_display_mode_t;
typedef struct {
    uint32_t magic; uint32_t version;
    uint64_t fb_base; uint32_t fb_width; uint32_t fb_height; uint32_t fb_pitch;
    uint64_t mmap_addr; uint64_t mmap_size; uint64_t mmap_desc_size;
    uint32_t mmap_desc_ver; uint32_t _pad0;
    uint64_t acpi_rsdp;
    uint64_t kernel_phys_base; uint64_t kernel_size;
    uint32_t display_mode_count; uint32_t display_current_mode;
    boot_display_mode_t display_modes[16];
} boot_info_t;
boot_info_t saved_boot_info;  /* zero-initialized: WASM has no UEFI */

/* ── Compositor deps (called by compositor.c) ───────────────── */

void proc_set_qos(uint8_t qos) { (void)qos; }
uint32_t proc_count_active(void) { return 1; }

/* Keyboard push (compositor routes key events to shell ring buffer) */
void kb_push(uint8_t scancode) { (void)scancode; }
void kb_push_esc(const char *seq) { (void)seq; }

/* HID scancode-to-ASCII tables (referenced by compositor key handler) */
const char hid_normal[256] = {0};
const char hid_shifted[256] = {0};

/* Memory stats — mem_get_total/mem_get_used provided by hal/mem.c */
uint64_t mem_get_free(void)  { extern uint64_t mem_get_total(void); extern uint64_t mem_get_used(void); return mem_get_total() - mem_get_used(); }

/* ── Dynamic linker ──────────────────────────────────────────── */

void *dl_open(const char *filename) { (void)filename; return NULL; }
void *dl_sym(void *handle, const char *name) { (void)handle; (void)name; return NULL; }
int   dl_close(void *handle) { (void)handle; return -1; }
void  dl_list_modules(void) { extern void serial_puts(const char *); serial_puts("  (no modules)\n"); }
void *dl_find(const char *name) { (void)name; return NULL; }

/* ── HDA audio ───────────────────────────────────────────────── */

bool hda_is_ready(void) { return false; }
void hda_play_tone(uint32_t freq, uint32_t ms) { (void)freq; (void)ms; }

/* ── OsitoFS write/create/file_at: now in ositofs2.c ─────────── */

/* ── kexec trampoline (normally from kexec_tramp.S) ─────────── */

void kexec_trampoline(void) {}
void kexec_trampoline_end(void) {}
/* osfs2_file_name: now in ositofs2.c */

/* ── prompt_llama (set by main on x86; NULL until inference loads) */

void *prompt_llama = NULL;

/* ── osito_chat_sync: kernel-side chat for the worker bridge ─────
 *
 * Invoked from JS via Module.ccall when the user wasm program calls
 * oi_chat(...) and the worker postMessages 'osito-chat'. Synchronous
 * (blocks main thread for 1-5s). Output stored in a static buffer
 * whose pointer we return — JS reads it back as a string. */

/* Avoid the function-pointer indirect-call to llama_chat (MAIN_MODULE
 * function-table issues): route through shell_exec("chat ...") and
 * capture the output via the existing shell redirect mechanism. */

#define OSITO_CHAT_OUT 8192
static char osito_chat_buf[OSITO_CHAT_OUT];
static int  osito_chat_pos;

/* shell.c statics we hijack temporarily */
extern char    *redir_buf;
extern uint32_t redir_pos;
extern uint32_t redir_max;
typedef void (*sh_redir_fn_t)(const char *, size_t);
extern sh_redir_fn_t sh_redir_fn;
extern void redir_capture(const char *s, size_t len);

EMSCRIPTEN_KEEPALIVE
const char *osito_chat_sync(const char *prompt)
{
    /* Kept as a stub for direct JS testing (Module.ccall path), but
     * doesn't actually run inference: that path hits a wasm function
     * table indirect-call error under MAIN_MODULE+Asyncify. The real
     * entry is osito_kernel_poll() below, called from kb_getchar so
     * inference runs inside the kernel's natural Asyncify-aware
     * execution context. */
    (void)prompt;
    osito_chat_buf[0] = 0;
    const char *m = "[oi_chat] use poll path (osito_kernel_poll)";
    memcpy(osito_chat_buf, m, strlen(m) + 1);
    return osito_chat_buf;
}

/* ── Poll-based oi_chat bridge ───────────────────────────────────
 *
 * SAB layout (Int32 indices):
 *   [0] = state (0 idle, 1 request, 2 response)
 *   [1] = prompt length
 *   [2] = response length
 *   bytes [16..16+4096) = prompt
 *   bytes [16+4096..16+4096+8192) = response
 *
 * The cc-worker writes a prompt and Atomics.wait's on state==1.
 * Here in the kernel, kb_getchar's emscripten_sleep loop polls
 * osito_kernel_poll() which checks the flag, runs the inference
 * via shell_exec("chat ..."), writes the response back, sets state=2,
 * Atomics.notify's the worker. Because the inference happens inside
 * the kernel's running execution (not entered via Module.ccall), the
 * Asyncify state is intact and llama_chat works normally.
 */

EM_JS(int, osito_poll_request_len, (), {
    if (!window.__ositoSab) return 0;
    var i32 = new Int32Array(window.__ositoSab);
    return Atomics.load(i32, 0) === 1 ? i32[1] : 0;
});

EM_JS(void, osito_poll_get_prompt, (char *dst, int max), {
    if (!window.__ositoSab) return;
    var i32 = new Int32Array(window.__ositoSab);
    var n = Math.min(i32[1], max);
    var src = new Uint8Array(window.__ositoSab, 16, n);
    HEAPU8.set(src, dst);
});

EM_JS(void, osito_poll_finish, (const char *src, int len), {
    if (!window.__ositoSab) return;
    var i32 = new Int32Array(window.__ositoSab);
    var dst = new Uint8Array(window.__ositoSab, 16 + 4096, 8192);
    var n = Math.min(len, 8192);
    dst.set(HEAPU8.subarray(src, src + n));
    i32[2] = n;
    Atomics.store(i32, 0, 2);
    Atomics.notify(i32, 0);
});

extern int llama_chat(void *state, const char *text, uint32_t max_tokens,
                      void (*on_token)(const char *text, void *ctx),
                      void *ctx);

EMSCRIPTEN_KEEPALIVE
void osito_chat_token_cb(const char *piece, void *ctx)
{
    (void)ctx;
    if (!piece) return;
    int len = (int)strlen(piece);
    if (osito_chat_pos + len + 1 >= OSITO_CHAT_OUT) return;
    memcpy(osito_chat_buf + osito_chat_pos, piece, (size_t)len);
    osito_chat_pos += len;
    osito_chat_buf[osito_chat_pos] = 0;
}

EMSCRIPTEN_KEEPALIVE
void osito_kernel_poll(void)
{
    int plen = osito_poll_request_len();
    if (plen <= 0) return;

    char prompt[4096];
    if (plen >= (int)sizeof(prompt)) plen = sizeof(prompt) - 1;
    osito_poll_get_prompt(prompt, plen);
    prompt[plen] = 0;

    osito_chat_pos = 0;
    osito_chat_buf[0] = 0;

    if (prompt_llama) {
        /* Direct call to llama_chat — same Asyncify context as the
         * shell's own `chat` command, so the indirect call to the
         * token callback works. 8 tokens cap (browser is slow without
         * SIMD; user can chain multiple calls if more output needed). */
        llama_chat(prompt_llama, prompt, 8, osito_chat_token_cb, NULL);
    } else {
        const char *m = "[oi_chat] no model";
        memcpy(osito_chat_buf, m, strlen(m));
        osito_chat_pos = strlen(m);
    }

    osito_poll_finish(osito_chat_buf, osito_chat_pos);
}

/* ──────────────────────────────────────────────────────────────────
 * Stubs for x86 subsystems added since 2026-04-01.
 * Symbols referenced from shell.c / compositor.c / inference.c (which
 * we compile) but whose real impl is HW-bound or x86-asm-bound.
 * ────────────────────────────────────────────────────────────────── */

void sshd_init(void) {}
void sshd_poll(void) {}
int  tls13_connect(void *t, int c, const char *h) { (void)t; (void)c; (void)h; return -1; }
void tls13_close(void *t) { (void)t; }
void dhcp_init(void) {}
void dhcp_request(void) {}
int  dhcp_get_ip(uint8_t ip[4]) { (void)ip; return -1; }
int  dhcp_lease_remaining(void) { return 0; }
void ntp_init(void) {}
int  ntp_sync(void) { return -1; }
uint64_t ntp_get_unix_time(void) { return 0; }
void ipv6_init(void) {}
void mdns_init(const char *hostname) { (void)hostname; }
void mdns_poll(void) {}
void apipa_init(void) {}
void nf_init(void) {}
int  nf_add_rule(int chain, int proto, uint32_t src, uint32_t dst, uint16_t port, int verdict)
{ (void)chain; (void)proto; (void)src; (void)dst; (void)port; (void)verdict; return -1; }
void nf_list(void) {}
void nf_clear(void) {}
void nic_init(void) {}
void nic_stats(void) { extern void serial_puts(const char *); serial_puts("  (no NIC in WASM)\n"); }

void cpu_features_init(void) {}
bool cpu_has_avx2(void)   { return false; }
bool cpu_has_avx512(void) { return false; }
bool cpu_has_fma(void)    { return false; }
bool cpu_has_rdrand(void) { return false; }
bool cpu_has_rdseed(void) { return false; }
const char *cpu_vendor_string(void) { return "Emscripten/WASM"; }
const char *cpu_brand_string(void)  { return "WebAssembly virtual CPU"; }
void cpu_topology_init(void) {}
uint32_t cpu_topology_socket_count(void)        { return 1; }
uint32_t cpu_topology_core_count(uint32_t sock) { (void)sock; return 1; }
uint32_t cpu_topology_thread_count(uint32_t s, uint32_t c) { (void)s; (void)c; return 1; }
void dispatch_init(void) {}

#include "hwbp.h"
hwbp_t hwbps[4] = {{0}};
void hwbp_init(void) {}
int  hwbp_set(int slot, uint64_t addr, hwbp_cond_t cond, hwbp_len_t len, const char *name)
{ (void)slot; (void)addr; (void)cond; (void)len; (void)name; return -1; }
int  hwbp_clear(int slot) { (void)slot; return -1; }
void hwbp_clear_all(void) {}
void hwbp_list(void) {}
void self_opt_init(void) {}
int  self_opt_register_branch(void *site, const char *name) { (void)site; (void)name; return 0; }
void self_opt_apply(void) {}
void self_opt_status(void) {}
void spec_init(void) {}
void spec_analyze_init(void) {}
void spec_prefetch_init(void) {}
void spec_tls_init(void) {}
int  spec_record(uint64_t pc, uint64_t target) { (void)pc; (void)target; return 0; }
int  spec_prefetch(uint64_t pc) { (void)pc; return 0; }
void pred_sched_init(void) {}
void pred_record(uint32_t pid, uint64_t ts) { (void)pid; (void)ts; }
void pred_prewarm(uint32_t pid) { (void)pid; }
void io_predict_init(void) {}
int  io_predict_record(const char *path) { (void)path; return 0; }
int  io_predict_lookup(const char *path) { (void)path; return -1; }

int tensor_arena_init(void *a, uint64_t size_mb) { (void)a; (void)size_mb; return 0; }
void *tensor_arena_alloc(void *a, uint64_t bytes, uint64_t align)
{ (void)a; (void)align; return malloc((size_t)bytes); }
void  tensor_arena_reset(void *a) { (void)a; }
uint64_t tensor_arena_used(void *a) { (void)a; return 0; }
void sys_inference_init(void) {}
int64_t sys_inference_dispatch(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)op; (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }

void coredump_init(void) {}
int  coredump_write(const char *path, void *regs) { (void)path; (void)regs; return -1; }
void crash_report_init(void) {}
void crash_report_save(const char *reason) { (void)reason; }
void crash_report_show(void) {}
void audio_sched_init(void) {}
void audio_sched_submit(void *frame, uint32_t bytes, uint64_t deadline)
{ (void)frame; (void)bytes; (void)deadline; }
void dma_sched_init(void) {}
void dma_sched_enqueue(int dev, int prio, void *req) { (void)dev; (void)prio; (void)req; }
void pci_hotplug_init(void) {}
void pci_hotplug_rescan(void) {}
void smp_work_init(void) {}
int  smp_submit_ff(void (*fn)(void *), void *arg) { if (fn) fn(arg); return 0; }
void vdso_init(void) {}
uint64_t vdso_clock_gettime(int id) { (void)id; return idt_get_ticks() * 10000000ULL; }
void kmod_init(void) {}
int  kmod_load(const char *name, const uint8_t *data, uint64_t data_len)
{ (void)name; (void)data; (void)data_len; return -1; }
int  kmod_unload(const char *name) { (void)name; return -1; }
void kmod_list(void) {}
void wayland_init(void) {}
void wayland_poll(void) {}
void evdev_init(void) {}
int  evdev_open(const char *path) { (void)path; return -1; }
void evdev_poll(void) {}
void pty_init(void) {}
int  pty_open(int *master, int *slave) { (void)master; (void)slave; return -1; }
void fuse_init(void) {}
int  fuse_register(const char *name, void *ops) { (void)name; (void)ops; return -1; }

void elf_init(void) {}
int  elf_load(const char *path, void **out_entry) { (void)path; (void)out_entry; return -1; }
int  proc_is_executing(const char *path) { (void)path; return 0; }

int  kexec_load(const char *path) { (void)path; return -1; }
void kexec_jump(void) {}

void power_shutdown(void) { EM_ASM({ if (typeof window !== 'undefined') window.close(); }); }
void power_reboot(void)   { EM_ASM({ if (typeof window !== 'undefined') location.reload(); }); }
uint32_t power_cpu_freq_mhz(void) { return 0; }

void http_init(void) {}
void claude_init(void) {}

/* AVX2 matvec — stub never executes (cpu_has_avx2() = false → scalar path) */
void matvec_q4_0_avx2(float *out, const void *weight, const float *input,
                      uint32_t rows, uint32_t cols)
{
    (void)out; (void)weight; (void)input; (void)rows; (void)cols;
}

void initramfs_init(void) {}
int  initramfs_extract(const void *cpio, uint64_t size) { (void)cpio; (void)size; return -1; }

int  dos_run(const char *filename, int argc, const char **argv)
{ (void)filename; (void)argc; (void)argv; return -1; }
int  pe_load(const char *path) { (void)path; return -1; }

int  virtio_init(void) { return -1; }
int  virtio_blk_init(void) { return -1; }
int  virtio_net_init(void) { return -1; }
int  virtio_gpu_init(void) { return -1; }
int  virtio_gpu_3d_init(void) { return -1; }

int  usb_storage_init(void) { return -1; }
void usb_hid_poll(void) {}

/* ── cc / clang.wasm bridge ──────────────────────────────────────
 *
 * `cc` shell command: reads a C source file from OsitoFS and submits
 * it to window.__cc.compileLinkRun() (defined in shell.html), which
 * lazy-loads clang.wasm + lld.wasm + sysroot.tar from R2 and runs them
 * in a Web Worker. Output streams back via __ccPending → serial_puts.
 * Like `tcc -run`: compile + link + execute inline. */

EM_JS(void, js_cc_kick, (const char *src, const char *lang), {
    var s = UTF8ToString(src);
    var l = UTF8ToString(lang);
    window.__ccPending = '';
    window.__ccDone = false;
    window.__cc.onWrite = function(chunk) { window.__ccPending += chunk; };
    window.__cc.compileLinkRun(s, l).then(function() { window.__ccDone = true; });
});

EM_JS(void, js_cc_preprocess, (const char *src, const char *lang), {
    var s = UTF8ToString(src);
    var l = UTF8ToString(lang);
    window.__ccPending = '';
    window.__ccDone = false;
    window.__cc.onWrite = function(chunk) { window.__ccPending += chunk; };
    window.__cc.preprocess(s, l).then(function() { window.__ccDone = true; });
});

EM_JS(int, js_cc_done, (), { return window.__ccDone ? 1 : 0; });

EM_JS(int, js_cc_drain, (char *dst, int max), {
    var p = window.__ccPending || '';
    if (!p.length) return 0;
    var n = Math.min(p.length, max);
    var slice = p.substring(0, n);
    window.__ccPending = p.substring(n);
    var bytes = new TextEncoder().encode(slice);
    var copy = Math.min(bytes.length, max);
    HEAPU8.set(bytes.subarray(0, copy), dst);
    return copy;
});

/* Compile + link only (no run). Result polled via js_cc_done; bytes
 * fetched via js_cc_compiled_size + js_cc_compiled_get. */
EM_JS(void, js_cc_compile, (const char *src, const char *lang, int object_only), {
    var s = UTF8ToString(src);
    var l = UTF8ToString(lang);
    window.__ccDone = false;
    window.__ccCompiledBytes = null;
    window.__ccCompileError = null;
    window.__cc.compile(s, l, !!object_only).then(function(res) {
        if (res.error) window.__ccCompileError = res.error;
        else window.__ccCompiledBytes = new Uint8Array(res.wasm);
        window.__ccDone = true;
    });
});

EM_JS(int, js_cc_compiled_size, (), {
    return window.__ccCompiledBytes ? window.__ccCompiledBytes.length : 0;
});

EM_JS(int, js_cc_compile_error, (char *dst, int max), {
    var e = window.__ccCompileError || '';
    if (!e.length) return 0;
    var bytes = new TextEncoder().encode(e);
    var n = Math.min(bytes.length, max);
    HEAPU8.set(bytes.subarray(0, n), dst);
    return n;
});

EM_JS(void, js_cc_compiled_get, (uint8_t *dst), {
    if (window.__ccCompiledBytes) HEAPU8.set(window.__ccCompiledBytes, dst);
});

/* Run a pre-compiled wasm via WASI shim. Output streams via the same
 * __ccPending mechanism as compileLinkRun. */
EM_JS(void, js_cc_run_wasi, (const uint8_t *src, int size, const char *name,
                              const char *argv_joined,
                              const uint8_t *stdin_buf, int stdin_len), {
    var bytes = HEAPU8.slice(src, src + size).buffer;  /* copy to standalone ArrayBuffer */
    var nm = UTF8ToString(name);
    var joined = UTF8ToString(argv_joined);
    var argv = (joined && joined.length) ? joined.split('\x1f') : [];
    /* Snapshot stdin contents for the worker; transferred along with bytes. */
    var stdin_ab = (stdin_buf && stdin_len > 0)
        ? HEAPU8.slice(stdin_buf, stdin_buf + stdin_len).buffer
        : new ArrayBuffer(0);
    window.__ccPending = '';
    window.__ccDone = false;
    window.__cc.onWrite = function(chunk) { window.__ccPending += chunk; };
    window.__cc.runWasi(bytes, nm, argv, stdin_ab).then(function() { window.__ccDone = true; });
});

/* Drain any pending output text into a kernel buffer + sleep until done.
 * Also pumps osito_kernel_poll so user wasm programs running in the
 * cc-worker can call oi_chat → kernel runs llama_chat → response. */
extern void osito_kernel_poll(void);

static void cc_drain_until_done(void)
{
    /* Route through sh_puts so pipe capture (sh_redir_fn) sees the
     * output. Falls back to serial_puts inside sh_puts when no
     * redirect is active. Without this, `pkg run X | pkg run Y`
     * loses stage 1's stdout because cc_drain wrote straight to the
     * terminal. */
    extern void sh_puts(const char *);
    char buf[1024];
    while (!js_cc_done()) {
        osito_kernel_poll();
        int n = js_cc_drain(buf, (int)sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; sh_puts(buf); }
        else       { emscripten_sleep(50); }
    }
    int n;
    while ((n = js_cc_drain(buf, (int)sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        sh_puts(buf);
    }
}

void cmd_cc(int argc, char **argv)
{
    extern void serial_puts(const char *);
    extern void serial_putdec(uint64_t);
    extern void *osfs2_find(const char *);
    extern uint64_t osfs2_file_size(void *);
    extern int osfs2_read(void *, uint64_t, void *, uint64_t);
    extern void *osfs2_create(const char *, uint64_t);
    extern int osfs2_write(void *, uint64_t, const void *, uint64_t);
    extern int osfs2_delete(const char *);

    /* Parse args: pick first non-flag as source, look for -o, -E, -c */
    const char *src_name = NULL;
    const char *out_name = NULL;
    bool        preprocess = false;
    bool        object_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out_name = argv[++i];
        } else if (!strcmp(argv[i], "-E")) {
            preprocess = true;
        } else if (!strcmp(argv[i], "-c")) {
            object_only = true;
        } else if (argv[i][0] != '-' && !src_name) {
            src_name = argv[i];
        }
    }

    /* Source comes from a file (named arg) or piped stdin (`<file` or pipe). */
    extern const char *sh_stdin_buf;
    extern uint32_t    sh_stdin_len;

    char *src = NULL;
    uint64_t size = 0;
    if (src_name) {
        void *file = osfs2_find(src_name);
        if (!file) {
            serial_puts("cc: file not found: ");
            serial_puts(src_name);
            serial_puts("\n");
            return;
        }
        size = osfs2_file_size(file);
        src = malloc((size_t)size + 1);
        if (!src) { serial_puts("cc: malloc failed\n"); return; }
        osfs2_read(file, 0, src, size);
        src[size] = 0;
    } else if (sh_stdin_buf && sh_stdin_len > 0) {
        size = sh_stdin_len;
        src = malloc((size_t)size + 1);
        if (!src) { serial_puts("cc: malloc failed\n"); return; }
        memcpy(src, sh_stdin_buf, (size_t)size);
        src[size] = 0;
        src_name = "stdin";
    } else {
        serial_puts("usage: cc <src.c> [-o <out.wasm>] [-E]\n");
        serial_puts("       cmd | cc       (read source from pipe)\n");
        serial_puts("       cc < file.c    (read source from file)\n");
        return;
    }

    const char *ext = strrchr(src_name, '.');
    const char *lang = (ext && (!strcmp(ext, ".c") || !strcmp(ext, ".h"))) ? "c" : "c++";

    if (preprocess) {
        /* `cc -E`: run clang's preprocessor and stream to terminal. */
        js_cc_preprocess(src, lang);
        free(src);
        cc_drain_until_done();
        serial_puts("\n");
        return;
    }

    /* `-c`: produce a .o; default output name = src basename with .o */
    if (object_only && !out_name) {
        static char auto_out[256];
        const char *base = strrchr(src_name, '/');
        base = base ? base + 1 : src_name;
        const char *dot = strrchr(base, '.');
        size_t blen = dot ? (size_t)(dot - base) : strlen(base);
        if (blen + 3 < sizeof(auto_out)) {
            memcpy(auto_out, base, blen);
            memcpy(auto_out + blen, ".o", 3);
            out_name = auto_out;
        }
    }

    if (out_name) {
        /* Compile + (link unless -c): produce bytes, write to OsitoFS.
         * Drain stdout/stderr from clang/lld in real time so the user
         * sees compile errors and warnings (not just a final exit code). */
        /* Set up a pending buffer for streaming writes (same channel as
         * compileLinkRun uses). */
        EM_ASM({
            window.__ccPending = '';
            window.__cc.onWrite = function(c) { window.__ccPending += c; };
        });
        js_cc_compile(src, lang, object_only ? 1 : 0);
        free(src);

        char buf[1024];
        while (!js_cc_done()) {
            int n = js_cc_drain(buf, (int)sizeof(buf) - 1);
            if (n > 0) { buf[n] = 0; serial_puts(buf); }
            else       { emscripten_sleep(50); }
        }
        int dn;
        while ((dn = js_cc_drain(buf, (int)sizeof(buf) - 1)) > 0) {
            buf[dn] = 0;
            serial_puts(buf);
        }

        char errbuf[512];
        int en = js_cc_compile_error(errbuf, (int)sizeof(errbuf) - 1);
        if (en > 0) {
            errbuf[en] = 0;
            serial_puts("cc: compile failed: ");
            serial_puts(errbuf);
            serial_puts("\n");
            return;
        }

        int wsz = js_cc_compiled_size();
        if (wsz <= 0) {
            serial_puts("cc: empty output\n");
            return;
        }
        uint8_t *wbuf = malloc(wsz);
        if (!wbuf) { serial_puts("cc: malloc failed\n"); return; }
        js_cc_compiled_get(wbuf);

        /* Replace existing file if present */
        osfs2_delete(out_name);
        void *of = osfs2_create(out_name, (uint64_t)wsz);
        if (!of) {
            free(wbuf);
            serial_puts("cc: cannot create ");
            serial_puts(out_name);
            serial_puts("\n");
            return;
        }
        osfs2_write(of, 0, wbuf, (uint64_t)wsz);
        free(wbuf);

        serial_puts("[cc] ");
        serial_putdec((uint64_t)wsz);
        serial_puts(" bytes -> ");
        serial_puts(out_name);
        serial_puts("\n");
        return;
    }

    /* Inline compile + link + run (tcc -run style). */
    js_cc_kick(src, lang);
    free(src);
    cc_drain_until_done();
    serial_puts("\n");
}

/* ── make: tiny Makefile runner ─────────────────────────────────
 *
 * Supports the bare bones of GNU make:
 *  - `target: deps\n\tcmd1\n\tcmd2\n` rules (tab-indented commands).
 *  - `# comment` lines and blank lines.
 *  - `var = value` simple variables (no `:=`/`?=`/`+=`); $(VAR) expansion.
 *  - Commands invoked through the kernel shell (`shell_exec`), so any
 *    builtin (cc, exec, echo) works inside Makefiles.
 *  - First target = default. `make name` selects.
 *
 * Out of scope (PoC): pattern rules, conditionals, includes, parallelism,
 * timestamp checks (rebuilds every time). Useful for short Makefiles like
 *
 *     hello.wasm: hello.c
 *         cc hello.c -o hello.wasm
 *
 *     run: hello.wasm
 *         exec hello.wasm
 */
extern void shell_exec(char *line);

typedef struct mk_var { char *name; char *val; struct mk_var *next; } mk_var_t;
typedef struct mk_rule {
    char *target;
    char *deps;     /* raw, expanded later */
    char **cmds;
    int   ncmds;
    struct mk_rule *next;
} mk_rule_t;

static char *mk_dup_strip(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r')) len--;
    char *r = malloc(len + 1);
    if (r) { memcpy(r, s, len); r[len] = 0; }
    return r;
}

static char *mk_lookup(mk_var_t *vars, const char *name)
{
    for (mk_var_t *v = vars; v; v = v->next)
        if (!strcmp(v->name, name)) return v->val;
    return NULL;
}

/* Expand $(VAR) refs in `s` against `vars`. Caller frees. */
static char *mk_expand(const char *s, mk_var_t *vars)
{
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (*s) {
        if (s[0] == '$' && s[1] == '(') {
            const char *e = strchr(s + 2, ')');
            if (e) {
                char name[64]; int nl = (int)(e - (s + 2));
                if (nl >= (int)sizeof(name)) nl = (int)sizeof(name) - 1;
                memcpy(name, s + 2, nl); name[nl] = 0;
                const char *v = mk_lookup(vars, name);
                if (v) {
                    size_t vl = strlen(v);
                    if (len + vl + 1 >= cap) { cap = (len + vl + 1) * 2; out = realloc(out, cap); }
                    memcpy(out + len, v, vl); len += vl;
                }
                s = e + 1; continue;
            }
        }
        if (len + 2 >= cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = *s++;
    }
    out[len] = 0;
    return out;
}

void cmd_make(int argc, char **argv)
{
    extern void serial_puts(const char *);
    extern void *osfs2_find(const char *);
    extern uint64_t osfs2_file_size(void *);
    extern int  osfs2_read(void *, uint64_t, void *, uint64_t);

    /* Locate Makefile (`-f path` overrides). */
    const char *mkfile = "Makefile";
    const char *want_target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) mkfile = argv[++i];
        else if (argv[i][0] != '-' && !want_target) want_target = argv[i];
    }
    void *file = osfs2_find(mkfile);
    if (!file) {
        serial_puts("make: cannot find ");
        serial_puts(mkfile);
        serial_puts("\n");
        return;
    }
    uint64_t size = osfs2_file_size(file);
    char *body = malloc((size_t)size + 1);
    if (!body) return;
    osfs2_read(file, 0, body, size);
    body[size] = 0;

    /* Parse: walk line by line. Tab-indented lines are commands of the
     * preceding rule; lines with ':' (and not tab-indented) are targets;
     * lines with '=' (no leading tab, no ':' before '=') are variables. */
    mk_var_t  *vars = NULL;
    mk_rule_t *rules = NULL, *cur = NULL;

    char *p = body;
    while (*p) {
        char *line = p;
        char *eol = strchr(p, '\n');
        if (eol) { *eol = 0; p = eol + 1; }
        else p += strlen(p);

        /* Skip leading whitespace (tab or space — OsitoK's edit can't
         * easily insert tabs, so we accept either or none). */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == 0 || *s == '#') continue;

        /* Classify the line. A `:` before any `=` makes it a rule
         * header; an `=` before any `:` makes it a variable; otherwise
         * it's a command attached to the most recent rule. */
        char *colon = strchr(s, ':');
        char *eq    = strchr(s, '=');
        bool is_rule = colon && (!eq || colon < eq);
        bool is_var  = eq && (!colon || eq < colon);

        if (!is_rule && !is_var && cur) {
            char *cmd = mk_dup_strip(s);
            cur->cmds = realloc(cur->cmds, (cur->ncmds + 1) * sizeof(char *));
            cur->cmds[cur->ncmds++] = cmd;
            continue;
        }

        if (is_var) {
            *eq = 0;
            mk_var_t *v = malloc(sizeof(mk_var_t));
            v->name = mk_dup_strip(s);
            v->val  = mk_dup_strip(eq + 1);
            v->next = vars;
            vars = v;
            continue;
        }
        if (is_rule) {
            *colon = 0;
            mk_rule_t *r = calloc(1, sizeof(mk_rule_t));
            r->target = mk_dup_strip(s);
            r->deps   = mk_dup_strip(colon + 1);
            if (!rules) rules = r;
            else { mk_rule_t *t = rules; while (t->next) t = t->next; t->next = r; }
            cur = r;
            continue;
        }
        /* Orphan command (no current rule) — ignore */
    }

    /* Pick target: arg or first defined */
    mk_rule_t *target_rule = NULL;
    if (want_target) {
        for (mk_rule_t *r = rules; r; r = r->next)
            if (!strcmp(r->target, want_target)) { target_rule = r; break; }
        if (!target_rule) {
            serial_puts("make: no rule for ");
            serial_puts(want_target);
            serial_puts("\n");
            goto done;
        }
    } else if (rules) {
        target_rule = rules;
    } else {
        serial_puts("make: no rules\n");
        goto done;
    }

    /* Resolve deps recursively before running this target's commands.
     * Cycles aren't detected (PoC); rebuild always (no timestamps). */
    {
        char *deps = mk_expand(target_rule->deps, vars);
        char *tok = strtok(deps, " \t");
        while (tok) {
            for (mk_rule_t *dr = rules; dr; dr = dr->next) {
                if (!strcmp(dr->target, tok)) {
                    /* Recurse via cmd_make-like inline: just run dr's cmds */
                    for (int i = 0; i < dr->ncmds; i++) {
                        char *cmd = mk_expand(dr->cmds[i], vars);
                        serial_puts(cmd); serial_puts("\n");
                        shell_exec(cmd);
                        free(cmd);
                    }
                    break;
                }
            }
            tok = strtok(NULL, " \t");
        }
        free(deps);
    }

    /* Run the target's own commands */
    for (int i = 0; i < target_rule->ncmds; i++) {
        char *cmd = mk_expand(target_rule->cmds[i], vars);
        serial_puts(cmd); serial_puts("\n");
        shell_exec(cmd);
        free(cmd);
    }

done:
    /* Cleanup (omit per-node frees for brevity — heap reclaimed on exit). */
    free(body);
}

/* ── edit: multi-line text editor saved to OsitoFS ──────────────
 * Reads lines via term_readline until a line containing only "."
 * (or empty + Ctrl-D); writes accumulated text to OsitoFS. Limit 64KB. */
void cmd_edit(int argc, char **argv)
{
    extern void serial_puts(const char *);
    extern void serial_putdec(uint64_t);
    extern int  term_readline(const char *prompt, char *buf, uint32_t buf_size);
    extern void *osfs2_find(const char *);
    extern int  osfs2_delete(const char *);
    extern void *osfs2_create(const char *, uint64_t);
    extern int  osfs2_write(void *, uint64_t, const void *, uint64_t);

    if (argc < 2) { serial_puts("usage: edit <file>\n"); return; }

    serial_puts("[edit] type lines; '.' on its own line ends.\n");

    enum { CAP = 64 * 1024 };
    char *buffer = (char *)malloc(CAP);
    if (!buffer) { serial_puts("edit: malloc failed\n"); return; }
    size_t pos = 0;

    char line[1024];
    while (pos < CAP - 2) {
        int n = term_readline("> ", line, sizeof(line));
        if (n < 0) break;
        /* terminator: line == "." */
        if (line[0] == '.' && (line[1] == 0)) break;

        size_t len = strlen(line);
        if (pos + len + 1 >= CAP) break;
        memcpy(buffer + pos, line, len);
        pos += len;
        buffer[pos++] = '\n';
    }

    osfs2_delete(argv[1]);
    void *f = osfs2_create(argv[1], (uint64_t)pos);
    if (!f) { free(buffer); serial_puts("edit: cannot create file\n"); return; }
    osfs2_write(f, 0, buffer, (uint64_t)pos);
    free(buffer);

    serial_puts("[edit] ");
    serial_putdec((uint64_t)pos);
    serial_puts(" bytes -> ");
    serial_puts(argv[1]);
    serial_puts("\n");
}

/* ── perf events / kprof / panic / rcu (x86 asm — excluded) ───── */
void perf_init(void) {}
void perf_enable_all(void) {}
void perf_disable_all(void) {}
uint64_t perf_read_counter(int c) { (void)c; return 0; }
void perf_phase_begin(int p) { (void)p; }
void perf_phase_end(int p) { (void)p; }
void perf_phase_dump(void) {}
bool perf_enabled = false;

void rcu_init(void) {}
void rcu_read_lock(void) {}
void rcu_read_unlock(void) {}
void synchronize_rcu(void) {}
void call_rcu(void *head, void (*func)(void *)) { (void)head; if (func) func(NULL); }

void panic(const char *msg) {
    extern void serial_puts(const char *);
    serial_puts("\n[PANIC] ");
    serial_puts(msg ? msg : "(no message)");
    serial_puts("\n");
    EM_ASM({ throw new Error('kernel panic'); });
    while (1) {}
}
void panic_init(void) {}
void watchdog_kick(void) {}
void crash_dump(void *regs) { (void)regs; }

/* ── Network helpers referenced by shell/dhcp/sshd/etc. ────────── */
void net_get_mac(uint8_t mac_out[6])     { memset(mac_out, 0, 6); }
void net_set_ip(const uint8_t ip[4])     { (void)ip; }
void net_set_gateway(const uint8_t gw[4]){ (void)gw; }
void net_set_netmask(const uint8_t m[4]) { (void)m; }
void net_dns_set_server(const uint8_t ip[4]) { (void)ip; }
const uint8_t *net_get_ip_ptr(void)      { static const uint8_t z[4] = {0,0,0,0}; return z; }
int  net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac_out[6])
{ (void)ip; memset(mac_out, 0, 6); return -1; }
void net_arp_probe(const uint8_t ip[4])  { (void)ip; }
int  apipa_assign(void)   { return -1; }
int  dhcp_discover(void)  { return -1; }

/* ── ACPI shutdown/reboot direct symbols ──────────────────────── */
void acpi_shutdown(void) { power_shutdown(); }
void acpi_reboot(void)   { power_reboot(); }

/* ── SMP work queue helpers ───────────────────────────────────── */
typedef void (*smp_fn_t)(void *, void *);
int  smp_submit(int ap_idx, smp_fn_t fn, void *arg, void *result)
{ (void)ap_idx; if (fn) fn(arg, result); return 0; }
int  smp_submit_any(smp_fn_t fn, void *arg, void *result)
{ if (fn) fn(arg, result); return 0; }
void smp_wait(int task_id) { (void)task_id; }
bool smp_task_done(int task_id) { (void)task_id; return true; }
uint32_t ap_worker_count = 0;

/* ── Process helpers ──────────────────────────────────────────── */
int32_t  proc_current_pid(void) { return 1; }
void     proc_signal_pid(uint32_t pid, int sig) { (void)pid; (void)sig; }

/* ── usym / kallsyms user-side getters ────────────────────────── */
uint64_t user_load_bias_get(void *p)     { (void)p; return 0; }
void    *user_strtab_get(void *p)        { (void)p; return NULL; }
uint64_t user_strtab_size_get(void *p)   { (void)p; return 0; }
void    *user_symtab_get(void *p)        { (void)p; return NULL; }
uint64_t user_symtab_size_get(void *p)   { (void)p; return 0; }

/* ── perf phases / cmd_perf ───────────────────────────────────── */
void perf_phase_enter(int slot, const char *name) { (void)slot; (void)name; }
void perf_phase_exit(int slot) { (void)slot; }
void cmd_perf(int argc, char **argv) {
    (void)argc; (void)argv;
    extern void serial_puts(const char *);
    serial_puts("  (perf counters disabled in WASM)\n");
}
void cpu_features_dump(void) {
    extern void serial_puts(const char *);
    serial_puts("  CPU: WebAssembly virtual (no SIMD/AVX2)\n");
}

/* ── self_opt extras ──────────────────────────────────────────── */
void self_opt_undo_all(void) {}
void self_opt_stats(void) {}
bool self_opt_enabled = false;

/* ── pred_sched extras ────────────────────────────────────────── */
void pred_reset(void) {}
void pred_stats(void) {}

/* ── io_predict extras ────────────────────────────────────────── */
void io_predict_observe(const char *path) { (void)path; }
void io_predict_reset(void) {}
void io_predict_stats(void) {}

/* ── NVMe extra helpers ───────────────────────────────────────── */
/* Dispatch flag — when 1, nvme_read serves the active aux slot
 * (used by fat32_mount which calls nvme_read internally). Multi-slot
 * support: the wrapping shell command sets g_active_aux_slot to the
 * right slot before calling fat32 ops; this flag enables routing. */
static int g_nvme_route_aux = 0;
void wasm_nvme_route_aux(int on) { g_nvme_route_aux = on != 0; }

extern int aux_disk_read_512(uint64_t lba, uint32_t count, void *buf);

int  nvme_read(uint64_t lba, uint32_t count, void *buf)
{
    if (g_nvme_route_aux) {
        return aux_disk_read_512(lba, count, buf);
    }
    return nvme_read_bytes(lba * 512, buf, (uint64_t)count * 512);
}
int  nvme_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (g_nvme_route_aux) return -1;  /* aux is read-only */
    return nvme_write_bytes(lba * 512, buf, (uint64_t)count * 512);
}
int  nvme_read_async(uint64_t lba, uint32_t count, uint64_t phys_addr)
{ (void)lba; (void)count; (void)phys_addr; return -1; }
int  nvme_wait_cq(uint16_t cid)        { (void)cid; return 0; }
uint32_t nvme_get_lba_size(void)        { return 512; }

/* ── FAT32 (browser MEMFS handles real files) ─────────────────── */
bool fat32_is_mounted(void) { return false; }
int  fat32_find(const char *name, uint32_t *cluster_out, uint32_t *size_out)
{ (void)name; (void)cluster_out; (void)size_out; return -1; }
int  fat32_ls(const char *path) { (void)path; return -1; }
int  fat32_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{ (void)name; (void)offset; (void)buf; (void)len; return -1; }

/* ── OsitoFS v3 (only v2 is mounted in wasm MVP) ──────────────── */
bool osfs3_is_mounted(void)             { return false; }
uint32_t osfs3_resolve_path(const char *p) { (void)p; return 0; }
int  osfs3_read(uint32_t inode, uint64_t off, void *buf, uint64_t len)
{ (void)inode; (void)off; (void)buf; (void)len; return -1; }
void osfs3_list_dir(uint32_t inode) { (void)inode; }
bool osfs3_is_dir(uint32_t inode)   { (void)inode; return false; }
uint64_t osfs3_get_size(uint32_t inode) { (void)inode; return 0; }

/* ── Display extras ───────────────────────────────────────────── */
int  display_resize(uint32_t w, uint32_t h, uint32_t pitch)
{ (void)w; (void)h; (void)pitch; return -1; }
void display_enable_gpu_scanout(void) {}

/* compositor_start_wasm is now defined in compositor.c (under __EMSCRIPTEN__) */

/* ── Globals expected by various subsystems ──────────────────── */
/* hwbps[] defined above with proper hwbp_t type from hwbp.h */
int g_compat32_mode = 0;
uint8_t g_tensor_arena[1] = {0};   /* placeholder symbol */
uint8_t ist1_stack[4096] = {0};
void *tss_ist1_ptr = NULL;
uint64_t dos_native_exit_jmpbuf[16] = {0};

/* ── Tensor dispatch table (struct disp) ──────────────────────── */
/* tensor.c references `disp` as the runtime dispatch table. With AVX2
 * disabled, it should just point at scalar fns. Make a minimal symbol
 * that holds zeros — tensor.c reads it but cpu_has_avx2()=false means
 * scalar paths are taken, so the contents don't matter for correctness. */
struct { void *matmul; void *softmax; void *rmsnorm; void *swiglu; } disp = {0};

/* ── Syscall dispatch (no userspace in WASM) ─────────────────── */
int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)nr; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return -1; }

/* syscall fd table accessor (referenced as a function by io_uring.c) */
void *syscall_fds(void) {
    static char dummy_fd_table[256 * 64];   /* opaque buffer; never used */
    return dummy_fd_table;
}

/* tls13.c calls x25519_scalarmult; crypto.c provides x25519 with the
 * same signature. Alias here so tls13 links cleanly. */
extern void x25519(uint8_t result[32], const uint8_t scalar[32],
                    const uint8_t point[32]);
void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32])
{ x25519(out, scalar, point); }

int  ccp_init(void) { return -1; }
int  ccp_get_random(uint8_t *buf, uint32_t len)
{
    EM_ASM({
        var dst = $0; var n = $1;
        for (var i = 0; i < n; i++)
            HEAPU8[dst + i] = (Math.random() * 256) | 0;
    }, buf, len);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Auxiliary disk mount — N slots, one per FS type.
 *
 *  The primary OsitoFS image is loaded into wasm_nvme_buf at boot.
 *  Up to AUX_DISK_SLOTS auxiliary FS images can be mounted at once
 *  (e.g. one ISO + one FAT + one ext2). Each slot owns its own
 *  HEAP-allocated buffer.
 *
 *  Dispatch flow:
 *    - g_active_aux_slot = -1   → reads served by wasm_nvme_buf
 *    - g_active_aux_slot >= 0   → reads served by aux_disks[slot]
 *  The shell dispatcher sets g_active_aux_slot around each fs call
 *  (cmd_aux_fs_read / cmd_aux_fs_ls) so individual fs drivers don't
 *  need to know about slots.
 *
 *  Backwards compat: aux_disk_fetch / aux_disk_size / aux_disk_ptr
 *  operate on slot 0 (the "default" aux). New code should use the
 *  _slot variants.
 * ══════════════════════════════════════════════════════════════ */

#define AUX_DISK_SLOTS 10

typedef struct {
    uint8_t *buf;
    uint64_t size;
    char     name[16];   /* fs-type tag, e.g. "iso", "fat", "ext" */
    bool     in_use;
} aux_disk_t;

static aux_disk_t g_aux_disks[AUX_DISK_SLOTS];
static int        g_active_aux_slot = -1;

void wasm_aux_route_slot(int slot)
{
    if (slot < -1 || slot >= AUX_DISK_SLOTS) slot = -1;
    g_active_aux_slot = slot;
}

int wasm_aux_active_slot(void) { return g_active_aux_slot; }

/* Find a slot by FS-type name. Returns idx ≥ 0 if found, -1 if not. */
int wasm_aux_find_slot(const char *name)
{
    for (int i = 0; i < AUX_DISK_SLOTS; i++)
        if (g_aux_disks[i].in_use && strcmp(g_aux_disks[i].name, name) == 0)
            return i;
    return -1;
}

/* Allocate a slot for the given FS type (replacing any prior mount of
 * that same type). Returns slot index. */
int wasm_aux_alloc_slot(const char *name)
{
    int existing = wasm_aux_find_slot(name);
    if (existing >= 0) {
        extern void free(void *);
        if (g_aux_disks[existing].buf) free(g_aux_disks[existing].buf);
        g_aux_disks[existing].buf = NULL;
        g_aux_disks[existing].size = 0;
        return existing;
    }
    for (int i = 0; i < AUX_DISK_SLOTS; i++) {
        if (!g_aux_disks[i].in_use) {
            g_aux_disks[i].in_use = true;
            int n = 0;
            while (n < 15 && name[n]) { g_aux_disks[i].name[n] = name[n]; n++; }
            g_aux_disks[i].name[n] = '\0';
            g_aux_disks[i].buf = NULL;
            g_aux_disks[i].size = 0;
            return i;
        }
    }
    return -1;
}

void wasm_aux_release_slot(int slot)
{
    if (slot < 0 || slot >= AUX_DISK_SLOTS) return;
    extern void free(void *);
    if (g_aux_disks[slot].buf) free(g_aux_disks[slot].buf);
    g_aux_disks[slot].buf = NULL;
    g_aux_disks[slot].size = 0;
    g_aux_disks[slot].in_use = false;
    g_aux_disks[slot].name[0] = '\0';
}

/* Backwards-compat aliases that operate on slot 0 — the "default" aux. */
#define g_aux_disk_buf  (g_aux_disks[0].buf)
#define g_aux_disk_size (g_aux_disks[0].size)

EM_JS(void, js_aux_fetch_kick, (const char *url), {
    var u = UTF8ToString(url);
    window.__auxFetchDone = false;
    window.__auxFetchData = null;
    window.__auxFetchError = null;
    fetch(u)
        .then(function(r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.arrayBuffer();
        })
        .then(function(ab) {
            window.__auxFetchData = new Uint8Array(ab);
            window.__auxFetchDone = true;
        })
        .catch(function(e) {
            window.__auxFetchError = String(e);
            window.__auxFetchDone = true;
        });
});

EM_JS(int, js_aux_fetch_done, (), { return window.__auxFetchDone ? 1 : 0; });

EM_JS(int, js_aux_fetch_size, (), {
    return window.__auxFetchData ? window.__auxFetchData.byteLength : 0;
});

EM_JS(void, js_aux_fetch_copy, (uint8_t *dst, int max), {
    var src = window.__auxFetchData;
    if (!src) return;
    var n = src.byteLength < max ? src.byteLength : max;
    HEAPU8.set(src.subarray(0, n), dst);
});

EM_JS(int, js_aux_fetch_error, (char *dst, int max), {
    var s = window.__auxFetchError || '';
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return s ? bytes - 1 : 0;
});

/* Synchronous fetch helper — relies on Asyncify to suspend the kernel
 * until the JS Promise resolves. Returns 0 on success, -1 on error. */
extern void emscripten_sleep(unsigned int ms);

/* Fetch into a specific aux slot (multi-mount). Returns 0 on success. */
int aux_disk_fetch_slot(int slot, const char *url)
{
    if (slot < 0 || slot >= AUX_DISK_SLOTS) return -1;

    js_aux_fetch_kick(url);
    while (!js_aux_fetch_done()) emscripten_sleep(20);

    int sz = js_aux_fetch_size();
    if (sz <= 0) {
        char errbuf[256];
        if (js_aux_fetch_error(errbuf, sizeof(errbuf)) > 0) {
            serial_puts("[AUX] fetch error: ");
            serial_puts(errbuf);
            serial_puts("\n");
        }
        return -1;
    }

    extern void *malloc(size_t); extern void free(void *);
    if (g_aux_disks[slot].buf) free(g_aux_disks[slot].buf);
    g_aux_disks[slot].buf = (uint8_t *)malloc(sz);
    g_aux_disks[slot].size = sz;
    if (!g_aux_disks[slot].buf) return -1;
    js_aux_fetch_copy(g_aux_disks[slot].buf, sz);

    serial_puts("[AUX] slot ");
    serial_putdec((uint64_t)slot);
    serial_puts(" (");
    serial_puts(g_aux_disks[slot].name);
    serial_puts(") fetched ");
    serial_putdec((uint64_t)sz);
    serial_puts(" bytes\n");
    return 0;
}

/* Generic URL → malloc'd buffer. Caller frees. Reuses the aux fetch
 * Promise plumbing so we don't double-define JS bridges. Returns NULL
 * on error; sets *out_size on success. */
uint8_t *wasm_url_fetch(const char *url, int *out_size)
{
    js_aux_fetch_kick(url);
    while (!js_aux_fetch_done()) emscripten_sleep(20);
    int sz = js_aux_fetch_size();
    if (sz <= 0) return NULL;
    extern void *malloc(size_t);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return NULL;
    js_aux_fetch_copy(buf, sz);
    if (out_size) *out_size = sz;
    return buf;
}

/* ── Simple HTTP POST with JSON body ──────────────────────────
 * Returns a malloc'd response buffer + size, NULL on error. Caller
 * frees. Used by 'rag' for /embed and any future POST endpoint
 * that returns a non-streaming JSON response. */
EM_JS(void, js_http_post_kick, (const char *url, const char *body), {
    var u = UTF8ToString(url);
    var b = UTF8ToString(body);
    window.__postDone = false;
    window.__postData = null;
    window.__postError = null;
    fetch(u, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: b
    })
        .then(function(r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.arrayBuffer();
        })
        .then(function(ab) {
            window.__postData = new Uint8Array(ab);
            window.__postDone = true;
        })
        .catch(function(e) {
            window.__postError = String(e);
            window.__postDone = true;
        });
});
EM_JS(int, js_http_post_done, (), { return window.__postDone ? 1 : 0; });
EM_JS(int, js_http_post_size, (), {
    return window.__postData ? window.__postData.byteLength : 0;
});
EM_JS(void, js_http_post_copy, (uint8_t *dst, int max), {
    var src = window.__postData;
    if (!src) return;
    var n = src.byteLength < max ? src.byteLength : max;
    HEAPU8.set(src.subarray(0, n), dst);
});

uint8_t *wasm_http_post_json(const char *url, const char *body, int *out_size)
{
    js_http_post_kick(url, body);
    while (!js_http_post_done()) emscripten_sleep(20);
    int sz = js_http_post_size();
    if (sz <= 0) return NULL;
    extern void *malloc(size_t);
    uint8_t *buf = (uint8_t *)malloc(sz + 1);
    if (!buf) return NULL;
    js_http_post_copy(buf, sz);
    buf[sz] = 0;
    if (out_size) *out_size = sz;
    return buf;
}

/* Backwards-compat: fetch into slot 0. */
int aux_disk_fetch(const char *url) {
    /* Ensure slot 0 is allocated/named for legacy callers. */
    if (!g_aux_disks[0].in_use) {
        g_aux_disks[0].in_use = true;
        g_aux_disks[0].name[0] = 'a'; g_aux_disks[0].name[1] = 'u';
        g_aux_disks[0].name[2] = 'x'; g_aux_disks[0].name[3] = '\0';
    }
    return aux_disk_fetch_slot(0, url);
}

/* Pick the buffer to read from: the active slot if set, else slot 0
 * (backwards-compat default). */
static const aux_disk_t *active_aux(void)
{
    int s = (g_active_aux_slot >= 0) ? g_active_aux_slot : 0;
    if (!g_aux_disks[s].in_use) return NULL;
    return &g_aux_disks[s];
}

/* iso9660_mount calls this with (lba, count, buf). Each LBA is 2048 B. */
int aux_disk_read_iso(uint64_t lba, uint32_t count, void *buf) {
    const aux_disk_t *a = active_aux();
    uint64_t off = lba * 2048;
    uint64_t len = (uint64_t)count * 2048;
    if (!a || !a->buf || off + len > a->size) return -1;
    memcpy(buf, a->buf + off, (size_t)len);
    return 0;
}

/* ext2 / others using 512-byte LBA */
int aux_disk_read_512(uint64_t lba, uint32_t count, void *buf) {
    const aux_disk_t *a = active_aux();
    uint64_t off = lba * 512;
    uint64_t len = (uint64_t)count * 512;
    if (!a || !a->buf || off + len > a->size) return -1;
    memcpy(buf, a->buf + off, (size_t)len);
    return 0;
}

uint64_t aux_disk_size(void) {
    const aux_disk_t *a = active_aux();
    return a ? a->size : 0;
}
uint8_t *aux_disk_ptr(void)  {
    const aux_disk_t *a = active_aux();
    return a ? a->buf : NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Generic HTTP fetch bridge — used by `curl` and `claude` REPL.
 *
 *  WASM has no real TCP/TLS stack, so HTTP commands route through
 *  the browser's fetch() API. The kernel suspends via Asyncify
 *  while the JS Promise resolves.
 * ══════════════════════════════════════════════════════════════ */

EM_JS(void, js_http_kick, (const char *url, const char *method,
                            const char *headers_json, const char *body), {
    var u = UTF8ToString(url);
    var m = UTF8ToString(method);
    var hj = headers_json ? UTF8ToString(headers_json) : '';
    var b = body ? UTF8ToString(body) : null;
    var hdrs = {};
    if (hj) {
        try { hdrs = JSON.parse(hj); } catch (e) { hdrs = {}; }
    }
    window.__httpDone = false;
    window.__httpStatus = 0;
    window.__httpBody = null;
    window.__httpError = null;
    var opts = { method: m, headers: hdrs };
    if (b !== null) opts.body = b;
    fetch(u, opts)
        .then(function(r) {
            window.__httpStatus = r.status;
            return r.text();
        })
        .then(function(t) {
            var enc = new TextEncoder();
            window.__httpBody = enc.encode(t);
            window.__httpDone = true;
        })
        .catch(function(e) {
            window.__httpError = String(e);
            window.__httpDone = true;
        });
});

EM_JS(int, js_http_done,   (), { return window.__httpDone ? 1 : 0; });
EM_JS(int, js_http_status, (), { return window.__httpStatus | 0; });
EM_JS(int, js_http_size,   (), {
    return window.__httpBody ? window.__httpBody.byteLength : 0;
});
EM_JS(void, js_http_copy, (uint8_t *dst, int max), {
    var src = window.__httpBody;
    if (!src) return;
    var n = src.byteLength < max ? src.byteLength : max;
    HEAPU8.set(src.subarray(0, n), dst);
});
EM_JS(int, js_http_error, (char *dst, int max), {
    var s = window.__httpError || '';
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return s ? bytes - 1 : 0;
});

/* ══════════════════════════════════════════════════════════════
 *  WebSocket bridge — gives the kernel a packet-oriented socket
 *  to anywhere wss:// reachable from the browser. Connection
 *  state, RX queue, and TX path live in JS; the kernel calls
 *  wasm_ws_* and gets back handles + bytes.
 * ══════════════════════════════════════════════════════════════ */

EM_JS(int, js_ws_open, (const char *url), {
    var u = UTF8ToString(url);
    if (!window.__wsHandles) {
        window.__wsHandles = {};
        window.__wsNextId  = 1;
    }
    var id = window.__wsNextId++;
    var entry = {
        url: u,
        ws: null,
        state: 0,        /* 0=connecting 1=open 2=closing 3=closed 4=error */
        rxq: [],         /* array of Uint8Array */
        rxbytes: 0,
        err: ''
    };
    try {
        entry.ws = new WebSocket(u);
        entry.ws.binaryType = 'arraybuffer';
        entry.ws.onopen    = function() { entry.state = 1; };
        entry.ws.onclose   = function() { entry.state = 3; };
        entry.ws.onerror   = function(e) {
            entry.state = 4;
            entry.err = 'ws error';
        };
        entry.ws.onmessage = function(ev) {
            var data = ev.data;
            var u8;
            if (typeof data === 'string') {
                u8 = new TextEncoder().encode(data);
            } else if (data instanceof ArrayBuffer) {
                u8 = new Uint8Array(data);
            } else if (data instanceof Blob) {
                /* Async — drop blobs we can't read sync. Most servers
                 * use string or ArrayBuffer with binaryType set. */
                return;
            } else {
                return;
            }
            entry.rxq.push(u8);
            entry.rxbytes += u8.byteLength;
        };
    } catch (e) {
        entry.state = 4;
        entry.err = String(e);
    }
    window.__wsHandles[id] = entry;
    return id;
});

EM_JS(int, js_ws_state, (int handle), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    return e ? e.state : 4;
});

EM_JS(int, js_ws_send, (int handle, const uint8_t *data, int len), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    if (!e || e.state !== 1) return -1;
    try {
        var u8 = HEAPU8.slice(data, data + len);
        e.ws.send(u8);
        return len;
    } catch (err) {
        e.err = String(err);
        return -1;
    }
});

EM_JS(int, js_ws_recv_size, (int handle), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    return e ? e.rxbytes : 0;
});

/* Drain up to `max` bytes from the head of the rx queue into dst.
 * Returns bytes drained. Partial drains leave the rest in place for
 * the next call. */
EM_JS(int, js_ws_recv_drain, (int handle, uint8_t *dst, int max), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    if (!e) return 0;
    var written = 0;
    while (e.rxq.length > 0 && written < max) {
        var head = e.rxq[0];
        var room = max - written;
        if (head.byteLength <= room) {
            HEAPU8.set(head, dst + written);
            written += head.byteLength;
            e.rxbytes -= head.byteLength;
            e.rxq.shift();
        } else {
            HEAPU8.set(head.subarray(0, room), dst + written);
            e.rxq[0] = head.subarray(room);
            e.rxbytes -= room;
            written  += room;
        }
    }
    return written;
});

EM_JS(void, js_ws_close, (int handle), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    if (!e) return;
    try { if (e.ws && e.state < 3) e.ws.close(); } catch (err) {}
    e.state = 3;
});

EM_JS(int, js_ws_error, (int handle, char *dst, int max), {
    var e = window.__wsHandles && window.__wsHandles[handle];
    var s = e ? (e.err || '') : '';
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return s ? bytes - 1 : 0;
});

/* Public C-side wrappers */

int wasm_ws_open(const char *url) { return js_ws_open(url); }

int wasm_ws_state(int handle) { return js_ws_state(handle); }

int wasm_ws_wait_open(int handle, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int s = js_ws_state(handle);
        if (s == 1) return 0;
        if (s >= 3) return -1;
        emscripten_sleep(10);
        waited += 10;
    }
    return -1;
}

int wasm_ws_send(int handle, const void *data, int len)
{
    return js_ws_send(handle, (const uint8_t *)data, len);
}

int wasm_ws_recv(int handle, void *dst, int max)
{
    return js_ws_recv_drain(handle, (uint8_t *)dst, max);
}

int wasm_ws_recv_wait(int handle, void *dst, int max, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (js_ws_recv_size(handle) > 0)
            return js_ws_recv_drain(handle, (uint8_t *)dst, max);
        if (js_ws_state(handle) >= 3) return -1;
        emscripten_sleep(20);
        waited += 20;
    }
    return 0;
}

void wasm_ws_close(int handle) { js_ws_close(handle); }

/* ══════════════════════════════════════════════════════════════
 *  Streaming HTTP/SSE — for the claude REPL.
 *
 *  fetch() with stream parsing happens in JS; chunks land in a
 *  JS-side queue (similar shape to ws_recv_drain). C polls for chunks
 *  via js_stream_drain and emits them through the callback as they
 *  arrive, instead of waiting for the full response.
 *
 *  Anthropic SSE format:
 *    event: content_block_delta
 *    data: {"type":"content_block_delta","delta":{"type":"text_delta",
 *           "text":"..."}}
 *  We extract delta.text from each event.
 * ══════════════════════════════════════════════════════════════ */

EM_JS(int, js_stream_kick, (const char *url, const char *headers_json,
                             const char *body), {
    var u = UTF8ToString(url);
    var hj = headers_json ? UTF8ToString(headers_json) : '';
    var b = body ? UTF8ToString(body) : null;
    var hdrs = {};
    if (hj) { try { hdrs = JSON.parse(hj); } catch (e) { hdrs = {}; } }
    window.__streamDone = false;
    window.__streamQ = [];           /* array of strings (SSE data fields) */
    window.__streamStatus = 0;
    window.__streamError = null;

    var opts = { method: 'POST', headers: hdrs };
    if (b !== null) opts.body = b;
    fetch(u, opts).then(function(r) {
        window.__streamStatus = r.status;
        if (!r.body) {
            window.__streamDone = true;
            return;
        }
        var reader = r.body.getReader();
        var dec = new TextDecoder();
        var partial = '';
        function pump() {
            return reader.read().then(function(res) {
                if (res.done) { window.__streamDone = true; return; }
                partial += dec.decode(res.value, { stream: true });
                /* Split on double-newline (SSE event boundary) */
                var parts = partial.split('\n\n');
                partial = parts.pop();
                for (var i = 0; i < parts.length; i++) {
                    var ev = parts[i];
                    /* Only keep lines starting with 'data: ' */
                    var lines = ev.split('\n');
                    for (var j = 0; j < lines.length; j++) {
                        var ln = lines[j];
                        if (ln.indexOf('data: ') === 0) {
                            window.__streamQ.push(ln.substring(6));
                        }
                    }
                }
                return pump();
            });
        }
        pump();
    }).catch(function(e) {
        window.__streamError = String(e);
        window.__streamDone = true;
    });
    return 0;
});

EM_JS(int, js_stream_done,    (), { return window.__streamDone ? 1 : 0; });
EM_JS(int, js_stream_status,  (), { return window.__streamStatus | 0; });
EM_JS(int, js_stream_q_size,  (), {
    return window.__streamQ ? window.__streamQ.length : 0;
});
/* Pop the head of the queue into dst (NUL-terminated). Returns bytes
 * written including NUL, or 0 if queue empty. */
EM_JS(int, js_stream_q_pop, (char *dst, int max), {
    var q = window.__streamQ;
    if (!q || q.length === 0) return 0;
    var s = q.shift();
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return bytes;
});
EM_JS(int, js_stream_error, (char *dst, int max), {
    var s = window.__streamError || '';
    var bytes = lengthBytesUTF8(s) + 1;
    if (bytes > max) bytes = max;
    stringToUTF8(s, dst, bytes);
    return s ? bytes - 1 : 0;
});

/* Synchronous HTTP — suspends via Asyncify until the Promise resolves.
 * Allocates the body buffer; caller must free.
 * Returns HTTP status code (>0) or -1 on transport error.
 * On success: *out_buf points to malloc'd response body, *out_len its size. */
int wasm_http_request(const char *url, const char *method,
                       const char *headers_json, const char *body,
                       uint8_t **out_buf, int *out_len)
{
    js_http_kick(url, method, headers_json, body);
    while (!js_http_done()) emscripten_sleep(20);

    if (js_http_status() == 0) {
        char err[256];
        if (js_http_error(err, sizeof(err)) > 0) {
            serial_puts("[http] error: ");
            serial_puts(err);
            serial_puts("\n");
        }
        return -1;
    }

    int sz = js_http_size();
    extern void *malloc(size_t);
    uint8_t *buf = (uint8_t *)malloc(sz + 1);
    if (!buf) return -1;
    js_http_copy(buf, sz);
    buf[sz] = '\0';

    if (out_buf) *out_buf = buf;
    if (out_len) *out_len = sz;
    return js_http_status();
}
