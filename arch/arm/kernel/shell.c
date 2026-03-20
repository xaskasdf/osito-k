/*
 * shell.c -- Interactive shell for OsitoK AArch64
 */
#include "../include/hal.h"
#include "../include/types.h"
#include "task.h"
#include <stdbool.h>

extern void hda_play_tone(uint32_t freq_hz, uint32_t duration_ms);
extern bool hda_is_ready(void);
extern int proc_run(const char *filename);
extern void net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
extern void net_poll(void);
extern int  smp_get_cpu_count(void);

#define MAX_ARGS 16

static int parse_args(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ') line++;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = 0;
    }
    return argc;
}

static void cmd_help(void)
{
    serial_puts("Commands:\n");
    serial_puts("  help     Show this help\n");
    serial_puts("  ps       List tasks\n");
    serial_puts("  mem      Show heap stats\n");
    serial_puts("  uname    Show system info\n");
    serial_puts("  uptime   Show uptime\n");
    serial_puts("  ls       List files\n");
    serial_puts("  cat      Show file contents\n");
    serial_puts("  exec     Run ELF binary\n");
    serial_puts("  ping     Ping IP address\n");
    serial_puts("  cpus     Show CPU count\n");
    serial_puts("  echo     Print text\n");
    serial_puts("  hexdump  Hex dump file\n");
    serial_puts("  df       Filesystem free space\n");
    serial_puts("  halt     Shutdown system\n");
    serial_puts("  resolve  DNS lookup\n");
    serial_puts("  beep     Play a tone\n");
    serial_puts("  desktop  Launch graphical desktop\n");
    serial_puts("  reboot   Reboot system\n");
    serial_puts("  clear    Clear screen\n");
}

static void cmd_ps(void)
{
    serial_puts("ID  State    Pri  Ticks   Name\n");
    serial_puts("--  -------  ---  ------  ----\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) continue;

        /* ID */
        if (i < 10) serial_puts(" ");
        serial_putdec(i);
        serial_puts("  ");

        /* State */
        const char *st;
        switch (task_pool[i].state) {
        case TASK_STATE_READY:   st = "ready  "; break;
        case TASK_STATE_RUNNING: st = "RUN    "; break;
        case TASK_STATE_BLOCKED: st = "blocked"; break;
        case TASK_STATE_DEAD:    st = "dead   "; break;
        default:                 st = "???    "; break;
        }
        serial_puts(st);
        serial_puts("  ");

        /* Priority */
        if (task_pool[i].priority < 10) serial_puts(" ");
        serial_putdec(task_pool[i].priority);
        serial_puts("   ");

        /* Ticks */
        serial_putdec(task_pool[i].ticks_run);
        /* Pad ticks to 6 chars */
        uint32_t t = task_pool[i].ticks_run;
        int digits = 1;
        while (t >= 10) { t /= 10; digits++; }
        for (int p = digits; p < 6; p++) serial_putc(' ');
        serial_puts("  ");

        /* Name */
        serial_puts(task_pool[i].name ? task_pool[i].name : "?");
        serial_puts("\n");
    }
}

static void cmd_mem(void)
{
    serial_puts("RAM:  ");
    serial_putdec(mem_get_total() / (1024 * 1024));
    serial_puts(" MB total, ");
    serial_putdec(mem_get_free() / (1024 * 1024));
    serial_puts(" MB free, ");
    serial_putdec(mem_get_used() / (1024 * 1024));
    serial_puts(" MB used\n");
    serial_puts("Heap: ");
    serial_putdec(heap_get_used() / 1024);
    serial_puts(" KB used / ");
    serial_putdec(heap_get_total() / 1024);
    serial_puts(" KB total\n");
}

static void cmd_uname(void)
{
    serial_puts("OsitoK AArch64 v0.1 (bare-metal)\n");
}

