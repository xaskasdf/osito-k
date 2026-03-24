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

/* ── Shell output helpers ────────────────────────────────────── */

/* Output redirect hook (set by shell_exec for > and >> operators) */
static void (*sh_redir_fn)(const char *s, size_t len);

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
    sh_puts("  exec      Run an ELF binary\n");
    sh_puts("  cc        Compile C with TCC (cc file.c [-run])\n");
    sh_puts("  build     Self-build kernel (TCC compile + link)\n");
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
    sh_puts("  js        QuickJS REPL (js [script.js])\n");
    sh_puts("  dl        Dynamic linker (dl load/sym/call/close/list)\n");
    sh_puts("  git       Version control (init/add/commit/log/status/diff/branch/checkout)\n");
    sh_puts("  sched     Scheduler test (sched [stats])\n");
    sh_puts("  httpd     HTTP server (httpd [port] / httpd stop)\n");
    sh_puts("  winexec   Run a Win32 PE executable (winexec file.exe)\n");
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
    sh_puts(" x86-64 AI OS (");
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

static void cmd_ls(void)
{
    if (!osfs2_is_mounted()) {
        sh_puts("No filesystem mounted\n");
        return;
    }
    osfs2_list();
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

    proc_exec(argv[1], argc - 1, (const char **)(argv + 1));
}

/* ── Builtin: cc (compile C with TCC) ────────────────────────── */

static void cmd_cc(int argc, char *argv[])
{
    if (!osfs2_is_mounted() || !osfs2_find("tcc.elf")) {
        sh_puts("tcc.elf not found on disk\n");
        return;
    }

    if (argc < 2) {
        sh_puts("Usage: cc <file.c> [-run]    Compile and optionally run\n");
        sh_puts("       cc -run <file.c>      Compile + run immediately\n");
        sh_puts("Auto-links with CRT+libc if crt.o/syscall.o/tcclib.o on disk\n");
        return;
    }

    /* Check for -run flag */
    int run_mode = 0;
    const char *source = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-run") == 0)
            run_mode = 1;
        else if (!source)
            source = argv[i];
    }

    if (!source) {
        sh_puts("No source file specified\n");
        return;
    }

    if (!osfs2_find(source)) {
        sh_puts("File not found: ");
        sh_puts(source);
        sh_puts("\n");
        return;
    }

    /* Build output filename: foo.c → foo.elf */
    char outname[64];
    int j = 0;
    const char *s = source;
    while (*s && *s != '.' && j < 58) outname[j++] = *s++;
    outname[j++] = '.'; outname[j++] = 'e'; outname[j++] = 'l'; outname[j++] = 'f'; outname[j] = '\0';

    /* Step 1: Compile + link */
    sh_puts_color("Compiling: ", 0x0000FF00);
    sh_puts(source);
    sh_puts(" → ");
    sh_puts(outname);
    sh_puts("\n");

    /* Check if CRT objects are available for libc-linked compilation */
    int has_crt = osfs2_find("crt.o") && osfs2_find("syscall.o")
               && osfs2_find("tcclib.o");

    const char *tcc_argv_bare[] = {
        "tcc", "-nostdlib", "-nostdinc", "-static",
        source, "-o", outname
    };
    const char *tcc_argv_crt[] = {
        "tcc", "-nostdlib", "-nostdinc", "-static",
        "-Wl,-Ttext,0x401000",
        "-Wl,-section-alignment,0x1000",
        "crt.o", "syscall.o", "tcclib.o",
        source, "-o", outname
    };

    int tcc_argc;
    const char **tcc_argv;
    if (has_crt) {
        sh_puts("  [CRT+libc linked]\n");
        tcc_argv = tcc_argv_crt;
        tcc_argc = 12;
    } else {
        tcc_argv = tcc_argv_bare;
        tcc_argc = 7;
    }

    int ret = proc_exec("tcc.elf", tcc_argc, tcc_argv);

    if (ret != 0) {
        sh_puts_color("Compilation failed", 0x00FF0000);
        sh_puts(" (exit ");
        sh_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
        sh_puts(")\n");
        return;
    }

    sh_puts_color("OK", 0x0000FF00);
    sh_puts(" — compiled successfully\n");

    /* Step 2: Run if -run flag */
    if (run_mode) {
        if (!osfs2_find(outname)) {
            sh_puts("Output file not found on disk\n");
            return;
        }
        sh_puts_color("Running: ", 0x0000FF00);
        sh_puts(outname);
        sh_puts("\n");
        ret = proc_exec(outname, 0, NULL);
        sh_puts("Exit code: ");
        sh_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
        sh_puts("\n");
    }
}

