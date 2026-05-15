/*
 * OsitoK x86-64 — Mini Shell
 *
 * X-OS9: Interactive command shell with builtins.
 * Reads lines from terminal, parses commands, dispatches.
 *
 * Builtins: help, ps, mem, echo, ls, cat, reboot, halt, clear, uname
 */

#include "../include/types.h"
#include "../include/boot_info.h"
#include "../fs/vfs.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putchar(char c);
extern void fb_putdec(uint64_t val);
extern void fb_clear(void);

/* Terminal */
extern int  term_readline(const char *prompt, char *buf, uint32_t buf_size);
extern void term_init(void);

/* Keyboard */
extern void kb_init(void);

/* Memory */
extern uint64_t mem_get_total(void);
extern uint64_t mem_get_used(void);

/* Processes */
extern void proc_list(void);
extern int  proc_exec(const char *filename, int argc, const char **argv);

/* Ticks */
extern uint64_t idt_get_ticks(void);

/* OsitoFS */
extern bool osfs2_is_mounted(void);
extern void osfs2_list(void);
extern void *osfs2_find(const char *name);
extern int  osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int  osfs2_delete(const char *name);
extern uint32_t osfs2_free_blocks(void);
extern uint32_t osfs2_get_block_size(void);
extern void acpi_shutdown(void);
extern void acpi_reboot(void);

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Scheduler (X-SCHED) */
extern int  sched_spawn(const char *name, void (*entry)(void));
extern void sched_yield(void);
extern uint64_t sched_get_switches(void);
extern bool sched_is_enabled(void);

/* Git */
extern int git_init(void);
extern int git_add(const char *filename);
extern int git_commit(const char *message);
extern int git_log(void);
extern int git_status(void);
extern int git_diff(void);
extern int git_branch(const char *name);
extern int git_checkout(const char *branch);

/* Network */
extern void net_poll(void);

/* HW detection (set during boot, used in shell banner) */
extern int  pci_get_device_count(void);
extern bool nvme_is_ready(void);
extern bool i211_link_up(void);
extern bool xhci_is_ready(void);
extern void     net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
extern uint32_t net_icmp_get_rx_count(void);
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t buf_size);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size, uint32_t timeout_ticks);
extern void net_tcp_close(int conn);
extern int  net_tcp_state(int conn);
extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int  net_tcp_listen(uint16_t port);
extern int  net_tcp_accept(int listener, uint32_t timeout_ticks);
extern void net_tcp_stop_listen(int listener);
extern uint64_t osfs2_file_size(void *file);

/* TLS — opaque pointer, allocated via kmalloc(tls_conn_size()) */
extern uint32_t tls_conn_size(void);
extern int  tls_connect(void *tls, int tcp_conn, const char *hostname);
extern int  tls_send(void *tls, const void *data, uint32_t len);
extern int  tls_recv(void *tls, void *buf, uint32_t buf_size, uint32_t timeout_ticks);
extern void tls_close(void *tls);

/* HTTP — uses http_session_t (includes tls_conn_t, ~17KB) */
extern int  http_open(void *session, const char *hostname);
extern int  http_request(void *session, const char *method, const char *path,
                         const char *hostname, const char *const *req_headers,
                         const void *body, uint32_t body_len, void *resp);
extern int  http_read_body_full(void *session, const void *resp,
                                void *buf, uint32_t buf_size);
extern void http_close(void *session);
extern const char *http_get_header(const void *resp, const char *name);

/* http_session_t size = sizeof(tls_conn_t) + int + bool + padding.
 * Approximate with tls_conn_size() + 16. Actually let's just define it: */
extern uint32_t http_session_size(void);
extern uint32_t http_response_size(void);

/* Claude API */
extern void claude_set_api_key(const char *key);
extern const char *claude_get_api_key(void);
extern int  claude_chat(const void *messages, int msg_count,
                        const char *model, int max_tokens,
                        int (*callback)(const char *, uint32_t, void *), void *ctx);

/* Claude Session (X-CL2/X-CL3) — opaque pointer, managed by claude.c */
extern void *claude_session_new(void);
extern void  claude_session_free(void *s);
extern void  claude_session_clear(void *s);
extern int   claude_session_send(void *s, const char *user_msg,
                                  int (*callback)(const char *, uint32_t, void *), void *ctx);
extern int   claude_session_send_with_tools(void *s, const char *user_msg,
                                             int (*callback)(const char *, uint32_t, void *), void *ctx);

/* Tokenizer */
extern char g_tokenizer[];  /* tokenizer_t (opaque) */
extern int  tok_encode(const void *tok, const char *text, uint32_t text_len,
                       uint32_t *out, uint32_t max_out);
extern const char *tok_decode_one(const void *tok, uint32_t token_id);
extern bool tok_is_ready(const void *tok);

/* Inference */
extern void *prompt_llama;  /* llama_state_t* from main.c */
extern int  llama_chat(void *state, const char *text, uint32_t max_tokens,
                       void (*on_token)(const char *text, void *ctx), void *ctx);
extern void llama_set_sampling(float temperature, float top_p);
extern void llama_set_penalty(float rep, float presence, float frequency);
extern void llama_get_penalty(float *rep, float *presence, float *frequency);
extern void brandon_set_features(int dwa, int v_residual, int registers);
extern void brandon_set_debug_logits(int on);
extern void     llama_set_ngram_size(uint32_t n);
extern uint32_t llama_get_ngram_size(void);

#ifdef __EMSCRIPTEN__
extern void wasm_config_save(const char *key, const char *value);
extern int  wasm_config_load(const char *key, char *dst, int max);

/* Tiny float→string helper: 2-decimal fixed-point as "1.20". */
static void fmt_float(float v, char *dst, int max)
{
    int n = 0;
    int neg = v < 0; if (neg) v = -v;
    int hundred = (int)(v * 100.0f + 0.5f);
    int whole = hundred / 100, frac = hundred % 100;
    if (neg && n < max - 1) dst[n++] = '-';
    char tmp[16]; int t = 0;
    if (whole == 0) tmp[t++] = '0';
    else while (whole) { tmp[t++] = '0' + whole % 10; whole /= 10; }
    while (t && n < max - 1) dst[n++] = tmp[--t];
    if (n < max - 1) dst[n++] = '.';
    if (n < max - 1) dst[n++] = '0' + frac / 10;
    if (n < max - 1) dst[n++] = '0' + frac % 10;
    dst[n] = '\0';
}

static float parse_cfg_float(const char *s)
{
    float r = 0.0f, frac = 0.0f, div = 1.0f;
    bool dot = false;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s) {
        if (*s == '.') { dot = true; s++; continue; }
        if (*s < '0' || *s > '9') break;
        if (dot) { div *= 10.0f; frac = frac * 10.0f + (*s - '0'); }
        else r = r * 10.0f + (*s - '0');
        s++;
    }
    float result = r + frac / div;
    return neg ? -result : result;
}

void shell_persist_load_config(void)
{
    char buf[64];
    if (wasm_config_load("temp", buf, sizeof(buf)) > 0) {
        float t = parse_cfg_float(buf);
        if (t > 0.0f && t < 5.0f) llama_set_sampling(t, 0.9f);
    }
    if (wasm_config_load("topp", buf, sizeof(buf)) > 0) {
        float p = parse_cfg_float(buf);
        if (p > 0.0f && p <= 1.0f) {
            char tbuf[16];
            float curr_t = 0.7f;
            if (wasm_config_load("temp", tbuf, sizeof(tbuf)) > 0)
                curr_t = parse_cfg_float(tbuf);
            llama_set_sampling(curr_t, p);
        }
    }
    if (wasm_config_load("rep", buf, sizeof(buf)) > 0) {
        float r = parse_cfg_float(buf);
        float pre = 0.0f, frq = 0.0f;
        char b2[64];
        if (wasm_config_load("pres", b2, sizeof(b2)) > 0) pre = parse_cfg_float(b2);
        if (wasm_config_load("freq", b2, sizeof(b2)) > 0) frq = parse_cfg_float(b2);
        if (r >= 1.0f && r <= 3.0f) llama_set_penalty(r, pre, frq);
    }
    if (wasm_config_load("ngram", buf, sizeof(buf)) > 0) {
        int n = 0;
        for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
            n = n * 10 + (*s - '0');
        if (n >= 0 && n <= 10) llama_set_ngram_size((uint32_t)n);
    }
}
#endif

#ifdef __EMSCRIPTEN__
/* Auxiliary disk fetch — used by mount-iso/mount-ext2 to load a disk
 * image from the network into a HEAP buffer that fs drivers read
 * through a custom callback (no contention with primary OsitoFS). */
extern int      aux_disk_fetch(const char *url);
extern int      aux_disk_read_iso(uint64_t lba, uint32_t count, void *buf);
extern int      aux_disk_read_512(uint64_t lba, uint32_t count, void *buf);
extern uint64_t aux_disk_size(void);

/* iso9660 */
extern int      iso9660_mount(int (*read_fn)(uint64_t lba, uint32_t count, void *buf));
extern bool     iso9660_is_mounted(void);
extern int      iso9660_ls(const char *path);
extern int      iso9660_read_file(const char *name, uint64_t offset, void *buf, uint64_t len);
extern int      iso9660_find(const char *name, uint32_t *lba_out, uint32_t *size_out);

/* ext2/3/4 */
extern int      ext2_mount(uint64_t part_lba,
                            int (*read_fn)(uint64_t lba, uint32_t count, void *buf));
extern bool     ext2_is_mounted(void);
extern int      ext2_ls(void);
extern int      ext2_find(const char *name, uint32_t *ino_out);
extern int      ext2_read_file(const char *name, uint64_t offset, void *buf, uint64_t len);

/* fat32 — uses nvme_read internally; we toggle the route flag around it */
extern void     wasm_nvme_route_aux(int on);
extern int      fat32_mount(uint64_t part_lba);
extern bool     fat32_is_mounted(void);
extern int      fat32_ls(const char *path);
extern int      fat32_find(const char *name, uint32_t *cluster_out, uint32_t *size_out);
extern int      fat32_read_file(const char *name, uint64_t offset, void *buf, uint64_t len);

/* part_lba-based mounts — all use nvme_read internally, need route flag. */
extern int      exfat_mount(uint64_t part_lba);
extern bool     exfat_is_mounted(void);
extern int      exfat_ls(void);
extern int      exfat_read_file(const char *name, uint64_t offset, void *buf, uint64_t len);

extern int      ntfs_mount(uint64_t part_lba);
extern bool     ntfs_is_mounted(void);
extern int      ntfs_ls(void);
extern int      ntfs_read_file(const char *name, uint64_t offset, void *buf, uint64_t len);

extern int      hfsplus_mount(uint64_t part_lba);
extern bool     hfsplus_is_mounted(void);
extern int      hfsplus_ls(void);

extern int      btrfs_mount(uint64_t part_lba);
extern bool     btrfs_is_mounted(void);
extern int      btrfs_ls(void);

extern int      apfs_mount(uint64_t part_lba);
extern bool     apfs_is_mounted(void);
extern int      apfs_ls(void);

/* Callback-based mounts (ls-only). */
extern int      udf_mount(int (*read_fn)(uint64_t lba, uint32_t count, void *buf));
extern bool     udf_is_mounted(void);
extern int      udf_ls(void);

extern int      squashfs_mount(int (*read_fn)(uint64_t offset, void *buf, uint64_t len),
                                uint64_t total_size);
extern bool     squashfs_is_mounted(void);
extern int      squashfs_ls(void);

/* squashfs read takes byte-offset, not LBA. Wrap via aux_disk_ptr(). */
extern uint8_t *aux_disk_ptr(void);
static int aux_disk_read_bytes_wrap(uint64_t offset, void *buf, uint64_t len)
{
    extern uint64_t aux_disk_size(void);
    uint8_t *p = aux_disk_ptr();
    if (!p || offset + len > aux_disk_size()) return -1;
    for (uint64_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = p[offset + i];
    return 0;
}

/* Generic mount table */
struct aux_fs_t {
    const char *name;                      /* short tag for `mount-fs` */
    int   route_aux;                       /* 1 if mount uses nvme_read */
    int  (*mount_partlba)(uint64_t);       /* either this … */
    int  (*mount_iso)(int (*)(uint64_t, uint32_t, void *));   /* … or this … */
    int  (*mount_sqfs)(int (*)(uint64_t, void *, uint64_t), uint64_t);
    bool (*is_mounted)(void);
    int  (*ls)(const char *path);          /* called with NULL or "/" */
    int  (*read_file)(const char *name, uint64_t off, void *buf, uint64_t len);
};
typedef struct aux_fs_t aux_fs_t;

static int wrap_iso_ls(const char *p)   { return iso9660_ls(p ? p : "/"); }
static int wrap_ext_ls(const char *p)   { (void)p; return ext2_ls(); }
static int wrap_fat_ls(const char *p)   { return fat32_ls(p ? p : "/"); }
static int wrap_exfat_ls(const char *p) { (void)p; return exfat_ls(); }
static int wrap_ntfs_ls(const char *p)  { (void)p; return ntfs_ls(); }
static int wrap_hfs_ls(const char *p)   { (void)p; return hfsplus_ls(); }
static int wrap_btrfs_ls(const char *p) { (void)p; return btrfs_ls(); }
static int wrap_apfs_ls(const char *p)  { (void)p; return apfs_ls(); }
static int wrap_udf_ls(const char *p)   { (void)p; return udf_ls(); }
static int wrap_sqfs_ls(const char *p)  { (void)p; return squashfs_ls(); }

static const aux_fs_t aux_fs_table[] = {
    { "iso",   0, NULL,           iso9660_mount, NULL,           iso9660_is_mounted, wrap_iso_ls,   iso9660_read_file },
    { "ext",   0, NULL,           NULL,          NULL,           ext2_is_mounted,    wrap_ext_ls,   ext2_read_file    },
    { "fat",   1, fat32_mount,    NULL,          NULL,           fat32_is_mounted,   wrap_fat_ls,   fat32_read_file   },
    { "exfat", 1, exfat_mount,    NULL,          NULL,           exfat_is_mounted,   wrap_exfat_ls, exfat_read_file   },
    { "ntfs",  1, ntfs_mount,     NULL,          NULL,           ntfs_is_mounted,    wrap_ntfs_ls,  ntfs_read_file    },
    { "hfs",   1, hfsplus_mount,  NULL,          NULL,           hfsplus_is_mounted, wrap_hfs_ls,   NULL              },
    { "btrfs", 1, btrfs_mount,    NULL,          NULL,           btrfs_is_mounted,   wrap_btrfs_ls, NULL              },
    { "apfs",  1, apfs_mount,     NULL,          NULL,           apfs_is_mounted,    wrap_apfs_ls,  NULL              },
    { "udf",   0, NULL,           udf_mount,     NULL,           udf_is_mounted,     wrap_udf_ls,   NULL              },
    { "sqfs",  0, NULL,           NULL,          squashfs_mount, squashfs_is_mounted,wrap_sqfs_ls,  NULL              },
};
#define AUX_FS_COUNT (int)(sizeof(aux_fs_table)/sizeof(aux_fs_table[0]))

/* Per-type mount table — each fs name owns its own slot. NULL means
 * not mounted. The "active" concept from the single-aux era survives
 * as g_last_active_aux_fs (tracks whatever was most recently mounted)
 * for /aux/ wildcard prefix support. */
static const aux_fs_t *g_aux_mounts[10];        /* indexed by aux_fs_table position */
static const aux_fs_t *g_last_active_aux_fs = NULL;
#define g_active_aux_fs g_last_active_aux_fs    /* legacy alias */

extern int  wasm_aux_alloc_slot(const char *name);
extern int  wasm_aux_find_slot(const char *name);
extern void wasm_aux_release_slot(int slot);
extern int  aux_disk_fetch_slot(int slot, const char *url);
extern void wasm_aux_route_slot(int slot);

static const aux_fs_t *find_aux_fs(const char *name)
{
    for (int i = 0; i < AUX_FS_COUNT; i++)
        if (strcmp(aux_fs_table[i].name, name) == 0)
            return &aux_fs_table[i];
    return NULL;
}

/* Public accessors used by cmd_cat / cmd_ls auto-mount path.
 * The "active" fs is whichever was most recently mounted, used as the
 * fallback for the /aux/ wildcard prefix. /<type>/ prefixes look up
 * specifically in g_aux_mounts[] by name. */
const aux_fs_t *cmd_active_aux_fs(void) { return g_last_active_aux_fs; }

const aux_fs_t *cmd_aux_fs_by_name(const char *name)
{
    for (int i = 0; i < AUX_FS_COUNT; i++)
        if (g_aux_mounts[i] && strcmp(g_aux_mounts[i]->name, name) == 0)
            return g_aux_mounts[i];
    return NULL;
}

/* Internal: set the disk slot for fs's name before invoking its driver. */
static int aux_route_for(const aux_fs_t *fs, int on)
{
    if (!fs) return -1;
    int slot = wasm_aux_find_slot(fs->name);
    if (slot < 0) return -1;
    wasm_aux_route_slot(on ? slot : -1);
    wasm_nvme_route_aux(on ? fs->route_aux : 0);
    return slot;
}

int cmd_aux_fs_read_for(const aux_fs_t *fs, const char *name, void *buf, int max)
{
    if (!fs || !fs->read_file) return -1;
    if (name[0] == '/') name++;
    if (aux_route_for(fs, 1) < 0) return -1;
    int rc = fs->read_file(name, 0, buf, max);
    aux_route_for(fs, 0);
    return rc;
}

int cmd_aux_fs_read(const char *name, void *buf, int max)
{
    return cmd_aux_fs_read_for(g_last_active_aux_fs, name, buf, max);
}

int cmd_aux_fs_ls_for(const aux_fs_t *fs, const char *path)
{
    if (!fs) return -1;
    const char *p = (!path || !*path) ? NULL : path;
    if (p && p[0] == '/' && p[1] == '\0') p = NULL;
    if (aux_route_for(fs, 1) < 0) return -1;
    int rc = fs->ls(p);
    aux_route_for(fs, 0);
    return rc;
}

int cmd_aux_fs_ls(const char *path)
{
    return cmd_aux_fs_ls_for(g_last_active_aux_fs, path);
}

void cmd_clear_aux_fs(void)
{
    /* Release all mounts. */
    for (int i = 0; i < AUX_FS_COUNT; i++) {
        if (g_aux_mounts[i]) {
            int slot = wasm_aux_find_slot(g_aux_mounts[i]->name);
            if (slot >= 0) wasm_aux_release_slot(slot);
            g_aux_mounts[i] = NULL;
        }
    }
    g_last_active_aux_fs = NULL;
}

void cmd_clear_aux_fs_by_name(const char *name)
{
    for (int i = 0; i < AUX_FS_COUNT; i++) {
        if (g_aux_mounts[i] && strcmp(g_aux_mounts[i]->name, name) == 0) {
            int slot = wasm_aux_find_slot(name);
            if (slot >= 0) wasm_aux_release_slot(slot);
            g_aux_mounts[i] = NULL;
            if (g_last_active_aux_fs &&
                strcmp(g_last_active_aux_fs->name, name) == 0)
                g_last_active_aux_fs = NULL;
            return;
        }
    }
}
#endif

/* ── WASM-compatibility shims ────────────────────────────────── */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static inline void sh_hlt(void)    { emscripten_sleep(0); }  /* yield to JS */
static inline void sh_cli(void)    { /* no interrupts to disable */ }
static inline void sh_sti(void)    { /* no interrupts to enable  */ }
static inline void sh_reboot(void) { serial_puts("\n[WASM] Reboot: reload the page.\n"); }
static inline void sh_halt(void)   { serial_puts("\n[WASM] Halted. Reload to restart.\n"); for(;;){} }
#else
static inline void sh_hlt(void)    { __asm__ volatile ("hlt"); }
static inline void sh_cli(void)    { __asm__ volatile ("cli"); }
static inline void sh_sti(void)    { __asm__ volatile ("sti"); }
static inline void sh_reboot(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
}
static inline void sh_halt(void)   { __asm__ volatile ("cli"); for (;;) __asm__ volatile ("hlt"); }
#endif

/* ── Shell output helpers ────────────────────────────────────── */

/* Output redirect hook (set by shell_exec for > and >> operators) */
/* Non-static so wasm cmd_osito_chat_sync can hijack temporarily. */
void (*sh_redir_fn)(const char *s, size_t len);

/* Pipe input — set by the pipeline driver to feed previous stage's
 * captured output as virtual stdin for the next stage.  Only consumed
 * by stdin-aware commands (grep/head/tail) when no file argument is
 * given.  Caller is responsible for buffer lifetime across the call. */
/* Pipe / `<` stdin handoff. Non-static so wasm cmd_cc can read it. */
const char *sh_stdin_buf;
uint32_t    sh_stdin_len;

void sh_puts(const char *s)
{
    if (sh_redir_fn) {
        size_t len = 0;
        while (s[len]) len++;
        sh_redir_fn(s, len);
    } else {
        serial_puts(s);
        fb_puts(s);
    }
}

static void sh_puts_color(const char *s, uint32_t color)
{
    if (sh_redir_fn) {
        size_t len = 0;
        while (s[len]) len++;
        sh_redir_fn(s, len);
    } else {
        serial_puts(s);
        fb_puts_color(s, color);
    }
}

static void sh_putdec(uint64_t val)
{
    if (sh_redir_fn) {
        /* Convert to decimal string */
        char buf[24];
        int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else {
            uint64_t tmp = val;
            char rev[24];
            int j = 0;
            while (tmp) { rev[j++] = '0' + (tmp % 10); tmp /= 10; }
            while (j--) buf[i++] = rev[j];
        }
        buf[i] = '\0';
        sh_redir_fn(buf, (size_t)i);
    } else {
        serial_putdec(val);
        fb_putdec(val);
    }
}

/* ── Parse command line into argv ────────────────────────────── */

#define MAX_ARGS 64

static int parse_args(char *line, char *argv[])
{
    int argc = 0;
    char *p = line;

    while (*p && argc < MAX_ARGS) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    return argc;
}

/* ── Builtin: help ───────────────────────────────────────────── */

static void cmd_help(void)
{
    sh_puts_color("OsitoK Shell Commands:\n", 0x00FF8800);
    sh_puts("  help      Show this message\n");
    sh_puts("  uname     System information\n");
    sh_puts("  ps        List processes\n");
    sh_puts("  cpus      Show CPU cores (SMP)\n");
    sh_puts("  mem       Memory usage\n");
    sh_puts("  uptime    Show uptime\n");
    sh_puts("  echo      Print arguments\n");
    sh_puts("  ls        List files on disk\n");
    sh_puts("  cat       Display file contents\n");
    sh_puts("  head      First N lines (head <file> [-n N | N])\n");
    sh_puts("  tail      Last N lines (tail <file> [-n N | N])\n");
    sh_puts("  grep      Match literal substring (grep <pat> [file])\n");
    sh_puts("  |         Pipe stdout of one cmd into next (e.g. dmesg | grep PCI)\n");
    sh_puts("  kexec     Boot a new kernel ELF (kexec [filename])\n");
    sh_puts("  kdownload Fetch a file via OFTP (kdownload <ip> <port> <name> [save|--kexec])\n");
    sh_puts("  kupload   Push a file via OFTP (kupload <ip> <port> <local|--dmesg> [remote])\n");
    sh_puts("  kupdate   (TODO) Pull a kernel update from naranjositos.tech via HTTPS\n");
    sh_puts("  exec      Run an ELF binary\n");
    sh_puts("  ping      Ping an IP address\n");
    sh_puts("  tcptest   TCP connection test (tcptest [ip] [port])\n");
    sh_puts("  resolve   DNS lookup (resolve hostname)\n");
    sh_puts("  tlstest   TLS connect test (tlstest [hostname])\n");
    sh_puts("  curl      HTTPS GET (curl hostname [path])\n");
    sh_puts("  apikey    Set Claude API key (apikey sk-ant-...)\n");
    sh_puts("  ask       Ask Claude (ask <prompt>)\n");
    sh_puts("  claude    Claude REPL (multi-turn conversation)\n");
    sh_puts("  chat      Local inference (chat <prompt>)\n");
    sh_puts("  temp      Set sampling (temp <temperature> [top_p])\n");
    sh_puts("  dl        Dynamic linker (dl load/sym/call/close/list)\n");
    sh_puts("  git       Version control (init/add/commit/log/status/diff/branch/checkout)\n");
    sh_puts("  sched     Scheduler test (sched [stats])\n");
    sh_puts("  httpd     HTTP server (httpd [port] / httpd stop)\n");
    sh_puts("  dhcp      Retry DHCP discovery (manual)\n");
    sh_puts("  apipa     Auto-assign link-local 169.254.X.Y (RFC 3927)\n");
    sh_puts("  ipconf    Set static IP (ipconf <ip> [gw] [mask] [dns])\n");
    sh_puts("  winexec   Run a Win32 PE executable (winexec file.exe)\n");
    sh_puts("  dosrun    Run a DOS 16-bit binary (dosrun file.com)\n");
    sh_puts("  clear     Clear screen\n");
    sh_puts("  desktop   Launch graphical desktop (elementaryOS style)\n");
    sh_puts("  kexec     Load + boot kernel from disk (kexec [file])\n");
    sh_puts("  reboot    Reboot system\n");
    sh_puts("  halt      Halt CPU\n");
    sh_puts_color("I/O Redirection:\n", 0x00FF8800);
    sh_puts("  cmd > file    Write output to file\n");
    sh_puts("  cmd >> file   Append output to file\n");

#ifdef __EMSCRIPTEN__
    sh_puts_color("\nLLM (brandon-tiny default, sampling auto-tuned):\n", 0x00FF8800);
    sh_puts("  chat <prompt>           Local inference (active model)\n");
    sh_puts("  wiki <query>            Browser-RAG: simple_en retrieval + chat\n");
    sh_puts("  wiki status             Show cached RAG shards on OsitoFS\n");
    sh_puts("  rag <ctx> ::: <q>       Inline-context RAG (no retrieval)\n");
    sh_puts("  bench [n]               Time inference, n tokens (default 32)\n");
    sh_puts("  temp <t> [topp]         Set sampling temp + top-p\n");
    sh_puts("  penalty <r> [pres][freq]  rep + presence + frequency\n");
    sh_puts("  ngram <n>               no_repeat_ngram_size (3 = balanced)\n");
    sh_puts("  bdebug <dwa|vr|reg|...> Toggle brandon-arch features\n");
    sh_puts("  model <name|url>        brandon | tinystories | smollm | URL\n");

    sh_puts_color("\nFilesystem (12 mountable read-only formats):\n", 0x00FF8800);
    sh_puts("  mount-fs <type> <url>   iso ext fat exfat ntfs hfs btrfs apfs udf sqfs\n");
    sh_puts("  ls /<type>/  cat /<type>/<file>   /aux/ for the last-active mount\n");
    sh_puts("  fs-ls / fs-cat          Legacy aliases\n");
    sh_puts("  umount [type]           Detach (no arg = all)\n");
    sh_puts("  du / df                 File sizes / free space\n");
    sh_puts("  find <substring>        Match file names\n");
    sh_puts("  touch <file>            Create empty file\n");
    sh_puts("  hexdump <file> [N]      Hex+ASCII dump (default 256 bytes)\n");
    sh_puts("  base64 [-d] <text>      Encode / decode\n");
    sh_puts("  samples list/cat/build/run   Walk the seeded /samples/*.c\n");
    sh_puts("  precache               Background-fetch cc toolchain (~50 MB)\n");

    sh_puts_color("\nPackage manager (WASI binaries from R2 catalog):\n", 0x00FF8800);
    sh_puts("  pkg list/search <q>     Browse the catalog (10 utilities)\n");
    sh_puts("  pkg install <name>      Fetch <name>.wasm to /pkg/\n");
    sh_puts("  pkg run <name> [args]   Auto-install + exec; piped stdin works\n");
    sh_puts("  pkg installed/uninstall Local list / remove\n");
    sh_puts("  e.g. echo hola | pkg run rev   ->  aloh\n");

    sh_puts_color("\nNetwork (browser-bridged):\n", 0x00FF8800);
    sh_puts("  curl <url>              JS fetch — any URL CORS allows\n");
    sh_puts("  https <host> [path]     Real TLS 1.2 over WS-tunneled TCP\n");
    sh_puts("  ws open|send|recv|...   WebSocket primitive\n");
    sh_puts("  tcp connect|proxy|...   TCP via configurable WS proxy\n");
    sh_puts("  crypto sha256/sha512    SHA hash on stdin/argv\n");

    sh_puts_color("\nIntrospection / persistence / UX:\n", 0x00FF8800);
    sh_puts("  info                    Kernel state dashboard\n");
    sh_puts("  version                 Build hash + timestamp\n");
    sh_puts("  uname [-a]              Kernel identity\n");
    sh_puts("  date                    UTC ISO time (JS)\n");
    sh_puts("  whoami                  Always 'osito'\n");
    sh_puts("  stress [net]            Smoke test the bridges\n");
    sh_puts("  benchmark               sha256 + osfs2 + tok/s\n");
    sh_puts("  time <cmd...>           Wall-clock the command\n");
    sh_puts("  bench [n]               Time inference, n tokens\n");
    sh_puts("  save                    Force-flush FS to IndexedDB\n");
    sh_puts("  reload                  Save + reload page\n");
    sh_puts("  alias [name=val] ...    Bash-style aliases (persisted)\n");
    sh_puts("  unalias <name>          Remove an alias\n");
    sh_puts("  history [clear]         localStorage cmd history (Up/Dn)\n");
    sh_puts("  demo                    Run 6 representative commands\n");
    sh_puts("  tutorial                Interactive 8-step tour\n");
    sh_puts("  Persistence:            FS image → IndexedDB; sampling → localStorage\n");
    sh_puts("                           .osito_init in OsitoFS auto-runs at boot\n");
#endif
}

/* ── Builtin: uname ──────────────────────────────────────────── */

static void cmd_uname(void)
{
    sh_puts_color("OsitoK", 0x00FF8800);
#ifdef WASM_BUILD
    sh_puts(" wasm32 AI OS (");
#else
    sh_puts(" x86-64 AI OS (");
#endif
    sh_puts("naranjositos.tech");
    sh_puts(")\n");
}

/* ── Builtin: mem ────────────────────────────────────────────── */

static void cmd_mem(void)
{
    uint64_t total = mem_get_total();
    uint64_t used = mem_get_used();
    uint64_t free = total - used;

    sh_puts("Memory:\n");
    sh_puts("  Total: ");
    sh_putdec(total / (1024 * 1024));
    sh_puts(" MB (");
    sh_putdec(total / 1024);
    sh_puts(" KB)\n");
    sh_puts("  Used:  ");
    sh_putdec(used / (1024 * 1024));
    sh_puts(" MB (");
    sh_putdec(used / 1024);
    sh_puts(" KB)\n");
    sh_puts("  Free:  ");
    sh_putdec(free / (1024 * 1024));
    sh_puts(" MB (");
    sh_putdec(free / 1024);
    sh_puts(" KB)\n");
}

/* ── Builtin: uptime ─────────────────────────────────────────── */

static void cmd_uptime(void)
{
    uint64_t ticks = idt_get_ticks();
    uint64_t secs = ticks / 100;  /* ~100 Hz timer */
    uint64_t mins = secs / 60;
    secs %= 60;

    sh_puts("up ");
    sh_putdec(mins);
    sh_puts("m ");
    sh_putdec(secs);
    sh_puts("s (");
    sh_putdec(ticks);
    sh_puts(" ticks)\n");
}

/* ── Builtin: echo ───────────────────────────────────────────── */

static void cmd_echo(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) sh_puts(" ");
        sh_puts(argv[i]);
    }
    sh_puts("\n");
}

/* ── Builtin: ls ─────────────────────────────────────────────── */

/* Print `n` spaces — used for column padding in cmd_ls. */
static void sh_pad(uint32_t n)
{
    while (n-- > 0) sh_puts(" ");
}

/* Print a uint64 right-justified in a `width`-character column. */
static void sh_putdec_padded(uint64_t v, uint32_t width)
{
    char buf[24];
    int  len = 0;
    if (v == 0) { buf[len++] = '0'; }
    else {
        char tmp[24];
        int  t = 0;
        while (v > 0 && t < 24) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t > 0) buf[len++] = tmp[--t];
    }
    if ((uint32_t)len < width) sh_pad(width - (uint32_t)len);
    for (int i = 0; i < len; i++) {
        char s[2] = { buf[i], 0 };
        sh_puts(s);
    }
}

/*
 * cmd_ls — list files in the mounted OsitoFS volume.
 *
 * Prints one line per file: <size>  <name>  [model info if GGUF].
 * Right-justifies sizes in a 10-char column so the output reads like
 * `ls -l` on Unix. Footer summarises file count + free blocks so the
 * user can see capacity at a glance, similar to df. Output goes to the
 * terminal (sh_puts), not just the serial console.
 */
static void cmd_ls(void)
{
    if (!osfs2_is_mounted()) {
        sh_puts("No filesystem mounted\n");
        return;
    }

    extern uint32_t osfs2_file_count(void);
    extern const char *osfs2_label(void);
    extern uint32_t osfs2_free_blocks(void);
    extern uint32_t osfs2_get_block_size(void);
    extern void *osfs2_file_at(int index);
    extern const char *osfs2_file_name(void *file);
    extern uint64_t osfs2_file_size(void *file);
    extern uint32_t osfs2_file_mtime(void *file);
    (void)osfs2_file_mtime;

    uint32_t total = osfs2_file_count();
    if (total == 0) {
        sh_puts("(empty)\n");
        return;
    }

    sh_puts("       size  name\n");

    /* osfs2_file_at iterates the file table, skipping invalid slots
     * internally — we just walk indices until we've seen `total` valid
     * entries. The caller-side cap stops a runaway when the table is
     * dense. */
    uint32_t shown = 0;
    for (uint32_t i = 0; i < 4096 && shown < total; i++) {
        void *f = osfs2_file_at(i);
        if (!f) continue;
        sh_putdec_padded(osfs2_file_size(f), 11);
        sh_puts("  ");
        sh_puts(osfs2_file_name(f));
        sh_puts("\n");
        shown++;
    }

    /* Footer: file count + free space (in blocks AND bytes for clarity). */
    uint32_t bs    = osfs2_get_block_size();
    uint32_t freeb = osfs2_free_blocks();
    sh_puts("\n");
    sh_putdec((uint64_t)shown);
    sh_puts(" file(s) on ");
    sh_puts(osfs2_label());
    sh_puts(", ");
    sh_putdec((uint64_t)freeb * (uint64_t)bs / (1024 * 1024));
    sh_puts(" MB free (");
    sh_putdec((uint64_t)freeb);
    sh_puts(" blocks of ");
    sh_putdec((uint64_t)bs);
    sh_puts("B)\n");
}

/* ── Builtin: cat ────────────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
/* Forward-decl — defined later in this file alongside mount-fs. */
struct aux_fs_t;
typedef struct aux_fs_t aux_fs_t;
extern const aux_fs_t *cmd_active_aux_fs(void);
extern int  cmd_aux_fs_read(const char *name, void *buf, int max);
extern int  cmd_aux_fs_ls(const char *path);
#endif

