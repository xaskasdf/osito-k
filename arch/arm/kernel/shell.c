/*
 * shell.c -- Interactive shell for OsitoK AArch64
 */
#include "../include/hal.h"
#include "../include/types.h"
#include "task.h"

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
