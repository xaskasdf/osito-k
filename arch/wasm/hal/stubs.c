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
#include <dlfcn.h>
#include <emscripten.h>

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

    /* Extract .so from OsitoFS to Emscripten MEMFS */
    void *buf = malloc((size_t)size);
    if (!buf) { serial_puts("[EXEC] malloc failed\n"); return -1; }
    osfs2_read(file, 0, buf, size);

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
    /* Call the entry point. With EMULATE_FUNCTION_POINTER_CASTS on the
     * kernel, direct C function pointer calls go through the emulation
     * wrapper which handles cross-module table entries. */
    int ret = entry(argc, (char **)argv);
    serial_puts("[EXEC] entry returned\n");

    /* Check if the app exported a frame function for rAF scheduling.
     * Side modules can't use emscripten_set_main_loop — the kernel
     * must schedule their frame callback via JS requestAnimationFrame. */
    typedef void (*frame_fn)(void);
    frame_fn frame = (frame_fn)dlsym(handle, "q2_frame");
    if (frame) {
        serial_puts("[EXEC] Setting up frame loop via rAF\n");
        /* Store frame function pointer for JS to call */
        EM_ASM({
            var framePtr = $0;
            function __appFrame() {
                try { dynCall('v', framePtr); } catch(e) { console.error('[EXEC] frame error:', e); return; }
                requestAnimationFrame(__appFrame);
            }
            requestAnimationFrame(__appFrame);
        }, frame);
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

/* ── Network ─────────────────────────────────────────────────── */

int  i211_init(uint64_t bar0) { (void)bar0; return -1; }
bool i211_link_up(void)       { return false; }

void net_init(const uint8_t ip[4]) { (void)ip; }
void net_poll(void) {}
void net_udp_send(const uint8_t *dst_ip, uint16_t dst_port, uint16_t src_port,
                  const void *data, uint32_t len)
{
    (void)dst_ip; (void)dst_port; (void)src_port; (void)data; (void)len;
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
void sched_yield(void) { emscripten_sleep(0); }
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
void kb_push_esc(const char *seq, int len) { (void)seq; (void)len; }

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
