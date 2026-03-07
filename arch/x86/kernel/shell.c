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
