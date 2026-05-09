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
extern void js_cc_run_wasi(const uint8_t *src, int size, const char *name);

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
        js_cc_run_wasi((const uint8_t *)buf, (int)size, filename);
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

int nvme_write_bytes(uint64_t offset, const void *buf, uint64_t len) {
    if (!wasm_nvme_buf || offset + len > wasm_nvme_size) return -1;
    memcpy(wasm_nvme_buf + offset, buf, (size_t)len);
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

/* ── Network TCP/DNS (no real networking in WASM MVP) ────────── */

int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port)
     { (void)dst_ip; (void)dst_port; (void)src_port; return -1; }
int  net_tcp_send(int conn, const void *data, uint32_t len)
     { (void)conn; (void)data; (void)len; return -1; }
int  net_tcp_recv(int conn, void *buf, uint32_t buf_size)
     { (void)conn; (void)buf; (void)buf_size; return -1; }
int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size, uint32_t timeout_ticks)
     { (void)conn; (void)buf; (void)buf_size; (void)timeout_ticks; return -1; }
void net_tcp_close(int conn)   { (void)conn; }
int  net_tcp_state(int conn)   { (void)conn; return -1; }
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

/* ── Claude API ──────────────────────────────────────────────── */

void claude_set_api_key(const char *key) { (void)key; }
const char *claude_get_api_key(void) { return NULL; }
int  claude_chat(const void *messages, int msg_count, const char *model,
                 int max_tokens, int (*callback)(const char *, uint32_t, void *), void *ctx)
     { (void)messages; (void)msg_count; (void)model; (void)max_tokens;
       (void)callback; (void)ctx; return -1; }
void *claude_session_new(void)  { return NULL; }
void  claude_session_free(void *s) { (void)s; }
void  claude_session_clear(void *s) { (void)s; }
int   claude_session_send(void *s, const char *user_msg,
                           int (*callback)(const char *, uint32_t, void *), void *ctx)
      { (void)s; (void)user_msg; (void)callback; (void)ctx; return -1; }
int   claude_session_send_with_tools(void *s, const char *user_msg,
                                      int (*callback)(const char *, uint32_t, void *), void *ctx)
      { (void)s; (void)user_msg; (void)callback; (void)ctx; return -1; }

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
EM_JS(void, js_cc_compile, (const char *src, const char *lang), {
    var s = UTF8ToString(src);
    var l = UTF8ToString(lang);
    window.__ccDone = false;
    window.__ccCompiledBytes = null;
    window.__ccCompileError = null;
    window.__cc.compile(s, l).then(function(res) {
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
EM_JS(void, js_cc_run_wasi, (const uint8_t *src, int size, const char *name), {
    var bytes = HEAPU8.slice(src, src + size).buffer;  /* copy to standalone ArrayBuffer */
    var nm = UTF8ToString(name);
    window.__ccPending = '';
    window.__ccDone = false;
    window.__cc.onWrite = function(chunk) { window.__ccPending += chunk; };
    window.__cc.runWasi(bytes, nm).then(function() { window.__ccDone = true; });
});

/* Drain any pending output text into a kernel buffer + sleep until done. */
static void cc_drain_until_done(void)
{
    extern void serial_puts(const char *);
    char buf[1024];
    while (!js_cc_done()) {
        int n = js_cc_drain(buf, (int)sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; serial_puts(buf); }
        else       { emscripten_sleep(50); }
    }
    int n;
    while ((n = js_cc_drain(buf, (int)sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        serial_puts(buf);
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

    if (argc < 2) {
        serial_puts("usage: cc <src.c> [-o <out.wasm>]\n");
        return;
    }

    /* Parse args: pick first non-flag as source, look for -o */
    const char *src_name = NULL;
    const char *out_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out_name = argv[++i];
        } else if (argv[i][0] != '-' && !src_name) {
            src_name = argv[i];
        }
    }
    if (!src_name) {
        serial_puts("cc: no input file\n");
        return;
    }

    /* Read source file from OsitoFS */
    void *file = osfs2_find(src_name);
    if (!file) {
        serial_puts("cc: file not found: ");
        serial_puts(src_name);
        serial_puts("\n");
        return;
    }
    uint64_t size = osfs2_file_size(file);
    char *src = malloc((size_t)size + 1);
    if (!src) { serial_puts("cc: malloc failed\n"); return; }
    osfs2_read(file, 0, src, size);
    src[size] = 0;

    const char *ext = strrchr(src_name, '.');
    const char *lang = (ext && (!strcmp(ext, ".c") || !strcmp(ext, ".h"))) ? "c" : "c++";

    if (out_name) {
        /* Compile-only mode: produce wasm bytes, write to OsitoFS */
        js_cc_compile(src, lang);
        free(src);
        /* No streaming output for pure compile, just block on done. */
        while (!js_cc_done()) emscripten_sleep(50);

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
int  nvme_read(uint64_t lba, uint32_t count, void *buf)
{ return nvme_read_bytes(lba * 512, buf, (uint64_t)count * 512); }
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