/* ── Builtin: build (kernel self-build with TCC) ────────────── */

/* TCC-compilable .c source files (basenames — flat FS) */
static const char *build_tcc_sources[] = {
    /* kernel/ */
    "main.c", "serial.c", "framebuffer.c", "pci.c", "memory.c",
    "net.c", "inference.c", "idt.c", "paging.c", "heap.c",
    "syscall.c", "elf.c", "process.c", "keyboard.c", "terminal.c",
    "shell.c", "crypto.c", "tls.c", "http.c", "claude.c",
    "tokenizer.c", "smp.c", "dynlink.c", "zlib.c", "git.c",
    "shm.c", "compositor.c", "display.c", "input_events.c", "memcompress.c",
    /* drivers/ */
    "nvme.c", "i211.c", "gpu.c", "gsp.c", "sass.c", "gmmu.c",
    "gpu_tensor.c", "gpu_inference.c", "xhci.c",
    /* fs/ */
    "ositofs2.c", "gpt.c", "gguf.c",
    /* win32/ (GCC-only: compat32.c, msvcrt_shim.c) */
    "win32_init.c", "pe.c", "winexec.c", "handle.c", "ntsyscall.c",
    "ntprocess.c", "ntsync.c", "dllloader.c", "kernel32_shim.c",
    "ntdll_shim.c", "user32_shim.c", "gdi32_shim.c", "advapi32_shim.c",
    "ddraw_shim.c", "dsound_shim.c", "ole32_shim.c", "shell32_shim.c",
    "comctl32_shim.c", "comdlg32_shim.c", "winmm_shim.c", "wsock32_shim.c",
    NULL
};

/* GCC pre-compiled .o files (AVX2, assembly, compat32) */
static const char *build_gcc_objects[] = {
    "tensor.o", "tensor_avx2.o",
    "isr_stubs.o", "syscall_entry.o", "setjmp.o", "kexec_tramp.o",
    "compat32.o", "msvcrt_shim.o", "int2e_stub.o",
    "entry_alias.o",
    NULL
};

static void c_to_o(const char *src, char *dst)
{
    int i = 0;
    while (src[i] && src[i] != '.' && i < 58) { dst[i] = src[i]; i++; }
    dst[i++] = '.'; dst[i++] = 'o'; dst[i] = '\0';
}