static void cmd_cat(int argc, char *argv[])
{
    /* No filename: route piped/redirected stdin to stdout (so
     * `echo hi | cat`, `cat < file`, and `cat << EOF` work). */
    if (argc < 2) {
        if (sh_stdin_buf && sh_stdin_len > 0) {
            char one[2] = {0, 0};
            for (uint32_t i = 0; i < sh_stdin_len; i++) {
                one[0] = sh_stdin_buf[i];
                sh_puts(one);
            }
            return;
        }
        sh_puts("Usage: cat <filename>  (or pipe/redirect/heredoc into cat)\n");
        return;
    }

#ifdef __EMSCRIPTEN__
    /* Auto-mount path: any of /aux/, /iso/, /ext/, /fat/, /exfat/,
     * /ntfs/, /hfs/, /btrfs/, /apfs/, /udf/, /sqfs/ routes to the
     * currently mounted aux FS, but only when the prefix matches the
     * mounted type (so /iso/foo errors clearly if a FAT image is
     * mounted instead of ISO). The catch-all /aux/ accepts whatever
     * is mounted. */
    {
        static const struct { const char *prefix; int plen; const char *fs_name; } pre[] = {
            { "/aux/",   5, NULL },
            { "/iso/",   5, "iso"   },
            { "/ext/",   5, "ext"   },
            { "/fat/",   5, "fat"   },
            { "/exfat/", 7, "exfat" },
            { "/ntfs/",  6, "ntfs"  },
            { "/hfs/",   5, "hfs"   },
            { "/btrfs/", 7, "btrfs" },
            { "/apfs/",  6, "apfs"  },
            { "/udf/",   5, "udf"   },
            { "/sqfs/",  6, "sqfs"  },
        };
        for (int k = 0; k < (int)(sizeof(pre)/sizeof(pre[0])); k++) {
            int plen = pre[k].plen;
            int matched = 1;
            for (int j = 0; j < plen; j++) {
                if (argv[1][j] != pre[k].prefix[j]) { matched = 0; break; }
            }
            if (!matched) continue;

            /* /<type>/ → look up by name; /aux/ → last active. */
            const aux_fs_t *fs = pre[k].fs_name
                ? cmd_aux_fs_by_name(pre[k].fs_name)
                : cmd_active_aux_fs();
            if (!fs) {
                sh_puts("Not mounted: ");
                sh_puts(pre[k].fs_name ? pre[k].fs_name : "any aux FS");
                sh_puts("\n");
                return;
            }
            static char aux_buf[65536];
            int n = cmd_aux_fs_read_for(fs, argv[1] + plen, aux_buf, sizeof(aux_buf));
            if (n <= 0) {
                sh_puts("File not found.\n");
                return;
            }
            char chunk[256];
            int i = 0;
            while (i < n) {
                int kk = n - i; if (kk > 255) kk = 255;
                for (int j = 0; j < kk; j++) chunk[j] = aux_buf[i + j];
                chunk[kk] = '\0';
                sh_puts(chunk);
                i += kk;
            }
            sh_puts("\n");
            return;
        }
    }
#endif

    if (!osfs2_is_mounted()) {
        sh_puts("No filesystem mounted\n");
        return;
    }

    /* Get file info */
    typedef struct {
        char     name[64];
        uint64_t size;
    } osfs2_file_min_t;

    void *file = osfs2_find(argv[1]);
    if (!file) {
        sh_puts("File not found: ");
        sh_puts(argv[1]);
        sh_puts("\n");
        return;
    }

    osfs2_file_min_t *finfo = (osfs2_file_min_t *)file;
    uint64_t file_size = finfo->size;

    /* Limit display to 4KB */
    if (file_size > 4096) {
        sh_puts("(showing first 4096 bytes of ");
        sh_putdec(file_size);
        sh_puts(")\n");
        file_size = 4096;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file_size + 1);
    if (!buf) {
        sh_puts("Out of memory\n");
        return;
    }

    if (osfs2_read(file, 0, buf, file_size) < 0) {
        sh_puts("Read error\n");
        kfree(buf);
        return;
    }

    /* Print as text, replacing non-printable with '.' */
    for (uint64_t i = 0; i < file_size; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) {
            char s[2] = { c, 0 };
            sh_puts(s);
        } else {
            sh_puts(".");
        }
    }
    sh_puts("\n");

    kfree(buf);
}

/* ── Builtin: exec ───────────────────────────────────────────── */

static void cmd_exec(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: exec <filename>\n");
        return;
    }

    if (!osfs2_is_mounted()) {
        sh_puts("No filesystem mounted\n");
        return;
    }

    if (!osfs2_find(argv[1])) {
        sh_puts("File not found: ");
        sh_puts(argv[1]);
        sh_puts("\n");
        return;
    }

    sh_puts_color("Executing: ", 0x0000FF00);
    sh_puts(argv[1]);
    sh_puts("\n");

    int ret = proc_exec(argv[1], argc - 1, (const char **)(argv + 1));
    if (ret != 0) {
        sh_puts_color("[exec] exited with code ", 0x00FF4444);
        sh_putdec((uint64_t)(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret));
        sh_puts("\n");
    }
}

/* ── OFTP client (kdownload) ──────────────────────────────────────
 *
 * UDP-based file fetch from a server (for now: a Python script on a
 * directly-attached Mac, eventually naranjositos.tech via HTTPS).
 *
 * Workaround for the I211 Mac→OsitoK→Mac reply-path bug: OsitoK
 * initiates a single UDP REQ outbound (TX from shell context — known
 * to work), then receives all chunks Mac→OsitoK (RX path — known to
 * work).  No reply ever flows along the broken net_poll-context TX.
 *
 * Wire format documented in tools/oftp-server.py.
 */

#define OFTP_BUF_MAX  (16u * 1024u * 1024u)   /* up to 16 MB kernels       */
#define OFTP_LOCAL_PORT  7780
#define OFTP_TIMEOUT_TICKS  5000               /* 5 s no-progress timeout  */

static struct {
    bool       active;
    bool       error;
    bool       done;
    uint8_t   *buf;
    uint32_t   buf_size;
    uint32_t   total_size;       /* set by first valid chunk           */
    uint32_t   high_watermark;   /* highest offset+chunk_len observed  */
    uint64_t   last_chunk_tick;
} oftp_state;

static void oftp_handler(const uint8_t *src_ip, uint16_t src_port,
                          const void *data, uint32_t len)
{
    (void)src_ip; (void)src_port;
    if (!oftp_state.active || len < 4) return;

    const uint8_t *p = (const uint8_t *)data;
    if (p[0] == 'O' && p[1] == 'F' && p[2] == 'T' && p[3] == 'E') {
        oftp_state.error = true;
        oftp_state.done  = true;
        return;
    }
    if (!(p[0] == 'O' && p[1] == 'F' && p[2] == 'T' && p[3] == 'D')) return;
    if (len < 16) return;

    uint32_t total  = ((uint32_t)p[4]  << 24) | ((uint32_t)p[5]  << 16) |
                      ((uint32_t)p[6]  <<  8) |  (uint32_t)p[7];
    uint32_t offset = ((uint32_t)p[8]  << 24) | ((uint32_t)p[9]  << 16) |
                      ((uint32_t)p[10] <<  8) |  (uint32_t)p[11];
    uint32_t cklen  = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) |
                      ((uint32_t)p[14] <<  8) |  (uint32_t)p[15];

    if (total == 0 || total > oftp_state.buf_size) {
        oftp_state.error = true;
        oftp_state.done  = true;
        return;
    }
    oftp_state.total_size = total;
    oftp_state.last_chunk_tick = idt_get_ticks();

    /* EOF marker */
    if (cklen == 0 && offset == total) {
        oftp_state.done = true;
        return;
    }
    if (offset > total || offset + cklen > total) return;
    if (16u + cklen > len) return;  /* truncated packet */

    /* Lazily grow high_watermark for done-detection */
    uint32_t end = offset + cklen;
    if (end > oftp_state.high_watermark) oftp_state.high_watermark = end;

    uint8_t *dst = oftp_state.buf + offset;
    const uint8_t *s = p + 16;
    for (uint32_t i = 0; i < cklen; i++) dst[i] = s[i];

    if (oftp_state.high_watermark >= total) oftp_state.done = true;
}

static int parse_ip(const char *s, uint8_t ip[4]);  /* forward */
static void cmd_kexec(const char *arg);              /* forward */
extern void net_udp_listen(uint16_t port,
    void (*handler)(const uint8_t *, uint16_t, const void *, uint32_t));

static void cmd_kdownload(int argc, char *argv[])
{
    if (argc < 4) {
        sh_puts("Usage: kdownload <server-ip> <port> <filename> [save_as | --kexec]\n");
        sh_puts("  Fetches <filename> from oftp-server (tools/oftp-server.py).\n");
        sh_puts("  With save_as, writes to OsitoFS.  With --kexec, saves to\n");
        sh_puts("  /tmp.kexec.elf and reboots into it.\n");
        return;
    }

    uint8_t server_ip[4];
    if (parse_ip(argv[1], server_ip) < 0) {
        sh_puts("kdownload: bad IP\n");
        return;
    }
    /* Parse port */
    uint16_t server_port = 0;
    for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
        server_port = server_port * 10 + (uint16_t)(*p - '0');
    if (server_port == 0) {
        sh_puts("kdownload: bad port\n");
        return;
    }

    const char *filename = argv[3];
    const char *save_as  = (argc >= 5) ? argv[4] : NULL;
    bool do_kexec = save_as && save_as[0] == '-' && save_as[1] == '-' &&
                    save_as[2] == 'k';   /* "--kexec" */
    if (do_kexec) save_as = "tmp.kexec.elf";

    uint8_t *buf = (uint8_t *)kmalloc(OFTP_BUF_MAX);
    if (!buf) { sh_puts("kdownload: out of memory\n"); return; }

    /* Reset state */
    for (uint32_t i = 0; i < sizeof(oftp_state); i++)
        ((uint8_t *)&oftp_state)[i] = 0;
    oftp_state.buf       = buf;
    oftp_state.buf_size  = OFTP_BUF_MAX;
    oftp_state.active    = true;
    oftp_state.last_chunk_tick = idt_get_ticks();

    /* Listener registered once per kernel boot */
    static bool listener_registered = false;
    if (!listener_registered) {
        net_udp_listen(OFTP_LOCAL_PORT, oftp_handler);
        listener_registered = true;
    }

    /* Build & send REQ */
    uint8_t req[64];
    for (int i = 0; i < 64; i++) req[i] = 0;
    req[0] = 'O'; req[1] = 'F'; req[2] = 'T'; req[3] = 'Q';
    int fnlen = 0;
    while (filename[fnlen] && fnlen < 60) {
        req[4 + fnlen] = (uint8_t)filename[fnlen];
        fnlen++;
    }

    extern int net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                             uint16_t src_port, const void *data, uint32_t len);
    if (net_udp_send(server_ip, server_port, OFTP_LOCAL_PORT, req, 64) < 0) {
        sh_puts("kdownload: REQ send failed\n");
        oftp_state.active = false;
        kfree(buf);
        return;
    }

    sh_puts("kdownload: REQ sent to ");
    sh_puts(argv[1]); sh_puts(":"); sh_putdec(server_port);
    sh_puts(" for "); sh_puts(filename); sh_puts("\n");

    /* Drain loop with NAK-retransmit on stall.
     *
     * UDP loses tail packets when the server bursts faster than our drain.
     * Instead of failing outright, send a NAK("OFTN" + offset) asking the
     * server to resume from the current high_watermark. Try up to NAK_MAX
     * times before giving up. */
    extern void net_poll(void);
    uint64_t last_print  = idt_get_ticks();
    uint32_t last_high   = 0;
    uint64_t stall_since = idt_get_ticks();
    int nak_attempts = 0;
    const int NAK_MAX = 5;
    const uint64_t STALL_TICKS = 500;   /* NAK after 500 ms no progress */

    while (!oftp_state.done) {
        net_poll();
        uint64_t now = idt_get_ticks();

        /* Reset stall window whenever data flows */
        if (oftp_state.high_watermark != last_high) {
            last_high   = oftp_state.high_watermark;
            stall_since = now;
        }

        /* NAK retransmit on short stall — far faster than waiting the
         * full 5 s hard timeout, which leaves the user staring at a
         * frozen progress line for the tail-loss case (3 chunks lost
         * out of 887). */
        if (oftp_state.total_size > 0 &&
            oftp_state.high_watermark < oftp_state.total_size &&
            now - stall_since > STALL_TICKS) {
            if (nak_attempts >= NAK_MAX) {
                sh_puts("kdownload: NAK budget exhausted (");
                sh_putdec(oftp_state.high_watermark); sh_puts("/");
                sh_putdec(oftp_state.total_size); sh_puts(")\n");
                oftp_state.error = true;
                break;
            }
            nak_attempts++;
            sh_puts("  NAK resume@");
            sh_putdec(oftp_state.high_watermark);
            sh_puts(" (");
            sh_putdec(nak_attempts); sh_puts("/"); sh_putdec(NAK_MAX);
            sh_puts(")\n");
            uint8_t nak[8];
            nak[0]='O'; nak[1]='F'; nak[2]='T'; nak[3]='N';
            uint32_t off = oftp_state.high_watermark;
            nak[4] = (uint8_t)(off >> 24);
            nak[5] = (uint8_t)(off >> 16);
            nak[6] = (uint8_t)(off >>  8);
            nak[7] = (uint8_t)(off);
            net_udp_send(server_ip, server_port, OFTP_LOCAL_PORT, nak, 8);
            stall_since = now;
            oftp_state.last_chunk_tick = now;
        }

        /* Absolute timeout — no chunk in 5 s even after NAKs */
        if (now - oftp_state.last_chunk_tick > OFTP_TIMEOUT_TICKS) {
            sh_puts("kdownload: hard timeout\n");
            oftp_state.error = true;
            break;
        }

        if (now - last_print > 500) {
            last_print = now;
            sh_puts("  rx ");
            sh_putdec(oftp_state.high_watermark);
            sh_puts(" / ");
            sh_putdec(oftp_state.total_size);
            sh_puts(" B\n");
        }
    }

    bool ok = !oftp_state.error &&
              oftp_state.high_watermark >= oftp_state.total_size &&
              oftp_state.total_size > 0;
    uint32_t got   = oftp_state.high_watermark;
    uint32_t total = oftp_state.total_size;
    oftp_state.active = false;

    if (!ok) {
        sh_puts("kdownload: FAILED (");
        sh_putdec(got); sh_puts("/"); sh_putdec(total); sh_puts(")\n");
        kfree(buf);
        return;
    }
    sh_puts("kdownload: OK ");
    sh_putdec(got); sh_puts(" B\n");

    /* Save to OsitoFS */
    if (save_as) {
        if (!osfs2_is_mounted()) {
            sh_puts("kdownload: no FS mounted, cannot save\n");
            kfree(buf);
            return;
        }
        extern void *osfs2_create(const char *name, uint64_t size);
        extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
        extern int   osfs2_delete(const char *name);
        /* If exists and size differs, recreate */
        void *f = osfs2_find(save_as);
        if (f) {
            uint64_t cur = osfs2_file_size(f);
            if (cur != got) {
                osfs2_delete(save_as);
                f = NULL;
            }
        }
        if (!f) f = osfs2_create(save_as, got);
        if (!f) {
            sh_puts("kdownload: cannot create file\n");
            kfree(buf);
            return;
        }
        osfs2_write(f, 0, buf, got);
        extern int disk_flush(void);
        disk_flush();
        sh_puts("kdownload: saved -> ");
        sh_puts(save_as);
        sh_puts(" (synced)\n");
    }

    kfree(buf);

    if (do_kexec) {
        sh_puts("kdownload: chaining to kexec...\n");
        cmd_kexec(save_as);
    }
}

/* ── OFTP push (kupload) ──────────────────────────────────────────
 *
 * Inverse of kdownload: stream an OsitoFS file (or the live klog ring
 * buffer) to an oftp-server.  Same wire format as kdownload, but
 * directions reversed:
 *   OFTU req      OsitoK → server (filename + total_size)
 *   OFTA ack      server → OsitoK (ready to receive)
 *   OFTD chunks   OsitoK → server  (data stream)
 *   OFTD EOF      OsitoK → server  (cklen=0, offset==total)
 *
 * This bypasses the reply-path TX bug because every TX from the
 * kupload command runs in shell (process) context — the proven-good
 * direction.  The server's ACK and any other RX comes back via
 * Mac→OsitoK RX which works.                                          */

#define KUPLOAD_CHUNK_SZ    1400
#define KUPLOAD_PACE_TICKS  1     /* ~1 ms between chunks (pacing)     */

static void cmd_kupload(int argc, char *argv[])
{
    if (argc < 4) {
        sh_puts("Usage: kupload <server-ip> <port> <local-name> [remote-name]\n");
        sh_puts("       kupload <server-ip> <port> --dmesg [remote-name]\n");
        sh_puts("  Pushes a file from OsitoFS to oftp-server's uploads/ dir.\n");
        sh_puts("  --dmesg streams the live kernel log buffer (works even when\n");
        sh_puts("  the FS write path is broken — bypasses blkdev entirely).\n");
        return;
    }

    uint8_t server_ip[4];
    if (parse_ip(argv[1], server_ip) < 0) {
        sh_puts("kupload: bad IP\n");
        return;
    }
    uint16_t server_port = 0;
    for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
        server_port = server_port * 10 + (uint16_t)(*p - '0');
    if (server_port == 0) {
        sh_puts("kupload: bad port\n");
        return;
    }

    const char *local_name = argv[3];
    bool from_klog = (local_name[0] == '-' && local_name[1] == '-' &&
                      local_name[2] == 'd');   /* "--dmesg" */
    const char *remote_name = (argc >= 5) ? argv[4]
                              : (from_klog ? "dmesg.log" : local_name);

    uint8_t  *src_buf  = NULL;
    uint32_t  src_size = 0;
    bool      free_buf = false;

    if (from_klog) {
        extern uint32_t klog_read(char *buf, uint32_t max_len);
        const uint32_t klog_max = 256 * 1024;
        src_buf = (uint8_t *)kmalloc(klog_max);
        if (!src_buf) { sh_puts("kupload: out of memory\n"); return; }
        src_size = klog_read((char *)src_buf, klog_max);
        free_buf = true;
        sh_puts("kupload: read ");
        sh_putdec(src_size);
        sh_puts(" B from klog\n");
        /* Sanity check: kupload --dmesg post-kexec uploaded 100% NUL.
         * Print first 32 bytes from what klog_read gave us.  If they
         * are zero too → klog_read bug (or src_buf is page-zero COW).
         * If they are real ASCII → bug is in our send loop. */
        sh_puts("kupload: src[0..31]=\"");
        for (uint32_t i = 0; i < 32 && i < src_size; i++) {
            char c = (char)src_buf[i];
            if (c == 0)        sh_puts("\\0");
            else if (c == '\n') sh_puts("\\n");
            else if (c < 32 || c > 126) sh_puts(".");
            else { char s[2] = {c, 0}; sh_puts(s); }
        }
        sh_puts("\"\n");
    } else {
        if (!osfs2_is_mounted()) {
            sh_puts("kupload: no FS mounted\n");
            return;
        }
        void *f = osfs2_find(local_name);
        if (!f) {
            sh_puts("kupload: file not found: ");
            sh_puts(local_name); sh_puts("\n");
            return;
        }
        uint64_t fsize = osfs2_file_size(f);
        if (fsize == 0 || fsize > 64u * 1024u * 1024u) {
            sh_puts("kupload: bad file size\n");
            return;
        }
        src_size = (uint32_t)fsize;
        src_buf  = (uint8_t *)kmalloc(src_size);
        if (!src_buf) { sh_puts("kupload: out of memory\n"); return; }
        extern int osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
        if (osfs2_read(f, 0, src_buf, src_size) < 0) {
            sh_puts("kupload: read failed\n");
            kfree(src_buf);
            return;
        }
        free_buf = true;
    }

    /* Note: we don't strictly need to wait for the OFTA ack — the
     * server starts accepting OFTD chunks the moment it sees the OFTU
     * request.  We just need a brief delay so the request lands first.
     * The OFTA handler is left unwired for simplicity.                  */

    /* Build PUT request: OFTU + total_size(BE32) + filename(60 B) */
    uint8_t req[68];
    for (int i = 0; i < 68; i++) req[i] = 0;
    req[0]='O'; req[1]='F'; req[2]='T'; req[3]='U';
    req[4] = (uint8_t)(src_size >> 24);
    req[5] = (uint8_t)(src_size >> 16);
    req[6] = (uint8_t)(src_size >>  8);
    req[7] = (uint8_t)(src_size);
    int n = 0;
    while (remote_name[n] && n < 60) { req[8 + n] = remote_name[n]; n++; }

    extern int net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                             uint16_t src_port, const void *data, uint32_t len);
    extern void net_arp_probe(const uint8_t target_ip[4]);
    extern int  net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac_out[6]);
    extern void net_poll(void);
    extern uint64_t idt_get_ticks(void);

    /* Resolve Mac's ARP first.  Cold-cache case: net_udp_send fails
     * fast because ARP isn't there yet; we'd lose the PUT.  Probe +
     * wait up to 1 s for the reply. */
    {
        uint8_t mac_dummy[6];
        if (net_arp_lookup_nowait(server_ip, mac_dummy) != 0) {
            net_arp_probe(server_ip);
            uint64_t deadline = idt_get_ticks() + 1000;
            while (idt_get_ticks() < deadline) {
                net_poll();
                if (net_arp_lookup_nowait(server_ip, mac_dummy) == 0) break;
            }
            if (net_arp_lookup_nowait(server_ip, mac_dummy) != 0) {
                sh_puts("kupload: ARP resolution failed for ");
                sh_puts(argv[1]); sh_puts("\n");
                if (free_buf) kfree(src_buf);
                return;
            }
        }
    }

    if (net_udp_send(server_ip, server_port, OFTP_LOCAL_PORT, req, 68) < 0) {
        sh_puts("kupload: PUT send failed\n");
        if (free_buf) kfree(src_buf);
        return;
    }

    sh_puts("kupload: PUT '"); sh_puts(remote_name);
    sh_puts("' "); sh_putdec(src_size); sh_puts(" B → ");
    sh_puts(argv[1]); sh_puts(":"); sh_putdec(server_port); sh_puts("\n");

    /* Wait briefly for ACK then start streaming.  Server is fast so
     * 100 ms wall is plenty; if no ACK we just send anyway and the
     * server will bin our data.  In practice the ACK arrives in <1ms. */
    extern void net_poll(void);
    extern uint64_t idt_get_ticks(void);
    uint64_t t0 = idt_get_ticks();
    while (idt_get_ticks() - t0 < 100) net_poll();

    /* Stream data.
     *
     * pkt buffer comes from kmalloc, NOT the stack — net_udp_send takes
     * a scatter-gather TX path when len >= 256, and that path computes
     * VIRT_TO_PHYS(data).  Stack pointers don't have a clean phys
     * mapping (the kernel stack is in a separate VA region), so the
     * chip DMAs garbage zeros from an unmapped phys.  Observed: 35473
     * bytes of pure NUL at the server even though src_buf has real
     * content (`src[0..31]=".\n[SMP] ..."` confirmed).               */
    uint8_t *pkt = (uint8_t *)kmalloc(16 + KUPLOAD_CHUNK_SZ);
    if (!pkt) {
        sh_puts("kupload: oom for pkt buffer\n");
        if (free_buf) kfree(src_buf);
        return;
    }
    uint32_t offset = 0;
    while (offset < src_size) {
        uint32_t cklen = (src_size - offset > KUPLOAD_CHUNK_SZ)
                        ? KUPLOAD_CHUNK_SZ : (src_size - offset);
        pkt[0]='O'; pkt[1]='F'; pkt[2]='T'; pkt[3]='D';
        pkt[4]=(uint8_t)(src_size >> 24); pkt[5]=(uint8_t)(src_size >> 16);
        pkt[6]=(uint8_t)(src_size >>  8); pkt[7]=(uint8_t)(src_size);
        pkt[8]=(uint8_t)(offset >> 24);   pkt[9]=(uint8_t)(offset >> 16);
        pkt[10]=(uint8_t)(offset >> 8);   pkt[11]=(uint8_t)(offset);
        pkt[12]=(uint8_t)(cklen >> 24);   pkt[13]=(uint8_t)(cklen >> 16);
        pkt[14]=(uint8_t)(cklen >> 8);    pkt[15]=(uint8_t)(cklen);
        for (uint32_t i = 0; i < cklen; i++) pkt[16 + i] = src_buf[offset + i];
        if (net_udp_send(server_ip, server_port, OFTP_LOCAL_PORT,
                          pkt, 16 + cklen) < 0) {
            sh_puts("kupload: chunk send failed @ off=");
            sh_putdec(offset); sh_puts("\n");
            break;
        }
        offset += cklen;
        /* pacing: ~1 ms per chunk */
        uint64_t s = idt_get_ticks();
        while (idt_get_ticks() - s < KUPLOAD_PACE_TICKS) { /* spin */ }
        if ((offset & 0x1FFFF) == 0) {  /* every 128 KB */
            sh_puts("  tx "); sh_putdec(offset); sh_puts("/");
            sh_putdec(src_size); sh_puts(" B\n");
        }
    }

    /* EOF marker */
    uint8_t eof[16];
    eof[0]='O'; eof[1]='F'; eof[2]='T'; eof[3]='D';
    eof[4]=(uint8_t)(src_size >> 24);  eof[5]=(uint8_t)(src_size >> 16);
    eof[6]=(uint8_t)(src_size >>  8);  eof[7]=(uint8_t)(src_size);
    eof[8]=(uint8_t)(src_size >> 24);  eof[9]=(uint8_t)(src_size >> 16);
    eof[10]=(uint8_t)(src_size >> 8);  eof[11]=(uint8_t)(src_size);
    eof[12]=0; eof[13]=0; eof[14]=0;   eof[15]=0;
    net_udp_send(server_ip, server_port, OFTP_LOCAL_PORT, eof, 16);

    sh_puts("kupload: done ");
    sh_putdec(offset);
    sh_puts(" B sent\n");

    kfree(pkt);
    if (free_buf) kfree(src_buf);
}

/* Web-based update path — placeholder for the eventual WAN flow that
 * uses naranjositos.tech.  Requires DNS + TLS + HTTP client (already
 * available in net.c/tls.c/http.c) but isn't wired up yet because the
 * kernel currently only has a direct cable to a Mac, no Internet.    */
static void cmd_kupdate(int argc, char *argv[])
{
    (void)argc; (void)argv;
    sh_puts("kupdate: TODO — fetch from https://naranjositos.tech/k/<arch>/<channel>/kernel.elf\n");
    sh_puts("  Will use: dns_resolve + tls13_connect + http_get + osfs2_write + cmd_kexec.\n");
    sh_puts("  Stubbed until the kernel has Internet (current setup is direct LAN cable).\n");
    sh_puts("  Use `kdownload <local-ip> <port> kernel.elf --kexec` for now.\n");
}

/* ── Builtin: ping ──────────────────────────────────────────── */

static int parse_ip(const char *s, uint8_t ip[4])
{
    int i = 0, val = 0;
    for (; *s && i < 4; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
        } else if (*s == '.') {
            if (val > 255) return -1;
            ip[i++] = (uint8_t)val;
            val = 0;
        } else return -1;
    }
    if (i != 3 || val > 255) return -1;
    ip[3] = (uint8_t)val;
    return 0;
}

static void cmd_ping(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: ping <ip>\n");
        return;
    }

    uint8_t dst[4];
    if (parse_ip(argv[1], dst) < 0) {
        sh_puts("Invalid IP: ");
        sh_puts(argv[1]);
        sh_puts("\n");
        return;
    }

    sh_puts("PING ");
    sh_putdec(dst[0]); sh_puts(".");
    sh_putdec(dst[1]); sh_puts(".");
    sh_putdec(dst[2]); sh_puts(".");
    sh_putdec(dst[3]); sh_puts("\n");

    uint32_t sent = 0, recv_before = net_icmp_get_rx_count();

    for (int i = 0; i < 4; i++) {
        net_icmp_send_echo(dst, (uint16_t)(i + 1));
        sent++;

        /* Poll for ~500ms (50 ticks at 100Hz) */
        uint64_t start = idt_get_ticks();
        uint32_t old_count = net_icmp_get_rx_count();
        while (idt_get_ticks() - start < 50) {
            net_poll();
            if (net_icmp_get_rx_count() > old_count) {
                sh_puts("  reply from ");
                sh_putdec(dst[0]); sh_puts(".");
                sh_putdec(dst[1]); sh_puts(".");
                sh_putdec(dst[2]); sh_puts(".");
                sh_putdec(dst[3]);
                sh_puts(" seq=");
                sh_putdec(i + 1);
                sh_puts("\n");
                break;
            }
            sh_hlt();
        }
        if (net_icmp_get_rx_count() == old_count) {
            sh_puts("  timeout seq=");
            sh_putdec(i + 1);
            sh_puts("\n");
        }
    }

    uint32_t recv_total = net_icmp_get_rx_count() - recv_before;
    sh_puts("--- ");
    sh_putdec(sent);
    sh_puts(" sent, ");
    sh_putdec(recv_total);
    sh_puts(" received ---\n");
}

/* ── Builtin: tcptest ─────────────────────────────────────────── */

static void cmd_tcptest(int argc, char *argv[])
{
    /* Default: connect to QEMU SLIRP host gateway HTTP (10.0.2.2:80) */
    uint8_t dst[4] = {10, 0, 2, 2};
    uint16_t port = 80;

    if (argc >= 2) {
        if (parse_ip(argv[1], dst) < 0) {
            sh_puts("Invalid IP: ");
            sh_puts(argv[1]);
            sh_puts("\n");
            return;
        }
    }
    if (argc >= 3) {
        port = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            port = port * 10 + (*p - '0');
    }

    sh_puts("TCP connect ");
    sh_putdec(dst[0]); sh_puts(".");
    sh_putdec(dst[1]); sh_puts(".");
    sh_putdec(dst[2]); sh_puts(".");
    sh_putdec(dst[3]); sh_puts(":");
    sh_putdec(port); sh_puts("...\n");

    int conn = net_tcp_connect(dst, port, 49152);
    if (conn < 0) {
        sh_puts("  Connection failed!\n");
        return;
    }

    sh_puts("  Connected! Sending HTTP GET...\n");

    const char *req = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    uint32_t req_len = 0;
    for (const char *p = req; *p; p++) req_len++;

    int sent = net_tcp_send(conn, req, req_len);
    sh_puts("  Sent ");
    sh_putdec(sent);
    sh_puts(" bytes\n");

    /* Read response */
    char buf[512];
    int total = 0;
    sh_puts("  Response:\n");

    for (;;) {
        int n = net_tcp_recv_timeout(conn, buf, sizeof(buf) - 1, 300);
        if (n <= 0) break;
        buf[n] = '\0';
        /* Print first 400 chars of response */
        if (total < 400) {
            int show = n;
            if (total + show > 400)
                show = 400 - total;
            for (int i = 0; i < show; i++) {
                char c = buf[i];
                if (c == '\r') continue;
                char s[2] = {c, 0};
                sh_puts(s);
            }
        }
        total += n;
    }

    sh_puts("\n  Total received: ");
    sh_putdec(total);
    sh_puts(" bytes\n");

    net_tcp_close(conn);
    sh_puts("  Connection closed.\n");
}

/* ── Builtin: resolve ─────────────────────────────────────────── */

static void cmd_resolve(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: resolve <hostname>\n");
        return;
    }

    uint8_t ip[4];
    if (net_dns_resolve(argv[1], ip) == 0) {
        sh_puts(argv[1]);
        sh_puts(" -> ");
        sh_putdec(ip[0]); sh_puts(".");
        sh_putdec(ip[1]); sh_puts(".");
        sh_putdec(ip[2]); sh_puts(".");
        sh_putdec(ip[3]); sh_puts("\n");
    } else {
        sh_puts("DNS resolution failed for ");
        sh_puts(argv[1]);
        sh_puts("\n");
    }
}

/* ── Builtin: tlstest ─────────────────────────────────────────── */

static void cmd_tlstest(int argc, char *argv[])
{
    const char *hostname = "example.com";
    if (argc >= 2) hostname = argv[1];

    /* Resolve hostname */
    uint8_t ip[4];
    sh_puts("Resolving ");
    sh_puts(hostname);
    sh_puts("...\n");
    if (net_dns_resolve(hostname, ip) < 0) {
        sh_puts("DNS resolution failed\n");
        return;
    }
    sh_puts("  -> ");
    sh_putdec(ip[0]); sh_puts(".");
    sh_putdec(ip[1]); sh_puts(".");
    sh_putdec(ip[2]); sh_puts(".");
    sh_putdec(ip[3]); sh_puts("\n");

    /* TCP connect to port 443 */
    int tcp = net_tcp_connect(ip, 443, 49200);
    if (tcp < 0) {
        sh_puts("TCP connect failed\n");
        return;
    }
    sh_puts("TCP connected, starting TLS...\n");

    /* Allocate TLS state */
    void *tls = kmalloc(tls_conn_size());
    if (!tls) {
        sh_puts("Out of memory for TLS\n");
        net_tcp_close(tcp);
        return;
    }

    if (tls_connect(tls, tcp, hostname) < 0) {
        sh_puts("TLS handshake failed\n");
        kfree(tls);
        net_tcp_close(tcp);
        return;
    }

    sh_puts_color("TLS established!\n", 0x0000FF00);

    /* Send HTTP GET */
    char req[256];
    int rlen = 0;
    const char *hdr1 = "GET / HTTP/1.1\r\nHost: ";
    while (hdr1[rlen]) { req[rlen] = hdr1[rlen]; rlen++; }
    const char *h = hostname;
    while (*h) req[rlen++] = *h++;
    const char *hdr2 = "\r\nConnection: close\r\n\r\n";
    for (int i = 0; hdr2[i]; i++) req[rlen++] = hdr2[i];

    tls_send(tls, req, (uint32_t)rlen);
    sh_puts("HTTP GET sent, waiting for response...\n");

    /* Read response */
    char buf[512];
    int total = 0;
    for (;;) {
        int n = tls_recv(tls, buf, sizeof(buf) - 1, 500);
        if (n <= 0) break;
        buf[n] = '\0';
        if (total < 400) {
            int show = n;
            if (total + show > 400) show = 400 - total;
            for (int i = 0; i < show; i++) {
                char c = buf[i];
                if (c == '\r') continue;
                char s[2] = {c, 0};
                sh_puts(s);
            }
        }
        total += n;
    }

    sh_puts("\n  Total: ");
    sh_putdec(total);
    sh_puts(" bytes\n");

    tls_close(tls);
    kfree(tls);
    net_tcp_close(tcp);
    sh_puts("Connection closed.\n");
}

/* ── Builtin: curl ───────────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
extern int wasm_http_request(const char *url, const char *method,
                              const char *headers_json, const char *body,
                              uint8_t **out_buf, int *out_len);
extern void free(void *);

extern int  wasm_ws_open(const char *url);
extern int  wasm_ws_state(int handle);
extern int  wasm_ws_wait_open(int handle, int timeout_ms);
extern int  wasm_ws_send(int handle, const void *data, int len);
extern int  wasm_ws_recv_wait(int handle, void *dst, int max, int timeout_ms);
extern void wasm_ws_close(int handle);

extern int  wasm_tcp_connect_host(const char *host, uint16_t port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size, uint32_t timeout_ticks);
extern void net_tcp_close(int conn);

/* tls.h provides tls_conn_t but conflicts with the void* externs at
 * line 99-102. We don't need the full struct shape — just allocate
 * a generous fixed buffer that's larger than any plausible
 * tls_conn_t (~17 KB observed). */
