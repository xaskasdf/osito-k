/*
 * shell.c -- Interactive shell for OsitoK AArch64
 */
#include "../include/hal.h"
#include "../include/types.h"
#include "task.h"
#include <stdbool.h>

extern void hda_play_tone(uint32_t freq_hz, uint32_t duration_ms);
extern bool hda_is_ready(void);
extern int proc_run(const char *path, int argc, char **argv);
extern void net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
extern void net_poll(void);
extern void net_hold(void);
extern void net_release(void);
extern void cmd_model(int argc, char **argv);   /* llm.c */
extern void cmd_chat(int argc, char **argv);
extern void cmd_llm(int argc, char **argv);

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
    serial_puts("  exec     Run a program from the filesystem <name> [args...]\n");
    serial_puts("  ping     Ping IP address\n");
    serial_puts("  cpus     List the online CPUs\n");
    serial_puts("  cpubench Timed task per CPU, all at once [any [n]: n unpinned]\n");
    serial_puts("           [mix: busy + light unpinned tasks, where they settle]\n");
    serial_puts("  cpufreq  CPU frequencies [max | min | <domain> <level>]\n");
    serial_puts("           [l3 | try <domain> <level> <s> [stay|busy]: watched run]\n");
    serial_puts("  temp     Temperature sensors, CPU thermal limit [limit <cap> <release>]\n");
    serial_puts("  burn     Keep CPUs busy in the background <cpu|all> <seconds>\n");
    serial_puts("  echo     Print text\n");
    serial_puts("  hexdump  Hex dump file\n");
    serial_puts("  df       Filesystem free space\n");
    serial_puts("  halt     Shutdown system\n");
    serial_puts("  resolve  DNS lookup\n");
    serial_puts("  net      USB networking (NCM): state [up | down]\n");
    serial_puts("  beep     Play a tone\n");
    serial_puts("  desktop  The desktop on the panel, phablet layout [off]\n");
    serial_puts("  model    Local LLM: the loaded one [<file.gguf>: load it]\n");
    serial_puts("  chat     Talk to the loaded model <text>\n");
    serial_puts("  llm      Compute path [simd on|off | smp on|off | selftest]\n");
    serial_puts("  reboot   Reboot system [bootloader: into fastboot]\n");
    serial_puts("  pmic     PMIC arbiter + restart reason [<sid> <hex addr> | list <sid>]\n");
    serial_puts("  rpmh     Apps RSC + AOP command DB [<resource>]\n");
    serial_puts("  keys     Power / volume keys: now and press counts\n");
    serial_puts("  screen   Screen mode [debug | normal]: diagnostics or full console\n");
    serial_puts("  ufs      UFS storage [luns | gpt <lun> | read <lun> <lba> [off] | mount |\n");
    serial_puts("           mkfs userdata]: writes go only to userdata (OsitoFS)\n");
    serial_puts("  put      Store a file sent over USB <name> (host: rog5-put.ps1)\n");
    serial_puts("  touch    Touchscreen: state, wiring, supplies [on | read | off]\n");
    serial_puts("  kload    Receive a kernel over USB and run it\n");
    serial_puts("  dmesg    Show the console log since boot\n");
    serial_puts("  clear    Clear screen\n");
}

