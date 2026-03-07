/*
 * OsitoK x86-64 — Mini Shell
 *
 * X-OS9: Interactive command shell with builtins.
 * Reads lines from terminal, parses commands, dispatches.
 *
 * Builtins: help, ps, mem, echo, ls, cat, reboot, halt, clear, uname
 */

#include "../include/types.h"

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

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Network */
extern void net_poll(void);
extern void     net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
extern uint32_t net_icmp_get_rx_count(void);
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t buf_size);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size, uint32_t timeout_ticks);
extern void net_tcp_close(int conn);
extern int  net_tcp_state(int conn);
extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);

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

/* ── Shell output helpers ────────────────────────────────────── */

static void sh_puts(const char *s)
{
    serial_puts(s);
    fb_puts(s);
}

static void sh_puts_color(const char *s, uint32_t color)
{
    serial_puts(s);
    fb_puts_color(s, color);
}

static void sh_putdec(uint64_t val)
{
    serial_putdec(val);
    fb_putdec(val);
}

/* ── Parse command line into argv ────────────────────────────── */

#define MAX_ARGS 16

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
    sh_puts("  mem       Memory usage\n");
    sh_puts("  uptime    Show uptime\n");
    sh_puts("  echo      Print arguments\n");
    sh_puts("  ls        List files on disk\n");
    sh_puts("  cat       Display file contents\n");
    sh_puts("  exec      Run an ELF binary\n");
    sh_puts("  cc        Compile C with TCC (cc file.c [-run])\n");
    sh_puts("  ping      Ping an IP address\n");
    sh_puts("  tcptest   TCP connection test (tcptest [ip] [port])\n");
    sh_puts("  resolve   DNS lookup (resolve hostname)\n");
    sh_puts("  tlstest   TLS connect test (tlstest [hostname])\n");
    sh_puts("  curl      HTTPS GET (curl hostname [path])\n");
    sh_puts("  clear     Clear screen\n");
    sh_puts("  reboot    Reboot system\n");
    sh_puts("  halt      Halt CPU\n");
}

/* ── Builtin: uname ──────────────────────────────────────────── */

static void cmd_uname(void)
{
    sh_puts_color("OsitoK", 0x00FF8800);
    sh_puts(" x86-64 bare-metal AI OS (");
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

    const char *tcc_argv[] = {
        "tcc", "-nostdlib", "-nostdinc", "-static",
        source, "-o", outname
    };
    int ret = proc_exec("tcc.elf", 7, tcc_argv);

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

/* ── Dispatch command ────────────────────────────────────────── */

static void shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = parse_args(line, argv);

    if (argc == 0) return;

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
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
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else {
        sh_puts("Unknown command: ");
        sh_puts(cmd);
        sh_puts("\n  Type 'help' for available commands.\n");
    }
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
    sh_puts(" Type 'help' for commands.\n\n");

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