static void cmd_build(void)
{
    if (!osfs2_is_mounted() || !osfs2_find("tcc.elf")) {
        sh_puts("tcc.elf not found on disk\n");
        return;
    }

    /* Count source files */
    int n_src = 0;
    while (build_tcc_sources[n_src]) n_src++;

    sh_puts_color("OsitoK kernel self-build\n", 0x00FF8800);
    sh_puts("  Sources: ");
    sh_putdec((uint64_t)n_src);
    sh_puts(" TCC + ");
    int n_gcc = 0;
    while (build_gcc_objects[n_gcc]) n_gcc++;
    sh_putdec((uint64_t)n_gcc);
    sh_puts(" GCC precompiled\n");

    /* Verify all GCC .o files exist */
    for (int i = 0; build_gcc_objects[i]; i++) {
        if (!osfs2_find(build_gcc_objects[i])) {
            sh_puts_color("MISSING: ", 0x00FF0000);
            sh_puts(build_gcc_objects[i]);
            sh_puts("\n");
            return;
        }
    }

    /* ── Phase 1: Compile each .c → .o ────────────────────── */

    int errors = 0;
    char oname[64];

    for (int i = 0; i < n_src; i++) {
        const char *src = build_tcc_sources[i];
        c_to_o(src, oname);

        /* Delete old .o if it exists */
        if (osfs2_find(oname))
            osfs2_delete(oname);

        /* Progress */
        sh_puts("  [");
        sh_putdec((uint64_t)(i + 1));
        sh_puts("/");
        sh_putdec((uint64_t)n_src);
        sh_puts("] ");
        sh_puts(src);

        const char *tcc_argv[] = {
            "tcc", "-c", "-nostdlib", "-nostdinc",
            "-D__KERNEL_X86__=1",
            src, "-o", oname
        };
        int ret = proc_exec("tcc.elf", 8, tcc_argv);

        if (ret != 0) {
            sh_puts_color(" FAIL", 0x00FF0000);
            sh_puts(" (exit ");
            sh_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
            sh_puts(")\n");
            errors++;
        } else {
            sh_puts_color(" OK\n", 0x0000FF00);
        }
    }

    if (errors > 0) {
        sh_puts_color("\nBuild failed: ", 0x00FF0000);
        sh_putdec((uint64_t)errors);
        sh_puts(" error(s)\n");
        return;
    }

    /* ── Phase 2: Link all .o → kernel.elf ────────────────── */

    sh_puts_color("\nLinking kernel.elf...\n", 0x00FF8800);

    /* Delete old kernel.elf */
    if (osfs2_find("kernel.elf"))
        osfs2_delete("kernel.elf");

    /* Build link argv: tcc -nostdlib -static -Wl,flags... all.o -o kernel.elf */
    /* Max: 5 flags + n_src .o + n_gcc .o + 2 (-o kernel.elf) + 1 (NULL safety) */
    #define BUILD_MAX_LINK_ARGS 128
    const char *link_argv[BUILD_MAX_LINK_ARGS];
    int la = 0;

    link_argv[la++] = "tcc";
    link_argv[la++] = "-nostdlib";
    link_argv[la++] = "-static";
    link_argv[la++] = "-Wl,-Ttext,0x2000000";
    link_argv[la++] = "-Wl,-section-alignment,0x1000";

    /* Add all TCC-compiled .o files */
    /* We need persistent oname strings — use a static buffer */
    static char onames[80][64];  /* 80 slots × 64 chars */
    for (int i = 0; i < n_src && la < BUILD_MAX_LINK_ARGS - 3; i++) {
        c_to_o(build_tcc_sources[i], onames[i]);
        link_argv[la++] = onames[i];
    }

    /* Add GCC pre-compiled .o files */
    for (int i = 0; build_gcc_objects[i] && la < BUILD_MAX_LINK_ARGS - 3; i++) {
        link_argv[la++] = build_gcc_objects[i];
    }

    link_argv[la++] = "-o";
    link_argv[la++] = "kernel.elf";

    sh_puts("  ");
    sh_putdec((uint64_t)la);
    sh_puts(" args, ");
    sh_putdec((uint64_t)(n_src + n_gcc));
    sh_puts(" object files\n");

    int ret = proc_exec("tcc.elf", la, link_argv);

    if (ret != 0) {
        sh_puts_color("Link failed", 0x00FF0000);
        sh_puts(" (exit ");
        sh_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
        sh_puts(")\n");
        return;
    }

    /* Verify output */
    void *kelf = osfs2_find("kernel.elf");
    if (!kelf) {
        sh_puts_color("kernel.elf not found after link!\n", 0x00FF0000);
        return;
    }

    sh_puts_color("\nBuild successful!\n", 0x0000FF00);
    sh_puts("  kernel.elf: ");
    sh_putdec(osfs2_file_size(kelf));
    sh_puts(" bytes\n");
    sh_puts("  Free: ");
    sh_putdec((uint64_t)osfs2_free_blocks());
    sh_puts(" MB\n");
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
            __asm__ volatile ("hlt");
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

    /* Step 2: Validate ELF header */
    kexec_elf64_hdr_t *ehdr = (kexec_elf64_hdr_t *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        sh_puts_color("  Not an ELF file\n", 0x00FF0000);
        kfree(elf_data);
        return;
    }

    sh_puts("  Entry: 0x");
    serial_puthex(ehdr->e_entry, 8);
    sh_puts(", ");
    sh_putdec(ehdr->e_phnum);
    sh_puts(" program headers\n");

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
    __asm__ volatile ("cli");

    /* Step 7: Jump to trampoline */
    typedef void (*tramp_fn)(void *info, uint64_t entry,
                             kexec_seg_t *segs, uint64_t nseg);
    tramp_fn tramp = (tramp_fn)KEXEC_TRAMP_ADDR;

    tramp(new_info, ehdr->e_entry, seg_copy, nseg);

    /* Should never reach here */
    for (;;) __asm__ volatile ("hlt");
}

/* ── Builtin: reboot ─────────────────────────────────────────── */