static void cmd_uptime(void)
{
    uint64_t ticks = timer_get_tick_count();
    uint64_t secs = ticks / 100;
    uint64_t mins = secs / 60;
    serial_puts("Up ");
    if (mins > 0) {
        serial_putdec(mins);
        serial_puts("m ");
    }
    serial_putdec(secs % 60);
    serial_puts("s (");
    serial_putdec(ticks);
    serial_puts(" ticks)\n");
}

/* Desktop GUI task (gui_task.c) */
extern void gui_task(void *arg);

static void cmd_desktop(void)
{
    serial_puts("[GUI ] Launching desktop...\n");
    task_create("desktop", gui_task, (void *)0, 2);
}

/* Simple atoi for shell args */
static uint32_t shell_atoi(const char *s)
{
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

static void cmd_beep(int argc, char **argv)
{
    if (!hda_is_ready()) {
        serial_puts("HDA not initialized\n");
        return;
    }
    uint32_t freq = 440;
    uint32_t ms   = 500;
    if (argc >= 2) freq = shell_atoi(argv[1]);
    if (argc >= 3) ms   = shell_atoi(argv[2]);
    if (freq == 0) freq = 440;
    if (ms == 0)   ms   = 500;
    hda_play_tone(freq, ms);
}

static void cmd_ls(void)
{
    osfs2_list();
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        serial_puts("Usage: cat <filename>\n");
        return;
    }
    osfs2_file_t *f = osfs2_find(argv[1]);
    if (!f) {
        serial_puts("File not found: ");
        serial_puts(argv[1]);
        serial_puts("\n");
        return;
    }
    /* Read and print up to 4KB */
    static char cat_buf[4096];
    uint64_t to_read = f->size;
    if (to_read > sizeof(cat_buf) - 1) to_read = sizeof(cat_buf) - 1;
    if (osfs2_read(f, 0, cat_buf, to_read) < 0) {
        serial_puts("Read error\n");
        return;
    }
    cat_buf[to_read] = '\0';
    serial_puts(cat_buf);
    serial_puts("\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) serial_putc(' ');
        serial_puts(argv[i]);
    }
    serial_puts("\n");
}

static void cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { serial_puts("Usage: hexdump <file> [offset] [len]\n"); return; }
    osfs2_file_t *f = osfs2_find(argv[1]);
    if (!f) { serial_puts("File not found\n"); return; }
    uint64_t off = (argc >= 3) ? shell_atoi(argv[2]) : 0;
    uint64_t len = (argc >= 4) ? shell_atoi(argv[3]) : 256;
    if (len > 512) len = 512;
    static uint8_t hbuf[512];
    if (off + len > f->size) len = f->size - off;
    if (osfs2_read(f, off, hbuf, len) < 0) { serial_puts("Read error\n"); return; }
    for (uint64_t i = 0; i < len; i += 16) {
        serial_puthex(off + i, 4);
        serial_puts(": ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            serial_puthex(hbuf[i + j], 2);
            serial_putc(' ');
        }
        serial_puts(" ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            char c = (char)hbuf[i + j];
            serial_putc((c >= 32 && c < 127) ? c : '.');
        }
        serial_puts("\n");
    }
}

static void cmd_df(void)
{
    /* OsitoFS free space — use capacity info from VirtIO-blk */
    serial_puts("OsitoFS: ");
    osfs2_list();  /* Shows file count + block usage */
}

