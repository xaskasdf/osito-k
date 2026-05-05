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
static void (*sh_redir_fn)(const char *s, size_t len);

/* Pipe input — set by the pipeline driver to feed previous stage's
 * captured output as virtual stdin for the next stage.  Only consumed
 * by stdin-aware commands (grep/head/tail) when no file argument is
 * given.  Caller is responsible for buffer lifetime across the call. */
static const char *sh_stdin_buf;
static uint32_t    sh_stdin_len;

static void sh_puts(const char *s)
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

static void cmd_cat(int argc, char *argv[])
{
    if (argc < 2) {
        sh_puts("Usage: cat <filename>\n");
        return;
    }

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

    /* Stream data */
    uint32_t offset = 0;
    while (offset < src_size) {
        uint32_t cklen = (src_size - offset > KUPLOAD_CHUNK_SZ)
                        ? KUPLOAD_CHUNK_SZ : (src_size - offset);
        uint8_t pkt[16 + KUPLOAD_CHUNK_SZ];
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

static void cmd_curl(int argc, char *argv[])
{
#ifdef WASM_BUILD
    (void)argc; (void)argv;
    sh_puts("curl: network not available in WASM\n");
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

    int r = llama_chat(prompt_llama, prompt, 128, chat_token_cb, NULL);
    if (r < 0) {
        sh_puts_color("[error]\n", 0x00FF0000);
    } else {
        sh_puts("\n");
    }
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

/* Captured output buffer for redirection */
static char    *redir_buf;
static uint32_t redir_pos;
static uint32_t redir_max;

static void redir_capture(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (redir_pos < redir_max - 1)
            redir_buf[redir_pos++] = s[i];
    }
}

/* ── Dispatch command ────────────────────────────────────────── */

/* Forward decl — pipeline driver calls into this for each stage. */
static void shell_exec(char *line);

static void shell_exec_pipeline(char *line)
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

static void shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = parse_args(line, argv);

    if (argc == 0) return;

    /* Parse I/O redirections */
    redir_t redir;
    parse_redirects(&argc, argv, &redir);

    if (argc == 0) return;

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
    sh_puts(" Type 'help' for commands.\n");

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

        shell_exec_pipeline(line);

        /* Poll network between commands */
        net_poll();
    }
}