#define WASM_TLS_CTX_BYTES (32 * 1024)
extern void *malloc(unsigned long);

/* Per-shell WS handle table — small fixed slots indexed by integer.
 * Users can name a connection ('ws open <url> myws') for nicer UX. */
#define WS_SLOT_MAX 8
typedef struct { int handle; char name[16]; } ws_slot_t;
static ws_slot_t ws_slots[WS_SLOT_MAX];

static ws_slot_t *ws_slot_find(const char *name)
{
    for (int i = 0; i < WS_SLOT_MAX; i++)
        if (ws_slots[i].handle && strcmp(ws_slots[i].name, name) == 0)
            return &ws_slots[i];
    return NULL;
}

static ws_slot_t *ws_slot_alloc(void)
{
    for (int i = 0; i < WS_SLOT_MAX; i++)
        if (!ws_slots[i].handle) return &ws_slots[i];
    return NULL;
}
#endif

static void cmd_curl(int argc, char *argv[])
{
#ifdef __EMSCRIPTEN__
    if (argc < 2) {
        sh_puts("Usage: curl <url> [host /path]\n");
        sh_puts("  In WASM: any URL the browser can reach (CORS permitting).\n");
        sh_puts("  Example: curl https://example.com\n");
        return;
    }
    /* Two-arg form: hostname + path → assume https://host/path */
    char url[1024];
    if (argc >= 3 && argv[1][0] != 'h') {
        int p = 0;
        const char *prefix = "https://";
        while (*prefix && p < (int)sizeof(url) - 1) url[p++] = *prefix++;
        const char *h = argv[1];
        while (*h && p < (int)sizeof(url) - 1) url[p++] = *h++;
        const char *path = argv[2];
        while (*path && p < (int)sizeof(url) - 1) url[p++] = *path++;
        url[p] = '\0';
    } else {
        int p = 0;
        const char *u = argv[1];
        while (*u && p < (int)sizeof(url) - 1) url[p++] = *u++;
        url[p] = '\0';
    }

    uint8_t *buf = NULL;
    int len = 0;
    int status = wasm_http_request(url, "GET", "{}", NULL, &buf, &len);
    if (status < 0) {
        sh_puts_color("[curl] transport error\n", 0x00FF0000);
        return;
    }
    sh_puts_color("HTTP ", 0x00FFD93D);
    sh_putdec((uint64_t)status);
    sh_puts(" — ");
    sh_putdec((uint64_t)len);
    sh_puts(" bytes\n");

    /* Stream body in 256-byte chunks; cap visible output at 16 KB. */
    int cap = len > 16384 ? 16384 : len;
    char chunk[256];
    int i = 0;
    while (i < cap) {
        int n = cap - i; if (n > 255) n = 255;
        for (int j = 0; j < n; j++) chunk[j] = (char)buf[i + j];
        chunk[n] = '\0';
        sh_puts(chunk);
        i += n;
    }
    if (len > cap) {
        sh_puts("\n... [");
        sh_putdec((uint64_t)(len - cap));
        sh_puts(" more bytes truncated]\n");
    } else {
        sh_puts("\n");
    }
    free(buf);
    return;
#endif
    if (argc < 2) {
        sh_puts("Usage: curl <hostname> [path]\n");
        sh_puts("  Example: curl example.com /\n");
        sh_puts("  Example: curl api.anthropic.com /v1/models\n");
        return;
    }

    const char *hostname = argv[1];
    const char *path = "/";
    if (argc >= 3) path = argv[2];

    void *session = kmalloc(http_session_size());
    if (!session) { sh_puts("Out of memory\n"); return; }

    void *resp = kmalloc(http_response_size());
    if (!resp) { kfree(session); sh_puts("Out of memory\n"); return; }

    /* Open HTTPS session */
    if (http_open(session, hostname) < 0) {
        kfree(resp);
        kfree(session);
        return;
    }

    /* Send GET request */
    const char *headers[] = {
        "Connection: close",
        "User-Agent: OsitoK/1.0",
        NULL
    };

    if (http_request(session, "GET", path, hostname, headers,
                     NULL, 0, resp) < 0) {
        http_close(session);
        kfree(resp);
        kfree(session);
        return;
    }

    /* Read body */
    char *body = (char *)kmalloc(8192);
    if (body) {
        int n = http_read_body_full(session, resp, body, 8191);
        if (n > 0) {
            body[n] = '\0';
            /* Print first 1000 chars */
            int show = n > 1000 ? 1000 : n;
            for (int i = 0; i < show; i++) {
                char c = body[i];
                if (c == '\r') continue;
                if (c < 0x20 && c != '\n' && c != '\t') c = '.';
                char s[2] = {c, 0};
                sh_puts(s);
            }
            if (n > 1000) {
                sh_puts("\n... (");
                sh_putdec((uint64_t)(n - 1000));
                sh_puts(" more bytes)\n");
            }
            sh_puts("\n");
        }
        kfree(body);
    }

    http_close(session);
    kfree(resp);
    kfree(session);
}

/* ── Builtin: apikey ──────────────────────────────────────────── */

static void cmd_apikey(int argc, char *argv[])
{
    if (argc < 2) {
        const char *key = claude_get_api_key();
        if (key) {
            sh_puts("API key set (");
            /* Show first 10 + last 4 chars */
            int len = 0;
            const char *p = key;
            while (*p) { len++; p++; }
            for (int i = 0; i < 10 && i < len; i++) {
                char s[2] = {key[i], 0};
                sh_puts(s);
            }
            sh_puts("...");
            if (len > 14) {
                for (int i = len - 4; i < len; i++) {
                    char s[2] = {key[i], 0};
                    sh_puts(s);
                }
            }
            sh_puts(")\n");
        } else {
            sh_puts("No API key set. Usage: apikey sk-ant-...\n");
        }
        return;
    }
    claude_set_api_key(argv[1]);
    sh_puts_color("API key set.\n", 0x0000FF00);
}

/* ── Builtin: ask ────────────────────────────────────────────── */

/* Streaming callback: print each text chunk to console */
static int ask_stream_cb(const char *text, uint32_t len, void *ctx)
{
    (void)ctx;
    for (uint32_t i = 0; i < len; i++) {
        char s[2] = {text[i], 0};
        sh_puts(s);
    }
    return 0;
}

static void cmd_ask(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: ask <prompt>\n");
        sh_puts("  Example: ask What is OsitoK?\n");
        return;
    }

    if (!claude_get_api_key()) {
        sh_puts("Set API key first: apikey sk-ant-...\n");
        return;
    }

    /* Reconstruct prompt from argv (join with spaces) */
    char prompt[1024];
    int pp = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && pp < (int)sizeof(prompt) - 1) prompt[pp++] = ' ';
        const char *w = argv[i];
        while (*w && pp < (int)sizeof(prompt) - 1) prompt[pp++] = *w++;
    }
    prompt[pp] = '\0';

    sh_puts_color("\nClaude: ", 0x00FF8800);

    /* Build message struct — must match claude_msg_t layout:
     * { const char *role; const char *content; } */
    struct { const char *role; const char *content; } msg;
    msg.role = "user";
    msg.content = prompt;

    int r = claude_chat(&msg, 1, NULL, 1024, ask_stream_cb, NULL);
    if (r < 0) {
        sh_puts_color("\n[error]\n", 0x00FF0000);
    } else {
        sh_puts("\n");
    }
}

/* ── Builtin: claude (multi-turn REPL, X-CL2) ────────────────── */

static void cmd_claude(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!claude_get_api_key()) {
        sh_puts("Set API key first: apikey sk-ant-...\n");
        return;
    }

    void *session = claude_session_new();
    if (!session) {
        sh_puts("Failed to allocate session\n");
        return;
    }

    sh_puts_color("Claude REPL", 0x00FF8800);
    sh_puts(" (multi-turn). Type 'quit' or Ctrl+D to exit, 'clear' to reset.\n\n");

    char line[1024];

    for (;;) {
        int len = term_readline("you> ", line, sizeof(line));

        /* Ctrl+D (EOF) or empty + Ctrl+D */
        if (len < 0) {
            sh_puts("\n");
            break;
        }

        /* Skip empty lines */
        if (len == 0) continue;

        /* Builtins within REPL */
        if (line[0] == 'q' && line[1] == 'u' && line[2] == 'i' &&
            line[3] == 't' && (line[4] == '\0' || line[4] == ' '))
            break;

        if (line[0] == 'c' && line[1] == 'l' && line[2] == 'e' &&
            line[3] == 'a' && line[4] == 'r' && line[5] == '\0') {
            claude_session_clear(session);
            sh_puts_color("Session cleared.\n\n", 0x00888888);
            continue;
        }

        sh_puts_color("\nClaude: ", 0x00FF8800);

        int r = claude_session_send_with_tools(session, line, ask_stream_cb, NULL);
        if (r < 0) {
            sh_puts_color("\n[error]\n", 0x00FF0000);
        } else {
            sh_puts("\n\n");
        }
    }

    claude_session_free(session);
    sh_puts("Session ended.\n");
}

/* ── Builtin: temp (sampling parameters) ─────────────────────── */

/* Parse simple decimal float: "0.7", "1.0", "0" */
static float parse_float(const char *s)
{
    float result = 0.0f;
    float frac = 0.0f;
    float div = 1.0f;
    bool after_dot = false;

    for (; *s; s++) {
        if (*s == '.') { after_dot = true; continue; }
        if (*s < '0' || *s > '9') break;
        if (after_dot) {
            div *= 10.0f;
            frac += (*s - '0') / div;
        } else {
            result = result * 10.0f + (*s - '0');
        }
    }
    return result + frac;
}

static void cmd_temp(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: temp <temperature> [top_p]\n");
        sh_puts("  temperature: 0=greedy, 0.6=default, 1.0=creative\n");
        sh_puts("  top_p:       0.9=default, 1.0=all tokens\n");
        return;
    }

    float t = parse_float(argv[1]);
    float p = 0.9f;
    if (argc >= 3) p = parse_float(argv[2]);

    llama_set_sampling(t, p);

#ifdef __EMSCRIPTEN__
    {
        char buf[16];
        fmt_float(t, buf, sizeof(buf)); wasm_config_save("temp", buf);
        fmt_float(p, buf, sizeof(buf)); wasm_config_save("topp", buf);
    }
#endif

    sh_puts("Sampling: temp=");
    sh_putdec((uint64_t)(t * 10.0f) / 10);
    sh_puts(".");
    sh_putdec((uint64_t)(t * 10.0f) % 10);
    sh_puts(", top_p=");
    sh_putdec((uint64_t)(p * 10.0f) / 10);
    sh_puts(".");
    sh_putdec((uint64_t)(p * 10.0f) % 10);
    sh_puts("\n");
}

static void cmd_penalty(int argc, char *argv[])
{
    if (argc < 2) {
        float r, pr, fq;
        llama_get_penalty(&r, &pr, &fq);
        sh_puts("Usage: penalty <rep> [presence] [frequency]\n");
        sh_puts("  rep:       1.0=off, 1.10=balanced, 1.30=strong (multiplicative)\n");
        sh_puts("  presence:  0.0=off, 0.5=mild, 1.0=strong (additive once-per-token)\n");
        sh_puts("  frequency: 0.0=off, 0.05=balanced, 0.20=strong (additive per-occurrence)\n");
        sh_puts("Current: rep=");
        sh_putdec((uint64_t)(r * 100.0f) / 100); sh_puts(".");
        sh_putdec((uint64_t)(r * 100.0f) % 100);
        sh_puts(" presence=");
        sh_putdec((uint64_t)(pr * 100.0f) / 100); sh_puts(".");
        sh_putdec((uint64_t)(pr * 100.0f) % 100);
        sh_puts(" frequency=");
        sh_putdec((uint64_t)(fq * 100.0f) / 100); sh_puts(".");
        sh_putdec((uint64_t)(fq * 100.0f) % 100);
        sh_puts("\n");
        return;
    }
    float rep = parse_float(argv[1]);
    float pre = argc >= 3 ? parse_float(argv[2]) : 0.0f;
    float frq = argc >= 4 ? parse_float(argv[3]) : 0.0f;
    llama_set_penalty(rep, pre, frq);
#ifdef __EMSCRIPTEN__
    {
        char buf[16];
        fmt_float(rep, buf, sizeof(buf)); wasm_config_save("rep",  buf);
        fmt_float(pre, buf, sizeof(buf)); wasm_config_save("pres", buf);
        fmt_float(frq, buf, sizeof(buf)); wasm_config_save("freq", buf);
    }
#endif
    sh_puts("Penalty set\n");
}

/* ── Builtin: chat (local inference) ─────────────────────────── */

static void chat_token_cb(const char *text, void *ctx)
{
    (void)ctx;
    sh_puts(text);
}

static void cmd_chat(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: chat <prompt>\n");
        sh_puts("  Local Llama inference with tokenizer\n");
        return;
    }

    if (!prompt_llama) {
        sh_puts("No model loaded.\n");
        return;
    }

    /* Reconstruct prompt from argv */
    char prompt[1024];
    int pp = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && pp < (int)sizeof(prompt) - 1) prompt[pp++] = ' ';
        const char *w = argv[i];
        while (*w && pp < (int)sizeof(prompt) - 1) prompt[pp++] = *w++;
    }
    prompt[pp] = '\0';

    sh_puts_color("\nLlama: ", 0x00FF8800);

    /* Detect Llama 3 by vocab size + arch and wrap with its chat template.
     * Llama 3 vocab is 128256; brandon-tiny and TinyStories are smaller. */
    extern int llama_state_vocab(void *s);
    char wrapped[2048];
    const char *send;
    if (llama_state_vocab(prompt_llama) >= 128000) {
        /* Llama 3 chat template — instruct models behave best with it. */
        int n = 0;
        const char *prefix =
            "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n";
        const char *suffix =
            "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";
        for (const char *p = prefix; *p && n < (int)sizeof(wrapped)-1; p++) wrapped[n++] = *p;
        for (const char *p = prompt;  *p && n < (int)sizeof(wrapped)-1; p++) wrapped[n++] = *p;
        for (const char *p = suffix;  *p && n < (int)sizeof(wrapped)-1; p++) wrapped[n++] = *p;
        wrapped[n] = 0;
        send = wrapped;
    } else {
        send = prompt;
    }
    int r = llama_chat(prompt_llama, send, 128, chat_token_cb, NULL);
    if (r < 0) {
        sh_puts_color("[error]\n", 0x00FF0000);
    } else {
        sh_puts("\n");
    }
}

extern int llama_chat_with_system(void *state,
                                   const char *system_text,
                                   const char *user_text,
                                   uint32_t max_tokens,
                                   void (*on_token)(const char *, void *),
                                   void *ctx);

static void cmd_rag(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: rag <context> ::: <question>\n");
        sh_puts("  Context and question separated by triple-colon (::: avoids the | pipe).\n");
        sh_puts("  Example: rag Einstein was born in Ulm in 1879. ::: Where was Einstein born?\n");
        return;
    }
    if (!prompt_llama) {
        sh_puts("No model loaded.\n");
        return;
    }

    /* Reassemble argv into a single string, then split on '|'. */
    static char buf[4096];
    int bp = 0;
    for (int i = 1; i < argc && bp < (int)sizeof(buf) - 1; i++) {
        if (i > 1 && bp < (int)sizeof(buf) - 1) buf[bp++] = ' ';
        const char *w = argv[i];
        while (*w && bp < (int)sizeof(buf) - 1) buf[bp++] = *w++;
    }
    buf[bp] = '\0';

    char *bar = buf;
    while (*bar) {
        if (bar[0] == ':' && bar[1] == ':' && bar[2] == ':') break;
        bar++;
    }
    if (!*bar) {
        sh_puts("No ':::' separator found. See: rag (no args)\n");
        return;
    }
    *bar = '\0';
    /* Strip trailing space on context */
    char *ctx_end = bar - 1;
    while (ctx_end > buf && *ctx_end == ' ') *ctx_end-- = '\0';
    char *question = bar + 3;
    while (*question == ' ') question++;

    /* Build the system message (matches rag-brandon.py PROMPT_RAG). */
    static char system_msg[4096];
    const char *prefix = "Answer the user's question using the context below. "
                         "If the context does not contain the answer, say you don't know.\n\n"
                         "Context:\n";
    int sp = 0;
    const char *p = prefix;
    while (*p && sp < (int)sizeof(system_msg) - 1) system_msg[sp++] = *p++;
    p = buf;
    while (*p && sp < (int)sizeof(system_msg) - 1) system_msg[sp++] = *p++;
    system_msg[sp] = '\0';

    sh_puts_color("\nLlama (RAG): ", 0x00FF8800);

    int r = llama_chat_with_system(prompt_llama, system_msg, question, 128,
                                    chat_token_cb, NULL);
    if (r < 0) sh_puts_color("[error]\n", 0x00FF0000);
    else       sh_puts("\n");
}

/* ── Builtin: kexec ──────────────────────────────────────────── */

/* Trampoline symbol + size (from kexec_tramp.S) */
extern void kexec_trampoline(void);
extern void kexec_trampoline_end(void);

/* Saved boot_info from main.c */
extern boot_info_t saved_boot_info;

/* ELF64 header (matching elf.c definitions) */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} kexec_elf64_hdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} kexec_elf64_phdr_t;

/* Segment copy descriptor for trampoline */
typedef struct {
    uint64_t src;
    uint64_t dst;
    uint64_t len;
} kexec_seg_t;

/* APIC registers for shutdown */
#define KEXEC_APIC_BASE    0xFEE00000
#define KEXEC_APIC_SVR     0xF0
#define KEXEC_APIC_ICR_LO  0x300
#define KEXEC_APIC_ICR_HI  0x310
#define KEXEC_APIC_LVT_TMR 0x320

static void cmd_kexec(const char *arg)
{
    const char *filename = (arg && *arg) ? arg : "kernel.elf";

    sh_puts("kexec: loading ");
    sh_puts(filename);
    sh_puts(" from OsitoFS...\n");

    /* Step 1: Read ELF from disk */
    void *file = osfs2_find(filename);
    if (!file) {
        sh_puts_color("  File not found\n", 0x00FF0000);
        return;
    }

    uint64_t file_size = osfs2_file_size(file);
    if (file_size < 64 || file_size > 8 * 1024 * 1024) {
        sh_puts_color("  Invalid file size\n", 0x00FF0000);
        return;
    }

    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) {
        sh_puts_color("  Out of memory\n", 0x00FF0000);
        return;
    }

    if (osfs2_read(file, 0, elf_data, file_size) < 0) {
        sh_puts_color("  Read failed\n", 0x00FF0000);
        kfree(elf_data);
        return;
    }

    /* Step 2: Validate ELF header — kexec is ring-0 with no return.
     * A bad jump can reboot the box (we've already burned a couple of
     * test cycles to that), so we're paranoid: every byte that the
     * ELF spec lets us check, we check.  Tradeoff: refuse to run a
     * potentially-good kernel rather than gamble on a partial download.
     */
    kexec_elf64_hdr_t *ehdr = (kexec_elf64_hdr_t *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        sh_puts_color("  validate: bad ELF magic\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    if (ehdr->e_ident[4] != 2) {  /* EI_CLASS: ELFCLASS64 */
        sh_puts_color("  validate: not ELF64\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    if (ehdr->e_ident[5] != 1) {  /* EI_DATA: little-endian */
        sh_puts_color("  validate: not little-endian\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    if (ehdr->e_type != 2) {      /* ET_EXEC */
        sh_puts_color("  validate: e_type != ET_EXEC (got 0x", 0x00FF0000);
        serial_puthex(ehdr->e_type, 4); sh_puts_color(")\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    if (ehdr->e_machine != 0x3E) { /* EM_X86_64 */
        sh_puts_color("  validate: e_machine != X86_64 (got 0x", 0x00FF0000);
        serial_puthex(ehdr->e_machine, 4); sh_puts_color(")\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    /* Entry point must be in the high-half kernel range (we only
     * support upper-half kernels — UEFI loads us at 0xFFFF8000_xxxxxxxx). */
    if ((ehdr->e_entry >> 32) != 0xFFFF8000ULL) {
        sh_puts_color("  validate: entry not in upper-half (0xFFFF8000_*); got 0x",
                       0x00FF0000);
        serial_puthex(ehdr->e_entry, 16); sh_puts_color("\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    /* Sanity: phnum/phentsize/phoff inside file */
    if (ehdr->e_phnum == 0 || ehdr->e_phnum > 16 ||
        ehdr->e_phentsize != 56 ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * 56 > file_size) {
        sh_puts_color("  validate: phdr table out of bounds\n", 0x00FF0000);
        kfree(elf_data); return;
    }
    /* Walk PT_LOAD segments; require at least one and check filesz/offset. */
    int n_load = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        kexec_elf64_phdr_t *ph = (kexec_elf64_phdr_t *)
            (elf_data + ehdr->e_phoff + i * 56);
        if (ph->p_type != 1) continue;
        n_load++;
        if (ph->p_offset + ph->p_filesz > file_size ||
            ph->p_filesz > ph->p_memsz) {
            sh_puts_color("  validate: PT_LOAD segment overflows file\n",
                           0x00FF0000);
            kfree(elf_data); return;
        }
        /* Each load vaddr must also be in upper-half */
        if ((ph->p_vaddr >> 32) != 0xFFFF8000ULL) {
            sh_puts_color("  validate: PT_LOAD vaddr not upper-half\n",
                           0x00FF0000);
            kfree(elf_data); return;
        }
    }
    if (n_load == 0) {
        sh_puts_color("  validate: no PT_LOAD segments\n", 0x00FF0000);
        kfree(elf_data); return;
    }

    sh_puts("  validate: OK (ELF64 x86_64 ET_EXEC, entry=0x");
    serial_puthex(ehdr->e_entry, 16);
    sh_puts(", ");
    sh_putdec(n_load);
    sh_puts(" PT_LOADs)\n");

    /* Step 3: Parse PT_LOAD segments, copy to temp area at 0x8000000 (128MB) */
    #define KEXEC_TEMP_BASE  0x8000000ULL
    #define KEXEC_MAX_SEGS   8

    kexec_seg_t segs[KEXEC_MAX_SEGS];
    int nseg = 0;
    uint64_t temp_off = 0;
    uint64_t kernel_lo = ~0ULL, kernel_hi = 0;

    for (int i = 0; i < ehdr->e_phnum && nseg < KEXEC_MAX_SEGS; i++) {
        uint64_t phoff = ehdr->e_phoff + i * ehdr->e_phentsize;
        if (phoff + sizeof(kexec_elf64_phdr_t) > file_size) break;

        kexec_elf64_phdr_t *ph = (kexec_elf64_phdr_t *)(elf_data + phoff);
        if (ph->p_type != 1 /* PT_LOAD */ || ph->p_memsz == 0) continue;

        /* Copy file data to temp area */
        uint64_t temp_addr = KEXEC_TEMP_BASE + temp_off;
        uint8_t *dst = (uint8_t *)temp_addr;

        /* Zero the full memsz range (covers BSS) */
        for (uint64_t j = 0; j < ph->p_memsz; j++)
            dst[j] = 0;

        /* Copy filesz from ELF data */
        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= file_size) {
            uint8_t *src = elf_data + ph->p_offset;
            for (uint64_t j = 0; j < ph->p_filesz; j++)
                dst[j] = src[j];
        }

        /* Record segment for trampoline */
        segs[nseg].src = temp_addr;
        segs[nseg].dst = ph->p_paddr;
        segs[nseg].len = ph->p_memsz;  /* copy full memsz (includes zeroed BSS) */
        nseg++;

        sh_puts("  LOAD: 0x");
        serial_puthex(ph->p_paddr, 8);
        sh_puts(" (");
        sh_putdec(ph->p_filesz);
        sh_puts("/");
        sh_putdec(ph->p_memsz);
        sh_puts(" bytes)\n");

        /* Track kernel extent */
        if (ph->p_paddr < kernel_lo) kernel_lo = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > kernel_hi)
            kernel_hi = ph->p_paddr + ph->p_memsz;

        temp_off += (ph->p_memsz + 4095) & ~4095ULL;  /* page-align */
    }

    if (nseg == 0) {
        sh_puts_color("  No LOAD segments found\n", 0x00FF0000);
        kfree(elf_data);
        return;
    }

    /* Step 4: Prepare boot_info_t + mmap copy at safe location */
    /* Must be above temp range: temp starts at 0x8000000 (128MB), BSS expands ~23MB
     * so temp ends around 0x9700000 (151MB). Place safe data at 256MB. */
    #define KEXEC_INFO_ADDR  0x10010000ULL  /* 256MB + 64KB */
    #define KEXEC_MMAP_ADDR  0x10000000ULL  /* 256MB (room for 64KB mmap) */

    boot_info_t *new_info = (boot_info_t *)KEXEC_INFO_ADDR;
    *new_info = saved_boot_info;
    new_info->kernel_phys_base = kernel_lo;
    new_info->kernel_size = kernel_hi - kernel_lo;

    /* Copy UEFI memory map to safe location (it lives in kernel BSS which gets overwritten) */
    {
        uint8_t *mmap_src = (uint8_t *)(uintptr_t)saved_boot_info.mmap_addr;
        uint8_t *mmap_dst = (uint8_t *)KEXEC_MMAP_ADDR;
        uint64_t mmap_sz = saved_boot_info.mmap_size;
        if (mmap_sz > 0x20000) mmap_sz = 0x20000;  /* cap at 128KB */
        for (uint64_t i = 0; i < mmap_sz; i++)
            mmap_dst[i] = mmap_src[i];
        new_info->mmap_addr = KEXEC_MMAP_ADDR;
        new_info->mmap_size = mmap_sz;
    }

    /* Step 5: Copy trampoline to safe location (above temp+safe data) */
    #define KEXEC_TRAMP_ADDR 0x10020000ULL  /* 256MB + 128KB */
    uint64_t tramp_size = (uint64_t)kexec_trampoline_end - (uint64_t)kexec_trampoline;
    uint8_t *tramp_dst = (uint8_t *)KEXEC_TRAMP_ADDR;
    uint8_t *tramp_src = (uint8_t *)(uint64_t)kexec_trampoline;
    for (uint64_t i = 0; i < tramp_size; i++)
        tramp_dst[i] = tramp_src[i];

    /* Copy segment table next to trampoline */
    kexec_seg_t *seg_copy = (kexec_seg_t *)(KEXEC_TRAMP_ADDR + 4096);
    for (int i = 0; i < nseg; i++)
        seg_copy[i] = segs[i];


    sh_puts_color("\n  Jumping to new kernel...\n\n", 0x0000FF00);

    /* Step 6: Shut down subsystems */

    /* Disable APIC timer (prevent timer interrupts during copy) */
    volatile uint32_t *apic = (volatile uint32_t *)KEXEC_APIC_BASE;
    apic[KEXEC_APIC_LVT_TMR / 4] = (1 << 16);  /* mask timer LVT */

    /* Send INIT IPI to all APs (puts them back to wait-for-SIPI state) */
    apic[KEXEC_APIC_ICR_HI / 4] = 0;
    apic[KEXEC_APIC_ICR_LO / 4] = 0x000C4500;  /* INIT, all-excluding-self */
    /* Brief spin wait for INIT to take effect */
    for (volatile int d = 0; d < 1000000; d++) {}

    /* Disable interrupts */
    sh_cli();

    /* Step 7: Jump to trampoline */
    typedef void (*tramp_fn)(void *info, uint64_t entry,
                             kexec_seg_t *segs, uint64_t nseg);
    tramp_fn tramp = (tramp_fn)KEXEC_TRAMP_ADDR;

    tramp(new_info, ehdr->e_entry, seg_copy, nseg);

    /* Should never reach here */
    for (;;) sh_hlt();
}

/* ── Builtin: reboot ─────────────────────────────────────────── */

static void cmd_reboot(void)
{
    sh_puts("Rebooting...\n");
    acpi_reboot();
}

/* ── Builtin: halt / shutdown ────────────────────────────────── */

static void cmd_halt(void)
{
    sh_puts_color("Shutting down...\n", 0x00FF8800);
    acpi_shutdown();
}

/* ── Builtin: clear ──────────────────────────────────────────── */

static void cmd_clear(void)
{
    fb_clear();
}

/* ── cmd_ps wrapper (proc_list outputs to serial/fb) ──────── */

static void cmd_ps(void)
{
    proc_list();
}

/* ── cmd_cpus — show SMP CPU state ─────────────────────────── */

extern uint32_t smp_cpu_count(void);

typedef struct {
    uint32_t apic_id;
    uint32_t cpu_index;
    bool     online;
    bool     bsp;
    uint64_t stack_top;
} cpu_info_t;

extern cpu_info_t *smp_cpu_info(uint32_t index);

static void cmd_cpus(void)
{
    uint32_t n = smp_cpu_count();
    sh_puts("CPUs: ");
    sh_putdec(n);
    sh_puts("\n");

    for (uint32_t i = 0; i < n; i++) {
        cpu_info_t *ci = smp_cpu_info(i);
        if (!ci) continue;
        sh_puts("  CPU ");
        sh_putdec(i);
        sh_puts(": APIC ");
        sh_putdec(ci->apic_id);
        sh_puts(ci->online ? " [online]" : " [offline]");
        if (ci->bsp) sh_puts(" (BSP)");
        sh_puts("\n");
    }
}

/* ── Builtin: dl (dynamic linker) ─────────────────────────────── */

extern void *dl_open(const char *filename);
extern void *dl_sym(void *handle, const char *name);
extern int   dl_close(void *handle);
extern void  dl_list_modules(void);
extern void *dl_find(const char *name);

/* ── HTTP Server (X-HTTPD) ───────────────────────────────────── */

static volatile bool httpd_running;
static int httpd_listener = -1;
static uint16_t httpd_port = 8080;

/* Simple integer to decimal string */
static int int_to_str(int val, char *buf)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int i = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int len = 0;
    if (neg) buf[len++] = '-';
    while (i > 0) buf[len++] = tmp[--i];
    return len;
}

/* Content-Type from file extension */
static const char *http_content_type(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    if (len > 5 && name[len-5]=='.' && name[len-4]=='h' && name[len-3]=='t' &&
        name[len-2]=='m' && name[len-1]=='l')
        return "text/html";
    if (len > 4 && name[len-4]=='.' && name[len-3]=='h' && name[len-2]=='t' &&
        name[len-1]=='m')
        return "text/html";
    if (len > 3 && name[len-3]=='.' && name[len-2]=='j' && name[len-1]=='s')
        return "application/javascript";
    if (len > 4 && name[len-4]=='.' && name[len-3]=='c' && name[len-2]=='s' &&
        name[len-1]=='s')
        return "text/css";
    if (len > 4 && name[len-4]=='.' && name[len-3]=='j' && name[len-2]=='s' &&
        name[len-1]=='n')
        return "application/json";
    if (len > 2 && name[len-2]=='.' && name[len-1]=='c')
        return "text/x-csrc";
    if (len > 2 && name[len-2]=='.' && name[len-1]=='h')
        return "text/x-chdr";
    if (len > 4 && name[len-4]=='.' && name[len-3]=='t' && name[len-2]=='x' &&
        name[len-1]=='t')
        return "text/plain";
    return "application/octet-stream";
}

/* Build directory listing HTML */
static int http_build_index(char *buf, int max)
{
    extern void *osfs2_file_at(int index);
    extern const char *osfs2_file_name(void *file);

    int pos = 0;
    const char *hdr =
        "<!DOCTYPE html><html><head><title>OsitoK</title>"
        "<style>body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:20px}"
        "a{color:#0ff;text-decoration:none}a:hover{text-decoration:underline}"
        "h1{color:#ff8800}table{border-collapse:collapse}td{padding:4px 16px}"
        "</style></head><body><h1>OsitoK File Server</h1><table>";
    while (*hdr && pos < max - 1) buf[pos++] = *hdr++;

    for (int i = 0; ; i++) {
        void *f = osfs2_file_at(i);
        if (!f) break;
        const char *name = osfs2_file_name(f);
        int size = osfs2_file_size(f);
        if (!name) continue;

        const char *tr1 = "<tr><td><a href=\"/";
        while (*tr1 && pos < max - 1) buf[pos++] = *tr1++;
        const char *n = name;
        while (*n && pos < max - 1) buf[pos++] = *n++;
        const char *tr2 = "\">";
        while (*tr2 && pos < max - 1) buf[pos++] = *tr2++;
        n = name;
        while (*n && pos < max - 1) buf[pos++] = *n++;
        const char *tr3 = "</a></td><td>";
        while (*tr3 && pos < max - 1) buf[pos++] = *tr3++;
        char sz[16];
        int slen = int_to_str(size, sz);
        for (int j = 0; j < slen && pos < max - 1; j++) buf[pos++] = sz[j];
        const char *tr4 = " B</td></tr>";
        while (*tr4 && pos < max - 1) buf[pos++] = *tr4++;
    }

    const char *ftr = "</table><hr><em>OsitoK X-HTTPD</em></body></html>";
    while (*ftr && pos < max - 1) buf[pos++] = *ftr++;
    buf[pos] = '\0';
    return pos;
}

/* Handle one HTTP request on an accepted connection */
static void http_handle_request(int conn)
{
    /* Read request (wait up to 3s for data) */
    char req[2048];
    int total = 0;
    uint64_t start = idt_get_ticks();

    while (total < (int)sizeof(req) - 1 && (idt_get_ticks() - start) < 300) {
        net_poll();
        int r = net_tcp_recv(conn, req + total, sizeof(req) - 1 - total);
        if (r > 0) {
            total += r;
            /* Check for end of headers */
            req[total] = '\0';
            bool found = false;
            for (int i = 0; i < total - 3; i++) {
                if (req[i]=='\r' && req[i+1]=='\n' && req[i+2]=='\r' && req[i+3]=='\n') {
                    found = true;
                    break;
                }
            }
            if (found) break;
        } else if (r < 0) {
            return;  /* Connection closed */
        }
        sh_hlt();
    }

    if (total <= 0) return;
    req[total] = '\0';

    /* Parse request line: "GET /path HTTP/1.x\r\n" */
    if (req[0] != 'G' || req[1] != 'E' || req[2] != 'T' || req[3] != ' ') {
        /* Only support GET */
        const char *resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        net_tcp_send(conn, resp, strlen(resp));
        return;
    }

    /* Extract path */
    char path[128];
    int pi = 0;
    for (int i = 4; i < total && req[i] != ' ' && req[i] != '?' && pi < 127; i++)
        path[pi++] = req[i];
    path[pi] = '\0';

    serial_puts("[HTTPD] GET ");
    serial_puts(path);
    serial_puts("\n");

    /* Build response */
    char hdr_buf[512];
    int hdr_len;

    if (path[0] == '/' && path[1] == '\0') {
        /* Directory listing */
        char *body = (char *)kmalloc(16384);
        if (!body) return;
        int body_len = http_build_index(body, 16384);

        hdr_len = 0;
        const char *h1 = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: ";
        while (*h1) hdr_buf[hdr_len++] = *h1++;
        hdr_len += int_to_str(body_len, hdr_buf + hdr_len);
        const char *h2 = "\r\nServer: OsitoK\r\n\r\n";
        while (*h2) hdr_buf[hdr_len++] = *h2++;

        net_tcp_send(conn, hdr_buf, hdr_len);
        net_tcp_send(conn, body, body_len);
        kfree(body);
    } else {
        /* Serve file from OsitoFS */
        const char *fname = path + 1;  /* Skip leading / */
        void *file = osfs2_find(fname);

        if (!file) {
            const char *not_found =
                "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
                "Connection: close\r\nContent-Length: 48\r\nServer: OsitoK\r\n\r\n"
                "<html><body><h1>404 Not Found</h1></body></html>";
            net_tcp_send(conn, not_found, strlen(not_found));
        } else {
            int file_size = osfs2_file_size(file);
            const char *ctype = http_content_type(fname);

            hdr_len = 0;
            const char *h1 = "HTTP/1.1 200 OK\r\nContent-Type: ";
            while (*h1) hdr_buf[hdr_len++] = *h1++;
            while (*ctype) hdr_buf[hdr_len++] = *ctype++;
            const char *h2 = "\r\nConnection: close\r\nContent-Length: ";
            while (*h2) hdr_buf[hdr_len++] = *h2++;
            hdr_len += int_to_str(file_size, hdr_buf + hdr_len);
            const char *h3 = "\r\nServer: OsitoK\r\n\r\n";
            while (*h3) hdr_buf[hdr_len++] = *h3++;

            /* Read file into memory, then send headers+body together.
             * Disable preemption during NVMe I/O — the NVMe driver's
             * polling loop isn't safe under preemptive scheduling. */
            int total_len = hdr_len + file_size;
            char *resp = (char *)kmalloc(total_len + 1);
            if (resp) {
                memcpy(resp, hdr_buf, hdr_len);
                if (file_size > 0) {
                    sh_cli();
                    int rc = osfs2_read(file, 0, resp + hdr_len, file_size);
                    sh_sti();
                    if (rc < 0) {
                        serial_puts("[HTTPD] File read error\n");
                        file_size = 0;
                        total_len = hdr_len;
                    }
                }
                net_tcp_send(conn, resp, total_len);
                kfree(resp);
            }
        }
    }
}

/* HTTP server thread (runs via scheduler) */
static void httpd_thread(void)
{
    httpd_listener = net_tcp_listen(httpd_port);
    if (httpd_listener < 0) {
        serial_puts("[HTTPD] Failed to listen\n");
        httpd_running = false;
        return;
    }

    serial_puts("[HTTPD] Server started on port ");
    serial_putdec(httpd_port);
    serial_puts("\n");

    while (httpd_running) {
        int conn = net_tcp_accept(httpd_listener, 100);  /* 1s timeout */
        if (conn >= 0) {
            http_handle_request(conn);
            /* Brief poll to let ACKs arrive before FIN */
            for (int i = 0; i < 20; i++) {
                net_poll();
                sh_hlt();
            }
            net_tcp_close(conn);
        }
    }

    net_tcp_stop_listen(httpd_listener);
    httpd_listener = -1;
    serial_puts("[HTTPD] Server stopped\n");
}

static void cmd_httpd(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        if (httpd_running) {
            httpd_running = false;
            sh_puts("Stopping HTTP server...\n");
        } else {
            sh_puts("HTTP server not running.\n");
        }
        return;
    }

    if (httpd_running) {
        sh_puts("HTTP server already running on port ");
        sh_putdec(httpd_port);
        sh_puts("\n");
        return;
    }

    if (argc >= 2) {
        /* Parse port number */
        uint16_t port = 0;
        const char *p = argv[1];
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            p++;
        }
        if (port > 0) httpd_port = port;
    }

    httpd_running = true;
    sched_spawn("httpd", httpd_thread);
    sh_puts("HTTP server started on port ");
    sh_putdec(httpd_port);
    /* Show the actual NIC IP (APIPA / DHCP / static) instead of the QEMU
     * SLIRP placeholder. Falls back to 0.0.0.0 if the stack hasn't bound
     * a v4 address yet, which itself is useful debugging info. */
    extern uint8_t *net_get_ip_ptr(void);
    uint8_t *ip = net_get_ip_ptr();
    sh_puts("\n  Access: http://");
    sh_putdec(ip[0]); sh_puts(".");
    sh_putdec(ip[1]); sh_puts(".");
    sh_putdec(ip[2]); sh_puts(".");
    sh_putdec(ip[3]); sh_puts(":");
    sh_putdec(httpd_port);
    sh_puts("/\n");
}