static void cmd_halt(void)
{
    serial_puts("System halting...\n");
    /* PSCI SYSTEM_OFF via HVC */
    register uint64_t x0 __asm__("x0") = 0x84000008ULL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

static void cmd_resolve(int argc, char **argv)
{
    if (argc < 2) { serial_puts("Usage: resolve <hostname>\n"); return; }
    extern int net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
    uint8_t ip[4];
    if (net_dns_resolve(argv[1], ip) == 0) {
        serial_puts(argv[1]);
        serial_puts(" -> ");
        serial_putdec(ip[0]); serial_putc('.');
        serial_putdec(ip[1]); serial_putc('.');
        serial_putdec(ip[2]); serial_putc('.');
        serial_putdec(ip[3]); serial_puts("\n");
    } else {
        serial_puts("DNS resolution failed\n");
    }
}

void shell_run(void)
{
    char buf[256];
    char *argv[MAX_ARGS];

    serial_puts("\nOsitoK shell ready. Type 'help' for commands.\n\n");

    for (;;) {
        int len = term_readline("shell> ", buf, sizeof(buf));
        if (len == 0) continue;

        int argc = parse_args(buf, argv);
        if (argc == 0) continue;

        if      (strcmp(argv[0], "help") == 0)    cmd_help();
        else if (strcmp(argv[0], "ps") == 0)      cmd_ps();
        else if (strcmp(argv[0], "mem") == 0)     cmd_mem();
        else if (strcmp(argv[0], "uname") == 0)   cmd_uname();
        else if (strcmp(argv[0], "uptime") == 0)  cmd_uptime();
        else if (strcmp(argv[0], "ls") == 0)      cmd_ls();
        else if (strcmp(argv[0], "cat") == 0)    cmd_cat(argc, argv);
        else if (strcmp(argv[0], "exec") == 0) {
            if (argc < 2) serial_puts("Usage: exec <filename>\n");
            else proc_run(argv[1]);
        }
        else if (strcmp(argv[0], "ping") == 0) {
            if (argc < 2) { serial_puts("Usage: ping <ip>\n"); }
            else {
                uint8_t ip[4] = {0};
                /* Simple IP parser: a.b.c.d */
                const char *p = argv[1];
                for (int i = 0; i < 4 && *p; i++) {
                    uint32_t v = 0;
                    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                    ip[i] = (uint8_t)v;
                    if (*p == '.') p++;
                }
                serial_puts("PING ");
                serial_puts(argv[1]);
                serial_puts("...\n");
                net_icmp_send_echo(ip, 1);
                /* Poll for reply */
                for (int i = 0; i < 500000; i++) net_poll();
            }
        }
        else if (strcmp(argv[0], "cpus") == 0) {
            serial_puts("CPUs online: ");
            serial_putdec(smp_get_cpu_count());
            serial_puts("\n");
        }
        else if (strcmp(argv[0], "svctest") == 0) {
            /* Inline SVC test — no ELF loading */
            serial_puts("[TEST] Calling SVC write...\n");
            const char *msg = "SVC write works!\n";
            register uint64_t x0 __asm__("x0") = 1;        /* fd */
            register uint64_t x1 __asm__("x1") = (uint64_t)msg;
            register uint64_t x2 __asm__("x2") = 17;       /* len */
            register uint64_t x8 __asm__("x8") = 64;       /* SYS_write */
            __asm__ volatile("svc #0" : "+r"(x0)
                             : "r"(x1), "r"(x2), "r"(x8)
                             : "memory");
            serial_puts("[TEST] SVC returned, x0=");
            serial_putdec(x0);
            serial_puts("\n");
        }
        else if (strcmp(argv[0], "echo") == 0)    cmd_echo(argc, argv);
        else if (strcmp(argv[0], "hexdump") == 0) cmd_hexdump(argc, argv);
        else if (strcmp(argv[0], "df") == 0)      cmd_df();
        else if (strcmp(argv[0], "halt") == 0)    cmd_halt();
        else if (strcmp(argv[0], "resolve") == 0) cmd_resolve(argc, argv);
        else if (strcmp(argv[0], "beep") == 0)    cmd_beep(argc, argv);
        else if (strcmp(argv[0], "desktop") == 0) cmd_desktop();
        else if (strcmp(argv[0], "reboot") == 0)  platform_reboot();
        else if (strcmp(argv[0], "clear") == 0)   serial_puts("\033[2J\033[H");
        else {
            serial_puts("Unknown: ");
            serial_puts(argv[0]);
            serial_puts("\n");
        }
    }
}