static void cmd_ps(void)
{
    serial_puts("ID  State    Pri  CPU  Util  Ticks   Name\n");
    serial_puts("--  -------  ---  ---  ----  ------  ----\n");
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

        /* CPU it runs or last ran on; '*' = not pinned */
        serial_putdec(task_pool[i].on_cpu);
        serial_puts(task_pool[i].cpu == TASK_ANY_CPU ? "*   " : "    ");

        /* Utilization the scheduler places it by; idle tasks have none */
        if (task_pool[i].idle) {
            serial_puts("   -  ");
        } else {
            uint32_t pct = task_pool[i].util * 100u / UTIL_MAX;
            serial_puts(pct < 10 ? "  " : pct < 100 ? " " : "");
            serial_putdec(pct);
            serial_puts("%  ");
        }

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
    /* The build stamp tells kernels apart: rog5-boot.sh --flash compares
     * it with the one in the boot.img it flashed */
    serial_puts("OsitoK AArch64 v0.1 (bare-metal), built " __DATE__ " " __TIME__ "\n");
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

/* `desktop`: the GUI on the panel, on whichever CPU is fastest and free;
 * `desktop off`: back to the console */
static void cmd_desktop(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        if (desktop_active)
            desktop_close();
        else
            serial_puts("desktop: not running\n");
        return;
    }
    if (desktop_active) {
        serial_puts("desktop: already running (desktop off)\n");
        return;
    }
    task_create_on("desktop", gui_task, (void *)0, 2, TASK_ANY_CPU);
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
    net_hold();
    int err = net_dns_resolve(argv[1], ip);
    net_release();
    if (err == 0) {
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
        else if (strcmp(argv[0], "kload") == 0)   platform_kload();
        else if (strcmp(argv[0], "dmesg") == 0)   klog_dump();
        else if (strcmp(argv[0], "ls") == 0)      cmd_ls();
        else if (strcmp(argv[0], "cat") == 0)    cmd_cat(argc, argv);
        else if (strcmp(argv[0], "exec") == 0) {
            if (argc < 2) serial_puts("Usage: exec <filename> [args...]\n");
            else proc_run(argv[1], argc - 1, argv + 1);
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
                serial_puts("\n");
                /* Four echoes, a second each. The first may go to ARP:
                 * net.c drops a packet whose next hop isn't resolved yet. */
                extern uint32_t net_icmp_get_rx_count(void);
                net_hold();
                for (uint16_t seq = 1; seq <= 4; seq++) {
                    uint32_t before = net_icmp_get_rx_count();
                    uint64_t t0 = timer_ms(), t = 0;
                    net_icmp_send_echo(ip, seq);
                    while (net_icmp_get_rx_count() == before && (t = timer_ms() - t0) < 1000) {
                        net_poll();
                        task_yield();
                    }
                    serial_puts(net_icmp_get_rx_count() != before ? "  reply, " : "  timeout\n");
                    if (net_icmp_get_rx_count() != before) {
                        serial_putdec(t);
                        serial_puts(" ms\n");
                    }
                }
                net_release();
            }
        }
        else if (strcmp(argv[0], "cpus") == 0) {
            smp_print_cpus();
        }
        else if (strcmp(argv[0], "cpubench") == 0 && argc > 1 &&
                 strcmp(argv[1], "mix") == 0) {
            smp_bench_mix();
        }
        else if (strcmp(argv[0], "cpubench") == 0) {
            int any = argc > 1 && strcmp(argv[1], "any") == 0;
            int count = 0;
            for (const char *p = argc > 2 ? argv[2] : ""; *p >= '0' && *p <= '9'; p++)
                count = count * 10 + (*p - '0');
            smp_bench(any, count);
        }
        else if (strcmp(argv[0], "cpufreq") == 0) {
            platform_cpufreq_cmd(argc, argv);
        }
        else if (strcmp(argv[0], "temp") == 0) {
            platform_temp_cmd(argc, argv);
        }
        else if (strcmp(argv[0], "burn") == 0) {
            int cpu = -1, secs = 0;
            if (argc == 3 && strcmp(argv[1], "all") != 0) {
                cpu = 0;
                for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
                    cpu = cpu * 10 + (*p - '0');
            }
            for (const char *p = argc == 3 ? argv[2] : ""; *p >= '0' && *p <= '9'; p++)
                secs = secs * 10 + (*p - '0');
            if (argc != 3 || secs <= 0 || cpu >= MAX_CPUS)
                serial_puts("usage: burn <cpu|all> <seconds>\n");
            else
                smp_burn(cpu, secs);
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
        else if (strcmp(argv[0], "net") == 0)     platform_net_cmd(argc, argv);
        else if (strcmp(argv[0], "beep") == 0)    cmd_beep(argc, argv);
        else if (strcmp(argv[0], "desktop") == 0) cmd_desktop(argc, argv);
        else if (strcmp(argv[0], "model") == 0)   cmd_model(argc, argv);
        else if (strcmp(argv[0], "chat") == 0)    cmd_chat(argc, argv);
        else if (strcmp(argv[0], "llm") == 0)     cmd_llm(argc, argv);
        else if (strcmp(argv[0], "reboot") == 0) {
            if (argc > 1 && strcmp(argv[1], "bootloader") == 0)
                platform_reboot_bootloader();
            else
                platform_reboot();
        }
        else if (strcmp(argv[0], "pmic") == 0)    platform_pmic_cmd(argc, argv);
        else if (strcmp(argv[0], "rpmh") == 0)    platform_rpmh_cmd(argc, argv);
        else if (strcmp(argv[0], "keys") == 0)    platform_keys_cmd(argc, argv);
        else if (strcmp(argv[0], "screen") == 0)  platform_screen_cmd(argc, argv);
        else if (strcmp(argv[0], "ufs") == 0)     platform_ufs_cmd(argc, argv);
        else if (strcmp(argv[0], "put") == 0)     platform_put_cmd(argc, argv);
        else if (strcmp(argv[0], "touch") == 0)   platform_touch_cmd(argc, argv);
        else if (strcmp(argv[0], "clear") == 0)   serial_puts("\033[2J\033[H");
        else {
            serial_puts("Unknown: ");
            serial_puts(argv[0]);
            serial_puts("\n");
        }
    }
}