/* ── Dynamic Linker (X-DYN) ─────────────────────────────────── */

static void cmd_dl(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage:\n");
        sh_puts("  dl load <file.so>        Load shared object\n");
        sh_puts("  dl sym <file.so> <name>  Look up symbol address\n");
        sh_puts("  dl call <file.so> <name> Call void(*)(void) function\n");
        sh_puts("  dl close <file.so>       Unload module\n");
        sh_puts("  dl list                  List loaded modules\n");
        return;
    }

    if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) { sh_puts("Usage: dl load <file.so>\n"); return; }
        void *h = dl_open(argv[2]);
        if (h) {
            sh_puts_color("Module loaded.\n", 0x0000FF00);
        } else {
            sh_puts_color("Load failed.\n", 0x00FF0000);
        }
    } else if (strcmp(argv[1], "sym") == 0) {
        if (argc < 4) { sh_puts("Usage: dl sym <file.so> <name>\n"); return; }
        void *h = dl_find(argv[2]);
        if (!h) { sh_puts("Module not loaded: "); sh_puts(argv[2]); sh_puts("\n"); return; }
        void *sym = dl_sym(h, argv[3]);
        if (sym) {
            sh_puts(argv[3]);
            sh_puts(" = 0x");
            serial_puthex((uint64_t)sym, 16);
            /* Also show on fb as decimal (no hex helper there) */
            fb_puts(argv[3]);
            fb_puts(" found\n");
            sh_puts("\n");
        } else {
            sh_puts("Symbol not found: ");
            sh_puts(argv[3]);
            sh_puts("\n");
        }
    } else if (strcmp(argv[1], "call") == 0) {
        if (argc < 4) { sh_puts("Usage: dl call <file.so> <name>\n"); return; }
        void *h = dl_find(argv[2]);
        if (!h) { sh_puts("Module not loaded: "); sh_puts(argv[2]); sh_puts("\n"); return; }
        void *sym = dl_sym(h, argv[3]);
        if (!sym) { sh_puts("Symbol not found: "); sh_puts(argv[3]); sh_puts("\n"); return; }
        sh_puts_color("Calling ", 0x0000FF00);
        sh_puts(argv[3]);
        sh_puts("()...\n");
        void (*fn)(void) = (void (*)(void))sym;
        fn();
    } else if (strcmp(argv[1], "close") == 0) {
        if (argc < 3) { sh_puts("Usage: dl close <file.so>\n"); return; }
        void *h = dl_find(argv[2]);
        if (!h) { sh_puts("Module not loaded: "); sh_puts(argv[2]); sh_puts("\n"); return; }
        if (dl_close(h) == 0)
            sh_puts_color("Module unloaded.\n", 0x0000FF00);
        else
            sh_puts_color("Unload failed.\n", 0x00FF0000);
    } else if (strcmp(argv[1], "list") == 0) {
        dl_list_modules();
    } else {
        sh_puts("Unknown: dl ");
        sh_puts(argv[1]);
        sh_puts("\n");
    }
}

/* ── I/O redirection (X-PIPE) ─────────────────────────────────── */

typedef struct {
    const char *out_file;   /* > or >> target */
    const char *in_file;    /* < target */
    bool        append;     /* >> vs > */
} redir_t;

static void parse_redirects(int *argc, char *argv[], redir_t *r)
{
    r->out_file = NULL;
    r->in_file  = NULL;
    r->append   = false;

    int new_argc = 0;
    for (int i = 0; i < *argc; i++) {
        if (argv[i][0] == '>' && argv[i][1] == '>') {
            /* >>file (no space) */
            r->append = true;
            if (argv[i][2])
                r->out_file = &argv[i][2];
            else if (i + 1 < *argc)
                r->out_file = argv[++i];
        } else if (argv[i][0] == '>' && argv[i][1] == '\0') {
            /* > file */
            r->append = false;
            if (i + 1 < *argc)
                r->out_file = argv[++i];
        } else if (argv[i][0] == '>' && argv[i][1] != '\0') {
            /* >file (no space) */
            r->append = false;
            r->out_file = &argv[i][1];
        } else if (argv[i][0] == '<' && argv[i][1] == '\0') {
            /* < file */
            if (i + 1 < *argc)
                r->in_file = argv[++i];
        } else if (argv[i][0] == '<' && argv[i][1] != '\0') {
            /* <file */
            r->in_file = &argv[i][1];
        } else {
            argv[new_argc++] = argv[i];
        }
    }
    *argc = new_argc;
}

/* Captured output buffer for redirection. Non-static so wasm
 * osito_chat_sync can hijack temporarily to capture chat output. */
char    *redir_buf;
uint32_t redir_pos;
uint32_t redir_max;

void redir_capture(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (redir_pos < redir_max - 1)
            redir_buf[redir_pos++] = s[i];
    }
}

/* ── Dispatch command ────────────────────────────────────────── */

/* Forward decl — pipeline driver calls into this for each stage. */
/* Non-static so wasm cmd_make can invoke commands via shell pipeline. */
void shell_exec(char *line);

void shell_exec_pipeline(char *line)
{
    char *segments[8];
    int   nseg = 0;
    {
        char *p = line;
        char *seg_start = line;
        bool in_sq = false, in_dq = false;
        segments[0] = line;
        while (*p) {
            if (*p == '\'' && !in_dq) in_sq = !in_sq;
            else if (*p == '"' && !in_sq) in_dq = !in_dq;
            else if (*p == '|' && !in_sq && !in_dq) {
                *p = 0;
                if (nseg + 1 >= 8) {
                    sh_puts("pipe: too many stages (max 8)\n");
                    return;
                }
                segments[nseg++] = seg_start;
                seg_start = p + 1;
            }
            p++;
        }
        segments[nseg++] = seg_start;
    }

    if (nseg == 1) { shell_exec(line); return; }

    char    *prev_buf = NULL;
    uint32_t prev_len = 0;

    for (int i = 0; i < nseg; i++) {
        while (*segments[i] == ' ' || *segments[i] == '\t') segments[i]++;
        sh_stdin_buf = prev_buf;
        sh_stdin_len = prev_len;

        if (i < nseg - 1) {
            char *cap = (char *)kmalloc(1024 * 1024);
            if (!cap) { sh_puts("pipe: out of memory\n"); break; }
            char    *saved_buf = redir_buf;
            uint32_t saved_pos = redir_pos;
            uint32_t saved_max = redir_max;
            void   (*saved_fn)(const char *, size_t) = sh_redir_fn;

            redir_buf  = cap;
            redir_pos  = 0;
            redir_max  = 1024 * 1024;
            sh_redir_fn = redir_capture;

            shell_exec(segments[i]);

            uint32_t this_len = redir_pos;
            sh_redir_fn = saved_fn;
            redir_buf   = saved_buf;
            redir_pos   = saved_pos;
            redir_max   = saved_max;

            if (prev_buf) kfree(prev_buf);
            prev_buf = cap;
            prev_len = this_len;
        } else {
            shell_exec(segments[i]);
        }
    }

    sh_stdin_buf = NULL;
    sh_stdin_len = 0;
    if (prev_buf) kfree(prev_buf);
}

void shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = parse_args(line, argv);

    if (argc == 0) return;

    /* Parse I/O redirections */
    redir_t redir;
    parse_redirects(&argc, argv, &redir);

    if (argc == 0) return;

    /* Input redirection: `<file` loads file contents into sh_stdin_buf so
     * the command sees it as piped stdin. Restored after exec. */
    char       *in_buf = NULL;
    const char *saved_stdin_buf = sh_stdin_buf;
    uint32_t    saved_stdin_len = sh_stdin_len;
    if (redir.in_file) {
        extern void *osfs2_find(const char *);
        extern uint64_t osfs2_file_size(void *);
        extern int osfs2_read(void *, uint64_t, void *, uint64_t);
        void *f = osfs2_find(redir.in_file);
        if (!f) {
            sh_puts("redirect: file not found: ");
            sh_puts(redir.in_file);
            sh_puts("\n");
            return;
        }
        uint64_t fsz = osfs2_file_size(f);
        in_buf = (char *)kmalloc((size_t)fsz + 1);
        if (in_buf) {
            osfs2_read(f, 0, in_buf, fsz);
            in_buf[fsz] = 0;
            sh_stdin_buf = in_buf;
            sh_stdin_len = (uint32_t)fsz;
        }
    }

    /* Setup output redirection — 1 MB buffer covers a full dmesg dump
     * (the prior 64 KB cap silently truncated everything past the first
     * boot block, hiding the diagnostic we needed). */
    bool redirected = false;
    char *out_buf = NULL;
    if (redir.out_file) {
        out_buf = (char *)kmalloc(1024 * 1024);
        if (out_buf) {
            redir_buf = out_buf;
            redir_pos = 0;
            redir_max = 1024 * 1024;
            sh_redir_fn = redir_capture;
            redirected = true;
        }
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "cpus") == 0) {
        cmd_cpus();
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "cpu") == 0) {
        extern void cpu_features_dump(void);
        cpu_features_dump();
    } else if (strcmp(cmd, "perf") == 0) {
        extern void cmd_perf(int, char **);
        cmd_perf(argc, argv);
#ifdef __EMSCRIPTEN__
    } else if (strcmp(cmd, "cc") == 0) {
        extern void cmd_cc(int, char **);
        cmd_cc(argc, argv);
    } else if (strcmp(cmd, "edit") == 0) {
        extern void cmd_edit(int, char **);
        cmd_edit(argc, argv);
    } else if (strcmp(cmd, "make") == 0) {
        extern void cmd_make(int, char **);
        cmd_make(argc, argv);
#endif
    } else if (strcmp(cmd, "pred") == 0) {
        extern void pred_stats(void);
        extern void pred_reset(void);
        if (argc >= 2 && strcmp(argv[1], "reset") == 0) pred_reset();
        else pred_stats();
    } else if (strcmp(cmd, "io_predict") == 0) {
        extern void io_predict_stats(void);
        extern void io_predict_reset(void);
        if (argc >= 2 && strcmp(argv[1], "reset") == 0) io_predict_reset();
        else io_predict_stats();
    } else if (strcmp(cmd, "hwbp") == 0) {
        extern void hwbp_list(void);
        hwbp_list();
    } else if (strcmp(cmd, "watch") == 0) {
        /* Usage: watch <addr_hex> [w|rw|x] [len]
         * Defaults: write, len=8. Slot is auto-allocated. */
        extern int hwbp_set(int slot, uint64_t addr, int cond, int len, const char *name);
        struct hwbp_entry_public {
            uint64_t    addr;
            int         cond, len;
            bool        active;
            uint64_t    hit_count;
            char        name[32];
        };
        extern struct hwbp_entry_public hwbps[4];
        if (argc < 2) {
            serial_puts("usage: watch <addr_hex> [w|rw|x] [1|2|4|8]\n");
        } else {
            uint64_t addr = 0;
            const char *s = argv[1];
            if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) s += 2;
            while (*s) {
                char c = *s++;
                addr <<= 4;
                if (c >= '0' && c <= '9') addr |= c - '0';
                else if (c >= 'a' && c <= 'f') addr |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') addr |= c - 'A' + 10;
            }
            int cond = 1; /* HWBP_WRITE */
            if (argc >= 3) {
                if (strcmp(argv[2], "x") == 0) cond = 0;
                else if (strcmp(argv[2], "rw") == 0) cond = 3;
                else cond = 1;
            }
            int len_code = 2; /* HWBP_LEN_8 */
            if (argc >= 4) {
                int bytes = argv[3][0] - '0';
                if (bytes == 1) len_code = 0;
                else if (bytes == 2) len_code = 1;
                else if (bytes == 4) len_code = 3;
                else if (bytes == 8) len_code = 2;
            }
            /* find free slot */
            int slot = -1;
            for (int i = 0; i < 4; i++) if (!hwbps[i].active) { slot = i; break; }
            if (slot < 0) { serial_puts("All 4 HW breakpoint slots in use\n"); }
            else if (hwbp_set(slot, addr, cond, len_code, argv[1]) == 0) {
                serial_puts("Set hwbp slot ");
                serial_putdec(slot);
                serial_puts(" on 0x");
                serial_puthex(addr, 12);
                serial_puts("\n");
            }
        }
    } else if (strcmp(cmd, "unwatch") == 0) {
        extern int hwbp_clear(int);
        int slot = (argc >= 2) ? (argv[1][0] - '0') : 0;
        if (hwbp_clear(slot) == 0) {
            serial_puts("Cleared hwbp slot ");
            serial_putdec(slot);
            serial_puts("\n");
        }
    } else if (strcmp(cmd, "self_opt") == 0) {
        extern void self_opt_stats(void);
        extern void self_opt_apply(void);
        extern void self_opt_undo_all(void);
        extern bool self_opt_enabled;
        if (argc >= 2 && strcmp(argv[1], "apply") == 0) self_opt_apply();
        else if (argc >= 2 && strcmp(argv[1], "undo") == 0) self_opt_undo_all();
        else if (argc >= 2 && strcmp(argv[1], "enable") == 0) {
            self_opt_enabled = true;
            serial_puts("[SELF-OPT] enabled\n");
        }
        else if (argc >= 2 && strcmp(argv[1], "disable") == 0) {
            self_opt_enabled = false;
            serial_puts("[SELF-OPT] disabled\n");
        }
        else self_opt_stats();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(cmd, "ls") == 0) {
#ifdef __EMSCRIPTEN__
        /* `ls /aux` or any per-fs prefix lists the active aux FS. */
        if (argc >= 2 && argv[1][0] == '/') {
            static const char *prefixes[] = {
                "aux", "iso", "ext", "fat", "exfat", "ntfs", "hfs",
                "btrfs", "apfs", "udf", "sqfs", NULL
            };
            int matched = 0;
            const char *rest = NULL;
            for (int k = 0; prefixes[k]; k++) {
                int plen = 0; while (prefixes[k][plen]) plen++;
                int ok = 1;
                for (int j = 0; j < plen; j++) {
                    if (argv[1][1 + j] != prefixes[k][j]) { ok = 0; break; }
                }
                if (ok && (argv[1][1 + plen] == '\0' || argv[1][1 + plen] == '/')) {
                    matched = 1;
                    rest = argv[1] + 1 + plen;
                    break;
                }
            }
            if (matched) {
                /* Determine which fs to list by examining the prefix
                 * (rest[-1] etc). If the matched prefix is "aux",
                 * use the last-active fs; otherwise look up by name. */
                int prefix_idx = -1;
                for (int k = 0; prefixes[k]; k++) {
                    int plen = 0; while (prefixes[k][plen]) plen++;
                    int ok = 1;
                    for (int j = 0; j < plen; j++)
                        if (argv[1][1 + j] != prefixes[k][j]) { ok = 0; break; }
                    if (ok && (argv[1][1 + plen] == '\0' ||
                               argv[1][1 + plen] == '/')) {
                        prefix_idx = k; break;
                    }
                }
                const aux_fs_t *fs = (prefix_idx == 0)
                    ? cmd_active_aux_fs()
                    : cmd_aux_fs_by_name(prefixes[prefix_idx]);
                if (!fs) {
                    sh_puts("Not mounted: ");
                    sh_puts(prefix_idx == 0 ? "any aux FS"
                                            : prefixes[prefix_idx]);
                    sh_puts("\n");
                } else {
                    cmd_aux_fs_ls_for(fs, rest);
                }
            } else {
                cmd_ls();
            }
        } else
#endif
        cmd_ls();
    } else if (strcmp(cmd, "cp") == 0) {
        /* cp <src> <dst> — duplicate a file inside OsitoFS. */
        if (argc < 3 || !osfs2_is_mounted()) {
            sh_puts(argc < 3 ? "Usage: cp <src> <dst>\n"
                              : "No filesystem mounted\n");
        } else {
            extern void *osfs2_find(const char *);
            extern uint64_t osfs2_file_size(void *);
            extern int osfs2_read(void *file, uint64_t off, void *buf, uint64_t len);
            extern void *osfs2_create(const char *name, uint64_t size);
            extern int osfs2_write(void *file, uint64_t off, const void *buf, uint64_t len);
            void *src = osfs2_find(argv[1]);
            if (!src) sh_puts("cp: source not found\n");
            else {
                uint64_t sz = osfs2_file_size(src);
                char *buf = (char *)kmalloc(sz);
                if (!buf) sh_puts("cp: out of memory\n");
                else {
                    if (osfs2_read(src, 0, buf, sz) < 0)
                        sh_puts("cp: read failed\n");
                    else {
                        void *dst = osfs2_create(argv[2], sz);
                        if (!dst) sh_puts("cp: create failed\n");
                        else if (osfs2_write(dst, 0, buf, sz) < 0)
                            sh_puts("cp: write failed\n");
                        else
                            sh_puts("cp: ok\n");
                    }
                    kfree(buf);
                }
            }
        }
    } else if (strcmp(cmd, "mv") == 0) {
        if (argc < 3 || !osfs2_is_mounted()) {
            sh_puts(argc < 3 ? "Usage: mv <src> <dst>\n"
                              : "No filesystem mounted\n");
        } else {
            extern void *osfs2_find(const char *);
            extern uint64_t osfs2_file_size(void *);
            extern int osfs2_read(void *file, uint64_t off, void *buf, uint64_t len);
            extern void *osfs2_create(const char *name, uint64_t size);
            extern int osfs2_write(void *file, uint64_t off, const void *buf, uint64_t len);
            extern int osfs2_delete(const char *name);
            void *src = osfs2_find(argv[1]);
            if (!src) sh_puts("mv: source not found\n");
            else {
                uint64_t sz = osfs2_file_size(src);
                char *buf = (char *)kmalloc(sz);
                if (!buf) sh_puts("mv: out of memory\n");
                else {
                    bool ok = false;
                    if (osfs2_read(src, 0, buf, sz) < 0)
                        sh_puts("mv: read failed\n");
                    else {
                        void *dst = osfs2_create(argv[2], sz);
                        if (!dst) sh_puts("mv: create failed\n");
                        else if (osfs2_write(dst, 0, buf, sz) < 0)
                            sh_puts("mv: write failed\n");
                        else ok = true;
                    }
                    kfree(buf);
                    if (ok) {
                        if (osfs2_delete(argv[1]) < 0)
                            sh_puts("mv: dest ok but src delete failed\n");
                        else
                            sh_puts("mv: ok\n");
                    }
                }
            }
        }
    } else if (strcmp(cmd, "rm") == 0) {
        if (argc < 2 || !osfs2_is_mounted()) {
            sh_puts(argc < 2 ? "Usage: rm <name>\n"
                              : "No filesystem mounted\n");
        } else {
            extern int osfs2_delete(const char *name);
            sh_puts(osfs2_delete(argv[1]) < 0 ? "rm: failed\n" : "rm: ok\n");
        }
    } else if (strcmp(cmd, "head") == 0 || strcmp(cmd, "tail") == 0
               || strcmp(cmd, "grep") == 0) {
        /*
         * head <file> [-n N | -N | N]   first N lines (default 10)
         * tail <file> [-n N | -N | N]   last  N lines (default 10)
         * grep <pattern> <file>         lines containing literal substring
         *
         * Streaming implementation: read the file in 8 KB chunks and
         * carry a partial-line accumulator across chunk boundaries. No
         * cap on input size for head/grep (memory is O(longest_line)).
         * tail keeps a rolling ring of the last N line offsets and emits
         * them at EOF, so memory is O(N + longest_line).
         *
         * Resolves via VFS so it works on every mounted filesystem.
         * sh_puts honours `>`/`>>` redirect.
         */
        bool is_head = (cmd[0] == 'h');
        bool is_grep = (cmd[0] == 'g');

        const char *pattern  = NULL;
        const char *fname    = NULL;
        int  n_lines = 10;
        bool use_stdin = false;

        /* Treat "-" or absent file as "use pipe stdin" when sh_stdin_buf
         * is set by the pipeline driver. */
        #define STDIN_TOKEN(s)  ((s)[0] == '-' && (s)[1] == 0)

        if (is_grep) {
            if (argc < 2) {
                sh_puts("Usage: grep <pattern> [file]\n");
                goto cmd_filter_done;
            }
            pattern = argv[1];
            if (argc >= 3 && !STDIN_TOKEN(argv[2])) {
                fname = argv[2];
            } else if (sh_stdin_buf) {
                use_stdin = true;
            } else {
                sh_puts("grep: no file and no pipe input\n");
                goto cmd_filter_done;
            }
        } else {
            /* head/tail: file may be omitted when piping. argv[1] starting
             * with '-' or a digit is the count, not a filename. */
            int first_arg = 1;
            bool have_file_arg =
                (argc >= 2)
                && argv[1][0] != '-'
                && !(argv[1][0] >= '0' && argv[1][0] <= '9')
                && !STDIN_TOKEN(argv[1]);

            if (have_file_arg) {
                fname = argv[1];
                first_arg = 2;
            } else if (argc >= 2 && STDIN_TOKEN(argv[1])) {
                first_arg = 2;
                if (sh_stdin_buf) use_stdin = true;
            } else if (sh_stdin_buf) {
                use_stdin = true;
            } else {
                sh_puts(is_head ? "Usage: head <file> [-n N | N]\n"
                                : "Usage: tail <file> [-n N | N]\n");
                goto cmd_filter_done;
            }
            for (int ai = first_arg; ai < argc; ai++) {
                const char *a = argv[ai];
                if (a[0] == '-' && a[1] == 'n' && a[2] == 0 && ai + 1 < argc) {
                    a = argv[++ai];
                } else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') {
                    a++;
                }
                int v = 0;
                for (const char *p = a; *p >= '0' && *p <= '9'; p++)
                    v = v * 10 + (*p - '0');
                if (v > 0) n_lines = v;
            }
        }
        #undef STDIN_TOKEN

        vfs_node_t node;
        uint64_t sz;
        if (use_stdin) {
            sz = sh_stdin_len;
        } else {
            if (!vfs_find(fname, VFS_MODE_NATIVE, &node)) {
                sh_puts(fname);
                sh_puts(": file not found\n");
                goto cmd_filter_done;
            }
            sz = node.size;
        }
        if (sz == 0) goto cmd_filter_done;

        /* Streaming buffers:
         *   chunk[CHUNK_SZ]      one disk read at a time
         *   line[LINE_MAX+1]     accumulator for current line across reads
         */
#define CMD_FILTER_CHUNK_SZ  8192
#define CMD_FILTER_LINE_MAX  65536
        char *chunk = (char *)kmalloc(CMD_FILTER_CHUNK_SZ);
        char *line  = (char *)kmalloc(CMD_FILTER_LINE_MAX + 1);
        if (!chunk || !line) {
            sh_puts("out of memory\n");
            if (chunk) kfree(chunk);
            if (line)  kfree(line);
            goto cmd_filter_done;
        }
        uint32_t line_len = 0;

        /* tail-only: ring of last N line strings. Each slot is a
         * heap-malloc'd C-string of the line's content. We rotate
         * head index as lines come in. */
        char    **tail_ring   = NULL;
        uint32_t  tail_head   = 0;
        uint32_t  tail_count  = 0;
        if (!is_head && !is_grep) {
            tail_ring = (char **)kmalloc((uint64_t)n_lines * sizeof(char *));
            if (!tail_ring) {
                sh_puts("out of memory\n");
                kfree(chunk); kfree(line);
                goto cmd_filter_done;
            }
            for (int t = 0; t < n_lines; t++) tail_ring[t] = NULL;
        }

        int    plen          = 0;
        if (is_grep) { while (pattern[plen]) plen++; }

        int    head_emitted  = 0;
        int    head_done     = 0;
        uint64_t off         = 0;

        /* Emit/process one completed logical line (line[0..line_len)). */
        #define FLUSH_LINE() do {                                            \
            if (line_len < CMD_FILTER_LINE_MAX) line[line_len] = 0;          \
            else line[CMD_FILTER_LINE_MAX] = 0;                              \
            if (is_head) {                                                   \
                if (head_emitted < n_lines) {                                \
                    sh_puts(line); sh_puts("\n");                            \
                    if (++head_emitted >= n_lines) head_done = 1;            \
                }                                                            \
            } else if (is_grep) {                                            \
                if (plen > 0 && (int)line_len >= plen) {                     \
                    for (uint32_t k = 0; k + plen <= line_len; k++) {        \
                        int match = 1;                                       \
                        for (int p = 0; p < plen; p++)                       \
                            if (line[k + p] != pattern[p]) { match = 0; break; } \
                        if (match) {                                         \
                            sh_puts(line); sh_puts("\n");                    \
                            break;                                           \
                        }                                                    \
                    }                                                        \
                }                                                            \
            } else {                                                          \
                /* tail: store into ring, replacing oldest. */                \
                char *slot = tail_ring[tail_head];                            \
                if (slot) kfree(slot);                                        \
                slot = (char *)kmalloc((uint64_t)line_len + 1);               \
                if (slot) {                                                   \
                    for (uint32_t i = 0; i < line_len; i++) slot[i] = line[i];\
                    slot[line_len] = 0;                                       \
                }                                                              \
                tail_ring[tail_head] = slot;                                   \
                tail_head = (tail_head + 1) % (uint32_t)n_lines;               \
                if (tail_count < (uint32_t)n_lines) tail_count++;              \
            }                                                                 \
            line_len = 0;                                                     \
        } while (0)

        while (off < sz && !head_done) {
            uint64_t want = sz - off;
            if (want > CMD_FILTER_CHUNK_SZ) want = CMD_FILTER_CHUNK_SZ;
            int got;
            if (use_stdin) {
                for (uint64_t k = 0; k < want; k++)
                    chunk[k] = sh_stdin_buf[off + k];
                got = (int)want;
            } else {
                got = vfs_read(&node, off, chunk, want);
            }
            if (got < 0) { sh_puts("read failed\n"); break; }
            if (got == 0) break;
            for (int i = 0; i < got && !head_done; i++) {
                char c = chunk[i];
                if (c == '\n') {
                    FLUSH_LINE();
                } else if (line_len < CMD_FILTER_LINE_MAX) {
                    line[line_len++] = c;
                }
                /* Lines longer than LINE_MAX get truncated silently —
                 * acceptable for log/source files; binary blobs aren't
                 * the target of head/tail/grep anyway. */
            }
            off += (uint64_t)got;
        }
        /* Flush trailing partial line (no final newline). */
        if (!head_done && line_len > 0) {
            FLUSH_LINE();
        }

        /* tail: emit ring in chronological order. */
        if (!is_head && !is_grep && tail_ring) {
            uint32_t start = (tail_head + (uint32_t)n_lines - tail_count)
                             % (uint32_t)n_lines;
            for (uint32_t i = 0; i < tail_count; i++) {
                char *l = tail_ring[(start + i) % (uint32_t)n_lines];
                if (l) { sh_puts(l); sh_puts("\n"); }
            }
            for (int t = 0; t < n_lines; t++) {
                if (tail_ring[t]) kfree(tail_ring[t]);
            }
            kfree(tail_ring);
        }

        kfree(chunk);
        kfree(line);
        #undef FLUSH_LINE
    cmd_filter_done: ;