static void cmd_reboot(void)
{
    sh_puts("Rebooting...\n");
    /* Triple fault — fastest way to reset on x86 */
    /* Load a zero-length IDT and trigger an interrupt */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
}

/* ── Builtin: halt ───────────────────────────────────────────── */

static void cmd_halt(void)
{
    sh_puts_color("System halted.\n", 0x00FF8800);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
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
        __asm__ volatile ("hlt");
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
                    __asm__ volatile ("cli");
                    int rc = osfs2_read(file, 0, resp + hdr_len, file_size);
                    __asm__ volatile ("sti");
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
                __asm__ volatile ("hlt");
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
    sh_puts("\n  Access: http://10.0.2.15:");
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

static void shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = parse_args(line, argv);

    if (argc == 0) return;

    /* Parse I/O redirections */
    redir_t redir;
    parse_redirects(&argc, argv, &redir);

    if (argc == 0) return;

    /* Setup output redirection */
    bool redirected = false;
    char *out_buf = NULL;
    if (redir.out_file) {
        out_buf = (char *)kmalloc(65536);
        if (out_buf) {
            redir_buf = out_buf;
            redir_pos = 0;
            redir_max = 65536;
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
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls();
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(cmd, "exec") == 0) {
        cmd_exec(argc, argv);
    } else if (strcmp(cmd, "cc") == 0 || strcmp(cmd, "tcc") == 0) {
        cmd_cc(argc, argv);
    } else if (strcmp(cmd, "build") == 0) {
        cmd_build();
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
    } else if (strcmp(cmd, "js") == 0) {
        /* QuickJS REPL — run qjs.elf with optional script argument */
        if (argc > 1) {
            const char *js_argv[] = { "qjs.elf", argv[1] };
            proc_exec("qjs.elf", 2, js_argv);
        } else {
            const char *js_argv[] = { "qjs.elf" };
            proc_exec("qjs.elf", 1, js_argv);
        }
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
    } else if (strcmp(cmd, "httpd") == 0) {
        cmd_httpd(argc, argv);
    } else if (strcmp(cmd, "winexec") == 0) {
        if (argc < 2) {
            sh_puts("Usage: winexec <file.exe>\n");
        } else {
            extern int win32_exec(const char *filename);
            extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
            extern uint64_t *compat32_crash_jmpbuf;
            static uint64_t winexec_jmpbuf[8];
            compat32_crash_jmpbuf = winexec_jmpbuf;
            if (kern_setjmp(winexec_jmpbuf) == 0) {
                win32_exec(argv[1]);
            } else {
                sh_puts("\n [WIN32] Process crashed — returned to shell\n");
            }
            compat32_crash_jmpbuf = NULL;
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
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "kexec") == 0) {
        cmd_kexec(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(cmd, "halt") == 0) {
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
                sh_puts("[");
                sh_putdec(redir_pos);
                sh_puts(" bytes -> ");
                sh_puts(redir.out_file);
                sh_puts("]\n");
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

void shell_run(void)
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
    sh_puts(" ");
    sh_puts_color("[", 0x00666666);
    fb_putdec(pci_get_device_count());
    sh_puts_color(" PCI", 0x00666666);
    if (nvme_is_ready()) sh_puts_color(" | NVMe", 0x00666666);
    if (osfs2_is_mounted()) sh_puts_color(" | FS", 0x00666666);
    if (i211_link_up()) sh_puts_color(" | NIC", 0x00666666);
    if (xhci_is_ready()) sh_puts_color(" | USB", 0x00666666);
    sh_puts_color("]\n\n", 0x00666666);

    /* Auto-launch UT99 if osfs2 is mounted and UnrealTournament.exe exists */
    if (osfs2_is_mounted() && osfs2_find("UnrealTournament.exe")) {
        sh_puts(" Auto-launching UnrealTournament.exe...\n");
        shell_exec("winexec UnrealTournament.exe");
    }

    for (;;) {
        int len = term_readline("osito> ", line, sizeof(line));

        if (len < 0) {
            /* EOF (Ctrl+D) */
            sh_puts("Use 'halt' to stop or 'reboot' to restart.\n");
            continue;
        }

        if (len == 0) continue;  /* Empty line or Ctrl+C */

        shell_exec(line);

        /* Poll network between commands */
        net_poll();
    }
}