#undef CMD_FILTER_CHUNK_SZ
#undef CMD_FILTER_LINE_MAX
    } else if (strcmp(cmd, "sync") == 0) {
        /* Flush the active disk's controller cache before unplugging. */
        extern int disk_flush(void);
        sh_puts(disk_flush() == 0 ? "sync: ok\n" : "sync: failed\n");
    } else if (strcmp(cmd, "nic_stats") == 0) {
        extern void i211_print_stats(void) __attribute__((weak));
        if (i211_print_stats) i211_print_stats();
        else sh_puts("nic_stats: i211 not built\n");
    } else if (strcmp(cmd, "dmesg") == 0) {
        /* Dump the kernel ring buffer (klog). serial_puts has been
         * teeing into klog since boot, so this is everything the
         * kernel printed — useful when boot output scrolls off-screen
         * on bare-metal. */
        extern uint32_t klog_read(char *buf, uint32_t max_len);
        char *buf = (char *)kmalloc(64 * 1024);
        if (!buf) { sh_puts("dmesg: out of memory\n"); }
        else {
            uint32_t n = klog_read(buf, 64 * 1024);
            for (uint32_t i = 0; i < n; i++) {
                char s[2] = { buf[i], 0 };
                sh_puts(s);
            }
            kfree(buf);
        }
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(cmd, "exec") == 0) {
        cmd_exec(argc, argv);
    } else if (strcmp(cmd, "ifconfig") == 0) {
        extern uint8_t *net_get_ip_ptr(void);
        extern void net_get_mac(uint8_t mac[6]);
        extern uint64_t mem_get_free(void);
        uint8_t *ip = net_get_ip_ptr();
        uint8_t mac[6];
        net_get_mac(mac);
        sh_puts("eth0: ");
        sh_putdec(ip[0]); sh_puts("."); sh_putdec(ip[1]); sh_puts(".");
        sh_putdec(ip[2]); sh_puts("."); sh_putdec(ip[3]); sh_puts("\n");
        sh_puts("  HW ");
        for (int i = 0; i < 6; i++) {
            if (i > 0) sh_puts(":");
            char h[3]; h[0] = "0123456789abcdef"[(mac[i]>>4)&0xF];
            h[1] = "0123456789abcdef"[mac[i]&0xF]; h[2] = 0;
            sh_puts(h);
        }
        sh_puts("\n");
    } else if (strcmp(cmd, "dhcp") == 0) {
        extern int dhcp_discover(void);
        sh_puts("Retrying DHCP discovery...\n");
        int rc = dhcp_discover();
        sh_puts(rc == 0 ? "DHCP: success — `ifconfig` to see IP\n"
                        : "DHCP: failed — try `apipa` or `ipconf`\n");
    } else if (strcmp(cmd, "apipa") == 0) {
        extern int apipa_assign(void);
        sh_puts("Running APIPA link-local autoconfig (RFC 3927)...\n");
        int rc = apipa_assign();
        sh_puts(rc == 0 ? "APIPA: assigned — `ifconfig` to see IP\n"
                        : "APIPA: failed — use `ipconf`\n");
    } else if (strcmp(cmd, "ipconf") == 0) {
        /* Uso: ipconf <ip> <gw> <mask> [<dns>]
         * Cada arg es A.B.C.D.  Sólo <ip> es obligatorio.                */
        if (argc < 2) {
            sh_puts("Usage: ipconf <ip> [gw] [mask] [dns]\n");
            sh_puts("       ipconf 192.168.1.50 192.168.1.1 255.255.255.0 8.8.8.8\n");
        } else {
            extern void net_set_ip(const uint8_t ip[4]);
            extern void net_set_gateway(const uint8_t gw[4]);
            extern void net_set_netmask(const uint8_t mask[4]);
            extern void net_dns_set_server(const uint8_t ip[4]);
            uint8_t buf[4][4] = {{0}};
            int n = (argc - 1 < 4) ? argc - 1 : 4;
            for (int i = 0; i < n; i++) {
                /* Parser inline A.B.C.D → buf[i][0..3].                  */
                const char *s = argv[i + 1];
                int field = 0, val = 0;
                while (*s && field < 4) {
                    if (*s >= '0' && *s <= '9') val = val*10 + (*s - '0');
                    else if (*s == '.') { buf[i][field++] = (uint8_t)val; val = 0; }
                    s++;
                }
                if (field < 4) buf[i][field] = (uint8_t)val;
            }
            net_set_ip(buf[0]);
            if (n > 1) net_set_gateway(buf[1]);
            if (n > 2) net_set_netmask(buf[2]);
            if (n > 3) net_dns_set_server(buf[3]);
            sh_puts("ipconf: applied\n");
        }
    } else if (strcmp(cmd, "route") == 0) {
        sh_puts("Destination     Gateway         Flags  Iface\n");
        sh_puts("0.0.0.0         (gateway)       UG     eth0\n");
    } else if (strcmp(cmd, "netstat") == 0) {
        sh_puts("Proto  Local     Remote    State\n");
        /* TODO: iterate TCP connection table */
        sh_puts("(use serial log for TCP connection details)\n");
    } else if (strcmp(cmd, "ping") == 0) {
        cmd_ping(argc, argv);
    } else if (strcmp(cmd, "tcptest") == 0) {
        cmd_tcptest(argc, argv);
    } else if (strcmp(cmd, "resolve") == 0) {
        cmd_resolve(argc, argv);
    } else if (strcmp(cmd, "tlstest") == 0) {
        cmd_tlstest(argc, argv);
    } else if (strcmp(cmd, "curl") == 0) {
        cmd_curl(argc, argv);
    } else if (strcmp(cmd, "apikey") == 0) {
        cmd_apikey(argc, argv);
    } else if (strcmp(cmd, "ask") == 0) {
        cmd_ask(argc, argv);
    } else if (strcmp(cmd, "claude") == 0) {
        cmd_claude(argc, argv);
    } else if (strcmp(cmd, "chat") == 0) {
        cmd_chat(argc, argv);
    } else if (strcmp(cmd, "rag") == 0) {
        cmd_rag(argc, argv);
    } else if (strcmp(cmd, "penalty") == 0) {
        cmd_penalty(argc, argv);
#ifdef __EMSCRIPTEN__
    } else if (strcmp(cmd, "crypto") == 0) {
        if (argc < 3) {
            sh_puts("Usage: crypto <hash|hmac> <text>\n");
            sh_puts("  crypto sha256 <text>   SHA-256 hex\n");
            sh_puts("  crypto sha512 <text>   SHA-512 hex\n");
            sh_puts("  crypto chacha <key32> <text>   ChaCha20 hex output\n");
        } else if (strcmp(argv[1], "sha256") == 0) {
            extern void sha256(const void *data, uint32_t len, uint8_t digest[32]);
            uint8_t d[32];
            char buf[1024]; int p = 0;
            for (int i = 2; i < argc && p < (int)sizeof(buf) - 1; i++) {
                if (i > 2 && p < (int)sizeof(buf) - 1) buf[p++] = ' ';
                const char *w = argv[i];
                while (*w && p < (int)sizeof(buf) - 1) buf[p++] = *w++;
            }
            sha256(buf, p, d);
            static const char hex[] = "0123456789abcdef";
            char out[65];
            for (int i = 0; i < 32; i++) {
                out[i*2]   = hex[(d[i] >> 4) & 0xF];
                out[i*2+1] = hex[d[i] & 0xF];
            }
            out[64] = '\0';
            sh_puts(out); sh_puts("\n");
        } else if (strcmp(argv[1], "sha512") == 0) {
            extern void sha512(const uint8_t *data, uint64_t len, uint8_t hash[64]);
            uint8_t d[64];
            char buf[1024]; int p = 0;
            for (int i = 2; i < argc && p < (int)sizeof(buf) - 1; i++) {
                if (i > 2 && p < (int)sizeof(buf) - 1) buf[p++] = ' ';
                const char *w = argv[i];
                while (*w && p < (int)sizeof(buf) - 1) buf[p++] = *w++;
            }
            sha512((const uint8_t *)buf, (uint64_t)p, d);
            static const char hex[] = "0123456789abcdef";
            char out[129];
            for (int i = 0; i < 64; i++) {
                out[i*2]   = hex[(d[i] >> 4) & 0xF];
                out[i*2+1] = hex[d[i] & 0xF];
            }
            out[128] = '\0';
            sh_puts(out); sh_puts("\n");
        } else {
            sh_puts("Unknown crypto subcommand.\n");
        }
    } else if (strcmp(cmd, "embed") == 0) {
        /* Tier 2 #9: local embedder. Runs the loaded llama/brandon
         * model forward over the input text and returns the
         * post-final-norm hidden state at the last token,
         * L2-normalized. Useful for self-similarity (query cache,
         * shell-history dedup); NOT compatible with bge-large
         * corpus vectors (different embedding space + dimension). */
        if (!prompt_llama) {
            sh_puts("embed: no model loaded\n");
            return;
        }
        if (argc < 2) {
            sh_puts("Usage: embed <text>\n");
            return;
        }
        char buf[512]; int blen = 0;
        for (int i = 1; i < argc && blen < (int)sizeof(buf) - 2; i++) {
            if (i > 1 && blen < (int)sizeof(buf) - 1) buf[blen++] = ' ';
            for (const char *p = argv[i];
                 *p && blen < (int)sizeof(buf) - 1; p++)
                buf[blen++] = *p;
        }
        buf[blen] = 0;
        extern int llama_embed_text(void *state, const char *text,
                                     float *out, int max_dim);
        extern void *malloc(unsigned long); extern void free(void *);
        const int MAX_D = 4096;
        float *vec = (float *)malloc(MAX_D * sizeof(float));
        if (!vec) { sh_puts("OOM\n"); return; }
        int dim = llama_embed_text(prompt_llama, buf, vec, MAX_D);
        if (dim < 0) { sh_puts("embed: failed\n"); free(vec); return; }
        double norm = 0.0;
        for (int i = 0; i < dim; i++) norm += (double)vec[i]*(double)vec[i];
        sh_puts("[embed] dim="); sh_putdec((uint64_t)dim);
        sh_puts(" norm*1e6="); sh_putdec((uint64_t)(norm * 1e6));
        sh_puts(" first8=");
        for (int i = 0; i < 8 && i < dim; i++) {
            int v = (int)(vec[i] * 10000.0f);
            if (i > 0) sh_puts(",");
            if (v < 0) { sh_puts("-"); v = -v; }
            sh_putdec((uint64_t)v);
        }
        sh_puts("\n");
        free(vec);
    } else if (strcmp(cmd, "parallel") == 0) {
#ifdef __EMSCRIPTEN__
        /* Tier 2 #8 demo: spawn N Web Workers and time a parallel
         * F32 matvec vs single-threaded. Real model integration is
         * a separate step; this cmd verifies the bridge + measures
         * the speedup ceiling on the user's machine.
         *
         * usage:
         *   parallel init [N]        Spawn N workers (default 4)
         *   parallel dot              Benchmark 512x512 F32 matvec
         *   parallel count            Show active worker count */
        extern int  wasm_workers_init(int n);
        extern int  wasm_workers_count(void);
        extern int  wasm_workers_dot_f32(const float *W, const float *x,
                                          int rows, int cols, float *out);
        if (argc < 2 || strcmp(argv[1], "count") == 0) {
            sh_puts("[parallel] active workers: ");
            sh_putdec((uint64_t)wasm_workers_count()); sh_puts("\n");
        } else if (strcmp(argv[1], "init") == 0) {
            int n = 4;
            if (argc >= 3) {
                int v = 0;
                for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
                    v = v * 10 + (*p - '0');
                if (v > 0 && v <= 8) n = v;
            }
            int got = wasm_workers_init(n);
            sh_puts("[parallel] spawned "); sh_putdec((uint64_t)got);
            sh_puts(" workers\n");
        } else if (strcmp(argv[1], "dot") == 0) {
            if (wasm_workers_count() <= 0) {
                sh_puts("[parallel] no workers — run 'parallel init' first\n");
                return;
            }
            int rows = 512, cols = 512;
            extern void *malloc(unsigned long); extern void free(void *);
            float *W = (float *)malloc((size_t)rows * cols * 4);
            float *x = (float *)malloc((size_t)cols * 4);
            float *out_p = (float *)malloc((size_t)rows * 4);
            float *out_s = (float *)malloc((size_t)rows * 4);
            if (!W || !x || !out_p || !out_s) { sh_puts("OOM\n"); goto pdone; }
            for (int i = 0; i < rows*cols; i++) W[i] = (float)((i*7%13)-6)*0.1f;
            for (int i = 0; i < cols; i++)      x[i] = (float)((i*3%11)-5)*0.1f;
            extern uint64_t idt_get_ticks(void);
            uint64_t t0 = idt_get_ticks();
            for (int r = 0; r < rows; r++) {
                float s = 0;
                for (int c = 0; c < cols; c++) s += W[r*cols+c] * x[c];
                out_s[r] = s;
            }
            uint64_t t1 = idt_get_ticks();
            wasm_workers_dot_f32(W, x, rows, cols, out_p);
            uint64_t t2 = idt_get_ticks();
            float md = 0;
            for (int i = 0; i < rows; i++) {
                float d = out_p[i] - out_s[i];
                if (d < 0) d = -d;
                if (d > md) md = d;
            }
            sh_puts("[parallel dot 512x512] single: ");
            sh_putdec(t1 - t0); sh_puts(" ms  parallel(");
            sh_putdec((uint64_t)wasm_workers_count()); sh_puts("): ");
            sh_putdec(t2 - t1); sh_puts(" ms  maxdiff*1e6=");
            sh_putdec((uint64_t)(md * 1e6f)); sh_puts("\n");
pdone:
            if (W) free(W); if (x) free(x);
            if (out_p) free(out_p); if (out_s) free(out_s);
        } else {
            sh_puts("Usage: parallel init [N] | parallel dot | parallel count\n");
        }
#else
        sh_puts("parallel: WASM-only\n");
#endif
    } else if (strcmp(cmd, "wiki") == 0) {
#ifdef __EMSCRIPTEN__
        /* Browser-RAG over simple_en. Builds a ChatML system+user turn
         * that puts Wikipedia hits in the system prompt and the user's
         * question in the user turn. Brandon-tiny gets nudged toward
         * citing rather than hallucinating; Llama 1B naturally treats
         * the system context as ground truth. */
        if (argc < 2) {
            sh_puts("Usage: wiki <query>              Retrieve + answer (simple_en)\n");
            sh_puts("       wiki -c <corpus> <query>  Use a different corpus (e.g. wiki_en)\n");
            sh_puts("       wiki status               Show cached RAG shards\n");
            sh_puts("       wiki clear                Drop all cached RAG shards\n");
            sh_puts("Recommended sampling: temp 0.4 + penalty 1.15 0.1 0.1 + ngram 3\n");
            return;
        }
        if (strcmp(argv[1], "status") == 0) {
            /* Walk OsitoFS for any file under rag/. */
            extern void *osfs2_get_file(int idx);
            extern const char *osfs2_file_name(void *f);
            extern uint64_t osfs2_file_size(void *f);
            uint64_t total = 0;
            int count = 0;
            for (int i = 0; i < 4096; i++) {
                void *f = osfs2_get_file(i);
                if (!f) continue;
                const char *nm = osfs2_file_name(f);
                if (!nm || nm[0] != 'r' || nm[1] != 'a' || nm[2] != 'g' || nm[3] != '/')
                    continue;
                uint64_t sz = osfs2_file_size(f);
                sh_puts("  "); sh_puts(nm);
                sh_puts(" ("); sh_putdec(sz); sh_puts(" B)\n");
                total += sz; count++;
            }
            if (count == 0) sh_puts("  (no shards cached yet — run 'wiki <query>' first)\n");
            else {
                sh_puts("  total: "); sh_putdec((uint64_t)count);
                sh_puts(" files, "); sh_putdec(total / 1024); sh_puts(" KB\n");
            }
            return;
        }
        if (strcmp(argv[1], "clear") == 0) {
            extern void *osfs2_get_file(int idx);
            extern const char *osfs2_file_name(void *f);
            extern int   osfs2_delete(const char *name);
            int removed = 0;
            /* Two-pass: collect names then delete (avoids index shift). */
            char names[64][128];
            int nn = 0;
            for (int i = 0; i < 4096 && nn < 64; i++) {
                void *f = osfs2_get_file(i);
                if (!f) continue;
                const char *nm = osfs2_file_name(f);
                if (!nm || nm[0]!='r'||nm[1]!='a'||nm[2]!='g'||nm[3]!='/') continue;
                int l = 0;
                while (nm[l] && l < 127) { names[nn][l] = nm[l]; l++; }
                names[nn][l] = 0;
                nn++;
            }
            for (int i = 0; i < nn; i++) {
                if (osfs2_delete(names[i]) == 0) removed++;
            }
            sh_puts("[wiki] cleared "); sh_putdec((uint64_t)removed); sh_puts(" shards\n");
            return;
        }
        if (!prompt_llama) {
            sh_puts("No model loaded — use 'model brandon' or 'model llama-1b' first.\n");
            return;
        }
        /* Optional '-c <corpus>' override; default simple_en. wiki_en
         * support lands once that index finishes embedding. */
        const char *corpus = "simple_en";
        int start = 1;
        if (argc >= 4 && strcmp(argv[1], "-c") == 0) {
            corpus = argv[2];
            start = 3;
        }
        char qbuf[512]; int qn = 0;
        for (int i = start; i < argc; i++) {
            if (i > start && qn < (int)sizeof(qbuf) - 1) qbuf[qn++] = ' ';
            for (const char *p = argv[i]; *p && qn < (int)sizeof(qbuf) - 1; p++)
                qbuf[qn++] = *p;
        }
        qbuf[qn] = 0;
        extern int rag_retrieve(const char *corpus, const char *query,
                                 char *result, int result_max);
        char *hits = (char *)malloc(2048);
        if (!hits) { sh_puts("wiki: OOM\n"); return; }
        if (rag_retrieve(corpus, qbuf, hits, 2048) != 0) {
            sh_puts_color("wiki: retrieval failed (network?)\n", 0x00FF0000);
            free(hits); return;
        }
        sh_puts_color("\n--- Retrieved context ---\n", 0x00FF8800);
        sh_puts(hits);
        sh_puts("\n--- LM response ---");

        /* Build a compact system prompt with the hits. Brandon's ctx
         * cap is 512 — keep system terse so question + generation fit. */
        char *system_msg = (char *)malloc(2048);
        int sp = 0;
        const char *pre = "Use the Wikipedia excerpts below to answer the user. "
                          "Cite only what the excerpts state; if they don't say, say you don't know.\n";
        for (const char *p = pre; *p && sp < 2046; p++) system_msg[sp++] = *p;
        const char *hp = hits;
        if (*hp == '\n') hp++;
        for (; *hp && sp < 2046; hp++) system_msg[sp++] = *hp;
        system_msg[sp] = 0;

        sh_puts_color("\nLlama: ", 0x00FF8800);
        int r = llama_chat_with_system(prompt_llama, system_msg, qbuf, 128,
                                        chat_token_cb, NULL);
        if (r < 0) sh_puts_color("[error]\n", 0x00FF0000);
        else       sh_puts("\n");
        free(hits); free(system_msg);
#else
        sh_puts("wiki: WASM-only\n");
#endif
    } else if (strcmp(cmd, "https") == 0) {
        if (argc < 2) {
            sh_puts("Usage: https <host> [path]\n");
            sh_puts("  Real TLS 1.2 client over the WS-tunneled TCP proxy.\n");
            sh_puts("  Requires the CF Worker proxy to be deployed (tcp proxy).\n");
            sh_puts("  Cert validation is OFF — for trusted/demo targets only.\n");
            return;
        }
        const char *host = argv[1];
        const char *path = argc >= 3 ? argv[2] : "/";

        sh_puts("[https] connecting to "); sh_puts(host); sh_puts(":443...\n");
        int tcp = wasm_tcp_connect_host(host, 443);
        if (tcp < 0) {
            sh_puts_color("[https] tcp connect failed (proxy down?)\n", 0x00FF0000);
            return;
        }

        void *tls = malloc(WASM_TLS_CTX_BYTES);
        if (!tls) {
            sh_puts("[https] OOM\n"); net_tcp_close(tcp); return;
        }
        memset(tls, 0, WASM_TLS_CTX_BYTES);

        sh_puts("[https] TLS handshake...\n");
        if (tls_connect(tls, tcp, host) < 0) {
            sh_puts_color("[https] handshake failed\n", 0x00FF0000);
            net_tcp_close(tcp); free(tls); return;
        }
        sh_puts_color("[https] connected\n", 0x0000FF00);

        /* Build minimal HTTP/1.0 GET. Connection: close so the server
         * EOFs after the body. */
        char req[1024];
        int p = 0;
        const char *prefix = "GET ";
        while (*prefix) req[p++] = *prefix++;
        while (*path)   req[p++] = *path++;
        const char *mid = " HTTP/1.0\r\nHost: ";
        while (*mid)    req[p++] = *mid++;
        const char *h = host;
        while (*h)      req[p++] = *h++;
        const char *suffix = "\r\nConnection: close\r\nUser-Agent: OsitoK/1.0\r\n\r\n";
        while (*suffix) req[p++] = *suffix++;

        if (tls_send(tls, req, p) < 0) {
            sh_puts_color("[https] tls_send failed\n", 0x00FF0000);
            tls_close(tls); free(tls); return;
        }

        /* Stream response */
        char rxbuf[2048];
        int total = 0;
        for (;;) {
            int n = tls_recv(tls, rxbuf, sizeof(rxbuf) - 1, 100 /* ticks */);
            if (n <= 0) break;
            rxbuf[n] = '\0';
            sh_puts(rxbuf);
            total += n;
            if (total > 64 * 1024) {
                sh_puts("\n... [truncated at 64 KB]\n");
                break;
            }
        }
        sh_puts("\n");
        tls_close(tls);
        free(tls);
    } else if (strcmp(cmd, "rendezvous") == 0) {
#ifdef __EMSCRIPTEN__
        /* Open a 'listen' WS to the proxy's room endpoint. The
         * counterparty connects via /connect?room=ID; the worker's
         * Durable Object pairs the two and pumps messages. Pure-JS
         * relay; no real TCP listener.
         *
         *   osito (A)> rendezvous listen demo123     # waits for B
         *   osito (B)> rendezvous connect demo123   # joins
         *   then either side: ws send <ws_name> hi  ws recv <ws_name>
         */
        if (argc < 3) {
            sh_puts("Usage: rendezvous <listen|connect> <room-id> [name]\n");
            sh_puts("  Pairs two browser kernels via a CF Worker room.\n");
            return;
        }
        const char *role = argv[1];
        const char *room = argv[2];
        const char *name = argc >= 4 ? argv[3] : "rdv";
        if (ws_slot_find(name)) { sh_puts("Slot in use.\n"); return; }

        char url[512];
        int p = 0;
        const char *base = "wss://tcp-proxy.naranjositos.tech/";
        while (*base) url[p++] = *base++;
        if (strcmp(role, "listen") == 0) {
            const char *e = "listen?room=";
            while (*e) url[p++] = *e++;
        } else if (strcmp(role, "connect") == 0) {
            const char *e = "connect?room=";
            while (*e) url[p++] = *e++;
        } else {
            sh_puts("role must be 'listen' or 'connect'\n");
            return;
        }
        while (*room && p < (int)sizeof(url) - 1) url[p++] = *room++;
        url[p] = '\0';

        ws_slot_t *slot = ws_slot_alloc();
        if (!slot) { sh_puts("No free slots.\n"); return; }
        int h = wasm_ws_open(url);
        if (h <= 0) { sh_puts_color("[rdv] open failed\n", 0x00FF0000); return; }
        if (wasm_ws_wait_open(h, 5000) < 0) {
            sh_puts_color("[rdv] handshake/timeout\n", 0x00FF0000);
            wasm_ws_close(h);
            return;
        }
        slot->handle = h;
        int n = 0;
        while (n < 15 && name[n]) { slot->name[n] = name[n]; n++; }
        slot->name[n] = '\0';
        sh_puts_color("[rdv] joined as '", 0x0000FF00);
        sh_puts(slot->name);
        sh_puts("' — use `ws send/recv ");
        sh_puts(slot->name);
        sh_puts("`\n");
#else
        sh_puts("rendezvous: WASM-only\n");
#endif
    } else if (strcmp(cmd, "tcp") == 0) {
        /* TCP-over-WS bridge: substitutes {host} and {port} into a
         * configured WSS proxy URL, then opens via wasm_ws_open. The
         * proxy must be a server that bridges WS frames ↔ TCP bytes. */
        static char tcp_proxy_url[256] =
            "wss://tcp-proxy.naranjositos.tech/?host={host}&port={port}";

        if (argc < 2) {
            sh_puts("Usage: tcp <proxy|connect|send|recv|close|list> [args]\n");
            sh_puts("  tcp proxy <url-template>      set the WS proxy URL\n");
            sh_puts("                                placeholders: {host}, {port}\n");
            sh_puts("  tcp connect <host> <port> [name]\n");
            sh_puts("  tcp send <name> <data...>     same shape as `ws send`\n");
            sh_puts("  tcp recv <name> [ms]\n");
            sh_puts("  tcp close <name>\n");
            sh_puts("  tcp list                       (alias of ws list)\n");
            sh_puts("Current proxy: ");
            sh_puts(tcp_proxy_url); sh_puts("\n");
        } else if (strcmp(argv[1], "proxy") == 0) {
            if (argc < 3) { sh_puts("Usage: tcp proxy <url-template>\n"); }
            else {
                int n = 0;
                while (argv[2][n] && n < (int)sizeof(tcp_proxy_url) - 1) {
                    tcp_proxy_url[n] = argv[2][n]; n++;
                }
                tcp_proxy_url[n] = '\0';
                sh_puts("[tcp] proxy set\n");
            }
        } else if (strcmp(argv[1], "connect") == 0) {
            if (argc < 4) { sh_puts("Usage: tcp connect <host> <port> [name]\n"); }
            else {
                const char *name = argc >= 5 ? argv[4] : "tcp";
                if (ws_slot_find(name)) { sh_puts("Slot in use.\n"); }
                else {
                    /* Substitute {host}/{port} into the template */
                    char url[512];
                    int up = 0;
                    const char *t = tcp_proxy_url;
                    while (*t && up < (int)sizeof(url) - 1) {
                        if (t[0] == '{' && t[1] == 'h' && t[2] == 'o' &&
                            t[3] == 's' && t[4] == 't' && t[5] == '}') {
                            const char *h = argv[2];
                            while (*h && up < (int)sizeof(url) - 1) url[up++] = *h++;
                            t += 6;
                        } else if (t[0] == '{' && t[1] == 'p' && t[2] == 'o' &&
                                   t[3] == 'r' && t[4] == 't' && t[5] == '}') {
                            const char *p = argv[3];
                            while (*p && up < (int)sizeof(url) - 1) url[up++] = *p++;
                            t += 6;
                        } else {
                            url[up++] = *t++;
                        }
                    }
                    url[up] = '\0';

                    ws_slot_t *slot = ws_slot_alloc();
                    if (!slot) { sh_puts("No free slots.\n"); }
                    else {
                        int h = wasm_ws_open(url);
                        if (h <= 0) { sh_puts_color("[tcp] open failed\n", 0x00FF0000); }
                        else if (wasm_ws_wait_open(h, 5000) < 0) {
                            sh_puts_color("[tcp] handshake/timeout\n", 0x00FF0000);
                            wasm_ws_close(h);
                        } else {
                            slot->handle = h;
                            int n = 0;
                            while (n < 15 && name[n]) { slot->name[n] = name[n]; n++; }
                            slot->name[n] = '\0';
                            sh_puts_color("[tcp] connected as '", 0x0000FF00);
                            sh_puts(slot->name); sh_puts("' via proxy\n");
                        }
                    }
                }
            }
        } else if (strcmp(argv[1], "send") == 0 ||
                   strcmp(argv[1], "recv") == 0 ||
                   strcmp(argv[1], "close") == 0 ||
                   strcmp(argv[1], "list") == 0) {
            sh_puts("Use 'ws ");
            sh_puts(argv[1]);
            sh_puts(" ...' — tcp slots share the same name table.\n");
        } else {
            sh_puts("Unknown tcp subcommand.\n");
        }
    } else if (strcmp(cmd, "ws") == 0) {
        if (argc < 2) {
            sh_puts("Usage: ws <open|send|recv|close|list> [args...]\n");
            sh_puts("  ws open <url> [name]    open WebSocket (default name 'ws')\n");
            sh_puts("  ws send <name> <data>   send a text frame\n");
            sh_puts("  ws recv <name> [ms]     wait up to ms for a message (default 2000)\n");
            sh_puts("  ws close <name>         close the connection\n");
            sh_puts("  ws list                 show open connections\n");
        } else if (strcmp(argv[1], "open") == 0) {
            if (argc < 3) { sh_puts("Usage: ws open <url> [name]\n"); }
            else {
                const char *name = argc >= 4 ? argv[3] : "ws";
                if (ws_slot_find(name)) {
                    sh_puts("Slot already in use. Use a different name or 'ws close'.\n");
                } else {
                    ws_slot_t *slot = ws_slot_alloc();
                    if (!slot) { sh_puts("No free WS slots.\n"); }
                    else {
                        int h = wasm_ws_open(argv[2]);
                        if (h <= 0) { sh_puts_color("[ws] open failed\n", 0x00FF0000); }
                        else if (wasm_ws_wait_open(h, 5000) < 0) {
                            sh_puts_color("[ws] handshake failed/timeout\n", 0x00FF0000);
                            wasm_ws_close(h);
                        } else {
                            slot->handle = h;
                            int n = 0;
                            while (n < 15 && name[n]) { slot->name[n] = name[n]; n++; }
                            slot->name[n] = '\0';
                            sh_puts_color("[ws] connected as '", 0x0000FF00);
                            sh_puts(slot->name); sh_puts("'\n");
                        }
                    }
                }
            }
        } else if (strcmp(argv[1], "send") == 0) {
            if (argc < 4) { sh_puts("Usage: ws send <name> <data...>\n"); }
            else {
                ws_slot_t *slot = ws_slot_find(argv[2]);
                if (!slot) { sh_puts("Unknown WS name.\n"); }
                else {
                    /* Reassemble argv[3..] with single spaces */
                    char buf[2048]; int p = 0;
                    for (int i = 3; i < argc && p < (int)sizeof(buf) - 1; i++) {
                        if (i > 3 && p < (int)sizeof(buf) - 1) buf[p++] = ' ';
                        const char *w = argv[i];
                        while (*w && p < (int)sizeof(buf) - 1) buf[p++] = *w++;
                    }
                    buf[p] = '\0';
                    int rc = wasm_ws_send(slot->handle, buf, p);
                    if (rc < 0) sh_puts_color("[ws] send failed\n", 0x00FF0000);
                    else { sh_puts("[ws] sent "); sh_putdec((uint64_t)rc); sh_puts(" bytes\n"); }
                }
            }
        } else if (strcmp(argv[1], "recv") == 0) {
            if (argc < 3) { sh_puts("Usage: ws recv <name> [timeout_ms]\n"); }
            else {
                ws_slot_t *slot = ws_slot_find(argv[2]);
                if (!slot) { sh_puts("Unknown WS name.\n"); }
                else {
                    int timeout = 2000;
                    if (argc >= 4) {
                        timeout = 0;
                        const char *s = argv[3];
                        while (*s >= '0' && *s <= '9') { timeout = timeout*10 + (*s - '0'); s++; }
                    }
                    static char rxbuf[8192];
                    int n = wasm_ws_recv_wait(slot->handle, rxbuf, sizeof(rxbuf) - 1, timeout);
                    if (n < 0) sh_puts_color("[ws] connection closed\n", 0x00FF0000);
                    else if (n == 0) sh_puts("[ws] timeout, no data\n");
                    else {
                        rxbuf[n] = '\0';
                        sh_puts("[ws] "); sh_putdec((uint64_t)n); sh_puts(" bytes:\n");
                        /* chunk-stream */
                        char chunk[256];
                        int i = 0;
                        while (i < n) {
                            int k = n - i; if (k > 255) k = 255;
                            for (int j = 0; j < k; j++) chunk[j] = rxbuf[i+j];
                            chunk[k] = '\0';
                            sh_puts(chunk);
                            i += k;
                        }
                        sh_puts("\n");
                    }
                }
            }
        } else if (strcmp(argv[1], "close") == 0) {
            if (argc < 3) { sh_puts("Usage: ws close <name>\n"); }
            else {
                ws_slot_t *slot = ws_slot_find(argv[2]);
                if (!slot) { sh_puts("Unknown WS name.\n"); }
                else {
                    wasm_ws_close(slot->handle);
                    slot->handle = 0; slot->name[0] = '\0';
                    sh_puts("[ws] closed\n");
                }
            }
        } else if (strcmp(argv[1], "list") == 0) {
            int any = 0;
            for (int i = 0; i < WS_SLOT_MAX; i++) {
                if (ws_slots[i].handle) {
                    int s = wasm_ws_state(ws_slots[i].handle);
                    const char *name_state =
                        s == 0 ? "connecting" :
                        s == 1 ? "open" :
                        s == 2 ? "closing" :
                        s == 3 ? "closed" : "error";
                    sh_puts("  ");
                    sh_puts(ws_slots[i].name);
                    sh_puts(" — ");
                    sh_puts(name_state);
                    sh_puts("\n");
                    any = 1;
                }
            }
            if (!any) sh_puts("(no open WS connections)\n");
        } else {
            sh_puts("Unknown ws subcommand.\n");
        }
    } else if (strcmp(cmd, "mount-fs") == 0) {
        if (argc < 3) {
            sh_puts("Usage: mount-fs <type> <url>\n");
            sh_puts("  Types: iso ext fat exfat ntfs hfs btrfs apfs udf sqfs\n");
            sh_puts("  Multi-mount: one of each type can be mounted at once\n");
            sh_puts("  After: ls /<type>/  cat /<type>/<name>\n");
            return;
        }
        const aux_fs_t *fs = find_aux_fs(argv[1]);
        if (!fs) {
            sh_puts("Unknown FS type. Run: mount-fs (no args) for list.\n");
        } else {
            /* Find the table index for this fs (used as slot id). */
            int idx = -1;
            for (int i = 0; i < AUX_FS_COUNT; i++)
                if (&aux_fs_table[i] == fs) { idx = i; break; }
            if (idx < 0) idx = 0;

            int slot = wasm_aux_alloc_slot(fs->name);
            if (slot < 0) { sh_puts("[mount-fs] no free slots\n"); return; }

            wasm_aux_route_slot(slot);
            int fetch_rc = aux_disk_fetch_slot(slot, argv[2]);
            if (fetch_rc < 0) {
                wasm_aux_route_slot(-1);
                wasm_aux_release_slot(slot);
                sh_puts_color("[mount-fs] fetch failed\n", 0x00FF0000);
            } else {
                int rc = -1;
                if (fs->mount_iso)
                    rc = fs->mount_iso(fs->name[0] == 'i' ? aux_disk_read_iso
                                                           : aux_disk_read_512);
                else if (fs->mount_sqfs)
                    rc = fs->mount_sqfs(aux_disk_read_bytes_wrap, aux_disk_size());
                else if (fs->mount_partlba) {
                    wasm_nvme_route_aux(fs->route_aux);
                    rc = fs->mount_partlba(0);
                    wasm_nvme_route_aux(0);
                } else if (strcmp(fs->name, "ext") == 0) {
                    rc = ext2_mount(0, aux_disk_read_512);
                }
                wasm_aux_route_slot(-1);

                if (rc < 0) {
                    wasm_aux_release_slot(slot);
                    sh_puts_color("[mount-fs] not a valid ", 0x00FF0000);
                    sh_puts(fs->name); sh_puts(" image\n");
                } else {
                    g_aux_mounts[idx] = fs;
                    g_last_active_aux_fs = fs;
                    sh_puts_color("[mount-fs] ", 0x0000FF00);
                    sh_puts(fs->name);
                    sh_puts(" mounted (");
                    sh_putdec(aux_disk_size() / (1024 * 1024));
                    sh_puts(" MB) at /");
                    sh_puts(fs->name); sh_puts("/\n");
                }
            }
        }
    } else if (strcmp(cmd, "umount") == 0 || strcmp(cmd, "umount-fs") == 0) {
        extern void cmd_clear_aux_fs(void);
        extern void cmd_clear_aux_fs_by_name(const char *name);
        if (argc >= 2) {
            cmd_clear_aux_fs_by_name(argv[1]);
            sh_puts("[umount] "); sh_puts(argv[1]); sh_puts(" detached\n");
        } else if (!cmd_active_aux_fs()) {
            sh_puts("No aux FS mounted. Use: umount <type> to be specific.\n");
        } else {
            cmd_clear_aux_fs();
            sh_puts("[umount] all aux FS detached\n");
        }
    } else if (strcmp(cmd, "fs-ls") == 0) {
        if (!g_active_aux_fs) sh_puts("No FS mounted (mount-fs <type> <url>).\n");
        else {
            wasm_nvme_route_aux(g_active_aux_fs->route_aux);
            g_active_aux_fs->ls(argc >= 2 ? argv[1] : NULL);
            wasm_nvme_route_aux(0);
        }
    } else if (strcmp(cmd, "fs-cat") == 0) {
        if (argc < 2) sh_puts("Usage: fs-cat <name>\n");
        else if (!g_active_aux_fs) sh_puts("No FS mounted.\n");
        else if (!g_active_aux_fs->read_file)
            sh_puts("This FS supports listing only (no read_file).\n");
        else {
            static char fbuf[65536];
            wasm_nvme_route_aux(g_active_aux_fs->route_aux);
            int rc = g_active_aux_fs->read_file(argv[1], 0, fbuf, sizeof(fbuf));
            wasm_nvme_route_aux(0);
            if (rc <= 0) sh_puts("File not found or read failed.\n");
            else {
                char chunk[256];
                int i = 0;
                while (i < rc) {
                    int n = rc - i; if (n > 255) n = 255;
                    for (int j = 0; j < n; j++) chunk[j] = fbuf[i + j];
                    chunk[n] = '\0';
                    sh_puts(chunk);
                    i += n;
                }
                if (rc == (int)sizeof(fbuf))
                    sh_puts("\n... [truncated at 64 KB]\n");
                else
                    sh_puts("\n");
            }
        }
    } else if (strcmp(cmd, "mount-iso") == 0) {
        if (argc < 2) {
            sh_puts("Usage: mount-iso <url>\n");
            sh_puts("  Fetches an ISO 9660 image and mounts it for browsing.\n");
            sh_puts("  After mount: iso-ls [path] / iso-cat <file>\n");
        } else if (aux_disk_fetch(argv[1]) < 0) {
            sh_puts_color("[mount-iso] fetch failed\n", 0x00FF0000);
        } else if (iso9660_mount(aux_disk_read_iso) < 0) {
            sh_puts_color("[mount-iso] not a valid ISO 9660 image\n", 0x00FF0000);
        } else {
            sh_puts_color("[mount-iso] mounted (", 0x0000FF00);
            sh_putdec(aux_disk_size() / (1024 * 1024));
            sh_puts(" MB) — try: iso-ls\n");
        }
    } else if (strcmp(cmd, "iso-ls") == 0) {
        if (!iso9660_is_mounted()) {
            sh_puts("No ISO mounted. Use: mount-iso <url>\n");
        } else {
            iso9660_ls(argc >= 2 ? argv[1] : "/");
        }
    } else if (strcmp(cmd, "iso-cat") == 0) {
        if (argc < 2) {
            sh_puts("Usage: iso-cat <filename>\n");
        } else if (!iso9660_is_mounted()) {
            sh_puts("No ISO mounted.\n");
        } else {
            uint32_t lba = 0, size = 0;
            if (iso9660_find(argv[1], &lba, &size) < 0) {
                sh_puts("File not found.\n");
            } else {
                uint32_t cap = size > 65536 ? 65536 : size;
                static char fbuf[65536];
                if (iso9660_read_file(argv[1], 0, fbuf, cap) < 0) {
                    sh_puts("Read failed.\n");
                } else {
                    char chunk[256];
                    uint32_t i = 0;
                    while (i < cap) {
                        uint32_t n = cap - i; if (n > 255) n = 255;
                        for (uint32_t j = 0; j < n; j++) chunk[j] = fbuf[i + j];
                        chunk[n] = '\0';
                        sh_puts(chunk);
                        i += n;
                    }
                    if (size > cap) {
                        sh_puts("\n... [truncated, ");
                        sh_putdec((size - cap) / 1024);
                        sh_puts(" KB more]\n");
                    } else {
                        sh_puts("\n");
                    }
                }
            }
        }
    } else if (strcmp(cmd, "mount-ext2") == 0) {
        if (argc < 2) {
            sh_puts("Usage: mount-ext2 <url>\n");
            sh_puts("  Fetches an ext2/3/4 image and mounts read-only.\n");
            sh_puts("  After mount: ext-ls / ext-cat <file>\n");
        } else if (aux_disk_fetch(argv[1]) < 0) {
            sh_puts_color("[mount-ext2] fetch failed\n", 0x00FF0000);
        } else if (ext2_mount(0, aux_disk_read_512) < 0) {
            sh_puts_color("[mount-ext2] not a valid ext2/3/4 image\n", 0x00FF0000);
        } else {
            sh_puts_color("[mount-ext2] mounted (", 0x0000FF00);
            sh_putdec(aux_disk_size() / (1024 * 1024));
            sh_puts(" MB)\n");
        }
    } else if (strcmp(cmd, "ext-ls") == 0) {
        if (!ext2_is_mounted()) sh_puts("No ext2 mounted.\n");
        else                    ext2_ls();
    } else if (strcmp(cmd, "mount-fat") == 0) {
        if (argc < 2) {
            sh_puts("Usage: mount-fat <url>\n");
            sh_puts("  Fetches a FAT32 image and mounts read-only.\n");
            sh_puts("  After mount: fat-ls / fat-cat <file>\n");
        } else if (aux_disk_fetch(argv[1]) < 0) {
            sh_puts_color("[mount-fat] fetch failed\n", 0x00FF0000);
        } else {
            wasm_nvme_route_aux(1);
            int rc = fat32_mount(0);
            wasm_nvme_route_aux(0);
            if (rc < 0) sh_puts_color("[mount-fat] not a valid FAT32 image\n", 0x00FF0000);
            else {
                sh_puts_color("[mount-fat] mounted (", 0x0000FF00);
                sh_putdec(aux_disk_size() / (1024 * 1024));
                sh_puts(" MB)\n");
            }
        }
    } else if (strcmp(cmd, "fat-ls") == 0) {
        if (!fat32_is_mounted()) sh_puts("No FAT32 mounted.\n");
        else {
            wasm_nvme_route_aux(1);
            fat32_ls(argc >= 2 ? argv[1] : "/");
            wasm_nvme_route_aux(0);
        }
    } else if (strcmp(cmd, "fat-cat") == 0) {
        if (argc < 2) sh_puts("Usage: fat-cat <name>\n");
        else if (!fat32_is_mounted()) sh_puts("No FAT32 mounted.\n");
        else {
            uint32_t clu = 0, sz = 0;
            wasm_nvme_route_aux(1);
            int found = fat32_find(argv[1], &clu, &sz);
            wasm_nvme_route_aux(0);
            if (found < 0) sh_puts("File not found.\n");
            else {
                static char fbuf[65536];
                uint32_t cap = sz > 65536 ? 65536 : sz;
                wasm_nvme_route_aux(1);
                int rc = fat32_read_file(argv[1], 0, fbuf, cap);
                wasm_nvme_route_aux(0);
                if (rc < 0) sh_puts("Read failed.\n");
                else {
                    char chunk[256];
                    uint32_t i = 0;
                    while (i < cap) {
                        uint32_t n = cap - i; if (n > 255) n = 255;
                        for (uint32_t j = 0; j < n; j++) chunk[j] = fbuf[i + j];
                        chunk[n] = '\0';
                        sh_puts(chunk);
                        i += n;
                    }
                    if (sz > cap) {
                        sh_puts("\n... [truncated, ");
                        sh_putdec((sz - cap) / 1024);
                        sh_puts(" KB more]\n");
                    } else sh_puts("\n");
                }
            }
        }
    } else if (strcmp(cmd, "ext-cat") == 0) {
        if (argc < 2) sh_puts("Usage: ext-cat <name>\n");
        else if (!ext2_is_mounted()) sh_puts("No ext2 mounted.\n");
        else {
            uint32_t ino = 0;
            if (ext2_find(argv[1], &ino) < 0) sh_puts("File not found.\n");
            else {
                static char ebuf[65536];
                int r = ext2_read_file(argv[1], 0, ebuf, sizeof(ebuf));
                if (r <= 0) sh_puts("Read failed.\n");
                else {
                    char chunk[256];
                    int i = 0;
                    while (i < r) {
                        int n = r - i; if (n > 255) n = 255;
                        for (int j = 0; j < n; j++) chunk[j] = ebuf[i + j];
                        chunk[n] = '\0';
                        sh_puts(chunk);
                        i += n;
                    }
                    sh_puts("\n");
                }
            }
        }
#endif
    } else if (strcmp(cmd, "model") == 0) {
#ifdef __EMSCRIPTEN__
        if (argc < 2) {
            sh_puts("Usage: model <name|url>\n");
            sh_puts("  Aliases: brandon | tinystories | smollm | llama-1b\n");
            sh_puts("  Or full URL. Reload page to apply.\n");
            return;
        }
        const char *url = NULL;
        if      (strcmp(argv[1], "brandon")     == 0) url =
            "https://factory.naranjositos.tech/models/brandon-tiny-10m-instruct.f16.gguf";
        else if (strcmp(argv[1], "tinystories") == 0) url =
            "https://factory.naranjositos.tech/models/tinystories-llama2-20m.Q4_K_M.gguf";
        else if (strcmp(argv[1], "smollm")      == 0) url =
            "https://factory.naranjositos.tech/models/smollm2-135m-q4_0.gguf";
        else if (strcmp(argv[1], "llama-1b") == 0 ||
                 strcmp(argv[1], "llama1b")  == 0) url =
            "https://factory.naranjositos.tech/models/llama-3.2-1b-instruct-q4_k_m.gguf";
        else if (strcmp(argv[1], "default")     == 0) url = "";
        else url = argv[1];

        /* Keyed under 'osito-model' (raw, no 'osito-cfg-' prefix) so
         * shell.html's localStorage.getItem('osito-model') reads it
         * directly. wasm_config_save adds its own prefix, so write via
         * a separate js_localstorage_set helper. */
        extern void wasm_localstorage_set(const char *key, const char *value);
        wasm_localstorage_set("osito-model", url);
        sh_puts("[model] set to: ");
        sh_puts(url[0] ? url : "(default — cleared)");
        sh_puts("\nReload the page to load the new model.\n");
#else
        sh_puts("model: WASM-only command\n");
#endif
    } else if (strcmp(cmd, "version") == 0) {
#ifdef OSITO_GIT_REV
        sh_puts("OsitoK build "); sh_puts(OSITO_GIT_REV);
        sh_puts("  ("); sh_puts(OSITO_BUILD_TS); sh_puts(")\n");
#else
        sh_puts("OsitoK build (no git rev embedded)\n");
#endif
    } else if (strcmp(cmd, "uname") == 0) {
        sh_puts("OsitoK (");
#ifdef __EMSCRIPTEN__
        sh_puts("wasm32");
#else
        sh_puts("x86_64");
#endif
        sh_puts(") — bare-metal kernel");
        if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
            sh_puts("\n  source: github.com/.../osito-k");
            sh_puts("\n  arch:   ");
#ifdef __EMSCRIPTEN__
            sh_puts("wasm32 (Emscripten)");
#else
            sh_puts("x86_64");
#endif
            sh_puts("\n  shell:  121+ builtins (chat/git/cc/mount-fs/ws/...)");
        }
        sh_puts("\n");
    } else if (strcmp(cmd, "hexdump") == 0 || strcmp(cmd, "xxd") == 0) {
        if (argc < 2) {
            sh_puts("Usage: hexdump <file> [bytes]   (default 256)\n");
            return;
        }
        if (!osfs2_is_mounted()) { sh_puts("No filesystem mounted\n"); return; }
        void *f = osfs2_find(argv[1]);
        if (!f) { sh_puts("File not found.\n"); return; }
        extern uint64_t osfs2_file_size(void *file);
        uint64_t fsz = osfs2_file_size(f);
        int want = 256;
        if (argc >= 3) {
            want = 0;
            for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
                want = want * 10 + (*s - '0');
        }
        if ((uint64_t)want > fsz) want = (int)fsz;
        if (want > 8192) want = 8192;

        static uint8_t buf[8192];
        if (osfs2_read(f, 0, buf, want) < 0) { sh_puts("Read failed.\n"); return; }
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < want; i += 16) {
            char line[80]; int p = 0;
            /* address */
            for (int s = 24; s >= 0; s -= 8) line[p++] = hex[(i >> s) & 0xF];
            line[p++] = ' '; line[p++] = ' ';
            /* hex bytes */
            for (int j = 0; j < 16; j++) {
                if (i + j < want) {
                    line[p++] = hex[(buf[i+j] >> 4) & 0xF];
                    line[p++] = hex[buf[i+j] & 0xF];
                } else {
                    line[p++] = ' '; line[p++] = ' ';
                }
                line[p++] = (j == 7) ? '-' : ' ';
            }
            line[p++] = ' ';
            /* ascii */
            for (int j = 0; j < 16 && i + j < want; j++) {
                uint8_t c = buf[i+j];
                line[p++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            line[p++] = '\n'; line[p] = '\0';
            sh_puts(line);
        }
        if ((uint64_t)want < fsz) {
            sh_puts("... ["); sh_putdec(fsz - want); sh_puts(" more bytes]\n");
        }
    } else if (strcmp(cmd, "base64") == 0) {
        if (argc < 2) {
            sh_puts("Usage: base64 [-d] <text>   encode/decode\n");
            return;
        }
        bool decode = false;
        int start = 1;
        if (argv[1][0] == '-' && argv[1][1] == 'd') { decode = true; start = 2; }
        if (start >= argc) { sh_puts("Need data after -d\n"); return; }

        /* Reassemble argv into one buffer */
        static uint8_t in[4096];
        int n = 0;
        for (int i = start; i < argc && n < (int)sizeof(in) - 1; i++) {
            if (i > start && n < (int)sizeof(in) - 1) in[n++] = ' ';
            const char *w = argv[i];
            while (*w && n < (int)sizeof(in) - 1) in[n++] = (uint8_t)*w++;
        }

        static const char enc_tab[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        if (!decode) {
            static char out[6000];
            int o = 0;
            for (int i = 0; i < n; i += 3) {
                uint32_t v = ((uint32_t)in[i]) << 16;
                if (i + 1 < n) v |= ((uint32_t)in[i+1]) << 8;
                if (i + 2 < n) v |= ((uint32_t)in[i+2]);
                out[o++] = enc_tab[(v >> 18) & 0x3F];
                out[o++] = enc_tab[(v >> 12) & 0x3F];
                out[o++] = (i + 1 < n) ? enc_tab[(v >> 6) & 0x3F] : '=';
                out[o++] = (i + 2 < n) ? enc_tab[v & 0x3F] : '=';
            }
            out[o] = '\0';
            sh_puts(out); sh_puts("\n");
        } else {
            /* Build inverse table */
            int8_t dec_tab[128]; for (int i = 0; i < 128; i++) dec_tab[i] = -1;
            for (int i = 0; i < 64; i++) dec_tab[(int)enc_tab[i]] = (int8_t)i;
            static uint8_t out[3000];
            int o = 0;
            for (int i = 0; i < n; i += 4) {
                int8_t a = (i   < n && in[i]   < 128) ? dec_tab[in[i]]   : -1;
                int8_t b = (i+1 < n && in[i+1] < 128) ? dec_tab[in[i+1]] : -1;
                int8_t c = (i+2 < n && in[i+2] < 128) ? dec_tab[in[i+2]] : -1;
                int8_t d = (i+3 < n && in[i+3] < 128) ? dec_tab[in[i+3]] : -1;
                if (a < 0 || b < 0) break;
                out[o++] = (uint8_t)((a << 2) | (b >> 4));
                if (c >= 0 && in[i+2] != '=') out[o++] = (uint8_t)((b << 4) | (c >> 2));
                if (d >= 0 && in[i+3] != '=') out[o++] = (uint8_t)((c << 6) | d);
            }
            out[o] = '\0';
            for (int i = 0; i < o; i++) {
                char c[2] = { (char)out[i], 0 };
                sh_puts(c);
            }
            sh_puts("\n");
        }
    } else if (strcmp(cmd, "touch") == 0) {
        if (argc < 2) { sh_puts("Usage: touch <file>\n"); return; }
        if (!osfs2_is_mounted()) { sh_puts("No filesystem mounted\n"); return; }
        extern void *osfs2_create(const char *name, uint64_t size);
        extern void *osfs2_find(const char *name);
        if (osfs2_find(argv[1])) {
            sh_puts("(file exists)\n");
            return;
        }
        if (osfs2_create(argv[1], 0)) {
            sh_puts(argv[1]); sh_puts(" created\n");
        } else {
            sh_puts("touch failed\n");
        }
    } else if (strcmp(cmd, "find") == 0) {
        if (argc < 2) { sh_puts("Usage: find <substring>\n"); return; }
        if (!osfs2_is_mounted()) { sh_puts("No filesystem mounted\n"); return; }
        extern uint32_t osfs2_file_count(void);
        extern void *osfs2_file_at(int index);
        extern const char *osfs2_file_name(void *file);
        extern uint64_t osfs2_file_size(void *file);
        const char *needle = argv[1];
        uint32_t total = osfs2_file_count();
        int matches = 0, shown = 0;
        for (uint32_t i = 0; i < 4096 && shown < (int)total; i++) {
            void *f = osfs2_file_at(i);
            if (!f) continue;
            shown++;
            const char *name = osfs2_file_name(f);
            const char *p = name; bool found = false;
            while (*p) {
                const char *a = p; const char *b = needle;
                while (*a && *b && *a == *b) { a++; b++; }
                if (!*b) { found = true; break; }
                p++;
            }
            if (found) {
                sh_putdec_padded(osfs2_file_size(f), 11);
                sh_puts("  ");
                sh_puts(name);
                sh_puts("\n");
                matches++;
            }
        }
        if (matches == 0) sh_puts("(no matches)\n");
    } else if (strcmp(cmd, "du") == 0) {
        if (!osfs2_is_mounted()) { sh_puts("No filesystem mounted\n"); return; }
        extern uint32_t osfs2_file_count(void);
        extern void *osfs2_file_at(int index);
        extern const char *osfs2_file_name(void *file);
        extern uint64_t osfs2_file_size(void *file);
        uint32_t total = osfs2_file_count();
        uint64_t bytes_total = 0;
        uint32_t shown = 0;
        for (uint32_t i = 0; i < 4096 && shown < total; i++) {
            void *f = osfs2_file_at(i);
            if (!f) continue;
            uint64_t sz = osfs2_file_size(f);
            sh_putdec_padded(sz, 11); sh_puts("  ");
            sh_puts(osfs2_file_name(f)); sh_puts("\n");
            bytes_total += sz;
            shown++;
        }
        sh_puts("       ───\n");
        sh_putdec_padded(bytes_total, 11); sh_puts("  total ");
        if (bytes_total > 1024 * 1024) {
            sh_puts("("); sh_putdec(bytes_total / (1024 * 1024)); sh_puts(" MB)");
        } else if (bytes_total > 1024) {
            sh_puts("("); sh_putdec(bytes_total / 1024); sh_puts(" KB)");
        }
        sh_puts("\n");
    } else if (strcmp(cmd, "df") == 0) {
        if (!osfs2_is_mounted()) { sh_puts("No filesystem mounted\n"); return; }
        extern uint32_t osfs2_file_count(void);
        extern uint32_t osfs2_free_blocks(void);
        extern uint32_t osfs2_get_block_size(void);
        extern const char *osfs2_label(void);
        uint32_t bs    = osfs2_get_block_size();
        uint32_t freeb = osfs2_free_blocks();
        sh_puts("Filesystem        Files  Free blocks  Free MB\n");
        sh_puts("/  \""); sh_puts(osfs2_label()); sh_puts("\"   ");
        sh_putdec_padded((uint64_t)osfs2_file_count(), 5); sh_puts("  ");
        sh_putdec_padded((uint64_t)freeb, 11); sh_puts("  ");
        sh_putdec((uint64_t)freeb * (uint64_t)bs / (1024 * 1024));
        sh_puts(" MB (block size ");
        sh_putdec((uint64_t)bs / 1024); sh_puts(" KB)\n");
#ifdef __EMSCRIPTEN__
        for (int i = 0; i < AUX_FS_COUNT; i++) {
            if (g_aux_mounts[i]) {
                sh_puts("/"); sh_puts(g_aux_mounts[i]->name);
                sh_puts("/  (read-only auxiliary mount)\n");
            }
        }
#endif
    } else if (strcmp(cmd, "samples") == 0) {
        if (argc < 2 || strcmp(argv[1], "list") == 0) {
            sh_puts_color("Available samples (in /samples/):\n", 0x00FF8800);
            sh_puts("  hello    classic stdio hello world\n");
            sh_puts("  fib      recursive Fibonacci, takes argv n\n");
            sh_puts("  oi_chat  user-space program calling kernel LLM\n");
            sh_puts("  cat      stdin → stdout (use with pipes)\n");
            sh_puts("Usage:\n");
            sh_puts("  samples cat <name>      Show source\n");
            sh_puts("  samples build <name>    cc + save to <name>.wasm\n");
            sh_puts("  samples run <name>      build + exec\n");
        } else if (strcmp(argv[1], "cat") == 0 && argc >= 3) {
            char path[64]; int p = 0;
            const char *prefix = "/samples/";
            while (*prefix && p < 60) path[p++] = *prefix++;
            const char *n = argv[2];
            while (*n && p < 58) path[p++] = *n++;
            const char *suffix = ".c";
            while (*suffix && p < 62) path[p++] = *suffix++;
            path[p] = '\0';
            char *fakeargv[2] = { (char *)"cat", path };
            cmd_cat(2, fakeargv);
        } else if (strcmp(argv[1], "build") == 0 && argc >= 3) {
            char line[160]; int p = 0;
            const char *prefix = "cc /samples/";
            while (*prefix && p < (int)sizeof(line) - 1) line[p++] = *prefix++;
            const char *n = argv[2];
            while (*n && p < (int)sizeof(line) - 4) line[p++] = *n++;
            const char *mid = ".c -o ";
            while (*mid && p < (int)sizeof(line) - 1) line[p++] = *mid++;
            n = argv[2];
            while (*n && p < (int)sizeof(line) - 6) line[p++] = *n++;
            const char *ext = ".wasm";
            while (*ext && p < (int)sizeof(line) - 1) line[p++] = *ext++;
            line[p] = '\0';
            sh_puts_color("> ", 0x00FF8800); sh_puts(line); sh_puts("\n");
            extern void shell_exec_pipeline(char *line);
            shell_exec_pipeline(line);
        } else if (strcmp(argv[1], "run") == 0 && argc >= 3) {
            /* Build then exec — two pipeline calls */
            char line[160]; int p = 0;
            const char *prefix = "cc /samples/";
            while (*prefix) line[p++] = *prefix++;
            const char *n = argv[2];
            while (*n) line[p++] = *n++;
            const char *mid = ".c -o ";
            while (*mid) line[p++] = *mid++;
            n = argv[2];
            while (*n) line[p++] = *n++;
            const char *ext = ".wasm";
            while (*ext) line[p++] = *ext++;
            line[p] = '\0';
            sh_puts_color("> ", 0x00FF8800); sh_puts(line); sh_puts("\n");
            extern void shell_exec_pipeline(char *line);
            shell_exec_pipeline(line);

            char line2[64]; int p2 = 0;
            const char *e = "exec ";
            while (*e) line2[p2++] = *e++;
            n = argv[2];
            while (*n) line2[p2++] = *n++;
            ext = ".wasm";
            while (*ext) line2[p2++] = *ext++;
            line2[p2] = '\0';
            sh_puts_color("> ", 0x00FF8800); sh_puts(line2); sh_puts("\n");
            shell_exec_pipeline(line2);
        } else {
            sh_puts("Unknown subcommand. Run: samples (no args) for help.\n");
        }
    } else if (strcmp(cmd, "demo") == 0) {
        sh_puts_color("\n=== OsitoK 30-second demo ===\n", 0x00FF8800);
        extern void shell_exec_pipeline(char *line);

        struct { const char *title; char line[64]; } steps[] = {
            { "1. Kernel state",      "info" },
            { "2. Sample sources",    "ls /samples" },
            { "3. Hello world",       "cat /samples/hello.c" },
            { "4. Chat (LLM)",        "chat tell me a 1-line tip" },
            { "5. Crypto",            "crypto sha256 osito-k" },
            { "6. Filesystem dashboard", "df" },
        };
        for (int i = 0; i < (int)(sizeof(steps)/sizeof(steps[0])); i++) {
            sh_puts_color("\n──── ", 0x00FFD93D);
            sh_puts(steps[i].title);
            sh_puts_color(" ────\n$ ", 0x00FFD93D);
            sh_puts(steps[i].line);
            sh_puts("\n");
            char copy[64];
            int n = 0;
            while (n < 63 && steps[i].line[n]) { copy[n] = steps[i].line[n]; n++; }
            copy[n] = '\0';
            shell_exec_pipeline(copy);
        }
        sh_puts_color("\n=== demo done — try `tutorial` for the guided tour ===\n\n",
                       0x00FF8800);
    } else if (strcmp(cmd, "tutorial") == 0) {
        sh_puts_color("\n=== OsitoK quick tour ===\n", 0x00FF8800);
        sh_puts("\n1. Run an LLM right here:\n");
        sh_puts_color("       chat Tell me a short story\n", 0x0000FF88);
        sh_puts("\n2. RAG with your own context:\n");
        sh_puts_color("       rag Albert was born in Ulm in 1879. ::: Where was Albert born?\n", 0x0000FF88);
        sh_puts("\n3. Real version control on a real filesystem:\n");
        sh_puts_color("       git init && git add README.md && git commit \"hello\"\n", 0x0000FF88);
        sh_puts_color("       git log\n", 0x0000FF88);
        sh_puts("\n4. Compile + execute C from inside the kernel:\n");
        sh_puts_color("       cc /samples/hello.c -o hello.wasm && exec hello.wasm\n", 0x0000FF88);
        sh_puts("\n5. Mount any disk image from a URL — 12 filesystems supported:\n");
        sh_puts_color("       mount-fs iso https://example.com/disc.iso\n", 0x0000FF88);
        sh_puts_color("       ls /iso/   cat /iso/README\n", 0x0000FF88);
        sh_puts("\n6. Talk to Anthropic's Claude API directly:\n");
        sh_puts_color("       apikey sk-ant-...   # one time\n", 0x0000FF88);
        sh_puts_color("       claude              # multi-turn REPL\n", 0x0000FF88);
        sh_puts("\n7. Open WebSocket to anywhere CORS-permitting:\n");
        sh_puts_color("       ws open wss://echo.websocket.events main\n", 0x0000FF88);
        sh_puts_color("       ws send main hello   ws recv main\n", 0x0000FF88);
        sh_puts("\n8. Time anything you run:\n");
        sh_puts_color("       time bench 32        info\n", 0x0000FF88);
        sh_puts_color("\nEverything you do persists across page reloads (IndexedDB).\n", 0x00888888);
        sh_puts_color("Press up-arrow to recall previous commands.\n", 0x00888888);
        sh_puts_color("`help` lists all 121+ builtins.\n\n", 0x00888888);
    } else if (strcmp(cmd, "stress") == 0) {
        /* Quick smoke test of the major bridges. Does NOT touch network
         * unless you pass `stress net` explicitly. */
        sh_puts_color("\n=== stress test ===\n", 0x00FF8800);
        bool with_net = (argc >= 2 && strcmp(argv[1], "net") == 0);

        /* 1. FS round-trip */
        sh_puts("[1/5] osfs2 create+read+verify... ");
        const char *p = "stress-test-payload";
        int plen = 0; while (p[plen]) plen++;
        extern void *osfs2_find(const char *);
        extern void *osfs2_create(const char *, uint64_t);
        extern int   osfs2_write(void *, uint64_t, const void *, uint64_t);
        extern int   osfs2_read (void *, uint64_t, void *, uint64_t);
        void *f = osfs2_find(".stress");
        if (!f) f = osfs2_create(".stress", 64);
        if (f && osfs2_write(f, 0, p, plen) >= 0) {
            char rb[64];
            if (osfs2_read(f, 0, rb, plen) >= 0 && rb[0] == 's')
                sh_puts_color("ok\n", 0x0000FF00);
            else sh_puts_color("FAIL (read)\n", 0x00FF0000);
        } else sh_puts_color("FAIL (create/write)\n", 0x00FF0000);

        /* 2. Crypto */
        sh_puts("[2/5] sha256... ");
        extern void sha256(const void *, uint32_t, uint8_t[32]);
        uint8_t d[32];
        sha256("hello", 5, d);
        /* 'hello' SHA-256 starts with 0x2cf24dba */
        if (d[0] == 0x2c && d[1] == 0xf2 && d[2] == 0x4d && d[3] == 0xba)
            sh_puts_color("ok\n", 0x0000FF00);
        else sh_puts_color("FAIL\n", 0x00FF0000);

        /* 3. Inference */
        sh_puts("[3/5] llama_chat... ");
        if (prompt_llama) {
            int g = llama_chat(prompt_llama, "hi", 8, NULL, NULL);
            if (g > 0) {
                sh_puts_color("ok (", 0x0000FF00);
                sh_putdec((uint64_t)g); sh_puts(" tok)\n");
            } else sh_puts_color("FAIL (gen=0)\n", 0x00FF0000);
        } else sh_puts_color("SKIP (no model)\n", 0x00888888);

        /* 4. Git */
        sh_puts("[4/5] git status... ");
        git_status();
        sh_puts_color("[git] section above\n", 0x0000FF00);

        /* 5. Network bridge (only if requested) */
        sh_puts("[5/5] network... ");
#ifdef __EMSCRIPTEN__
        if (with_net) {
            uint8_t *body = NULL; int len = 0;
            int status = wasm_http_request("https://1.1.1.1/cdn-cgi/trace",
                                            "GET", "{}", NULL, &body, &len);
            if (status > 0 && status / 100 == 2) {
                sh_puts_color("ok (HTTP ", 0x0000FF00);
                sh_putdec((uint64_t)status); sh_puts(", ");
                sh_putdec((uint64_t)len); sh_puts(" bytes)\n");
            } else sh_puts_color("FAIL\n", 0x00FF0000);
            if (body) free(body);
        } else {
            sh_puts_color("SKIP (run `stress net` to include)\n", 0x00888888);
        }
#else
        (void)with_net;
        sh_puts_color("SKIP (native build)\n", 0x00888888);
#endif
        sh_puts_color("=== done ===\n\n", 0x00FF8800);
    } else if (strcmp(cmd, "wgpu") == 0) {
#ifdef __EMSCRIPTEN__
        extern int wasm_wgpu_init(void);
        extern int wasm_wgpu_error(char *dst, int max);
        extern int wasm_wgpu_matvec(const float *w, const float *in, float *out,
                                     int rows, int cols);
        if (argc < 2 || strcmp(argv[1], "init") == 0) {
            sh_puts("[wgpu] requesting adapter + compiling shader...\n");
            int ok = wasm_wgpu_init();
            if (ok) {
                sh_puts_color("[wgpu] ready ✓\n", 0x0000FF00);
            } else {
                char err[256];
                wasm_wgpu_error(err, sizeof(err));
                sh_puts_color("[wgpu] not available: ", 0x00FF0000);
                sh_puts(err); sh_puts("\n");
            }
        } else if (strcmp(argv[1], "test") == 0) {
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            /* 256x256 random matvec, compare CPU vs GPU */
            int N = 256;
            extern void *malloc(unsigned long);
            extern void free(void *);
            float *w = (float *)malloc((size_t)N * N * 4);
            float *in = (float *)malloc((size_t)N * 4);
            float *out_cpu = (float *)malloc((size_t)N * 4);
            float *out_gpu = (float *)malloc((size_t)N * 4);
            if (!w || !in || !out_cpu || !out_gpu) {
                sh_puts("OOM\n");
                if (w) free(w); if (in) free(in);
                if (out_cpu) free(out_cpu); if (out_gpu) free(out_gpu);
                return;
            }
            for (int i = 0; i < N * N; i++) w[i]  = (float)((i * 7 % 13) - 6) * 0.1f;
            for (int i = 0; i < N;     i++) in[i] = (float)((i * 5 % 11) - 5) * 0.1f;

            extern uint64_t idt_get_ticks(void);
            uint64_t t0 = idt_get_ticks();
            for (int r = 0; r < N; r++) {
                float s = 0.0f;
                for (int c = 0; c < N; c++) s += w[r * N + c] * in[c];
                out_cpu[r] = s;
            }
            uint64_t t1 = idt_get_ticks();

            uint64_t t2 = idt_get_ticks();
            int rc = wasm_wgpu_matvec(w, in, out_gpu, N, N);
            uint64_t t3 = idt_get_ticks();

            float maxdiff = 0.0f;
            for (int i = 0; i < N; i++) {
                float d = out_cpu[i] - out_gpu[i];
                if (d < 0) d = -d;
                if (d > maxdiff) maxdiff = d;
            }

            sh_puts("CPU:  "); sh_putdec(t1 - t0); sh_puts(" ms\n");
            sh_puts("GPU:  "); sh_putdec(t3 - t2); sh_puts(" ms (rc=");
            sh_putdec(rc + 1000); sh_puts(", maxdiff*1000=");
            sh_putdec((uint64_t)(maxdiff * 1000.0f)); sh_puts(")\n");

            free(w); free(in); free(out_cpu); free(out_gpu);
        } else if (strcmp(argv[1], "test-h") == 0) {
            /* Handle-based matvec: upload weights+input once, dispatch
             * many matvecs without copy-out, download final once. The
             * holistic ideal: activations live in VRAM. */
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            extern int wgpu_alloc(int);
            extern void wgpu_free(int);
            extern void wgpu_upload(int, const float *, int);
            extern void wgpu_download(int, float *, int);
            extern int wasm_wgpu_matvec_h(int hw, int hi, int ho, int r, int c);
            extern void *malloc(unsigned long); extern void free(void *);
            int N = 256;
            float *w = (float *)malloc(N*N*4);
            float *in = (float *)malloc(N*4);
            float *cpu = (float *)malloc(N*4);
            float *gpu = (float *)malloc(N*4);
            for (int i = 0; i < N*N; i++) w[i] = (float)((i*7 % 13) - 6) * 0.1f;
            for (int i = 0; i < N; i++)   in[i] = (float)((i*5 % 11) - 5) * 0.1f;
            for (int r = 0; r < N; r++) {
                float s = 0;
                for (int c = 0; c < N; c++) s += w[r*N+c] * in[c];
                cpu[r] = s;
            }
            int hw = wgpu_alloc(N*N);
            int hi = wgpu_alloc(N);
            int ho = wgpu_alloc(N);
            wgpu_upload(hw, w, N*N);
            wgpu_upload(hi, in, N);
            extern uint64_t idt_get_ticks(void);
            uint64_t t0 = idt_get_ticks();
            for (int k = 0; k < 50; k++)
                wasm_wgpu_matvec_h(hw, hi, ho, N, N);
            uint64_t t1 = idt_get_ticks();
            wgpu_download(ho, gpu, N);
            float md = 0.0f;
            for (int i = 0; i < N; i++) {
                float d = cpu[i] - gpu[i]; if (d < 0) d = -d;
                if (d > md) md = d;
            }
            sh_puts("[wgpu test-h] 50x ");
            sh_putdec(N); sh_puts("x"); sh_putdec(N);
            sh_puts(" matvec via handles: ");
            sh_putdec(t1 - t0); sh_puts(" ms total, maxdiff*1e6=");
            sh_putdec((uint64_t)(md * 1e6f)); sh_puts("\n");
            wgpu_free(hw); wgpu_free(hi); wgpu_free(ho);
            free(w); free(in); free(cpu); free(gpu);
        } else if (strcmp(argv[1], "fattn") == 0) {
            /* Fused attention block end-to-end smoke test: pos=0,
             * brandon-tiny shapes. Compare GPU output against a
             * CPU reference implementing the same QKV+RoPE+attn+O
             * pipeline. */
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            extern int wasm_wgpu_kvcache_alloc(int layer, int max_seq, int kv_dim);
            extern int wasm_wgpu_fused_attn(int layer,
                const float *x, const float *wq, const float *wk,
                const float *wv, const float *wo, float *out,
                int dim, int kv_dim, int head_dim, int n_heads, int n_kv_heads,
                int gqa_ratio, int pos, int max_seq, float scale, float rope_base);
            extern void *malloc(unsigned long); extern void free(void *);
            extern double sqrt(double); extern double cos(double); extern double sin(double);
            extern double pow(double, double); extern double exp(double);
            int dim = 128, kv_dim = 64, head_dim = 4;
            int n_heads = 32, n_kv_heads = 16, gqa_ratio = 2;
            int max_seq = 16, pos = 0;
            float scale = 1.0f / (float)sqrt((double)head_dim);
            float rope_base = 10000.0f;
            wasm_wgpu_kvcache_alloc(0, max_seq, kv_dim);
            float *x  = (float*)malloc(dim*4);
            float *wq = (float*)malloc(dim*dim*4);
            float *wk = (float*)malloc(kv_dim*dim*4);
            float *wv = (float*)malloc(kv_dim*dim*4);
            float *wo = (float*)malloc(dim*dim*4);
            float *gpu = (float*)malloc(dim*4);
            float *cpu = (float*)malloc(dim*4);
            float *q  = (float*)malloc(dim*4);
            float *k  = (float*)malloc(kv_dim*4);
            float *v  = (float*)malloc(kv_dim*4);
            float *attn_out = (float*)malloc(dim*4);
            for (int i = 0; i < dim; i++) x[i] = (float)((i*5%11)-5)*0.07f;
            for (int i = 0; i < dim*dim; i++) wq[i] = (float)((i*7%13)-6)*0.03f;
            for (int i = 0; i < dim*dim; i++) wo[i] = (float)((i*11%17)-8)*0.03f;
            for (int i = 0; i < kv_dim*dim; i++) wk[i] = (float)((i*3%9)-4)*0.03f;
            for (int i = 0; i < kv_dim*dim; i++) wv[i] = (float)((i*13%19)-9)*0.03f;
            /* CPU reference */
            for (int r = 0; r < dim; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wq[r*dim+c]*x[c];
                q[r] = s;
            }
            for (int r = 0; r < kv_dim; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wk[r*dim+c]*x[c];
                k[r] = s;
            }
            for (int r = 0; r < kv_dim; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wv[r*dim+c]*x[c];
                v[r] = s;
            }
            /* RoPE on Q and K */
            for (int h = 0; h < n_heads; h++) {
                for (int p = 0; p < head_dim/2; p++) {
                    float exponent = (float)(2*p) / (float)head_dim;
                    float freq = (float)pow((double)rope_base, -(double)exponent);
                    float theta = (float)pos * freq;
                    float c = (float)cos((double)theta), s = (float)sin((double)theta);
                    int idx = h*head_dim + 2*p;
                    float x0 = q[idx], x1 = q[idx+1];
                    q[idx]   = x0*c - x1*s;
                    q[idx+1] = x0*s + x1*c;
                }
            }
            for (int h = 0; h < n_kv_heads; h++) {
                for (int p = 0; p < head_dim/2; p++) {
                    float exponent = (float)(2*p) / (float)head_dim;
                    float freq = (float)pow((double)rope_base, -(double)exponent);
                    float theta = (float)pos * freq;
                    float c = (float)cos((double)theta), s = (float)sin((double)theta);
                    int idx = h*head_dim + 2*p;
                    float x0 = k[idx], x1 = k[idx+1];
                    k[idx]   = x0*c - x1*s;
                    k[idx+1] = x0*s + x1*c;
                }
            }
            /* Attention at pos=0: only one position, softmax = 1.0, out = v_h */
            for (int h = 0; h < n_heads; h++) {
                int kv_h = h / gqa_ratio;
                for (int d = 0; d < head_dim; d++)
                    attn_out[h*head_dim + d] = v[kv_h*head_dim + d];
            }
            /* Output projection */
            for (int r = 0; r < dim; r++) {
                float s = 0;
                for (int c = 0; c < dim; c++) s += wo[r*dim+c]*attn_out[c];
                cpu[r] = s;
            }
            int rc = wasm_wgpu_fused_attn(0, x, wq, wk, wv, wo, gpu,
                dim, kv_dim, head_dim, n_heads, n_kv_heads, gqa_ratio,
                pos, max_seq, scale, rope_base);
            float md = 0;
            for (int i = 0; i < dim; i++) {
                float d = cpu[i] - gpu[i]; if (d < 0) d = -d;
                if (d > md) md = d;
            }
            sh_puts("[wgpu fattn] dim="); sh_putdec(dim);
            sh_puts(" pos="); sh_putdec(pos);
            sh_puts(" rc="); sh_putdec(rc + 1000);
            sh_puts(" maxdiff*1e6="); sh_putdec((uint64_t)(md*1e6f));
            sh_puts("\n");
            free(x); free(wq); free(wk); free(wv); free(wo);
            free(gpu); free(cpu); free(q); free(k); free(v); free(attn_out);
        } else if (strcmp(argv[1], "q4k") == 0) {
            /* Unit test: construct a Q4_K block with known scale/min/quants
             * and dequant via matvec on a one-hot input. Compare expected
             * vs computed values. */
            extern void matvec_q4_k_scalar(float *, const void *, const float *,
                                            uint32_t, uint32_t);
            extern void *malloc(unsigned long); extern void free(void *);
            /* Construct one 144-byte Q4_K block:
             *   d=1.0 (fp16 0x3C00), dmin=0.0 (0x0000)
             *   scales[0]=3 (scale for sub-block 0), scales[4]=0 (min)
             *   scales[1..3]=0, scales[5..11]=0
             *   qs[0..15]=0x21 (low nibble=1, high nibble=2 → sub-blk 0/1)
             *   qs[16..127]=0
             * Expected dequant of sub-block 0 (first 32 elems): 1.0 * 3 * 1 = 3.0
             *   for positions 0..15, then 0*3*0 = 0 for 16..31 (since qs[16..]=0)
             * Sub-block 1 (positions 32..63): 1.0 * 0 * 2 = 0 (scale 0) ... */
            uint8_t block[144] = {0};
            block[0] = 0x00; block[1] = 0x3C;  /* d=1.0 fp16 LE */
            block[2] = 0x00; block[3] = 0x00;  /* dmin=0.0 */
            block[4] = 3;                       /* scales[0]: scale_0=3 */
            block[8] = 0;                       /* scales[4]: min_0=0 */
            for (int i = 0; i < 16; i++) block[16 + i] = 0x21;  /* nib lo=1 hi=2 */
            /* One-hot input: vector of 256 floats, all zero except pos[0]=1 */
            float *inp = (float *)malloc(256 * 4);
            for (int i = 0; i < 256; i++) inp[i] = 0.0f;
            inp[0] = 1.0f;
            float out;
            /* rows=1, cols=256 → out is one float */
            matvec_q4_k_scalar(&out, block, inp, 1, 256);
            /* Expected: only inp[0] contributes. Block: low nibble qs[0]=1.
             * sub-block 0 scale=3, min=0, d=1.0, dmin=0.0
             * value = d * scale_0 * 1 - dmin * 0 = 1.0 * 3 * 1 - 0 = 3.0 */
            sh_puts("[q4k test] expected=3.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            /* Test 2: inp[16]=1.0 → sub-block 1 (high nibble), but scales[1]=0
             *   so scale_1=0 → value = 1.0 * 0 * 2 = 0. */
            for (int i = 0; i < 256; i++) inp[i] = 0.0f;
            inp[16] = 1.0f;
            matvec_q4_k_scalar(&out, block, inp, 1, 256);
            sh_puts("[q4k test2] expected=0.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            /* Test 3: same as test 1 but with scales[1]=5 to verify high nibble */
            block[5] = 5;
            for (int i = 0; i < 256; i++) inp[i] = 0.0f;
            inp[32] = 1.0f;  /* Position 32 reads qs[0] high nibble (2) with scale_1=5 */
            matvec_q4_k_scalar(&out, block, inp, 1, 256);
            sh_puts("[q4k test3] expected=10.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            free(inp);
            /* Test 4: 2 blocks (cols=512), input one-hot at pos 256
             * (= sub-block 0 of block 2). Block 2 has d=2.0, scale[0]=7,
             * qs[0] low nib=3 → value=2*7*3=42. */
            uint8_t big[288] = {0};
            /* Block 0: all zero (no quants → 0 output) */
            big[0] = 0x00; big[1] = 0x3C;  /* d=1.0 */
            /* Block 1: at offset 144 */
            big[144] = 0x00; big[145] = 0x40;  /* d=2.0 (fp16 0x4000) */
            big[148] = 7;                      /* scales[0]=7 */
            big[160] = 0x03;                   /* qs[0] low=3 hi=0 */
            float *inp2 = (float *)malloc(512 * 4);
            for (int i = 0; i < 512; i++) inp2[i] = 0.0f;
            inp2[256] = 1.0f;
            matvec_q4_k_scalar(&out, big, inp2, 1, 512);
            sh_puts("[q4k test4 multi-block] expected=42.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            /* Test 5: 2 rows (rows=2, cols=256). Row 0 same as test 1
             * (out[0]=3.0), row 1 has scales[0]=4 → out[1]=4.0. */
            uint8_t two_row[288] = {0};
            /* Row 0 */
            two_row[0] = 0x00; two_row[1] = 0x3C;
            two_row[4] = 3;
            for (int i = 0; i < 16; i++) two_row[16 + i] = 0x21;
            /* Row 1 at offset 144 */
            two_row[144] = 0x00; two_row[145] = 0x3C;
            two_row[148] = 4;  /* scales[0]=4 */
            for (int i = 0; i < 16; i++) two_row[160 + i] = 0x21;
            float out2r[2];
            for (int i = 0; i < 256; i++) inp2[i] = 0.0f;
            inp2[0] = 1.0f;
            matvec_q4_k_scalar(out2r, two_row, inp2, 2, 256);
            sh_puts("[q4k test5 multi-row] expected=3.0/4.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out2r[0] * 1000.0f));
            sh_puts("/");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out2r[1] * 1000.0f));
            sh_puts("\n");
            free(inp2);
            /* Test 6: dmin != 0 — the previous tests used dmin=0 which
             * masks any bug in the dmin*min subtraction. Set d=1, dmin=1,
             * scales[0]=3 (scale), scales[4]=2 (min), qs[0] low=5.
             * Expected: d*scale*q - dmin*min = 1*3*5 - 1*2 = 13. */
            uint8_t blk[144] = {0};
            blk[0] = 0x00; blk[1] = 0x3C;   /* d=1.0 */
            blk[2] = 0x00; blk[3] = 0x3C;   /* dmin=1.0 */
            blk[4] = 3;                      /* scales[0]=3 */
            blk[8] = 2;                      /* scales[4]=2 (min) */
            blk[16] = 0x05;                  /* qs[0] low=5 */
            for (int i = 0; i < 256; i++) inp[i] = 0.0f;
            inp[0] = 1.0f;
            matvec_q4_k_scalar(&out, blk, inp, 1, 256);
            sh_puts("[q4k test6 dmin] expected=13.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            free(inp);
            /* Test 7: 3 blocks (cols=768) — ffn_down stride. Block 2
             * (last) has d=1, scale=2, qs[0] low=4. Input one-hot at
             * pos 512 → expected: 1*2*4 = 8. */
            uint8_t big3[432] = {0};
            big3[288+0] = 0x00; big3[288+1] = 0x3C;  /* d=1.0 */
            big3[288+4] = 2;                          /* scales[0]=2 */
            big3[288+16] = 0x04;                      /* qs[0] low=4 */
            float *inp3 = (float *)malloc(768 * 4);
            for (int i = 0; i < 768; i++) inp3[i] = 0.0f;
            inp3[512] = 1.0f;
            float out3;
            matvec_q4_k_scalar(&out3, big3, inp3, 1, 768);
            sh_puts("[q4k test7 3-block] expected=8.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out3 * 1000.0f));
            sh_puts("\n");
            free(inp3);
        } else if (strcmp(argv[1], "shapes") == 0) {
            if (!prompt_llama) { sh_puts("No model loaded.\n"); return; }
            extern void llama_debug_dump_shapes(void *);
            extern void llama_debug_dump_row(void *);
            llama_debug_dump_shapes(prompt_llama);
            llama_debug_dump_row(prompt_llama);
        } else if (strcmp(argv[1], "embed-diff") == 0) {
            if (!prompt_llama) { sh_puts("No model loaded.\n"); return; }
            extern void llama_debug_embed(void *, uint32_t, float *);
            extern void llama_debug_matvec_row(void *, uint32_t, float *);
            uint32_t token = 5;
            if (argc >= 3) {
                uint32_t n = 0;
                for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
                    n = n * 10 + (*p - '0');
                token = n;
            }
            float a[8], b[8];
            llama_debug_embed(prompt_llama, token, a);
            llama_debug_matvec_row(prompt_llama, token, b);
            sh_puts("[embed-diff] token="); sh_putdec((uint64_t)token);
            sh_puts("\n  embed_token first8 (*1k):");
            for (int i = 0; i < 8; i++) {
                sh_puts(" ");
                sh_putdec((uint64_t)(uint32_t)(int32_t)(a[i] * 1000.0f));
            }
            sh_puts("\n  matvec_row first8 (*1k):");
            for (int i = 0; i < 8; i++) {
                sh_puts(" ");
                sh_putdec((uint64_t)(uint32_t)(int32_t)(b[i] * 1000.0f));
            }
            sh_puts("\n  diff*1k:");
            float md = 0;
            for (int i = 0; i < 8; i++) {
                float d = a[i] - b[i]; if (d < 0) d = -d;
                if (d > md) md = d;
                sh_puts(" ");
                sh_putdec((uint64_t)(uint32_t)(int32_t)((a[i]-b[i]) * 1000.0f));
            }
            sh_puts("\n  maxdiff*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(md * 1000.0f));
            sh_puts("\n");
        } else if (strcmp(argv[1], "q6k") == 0) {
            extern void matvec_q6_k_scalar(float *, const void *, const float *,
                                            uint32_t, uint32_t);
            extern void *malloc(unsigned long); extern void free(void *);
            /* Q6_K block (210 bytes):
             *   ql[128]: low 4 bits of each 6-bit quant
             *   qh[64]:  high 2 bits packed 4-per-byte
             *   sc[16]:  int8 scales per 16-element sub-block
             *   d:       fp16 (offset 208)
             *
             * For sub-block 0 (elements 0..15) the layout maps:
             *   q[0..31] uses ql[0..31].low + qh[0..31].bits[0..1], scale sc[0]
             *
             * Per llama.cpp: q1 = (ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4) - 32
             * For l=0: ql[0] low=5, qh[0] bits[0..1]=2 → q1 = (5 | (2<<4)) - 32 = 37-32 = 5
             * value = d * sc[0] * 5 */
            uint8_t blk[210] = {0};
            blk[208] = 0x00; blk[209] = 0x3C;  /* d=1.0 fp16 */
            blk[192 + 0] = 4;                  /* sc[0] = 4 */
            blk[0]   = 0x05;                   /* ql[0] = 0x05 → low=5 */
            blk[128] = 0x02;                   /* qh[0] = 0x02 → low 2 bits = 2 */
            /* So q1 = (5 | (2 << 4)) - 32 = 37-32 = 5 */
            /* Expected: 1.0 * 4 * 5 = 20.0 */
            float *inp = (float *)malloc(256 * 4);
            for (int i = 0; i < 256; i++) inp[i] = 0.0f;
            inp[0] = 1.0f;
            float out;
            matvec_q6_k_scalar(&out, blk, inp, 1, 256);
            sh_puts("[q6k test1] expected=20.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out * 1000.0f));
            sh_puts("\n");
            free(inp);
            /* Test 2: 8 Q6_K blocks (cols=2048) — attn_v stride for Llama 1B.
             * Last block (block 7) at offset 7*210=1470. d=1, sc[0]=3, ql[0] low=2, qh[0]=0
             * → q1 = (2 | (0 << 4)) - 32 = -30. value = 1 * 3 * (-30) = -90.
             * Input one-hot at pos 7*256 = 1792 → expected -90.0 */
            uint8_t big8[8*210] = {0};
            big8[7*210 + 208] = 0x00; big8[7*210 + 209] = 0x3C;  /* d=1.0 */
            big8[7*210 + 192 + 0] = 3;                            /* sc[0]=3 */
            big8[7*210 + 0]   = 0x02;                             /* ql[0] low=2 */
            big8[7*210 + 128] = 0x00;                             /* qh[0]=0 */
            float *inp2 = (float *)malloc(2048 * 4);
            for (int i = 0; i < 2048; i++) inp2[i] = 0.0f;
            inp2[1792] = 1.0f;
            float out2;
            extern void matvec_q6_k_scalar(float *, const void *, const float *,
                                            uint32_t, uint32_t);
            matvec_q6_k_scalar(&out2, big8, inp2, 1, 2048);
            sh_puts("[q6k test2 8-block] expected=-90.0  got*1k=");
            sh_putdec((uint64_t)(uint32_t)(int32_t)(out2 * 1000.0f));
            sh_puts("\n");
            free(inp2);
        } else if (strcmp(argv[1], "qkv") == 0) {
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            extern int wasm_wgpu_qkv(const float *x, const float *wq,
                const float *wk, const float *wv,
                float *outq, float *outk, float *outv,
                int dim, int q_rows, int kv_rows);
            extern void *malloc(unsigned long); extern void free(void *);
            int dim = 128, q_rows = 128, kv_rows = 64;
            float *x = (float*)malloc(dim*4);
            float *wq = (float*)malloc(q_rows*dim*4);
            float *wk = (float*)malloc(kv_rows*dim*4);
            float *wv = (float*)malloc(kv_rows*dim*4);
            float *gq = (float*)malloc(q_rows*4);
            float *gk = (float*)malloc(kv_rows*4);
            float *gv = (float*)malloc(kv_rows*4);
            float *cq = (float*)malloc(q_rows*4);
            float *ck = (float*)malloc(kv_rows*4);
            float *cv = (float*)malloc(kv_rows*4);
            for (int i = 0; i < dim; i++) x[i] = (float)((i*5%11)-5)*0.1f;
            for (int i = 0; i < q_rows*dim; i++)  wq[i] = (float)((i*7%13)-6)*0.05f;
            for (int i = 0; i < kv_rows*dim; i++) wk[i] = (float)((i*3%9)-4)*0.05f;
            for (int i = 0; i < kv_rows*dim; i++) wv[i] = (float)((i*11%17)-8)*0.05f;
            /* CPU reference */
            for (int r = 0; r < q_rows; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wq[r*dim+c]*x[c];
                cq[r] = s;
            }
            for (int r = 0; r < kv_rows; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wk[r*dim+c]*x[c];
                ck[r] = s;
            }
            for (int r = 0; r < kv_rows; r++) {
                float s = 0; for (int c = 0; c < dim; c++) s += wv[r*dim+c]*x[c];
                cv[r] = s;
            }
            int rc = wasm_wgpu_qkv(x, wq, wk, wv, gq, gk, gv, dim, q_rows, kv_rows);
            float md = 0;
            for (int i = 0; i < q_rows; i++) {
                float d = cq[i] - gq[i]; if (d < 0) d = -d; if (d > md) md = d;
            }
            for (int i = 0; i < kv_rows; i++) {
                float d = ck[i] - gk[i]; if (d < 0) d = -d; if (d > md) md = d;
                d = cv[i] - gv[i]; if (d < 0) d = -d; if (d > md) md = d;
            }
            sh_puts("[wgpu qkv] dim="); sh_putdec(dim);
            sh_puts(" q="); sh_putdec(q_rows);
            sh_puts(" kv="); sh_putdec(kv_rows);
            sh_puts(" rc="); sh_putdec(rc + 1000);
            sh_puts(" maxdiff*1e6="); sh_putdec((uint64_t)(md*1e6f));
            sh_puts("\n");
            free(x); free(wq); free(wk); free(wv);
            free(gq); free(gk); free(gv);
            free(cq); free(ck); free(cv);
        } else if (strcmp(argv[1], "rmsnorm") == 0) {
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            extern int wasm_wgpu_rmsnorm(const float *w, const float *in,
                                          float *out, int dim, float eps);
            extern void *malloc(unsigned long); extern void free(void *);
            int N = 256;
            float *w = (float *)malloc(N * 4);
            float *in = (float *)malloc(N * 4);
            float *gpu = (float *)malloc(N * 4);
            float *cpu = (float *)malloc(N * 4);
            for (int i = 0; i < N; i++) {
                w[i]  = 1.0f + ((i * 3 % 7) - 3) * 0.05f;
                in[i] = ((i * 11 % 17) - 8) * 0.13f;
            }
            extern double sqrt(double);
            float ssq = 0.0f;
            for (int i = 0; i < N; i++) ssq += in[i] * in[i];
            float rms = 1.0f / (float)sqrt(ssq / N + 1e-5f);
            for (int i = 0; i < N; i++) cpu[i] = in[i] * rms * w[i];
            int rc = wasm_wgpu_rmsnorm(w, in, gpu, N, 1e-5f);
            float md = 0.0f;
            for (int i = 0; i < N; i++) {
                float d = cpu[i] - gpu[i]; if (d < 0) d = -d;
                if (d > md) md = d;
            }
            sh_puts("[wgpu rmsnorm] N="); sh_putdec(N);
            sh_puts(" rc="); sh_putdec(rc + 1000);
            sh_puts(" maxdiff*1e6="); sh_putdec((uint64_t)(md * 1e6f));
            sh_puts("\n");
            free(w); free(in); free(gpu); free(cpu);
        } else if (strcmp(argv[1], "softmax") == 0) {
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            extern int wasm_wgpu_softmax(const float *in, float *out, int len);
            extern void *malloc(unsigned long); extern void free(void *);
            extern double exp(double);
            int N = 256;
            float *in = (float *)malloc(N * 4);
            float *gpu = (float *)malloc(N * 4);
            float *cpu = (float *)malloc(N * 4);
            for (int i = 0; i < N; i++)
                in[i] = ((i * 7 % 19) - 9) * 0.21f;
            float mx = in[0];
            for (int i = 1; i < N; i++) if (in[i] > mx) mx = in[i];
            float s = 0.0f;
            for (int i = 0; i < N; i++) { cpu[i] = (float)exp(in[i] - mx); s += cpu[i]; }
            for (int i = 0; i < N; i++) cpu[i] /= s;
            int rc = wasm_wgpu_softmax(in, gpu, N);
            float md = 0.0f, gsum = 0.0f;
            for (int i = 0; i < N; i++) {
                float d = cpu[i] - gpu[i]; if (d < 0) d = -d;
                if (d > md) md = d;
                gsum += gpu[i];
            }
            sh_puts("[wgpu softmax] N="); sh_putdec(N);
            sh_puts(" rc="); sh_putdec(rc + 1000);
            sh_puts(" maxdiff*1e6="); sh_putdec((uint64_t)(md * 1e6f));
            sh_puts(" gpu_sum*1000="); sh_putdec((uint64_t)(gsum * 1000.0f));
            sh_puts(" (should be ~1000)\n");
            free(in); free(gpu); free(cpu);
        } else if (strcmp(argv[1], "bench") == 0) {
            /* Repeat the same shape N times to exercise the buffer
             * cache: first call pays createBuffer, the rest only
             * writeBuffer + dispatch. Reports total + per-call avg. */
            if (!wasm_wgpu_init()) { sh_puts("WebGPU not initialized.\n"); return; }
            int N = 512, ITERS = 50;
            extern void *malloc(unsigned long);
            extern void free(void *);
            float *w = (float *)malloc((size_t)N * N * 4);
            float *in = (float *)malloc((size_t)N * 4);
            float *out = (float *)malloc((size_t)N * 4);
            if (!w || !in || !out) {
                sh_puts("OOM\n");
                if (w) free(w); if (in) free(in); if (out) free(out);
                return;
            }
            for (int i = 0; i < N * N; i++) w[i]  = (float)((i * 7 % 13) - 6) * 0.1f;
            for (int i = 0; i < N;     i++) in[i] = (float)((i * 5 % 11) - 5) * 0.1f;
            extern uint64_t idt_get_ticks(void);
            /* Warm-up call (fills cache) — timed separately. */
            uint64_t t0 = idt_get_ticks();
            wasm_wgpu_matvec(w, in, out, N, N);
            uint64_t t1 = idt_get_ticks();
            for (int k = 0; k < ITERS; k++) wasm_wgpu_matvec(w, in, out, N, N);
            uint64_t t2 = idt_get_ticks();
            sh_puts("[wgpu bench] shape "); sh_putdec(N); sh_puts("x"); sh_putdec(N);
            sh_puts(", iters="); sh_putdec(ITERS); sh_puts("\n");
            sh_puts("  warm-up (createBuffer): "); sh_putdec(t1 - t0); sh_puts(" ms\n");
            sh_puts("  cached avg: ");
            uint64_t avg = (t2 - t1) / ITERS;
            sh_putdec(avg); sh_puts(" ms/call (");
            sh_putdec(t2 - t1); sh_puts(" ms total)\n");
            free(w); free(in); free(out);
        } else {
            sh_puts("Usage: wgpu [init|test|test-h|bench|rmsnorm|softmax|qkv|fattn]\n");
        }
#else
        sh_puts("wgpu: WASM-only\n");
#endif
    } else if (strcmp(cmd, "precache") == 0) {
#ifdef __EMSCRIPTEN__
        sh_puts("[precache] kicking background fetch of clang/lld/sysroot/memfs (~50 MB)...\n");
        sh_puts("           subsequent 'cc' calls will be instant.\n");
        extern void wasm_precache_toolchain(void);
        wasm_precache_toolchain();
#else
        sh_puts("precache: WASM-only\n");
#endif
    } else if (strcmp(cmd, "pkg") == 0) {
#ifdef __EMSCRIPTEN__
        /* Lightweight package manager for the WASM kernel. The catalog
         * is a JSON file at PKG_BASE/index.json listing { name, url,
         * desc }. Subcommands:
         *   pkg list           — fetch + print catalog
         *   pkg install <name> — fetch package and write to /pkg/<name>
         *   pkg search <q>     — substring filter on names + descs */
        extern uint8_t *wasm_url_fetch(const char *url, int *out_size);
        extern void *osfs2_create(const char *name, uint64_t size);
        extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
        const char *PKG_BASE = "https://factory.naranjositos.tech/wasm/pkg";
        const char *sub = (argc >= 2) ? argv[1] : NULL;
        if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0) {
            const char *q = (sub && strcmp(sub, "search") == 0 && argc >= 3) ? argv[2] : NULL;
            char url[256]; strcpy(url, PKG_BASE); strcat(url, "/index.json");
            int sz = 0;
            uint8_t *idx = wasm_url_fetch(url, &sz);
            if (!idx) { sh_puts("pkg: catalog fetch failed (CORS or 404)\n"); }
            else {
                /* Tiny JSON walker: find each {"name":"X","url":"Y","desc":"Z"}.
                 * Not a real parser — just scans for keys. Catalog is trusted. */
                char *p = (char *)idx;
                int matches = 0;
                while ((p = strstr(p, "\"name\""))) {
                    char *nb = strchr(p + 6, '"'); if (!nb) break;
                    char *ne = strchr(nb + 1, '"'); if (!ne) break;
                    char name[64]; int nl = ne-nb-1; if (nl > 63) nl = 63;
                    memcpy(name, nb+1, nl); name[nl] = 0;
                    char *db = strstr(ne, "\"desc\""); char desc[128] = "";
                    if (db) {
                        char *dq = strchr(db + 6, '"');
                        if (dq) {
                            char *de = strchr(dq + 1, '"');
                            if (de) { int dl = de-dq-1; if (dl>127) dl=127; memcpy(desc, dq+1, dl); desc[dl]=0; }
                        }
                    }
                    int show = 1;
                    if (q && q[0]) show = (strstr(name, q) || strstr(desc, q)) ? 1 : 0;
                    if (show) {
                        sh_puts("  "); sh_puts(name);
                        if (desc[0]) { sh_puts(" — "); sh_puts(desc); }
                        sh_puts("\n");
                        matches++;
                    }
                    p = ne + 1;
                }
                if (matches == 0) sh_puts("pkg: no matches\n");
                extern void free(void *); free(idx);
            }
        } else if (strcmp(sub, "installed") == 0) {
            /* Walk the file table for /pkg/* entries. */
            extern void *osfs2_get_file(int index);
            extern const char *osfs2_file_name(void *file);
            extern uint64_t osfs2_file_size(void *file);
            int count = 0;
            for (int i = 0; i < 4096; i++) {  /* OSFS2_MAX_FILES upper bound */
                void *f = osfs2_get_file(i);
                if (!f) continue;
                const char *nm = osfs2_file_name(f);
                if (!nm) continue;
                if (nm[0] != 'p' || nm[1] != 'k' || nm[2] != 'g' || nm[3] != '/')
                    continue;
                sh_puts("  "); sh_puts(nm);
                sh_puts(" ("); sh_putdec(osfs2_file_size(f)); sh_puts(" B)\n");
                count++;
            }
            if (!count) sh_puts("pkg: nothing installed\n");
        } else if (strcmp(sub, "install") == 0) {
            const char *name = (argc >= 3) ? argv[2] : NULL;
            if (!name) { sh_puts("usage: pkg install <name>\n"); }
            else {
                char url[256]; strcpy(url, PKG_BASE); strcat(url, "/"); strcat(url, name); strcat(url, ".wasm");
                sh_puts("[pkg] fetching "); sh_puts(url); sh_puts("\n");
                int sz = 0;
                uint8_t *blob = wasm_url_fetch(url, &sz);
                if (!blob) { sh_puts("pkg: fetch failed\n"); }
                else {
                    char fname[80]; { int n = strlen(name); if (n>60) n=60;
                        memcpy(fname, "/pkg/", 5); memcpy(fname+5, name, n);
                        memcpy(fname+5+n, ".wasm", 6); }
                    /* Strip leading / since osfs2 is flat. Delete prior
                     * version so install is idempotent (acts as upgrade). */
                    extern int osfs2_delete(const char *);
                    osfs2_delete(fname + 1);
                    void *f = osfs2_create(fname + 1, sz);
                    if (!f) { sh_puts("pkg: osfs2_create failed\n"); }
                    else {
                        if (osfs2_write(f, 0, blob, sz) < 0)
                            sh_puts("pkg: osfs2_write failed\n");
                        else {
                            sh_puts("[pkg] installed "); sh_puts(name);
                            sh_puts(" ("); sh_putdec(sz); sh_puts(" bytes) at ");
                            sh_puts(fname); sh_puts("\n");
                            sh_puts("      run with: exec "); sh_puts(fname+1); sh_puts("\n");
                        }
                    }
                    extern void free(void *); free(blob);
                }
            }
        } else if (strcmp(sub, "run") == 0) {
            /* Convenience: install if not already there, then exec. */
            const char *name = (argc >= 3) ? argv[2] : NULL;
            if (!name) { sh_puts("usage: pkg run <name> [args...]\n"); }
            else {
                char fname[80];
                int n = strlen(name); if (n > 60) n = 60;
                memcpy(fname, "pkg/", 4); memcpy(fname + 4, name, n);
                memcpy(fname + 4 + n, ".wasm", 6);
                extern void *osfs2_find(const char *);
                if (!osfs2_find(fname)) {
                    sh_puts("[pkg run] not installed, fetching...\n");
                    char url[256]; strcpy(url, PKG_BASE); strcat(url, "/");
                    strcat(url, name); strcat(url, ".wasm");
                    int sz = 0;
                    uint8_t *blob = wasm_url_fetch(url, &sz);
                    if (!blob) { sh_puts("pkg: fetch failed\n"); }
                    else {
                        void *f = osfs2_create(fname, sz);
                        if (!f || osfs2_write(f, 0, blob, sz) < 0)
                            sh_puts("pkg: install failed\n");
                        extern void free(void *); free(blob);
                    }
                }
                if (osfs2_find(fname)) {
                    extern int proc_exec(const char *filename, int xargc,
                                          const char **xargv);
                    /* argv[0]="pkg" argv[1]="run" argv[2]=name argv[3..] = pkg args */
                    const char *xargv[8] = { fname };
                    int xc = 1;
                    for (int i = 3; i < argc && xc < 7; i++) xargv[xc++] = argv[i];
                    xargv[xc] = NULL;
                    proc_exec(fname, xc, xargv);
                }
            }
        } else if (strcmp(sub, "dev") == 0) {
            /* Install a local .wasm file as a pkg without going through R2.
             * Useful for dogfooding 'cc src.c -o foo.wasm' results.
             *   pkg dev foo /path/to/foo.wasm
             * If the source is /pkg/foo.wasm already, this is a no-op. */
            const char *name = (argc >= 3) ? argv[2] : NULL;
            const char *src  = (argc >= 4) ? argv[3] : NULL;
            if (!name || !src) { sh_puts("usage: pkg dev <name> <src>\n"); }
            else {
                extern void *osfs2_find(const char *);
                void *sf = osfs2_find(src[0] == '/' ? src + 1 : src);
                if (!sf) { sh_puts("pkg dev: source not found\n"); }
                else {
                    extern uint64_t osfs2_file_size(void *);
                    extern int osfs2_read(void *, uint64_t, void *, uint64_t);
                    uint64_t sz = osfs2_file_size(sf);
                    if (sz == 0 || sz > 4 * 1024 * 1024) {
                        sh_puts("pkg dev: bad size\n");
                    } else {
                        extern void *malloc(unsigned long); extern void free(void *);
                        uint8_t *blob = (uint8_t *)malloc(sz);
                        if (blob && osfs2_read(sf, 0, blob, sz) == 0) {
                            char fname[80]; int n = strlen(name); if (n>60) n=60;
                            memcpy(fname, "pkg/", 4); memcpy(fname+4, name, n);
                            memcpy(fname+4+n, ".wasm", 6);
                            extern int osfs2_delete(const char *);
                            osfs2_delete(fname);
                            void *f = osfs2_create(fname, sz);
                            if (f && osfs2_write(f, 0, blob, sz) == 0) {
                                sh_puts("[pkg dev] installed "); sh_puts(name);
                                sh_puts(" ("); sh_putdec(sz); sh_puts(" B) at /");
                                sh_puts(fname); sh_puts("\n");
                            } else sh_puts("pkg dev: write failed\n");
                        } else sh_puts("pkg dev: read failed\n");
                        if (blob) free(blob);
                    }
                }
            }
        } else if (strcmp(sub, "uninstall") == 0 || strcmp(sub, "rm") == 0) {
            const char *name = (argc >= 3) ? argv[2] : NULL;
            if (!name) { sh_puts("usage: pkg uninstall <name>\n"); }
            else {
                char fname[80];
                int nl = strlen(name); if (nl > 60) nl = 60;
                memcpy(fname, "pkg/", 4); memcpy(fname + 4, name, nl);
                memcpy(fname + 4 + nl, ".wasm", 6);
                extern int osfs2_delete(const char *);
                if (osfs2_delete(fname) == 0) {
                    sh_puts("[pkg] uninstalled "); sh_puts(name); sh_puts("\n");
                } else {
                    sh_puts("pkg: not installed: "); sh_puts(name); sh_puts("\n");
                }
            }
        } else if (strcmp(sub, "help") == 0) {
            sh_puts("pkg — fetch and run WASI programs from the OsitoK catalog.\n\n");
            sh_puts("Subcommands:\n");
            sh_puts("  pkg list                  show all packages in the catalog\n");
            sh_puts("  pkg search <query>        substring filter on names + descriptions\n");
            sh_puts("  pkg install <name>        download <name>.wasm to /pkg/<name>.wasm\n");
            sh_puts("  pkg installed             list packages already on local OsitoFS\n");
            sh_puts("  pkg run <name> [args]     install if needed, then exec\n");
            sh_puts("  pkg uninstall <name>      remove local /pkg/<name>.wasm\n");
            sh_puts("  pkg dev <name> <file>     install a local .wasm as a pkg\n");
            sh_puts("\nPackages are tiny WASI binaries (<1 KB) and can be piped:\n");
            sh_puts("  echo hola | pkg run rev   ->  aloh\n");
        } else {
            sh_puts("usage: pkg help | list | search <q> | install <name> | installed | run <name> | uninstall <name>\n");
        }
#else
        sh_puts("pkg: WASM-only\n");
#endif
    } else if (strcmp(cmd, "reload") == 0) {
#ifdef __EMSCRIPTEN__
        sh_puts("[reload] reloading page...\n");
        extern void wasm_reload_page(void);
        extern void wasm_persist_flush_decl(void);
        /* declare locally to avoid conflicting types */
        {
            extern void wasm_persist_flush(void);
            wasm_persist_flush();   /* save FS first */
        }
        wasm_reload_page();
#else
        sh_puts("reload: WASM-only\n");
#endif
    } else if (strcmp(cmd, "date") == 0) {
#ifdef __EMSCRIPTEN__
        extern int wasm_iso_now(char *dst, int max);
        char buf[64];
        if (wasm_iso_now(buf, sizeof(buf)) > 0) {
            sh_puts(buf); sh_puts("\n");
        } else {
            sh_puts("(date unavailable)\n");
        }
#else
        sh_puts("(date: native build doesn't have an RTC bridge)\n");
#endif
    } else if (strcmp(cmd, "whoami") == 0) {
        sh_puts("osito\n");
    } else if (strcmp(cmd, "history") == 0) {
#ifdef __EMSCRIPTEN__
        if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
            extern void wasm_localstorage_set(const char *key, const char *value);
            wasm_localstorage_set("osito-history", "");
            sh_puts("[history] cleared (reload to apply)\n");
        } else {
            sh_puts("History lives in JS / localStorage.\n");
            sh_puts("Use Up/Down arrow keys to recall previous commands.\n");
            sh_puts("`history clear` wipes localStorage entry.\n");
        }
#else
        sh_puts("history: WASM-only\n");
#endif
    } else if (strcmp(cmd, "save") == 0) {
#ifdef __EMSCRIPTEN__
        extern void wasm_persist_flush(void);
        wasm_persist_flush();
        sh_puts("[save] FS image flushed to IndexedDB\n");
#else
        sh_puts("save: no-op outside WASM\n");
#endif
    } else if (strcmp(cmd, "info") == 0) {
        sh_puts_color("\n── OsitoK kernel state ─────────────────────\n", 0x00FF8800);

#ifdef OSITO_GIT_REV
        sh_puts("build:    "); sh_puts(OSITO_GIT_REV);
        sh_puts("  ("); sh_puts(OSITO_BUILD_TS); sh_puts(")\n");
#endif

        /* Model */
        if (prompt_llama) {
            extern int llama_state_dim(void *s);
            extern int llama_state_layers(void *s);
            extern int llama_state_vocab(void *s);
            extern const char *llama_state_arch(void *s);
            sh_puts("model:    arch=");
            sh_puts(llama_state_arch(prompt_llama));
            sh_puts(" dim=");  sh_putdec((uint64_t)llama_state_dim(prompt_llama));
            sh_puts(" layers=");sh_putdec((uint64_t)llama_state_layers(prompt_llama));
            sh_puts(" vocab="); sh_putdec((uint64_t)llama_state_vocab(prompt_llama));
            sh_puts("\n");
        } else {
            sh_puts("model:    (none loaded)\n");
        }

        /* Sampling */
        {
            float r, pr, fq;
            llama_get_penalty(&r, &pr, &fq);
            sh_puts("sampling: penalty rep=");
            sh_putdec((uint64_t)(r * 100.0f) / 100); sh_puts(".");
            sh_putdec((uint64_t)(r * 100.0f) % 100);
            sh_puts(" pres="); sh_putdec((uint64_t)(pr * 100.0f) / 100);
            sh_puts("."); sh_putdec((uint64_t)(pr * 100.0f) % 100);
            sh_puts(" freq="); sh_putdec((uint64_t)(fq * 100.0f) / 100);
            sh_puts("."); sh_putdec((uint64_t)(fq * 100.0f) % 100);
            sh_puts(" ngram="); sh_putdec(llama_get_ngram_size());
            sh_puts("\n");
        }

        /* OsitoFS */
        if (osfs2_is_mounted()) {
            extern uint32_t osfs2_file_count(void);
            extern const char *osfs2_label(void);
            extern uint32_t osfs2_free_blocks(void);
            extern uint32_t osfs2_get_block_size(void);
            sh_puts("fs:       /  \"");
            sh_puts(osfs2_label()); sh_puts("\" — ");
            sh_putdec((uint64_t)osfs2_file_count()); sh_puts(" files, ");
            sh_putdec((uint64_t)osfs2_free_blocks() *
                      (uint64_t)osfs2_get_block_size() / (1024 * 1024));
            sh_puts(" MB free\n");
        }
#ifdef __EMSCRIPTEN__
        /* Aux mounts */
        for (int i = 0; i < AUX_FS_COUNT; i++) {
            if (g_aux_mounts[i]) {
                sh_puts("aux:      /");
                sh_puts(g_aux_mounts[i]->name);
                sh_puts("/   (multi-mount)\n");
            }
        }

        /* WS connections */
        for (int i = 0; i < WS_SLOT_MAX; i++) {
            if (ws_slots[i].handle) {
                int s = wasm_ws_state(ws_slots[i].handle);
                sh_puts("ws:       ");
                sh_puts(ws_slots[i].name);
                sh_puts(" — ");
                sh_puts(s == 0 ? "connecting" :
                        s == 1 ? "open" :
                        s == 2 ? "closing" :
                        s == 3 ? "closed" : "error");
                sh_puts("\n");
            }
        }
#endif
        sh_puts("\n");
    } else if (strcmp(cmd, "time") == 0) {
        if (argc < 2) { sh_puts("Usage: time <command...>\n"); return; }
        /* Reassemble argv[1..] into a single line and run via the
         * pipeline executor. Times the call wall-clock. */
        static char buf[1024];
        int p = 0;
        for (int i = 1; i < argc && p < (int)sizeof(buf) - 1; i++) {
            if (i > 1 && p < (int)sizeof(buf) - 1) buf[p++] = ' ';
            const char *w = argv[i];
            while (*w && p < (int)sizeof(buf) - 1) buf[p++] = *w++;
        }
        buf[p] = '\0';
        extern uint64_t idt_get_ticks(void);
        extern void shell_exec_pipeline(char *line);
        uint64_t t0 = idt_get_ticks();
        shell_exec_pipeline(buf);
        uint64_t t1 = idt_get_ticks();
#ifdef __EMSCRIPTEN__
        /* idt_get_ticks in WASM is emscripten_get_now (ms-resolution). */
        sh_puts_color("\n[time] ", 0x00FF8800);
        sh_putdec(t1 - t0); sh_puts(" ms\n");
#else
        sh_puts_color("\n[time] ", 0x00FF8800);
        sh_putdec((t1 - t0) * 10);  /* native ticks ~10ms */
        sh_puts(" ms (approx)\n");
#endif
    } else if (strcmp(cmd, "benchmark") == 0) {
        sh_puts_color("\n=== OsitoK benchmark ===\n", 0x00FF8800);
        extern uint64_t idt_get_ticks(void);

        /* SHA-256 throughput: 1 MB hashing */
        sh_puts("[1/3] sha256 throughput: ");
        extern void sha256(const void *, uint32_t, uint8_t[32]);
        static uint8_t blob[1024 * 1024];
        for (int i = 0; i < (int)sizeof(blob); i++) blob[i] = (uint8_t)(i & 0xFF);
        uint8_t digest[32];
        uint64_t t0 = idt_get_ticks();
        sha256(blob, sizeof(blob), digest);
        uint64_t t1 = idt_get_ticks();
        uint64_t ms = (t1 - t0);
#ifndef __EMSCRIPTEN__
        ms *= 10;  /* native ticks to ms approx */
#endif
        if (ms == 0) ms = 1;
        sh_putdec(1024 / ms);  /* 1 MB / ms ≈ MB/s */
        sh_puts(" MB/s ("); sh_putdec(ms); sh_puts(" ms / 1 MB)\n");

        /* FS write throughput: 256 KB → /tmp */
        sh_puts("[2/3] osfs2 write: ");
        extern void *osfs2_create(const char *, uint64_t);
        extern void *osfs2_find(const char *);
        extern int   osfs2_write(void *, uint64_t, const void *, uint64_t);
        void *bench_file = osfs2_find(".bench");
        if (!bench_file) bench_file = osfs2_create(".bench", 256 * 1024);
        if (bench_file) {
            uint64_t t2 = idt_get_ticks();
            osfs2_write(bench_file, 0, blob, 256 * 1024);
            uint64_t t3 = idt_get_ticks();
            uint64_t ms2 = (t3 - t2);
#ifndef __EMSCRIPTEN__
            ms2 *= 10;
#endif
            if (ms2 == 0) ms2 = 1;
            sh_putdec(256 / ms2); sh_puts(" MB/s (");
            sh_putdec(ms2); sh_puts(" ms / 256 KB)\n");
        } else {
            sh_puts_color("FAIL\n", 0x00FF0000);
        }

        /* Llama tokens/sec */
        sh_puts("[3/3] llama_chat: ");
        if (prompt_llama) {
            uint64_t t4 = idt_get_ticks();
            int g = llama_chat(prompt_llama, "Once upon a time", 32, NULL, NULL);
            uint64_t t5 = idt_get_ticks();
            uint64_t ms3 = (t5 - t4);
#ifndef __EMSCRIPTEN__
            ms3 *= 10;
#endif
            if (g > 0 && ms3 > 0) {
                sh_putdec((uint64_t)g * 1000 / ms3);
                sh_puts(" tok/s (");
                sh_putdec(ms3 / g); sh_puts(" ms/tok, ");
                sh_putdec((uint64_t)g); sh_puts(" tokens)\n");
            } else sh_puts_color("FAIL\n", 0x00FF0000);
        } else sh_puts_color("SKIP (no model)\n", 0x00888888);

        sh_puts_color("=== done ===\n\n", 0x00FF8800);
    } else if (strcmp(cmd, "bench") == 0) {
        /* Inference micro-bench: prefill a known prompt, time generation. */
        if (!prompt_llama) { sh_puts("No model loaded.\n"); return; }
        int n_tok = 32;
        if (argc >= 2) {
            n_tok = 0;
            const char *s = argv[1];
            while (*s >= '0' && *s <= '9') { n_tok = n_tok * 10 + (*s - '0'); s++; }
            if (n_tok < 4)   n_tok = 4;
            if (n_tok > 256) n_tok = 256;
        }
        sh_puts_color("\n[bench] ", 0x00FF8800);
        sh_putdec((uint64_t)n_tok); sh_puts(" tokens — ");
        const char *prompt = "Once upon a time";
        int gen = llama_chat(prompt_llama, prompt, (uint32_t)n_tok,
                              chat_token_cb, NULL);
        sh_puts("\n");
        if (gen <= 0) sh_puts_color("[bench] no output\n", 0x00FF0000);
    } else if (strcmp(cmd, "ngram") == 0) {
        if (argc < 2) {
            sh_puts("Usage: ngram <size>   (0=off, 3=balanced, 4=strict)\n");
            sh_puts("Current: ");
            sh_putdec(llama_get_ngram_size());
            sh_puts("\n");
        } else {
            int v = 0;
            const char *s = argv[1];
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            llama_set_ngram_size((uint32_t)v);
#ifdef __EMSCRIPTEN__
            {
                char buf[16];
                int n = 0; int x = v;
                if (x == 0) buf[n++] = '0';
                else { char t[8]; int tt = 0; while (x) { t[tt++]='0'+x%10; x/=10; }
                       while (tt) buf[n++]=t[--tt]; }
                buf[n] = '\0';
                wasm_config_save("ngram", buf);
            }
#endif
            sh_puts("ngram size set to ");
            sh_putdec((uint64_t)v);
            sh_puts("\n");
        }
    } else if (strcmp(cmd, "bdebug") == 0) {
        if (argc < 2) {
            sh_puts("Usage: bdebug <dwa|vr|reg|logits|all|none> [0|1]\n");
            sh_puts("  dwa: DenseFormer DWA mixing  vr: value residual\n");
            sh_puts("  reg: register prefill        logits: serial dump top-3 each token\n");
        } else {
            int v = (argc >= 3) ? (argv[2][0] != '0') : 1;
            if (strcmp(argv[1], "dwa") == 0)        brandon_set_features(v, 1, 1);
            else if (strcmp(argv[1], "vr") == 0)    brandon_set_features(1, v, 1);
            else if (strcmp(argv[1], "reg") == 0)   brandon_set_features(1, 1, v);
            else if (strcmp(argv[1], "logits") == 0) brandon_set_debug_logits(v);
            else if (strcmp(argv[1], "all") == 0)   { brandon_set_features(1,1,1); brandon_set_debug_logits(0); }
            else if (strcmp(argv[1], "none") == 0)  { brandon_set_features(0,0,0); brandon_set_debug_logits(0); }
            else if (strcmp(argv[1], "gpu") == 0) {
                extern int g_brandon_use_gpu_matvec;
                g_brandon_use_gpu_matvec = v;
                sh_puts(v ? "[brandon] GPU matvec ENABLED\n"
                          : "[brandon] GPU matvec disabled\n");
            }
            else if (strcmp(argv[1], "attn") == 0) {
                extern int g_brandon_use_gpu_attn;
                g_brandon_use_gpu_attn = v;
                sh_puts(v ? "[brandon] GPU fused-attn ENABLED (F32 + no VR/DWA)\n"
                          : "[brandon] GPU fused-attn disabled\n");
            }
            else if (strcmp(argv[1], "llama_attn") == 0) {
                extern bool g_llama_use_gpu_attn;
                g_llama_use_gpu_attn = (v != 0);
                sh_puts(v ? "[llama] GPU fused-attn ENABLED (F32 attn, Llama 2/3 RoPE)\n"
                          : "[llama] GPU fused-attn disabled\n");
            }
            else if (strcmp(argv[1], "predequant_attn") == 0) {
                /* Pre-dequant Q4_K attn weights → F32 to satisfy the
                 * GPU fused-attn F32 gate. Memory cost: ~640 MB for
                 * Llama 1B. Run AFTER model load, BEFORE first chat. */
#ifdef __EMSCRIPTEN__
                extern void *wasm_get_model(void);
                void *m = wasm_get_model();
                if (!m) { sh_puts("predequant_attn: no model loaded\n"); }
                else {
                    extern int gguf_dequant_q4k_to_f32(void *model, const char *filter);
                    int n = gguf_dequant_q4k_to_f32(m, ".attn_");
                    if (n < 0) sh_puts("[predequant_attn] FAILED (likely OOM)\n");
                    else { sh_puts("[predequant_attn] "); sh_putdec((uint64_t)n);
                           sh_puts(" tensors converted\n"); }
                }
#else
                sh_puts("predequant_attn: WASM-only\n");
#endif
            }
            else sh_puts("bdebug applied\n");
        }
    } else if (strcmp(cmd, "temp") == 0) {
        cmd_temp(argc, argv);
    } else if (strcmp(cmd, "dl") == 0) {
        cmd_dl(argc, argv);
    } else if (strcmp(cmd, "git") == 0) {
        if (argc < 2) {
            sh_puts("Usage: git <init|add|commit|log|status|diff|branch|checkout>\n");
        } else if (strcmp(argv[1], "init") == 0) {
            git_init();
        } else if (strcmp(argv[1], "add") == 0) {
            if (argc < 3) sh_puts("Usage: git add <file>\n");
            else git_add(argv[2]);
        } else if (strcmp(argv[1], "commit") == 0) {
            if (argc < 3) {
                sh_puts("Usage: git commit <message>\n");
            } else {
                /* Join remaining args as message */
                char msg[256];
                int pos = 0;
                for (int i = 2; i < argc && pos < 250; i++) {
                    if (i > 2) msg[pos++] = ' ';
                    const char *w = argv[i];
                    while (*w && pos < 250) msg[pos++] = *w++;
                }
                msg[pos] = '\0';
                git_commit(msg);
            }
        } else if (strcmp(argv[1], "log") == 0) {
            git_log();
        } else if (strcmp(argv[1], "status") == 0) {
            git_status();
        } else if (strcmp(argv[1], "diff") == 0) {
            git_diff();
        } else if (strcmp(argv[1], "branch") == 0) {
            git_branch(argc >= 3 ? argv[2] : NULL);
        } else if (strcmp(argv[1], "checkout") == 0) {
            if (argc < 3) sh_puts("Usage: git checkout <branch>\n");
            else git_checkout(argv[2]);
        } else {
            sh_puts("Unknown git command: ");
            sh_puts(argv[1]);
            sh_puts("\n");
        }
    } else if (strcmp(cmd, "sched") == 0) {
        /* X-SCHED: scheduler test and stats */
        if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
            sh_puts("Scheduler: ");
            sh_puts(sched_is_enabled() ? "active" : "inactive");
            sh_puts("\nContext switches: ");
            sh_putdec(sched_get_switches());
            sh_puts("\n");
        } else {
            /* Spawn two test threads that print alternating characters */
            extern void sched_test_a(void);
            extern void sched_test_b(void);
            sched_spawn("thread_a", sched_test_a);
            sched_spawn("thread_b", sched_test_b);
            sh_puts("Spawned 2 test threads (printing A and B).\n");
            sh_puts("Use 'ps' to see processes, 'sched stats' for switch count.\n");
        }
    } else if (strcmp(cmd, "tensor") == 0) {
        /* Tensor benchmark and info */
        if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
            extern void tensor_benchmark(void);
            tensor_benchmark();
        } else if (argc >= 2 && strcmp(argv[1], "info") == 0) {
            extern void *prompt_llama;
            if (prompt_llama) {
                /* Access llama_state_t fields via known offsets */
                sh_puts("Model loaded, use 'chat' for inference\n");
            } else {
                sh_puts("No model loaded\n");
            }
        } else {
            sh_puts("Usage: tensor bench|info\n");
        }
    } else if (strcmp(cmd, "kprof") == 0) {
        /* Always-on profiling: show RIP histogram */
        extern void kprof_report(void);
        kprof_report();
    } else if (strcmp(cmd, "crashdump") == 0) {
        /* List saved crash reports */
        extern void *osfs2_find(const char *name);
        char fname[32] = "crash_000.bin";
        int found = 0;
        for (int i = 1; i <= 100; i++) {
            fname[6] = '0' + (i / 100) % 10;
            fname[7] = '0' + (i / 10) % 10;
            fname[8] = '0' + i % 10;
            if (osfs2_find(fname)) {
                sh_puts("  ");
                sh_puts(fname);
                sh_puts("\n");
                found++;
            }
        }
        if (!found) sh_puts("No crash reports saved\n");
    } else if (strcmp(cmd, "httpd") == 0) {
        cmd_httpd(argc, argv);
    } else if (strcmp(cmd, "winexec") == 0) {
        if (argc < 2) {
            sh_puts("Usage: winexec <file.exe>\n");
        } else {
            extern int win32_exec(const char *filename);
            extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
            extern uint64_t *compat32_crash_jmpbuf;
            static uint64_t winexec_jmpbuf[9];
            compat32_crash_jmpbuf = winexec_jmpbuf;
            if (kern_setjmp(winexec_jmpbuf) == 0) {
                win32_exec(argv[1]);
            } else {
                sh_puts("\n [WIN32] Process crashed — returned to shell\n");
                /* Restore IST1 after longjmp — the compat32 exception path
                 * bypasses int2e_stub's IST1 restore, leaving it corrupted.
                 * Without this, the next IST1 interrupt loads RSP=0. */
                extern uint64_t *tss_ist1_ptr;
                extern uint8_t ist1_stack[];
                if (tss_ist1_ptr)
                    *tss_ist1_ptr = (uint64_t)(ist1_stack + 262144);
                /* Reset compat32 mode flag */
                extern int g_compat32_mode;
                g_compat32_mode = 0;
            }
            compat32_crash_jmpbuf = NULL;
        }
    } else if (strcmp(cmd, "dosrun") == 0) {
        if (argc < 2) {
            sh_puts("Usage: dosrun <file.com|file.exe>\n");
        } else {
            extern int dos_run(const char *filename, int argc, const char **argv);
            extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
            extern uint64_t *dos_native_exit_jmpbuf;
            extern uint64_t *tss_ist1_ptr;
            extern uint8_t   ist1_stack[];
            static uint64_t dosrun_jmpbuf[9];
            dos_native_exit_jmpbuf = dosrun_jmpbuf;
            int rc = kern_setjmp(dosrun_jmpbuf);
            if (rc == 0) {
                dos_run(argv[1], argc - 1, (const char **)&argv[1]);
            } else {
                sh_puts("\n [DOS] Program exited (");
                sh_puts(rc == 2 ? "crash" : "normal");
                sh_puts(") — returned to shell\n");
                if (tss_ist1_ptr)
                    *tss_ist1_ptr = (uint64_t)(ist1_stack + 262144);
            }
            dos_native_exit_jmpbuf = NULL;
        }
    } else if (strcmp(cmd, "desktop") == 0) {
        /* Launch compositor with elementaryOS desktop */
        extern int  display_init(uint32_t *gop_base, uint32_t w, uint32_t h,
                                 uint32_t pitch, uint32_t fps);
        extern void input_events_init(uint32_t scr_width, uint32_t scr_height);
        extern void shm_init(void);
        extern void compositor_init(void);
        extern void compositor_thread(void);
        extern uint32_t *fb_get_vram(void);
        extern uint32_t  fb_get_width(void);
        extern uint32_t  fb_get_height(void);
        extern uint32_t  fb_get_pitch(void);
        /* Terminal surface API */
        extern void compositor_get_terminal_dims(uint32_t *tw, uint32_t *th);
        extern void compositor_set_terminal_surface(uint32_t shm, uint32_t tw, uint32_t th);
        extern void fb_redirect(uint32_t *new_base, uint32_t tw, uint32_t th, uint32_t pitch);
        extern void fb_set_clear_color(uint32_t color);
        extern void fb_clear(void);
        /* SHM API */
        extern uint32_t  shm_create(uint64_t size, uint32_t flags);
        extern void     *shm_map(uint32_t handle);

        uint32_t *vram = fb_get_vram();
        uint32_t  w    = fb_get_width();
        uint32_t  h    = fb_get_height();
        uint32_t  p    = fb_get_pitch();

        if (display_init(vram, w, h, p, 0) < 0) {
            sh_puts("ERROR: display_init failed\n");
        } else {
            /* Pass GOP mode table from bootloader to display subsystem */
            extern void display_set_available_modes(const boot_display_mode_t *,
                                                    uint32_t, uint32_t);
            if (saved_boot_info.display_mode_count > 0)
                display_set_available_modes(saved_boot_info.display_modes,
                                            saved_boot_info.display_mode_count,
                                            saved_boot_info.display_current_mode);

            /* Try GPU display engine takeover (Phase A: GOP → GPU scanout).
             * On success, display_flip() will use page flips instead of memcpy.
             * Falls back gracefully in QEMU or when no GPU is present. */
            extern int gpu_display_init(uint32_t *gop_fb, uint32_t width,
                                        uint32_t height, uint32_t pitch)
                                        __attribute__((weak));
            extern void display_enable_gpu_scanout(void);
            if (gpu_display_init && gpu_display_init(vram, w, h, p * 4) == 0) {
                display_enable_gpu_scanout();
                sh_puts("GPU display engine active (page flip)\n");

                /* Phase B: detect monitor EDID and modeset to native resolution */
                typedef struct {
                    uint32_t pixel_clock_hz;
                    uint16_t h_active, h_blank, h_sync_offset, h_sync_width;
                    uint16_t v_active, v_blank, v_sync_offset, v_sync_width;
                    uint16_t h_total, v_total;
                    uint32_t refresh_hz;
                    _Bool    interlaced;
                } edid_mode_t;
                extern int gpu_display_detect_monitor(edid_mode_t *mode)
                    __attribute__((weak));
                extern int gpu_display_set_mode(const edid_mode_t *mode,
                    uint64_t fb_addr, uint32_t fb_pitch)
                    __attribute__((weak));
                extern int display_resize(uint32_t nw, uint32_t nh, uint32_t np);
                extern uint32_t *display_get_back_buffer(void);

                edid_mode_t native;
                if (gpu_display_detect_monitor &&
                    gpu_display_detect_monitor(&native) == 0 &&
                    (native.h_active != w || native.v_active != h)) {
                    sh_puts("Monitor native: ");
                    sh_putdec(native.h_active);
                    sh_puts("x");
                    sh_putdec(native.v_active);
                    sh_puts("@");
                    sh_putdec(native.refresh_hz);
                    sh_puts("Hz\n");

                    /* Resize display buffers to native resolution */
                    if (display_resize(native.h_active, native.v_active,
                                       native.h_active) == 0) {
                        uint32_t *bb = display_get_back_buffer();
                        uint32_t fb_pitch_bytes = native.h_active * 4;
                        if (gpu_display_set_mode &&
                            gpu_display_set_mode(&native,
                                (uint64_t)(uintptr_t)bb, fb_pitch_bytes) == 0) {
                            w = native.h_active;
                            h = native.v_active;
                            p = w;  /* our buffers have pitch == width */
                            sh_puts("Modeset OK: ");
                            sh_putdec(w);
                            sh_puts("x");
                            sh_putdec(h);
                            sh_puts("\n");
                        }
                    }
                }
            }

            input_events_init(w, h);
            shm_init();
            compositor_init();

            /* Create terminal surface: same size as the Terminal window content area */
            uint32_t tw, th;
            compositor_get_terminal_dims(&tw, &th);

            /* Allocate shm pixel buffer */
            uint32_t term_shm = shm_create((uint64_t)tw * th * 4, 3 /* CPU_RW */);
            uint32_t *term_px = (uint32_t *)shm_map(term_shm);

            /* Register with compositor (it will blit this surface each frame) */
            compositor_set_terminal_surface(term_shm, tw, th);

            /* Redirect framebuffer output to the new surface */
            fb_set_clear_color(0xFF1A1A2E);   /* dark terminal blue */
            fb_redirect(term_px, tw, th, tw);
            fb_clear();                        /* fill surface with background */

            sched_spawn("compositor", compositor_thread);
            sh_puts("Desktop launched. Compositor running.\n");
        }
    } else if (strcmp(cmd, "fatls") == 0) {
        extern int fat32_ls(const char *path);
        extern bool fat32_is_mounted(void);
        if (!fat32_is_mounted())
            sh_puts("FAT32 not mounted\n");
        else
            fat32_ls(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(cmd, "fatcat") == 0) {
        extern int fat32_read_file(const char *name, uint64_t offset,
                                   void *buf, uint64_t len);
        extern int fat32_find(const char *name, uint32_t *cluster, uint32_t *size);
        extern bool fat32_is_mounted(void);
        if (argc < 2) {
            sh_puts("Usage: fatcat <filename>\n");
        } else if (!fat32_is_mounted()) {
            sh_puts("FAT32 not mounted\n");
        } else {
            uint32_t fsize;
            if (fat32_find(argv[1], NULL, &fsize) < 0) {
                sh_puts("File not found: "); sh_puts(argv[1]); sh_puts("\n");
            } else {
                uint32_t to_read = fsize < 4096 ? fsize : 4096;
                char *buf = (char *)kmalloc(to_read + 1);
                if (buf) {
                    int n = fat32_read_file(argv[1], 0, buf, to_read);
                    if (n > 0) { buf[n] = '\0'; sh_puts(buf); }
                    kfree(buf);
                }
            }
        }
    } else if (strcmp(cmd, "beep") == 0) {
        extern void hda_play_tone(uint32_t freq, uint32_t duration_ms)
            __attribute__((weak));
        if (hda_play_tone) {
            uint32_t freq = 440;
            uint32_t dur = 500;
            if (argc > 1) { freq = 0; for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) freq = freq * 10 + (*p - '0'); }
            if (argc > 2) { dur = 0; for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) dur = dur * 10 + (*p - '0'); }
            hda_play_tone(freq, dur);
        } else {
            sh_puts("HDA audio not available\n");
        }
    } else if (strcmp(cmd, "insmod") == 0) {
        extern int kmod_load(const char *name, const uint8_t *data, uint64_t len);
        extern void *osfs2_find(const char *);
        extern uint64_t osfs2_file_size(void *);
        extern int osfs2_read(void *, uint64_t, void *, uint64_t);
        if (argc < 2) { sh_puts("Usage: insmod <module.ko>\n"); }
        else {
            void *f = osfs2_find(argv[1]);
            if (!f) { sh_puts("File not found: "); sh_puts(argv[1]); sh_puts("\n"); }
            else {
                uint64_t sz = osfs2_file_size(f);
                uint8_t *buf = (uint8_t *)kmalloc(sz);
                if (buf) {
                    osfs2_read(f, 0, buf, sz);
                    kmod_load(argv[1], buf, sz);
                    kfree(buf);
                }
            }
        }
    } else if (strcmp(cmd, "rmmod") == 0) {
        extern int kmod_unload(const char *name);
        if (argc < 2) sh_puts("Usage: rmmod <name>\n");
        else kmod_unload(argv[1]);
    } else if (strcmp(cmd, "lsmod") == 0) {
        extern void kmod_list(void);
        kmod_list();
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "kexec") == 0) {
        cmd_kexec(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(cmd, "kdownload") == 0) {
        cmd_kdownload(argc, argv);
    } else if (strcmp(cmd, "kupload") == 0) {
        cmd_kupload(argc, argv);
    } else if (strcmp(cmd, "kupdate") == 0) {
        cmd_kupdate(argc, argv);
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(cmd, "halt") == 0 || strcmp(cmd, "shutdown") == 0) {
        cmd_halt();
    } else if (strcmp(cmd, "beep") == 0) {
        extern bool hda_is_ready(void);
        extern void hda_play_tone(uint32_t, uint32_t);
        if (!hda_is_ready()) {
            sh_puts("HDA: not initialized\n");
        } else {
            uint32_t freq = 440, dur = 500;
            if (argc > 1) { freq = 0; for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) freq = freq * 10 + (*p - '0'); }
            if (argc > 2) { dur = 0;  for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) dur  = dur  * 10 + (*p - '0'); }
            hda_play_tone(freq, dur);
        }
    } else {
        sh_puts("Unknown command: ");
        sh_puts(cmd);
        sh_puts("\n  Type 'help' for available commands.\n");
    }

    /* Finalize output redirection — write captured output to file */
    if (redirected) {
        sh_redir_fn = NULL;  /* Restore normal output first */

        if (redir.out_file && out_buf && redir_pos > 0) {
            extern void *osfs2_create(const char *name, uint64_t size);
            extern int osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
            extern uint64_t osfs2_file_size(void *file);

            void *f = osfs2_find(redir.out_file);
            if (!f)
                f = osfs2_create(redir.out_file, redir_pos);
            if (f) {
                uint64_t off = redir.append ? osfs2_file_size(f) : 0;
                osfs2_write(f, off, out_buf, redir_pos);
                /* Flush write-back cache del controller — sin esto, USB
                 * MSC pierde la data al desenchufar.  El usuario espera
                 * que después de `>` el archivo esté en disco.            */
                extern int disk_flush(void);
                disk_flush();
                sh_puts("[");
                sh_putdec(redir_pos);
                sh_puts(" bytes -> ");
                sh_puts(redir.out_file);
                sh_puts(" (synced)]\n");
            } else {
                sh_puts("Error: cannot create ");
                sh_puts(redir.out_file);
                sh_puts("\n");
            }
        }
        redir_buf = NULL;
        redir_pos = 0;
    }
    if (out_buf) kfree(out_buf);

    /* Restore stdin and free input-redirect buffer if used */
    if (in_buf) {
        kfree(in_buf);
        sh_stdin_buf = saved_stdin_buf;
        sh_stdin_len = saved_stdin_len;
    }
}

/* ── Shell main loop ─────────────────────────────────────────── */

void __cold shell_run(void)
{
    char line[256];

    sh_puts("\n");
    sh_puts_color("  ____       _ _        _  __\n", 0x00FF8800);
    sh_puts_color(" / __ \\  ___(_) |_ ___ | |/ /\n", 0x00FF8800);
    sh_puts_color("| |  | |/ __| | __/ _ \\| ' / \n", 0x00FF8800);
    sh_puts_color("| |__| |\\__ \\ | || (_) | . \\ \n", 0x00FF8800);
    sh_puts_color(" \\____/ |___/_|\\__\\___/|_|\\_\\\n", 0x00FF8800);
    sh_puts("\n");
    sh_puts_color(" Welcome to OsitoK Shell\n", 0x0000FF88);
#ifdef __EMSCRIPTEN__
    sh_puts(" Type 'tutorial' for a quick tour, 'help' for commands.\n");
#else
    sh_puts(" Type 'help' for commands.\n");
#endif

    /* Compact HW summary */
#ifndef WASM_BUILD
    sh_puts(" ");
    sh_puts_color("[", 0x00666666);
    fb_putdec(pci_get_device_count());
    sh_puts_color(" PCI", 0x00666666);
    /* TODO(bare-metal): status checks below hung the bare-metal i5/R7 boots
     * (one of nvme_is_ready/i211_link_up/xhci_is_ready polls a register that
     * stalls on real HW). Skip until we identify which. */
    sh_puts_color("]\n\n", 0x00666666);
#else
    sh_puts_color(" [wasm32 | 256MB heap]\n\n", 0x00666666);
#endif

    /* Auto-launch hello_gl.elf if present (W4.10 runtime test) */
    if (osfs2_is_mounted() && osfs2_find("hello_gl.elf")) {
        sh_puts(" Auto-launching hello_gl.elf...\n");
        shell_exec("exec hello_gl.elf");
    }
    /* Auto-launch UT99 if osfs2 is mounted and UnrealTournament.exe exists */
    else if (osfs2_is_mounted() && osfs2_find("UnrealTournament.exe")) {
        sh_puts(" Auto-launching UnrealTournament.exe...\n");
        shell_exec("winexec UnrealTournament.exe");
    }
    /* Panorama: auto-launch DOOM.EXE (DOS4GW embedded) when no UT99 present */
    else if (osfs2_is_mounted() && osfs2_find("DOOM.EXE")) {
        sh_puts(" Auto-launching DOOM.EXE...\n");
        shell_exec("dosrun DOOM.EXE");
    }

    for (;;) {
        int len = term_readline("osito> ", line, sizeof(line));

        if (len < 0) {
            /* EOF (Ctrl+D) */
            sh_puts("Use 'halt' to stop or 'reboot' to restart.\n");
            continue;
        }

        if (len == 0) continue;  /* Empty line or Ctrl+C */

        /* Heredoc support: detect `<<TERM` (or `<< TERM`) in the line.
         * Read further lines until a line is exactly TERM, then feed
         * the accumulated text as stdin to the command (and strip the
         * heredoc marker from the line itself). */
        char *here_marker = NULL;
        for (char *p = line; *p; p++) {
            if (p[0] == '<' && p[1] == '<') { here_marker = p; break; }
        }
        char *here_buf = NULL;
        const char *saved_h_buf = sh_stdin_buf;
        uint32_t    saved_h_len = sh_stdin_len;
        if (here_marker) {
            char *t = here_marker + 2;
            while (*t == ' ' || *t == '\t') t++;
            char term[64] = {0};
            int tl = 0;
            while (t[tl] && t[tl] != ' ' && t[tl] != '\t' && tl < (int)sizeof(term) - 1) {
                term[tl] = t[tl]; tl++;
            }
            term[tl] = 0;

            /* Truncate the line at `<<` so the command doesn't see the marker. */
            *here_marker = 0;
            /* Rstrip trailing whitespace */
            int ll = (int)strlen(line);
            while (ll > 0 && (line[ll-1] == ' ' || line[ll-1] == '\t')) line[--ll] = 0;

            /* Read body until terminator. 64 KB cap. */
            enum { HERE_CAP = 64 * 1024 };
            here_buf = (char *)kmalloc(HERE_CAP);
            uint32_t hpos = 0;
            char hl[1024];
            for (;;) {
                int hn = term_readline("> ", hl, sizeof(hl));
                if (hn < 0) break;
                if (strcmp(hl, term) == 0) break;
                size_t hlen = strlen(hl);
                if (hpos + hlen + 1 >= HERE_CAP) break;
                if (here_buf) {
                    memcpy(here_buf + hpos, hl, hlen);
                    hpos += hlen;
                    here_buf[hpos++] = '\n';
                }
            }
            if (here_buf) {
                sh_stdin_buf = here_buf;
                sh_stdin_len = hpos;
            }
        }

        shell_exec_pipeline(line);

        if (here_buf) {
            kfree(here_buf);
            sh_stdin_buf = saved_h_buf;
            sh_stdin_len = saved_h_len;
        }

        /* Poll network between commands */
        net_poll();

#ifdef __EMSCRIPTEN__
        /* Flush dirty FS image to IndexedDB after each command so
         * git commits, file edits, and cc outputs survive reload. */
        extern void wasm_persist_flush(void);
        wasm_persist_flush();
#endif
    }
}
