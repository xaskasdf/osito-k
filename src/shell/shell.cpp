/*
 * OsitoK - Minimal interactive shell
 *
 * Commands:
 *   ps     - list all tasks with state and tick count
 *   mem    - show memory pool statistics
 *   ticks  - show current tick count
 *   help   - show available commands
 *   reboot - software reset
 */

#include "shell/shell.h"
#include "drivers/uart.h"
#include "mem/pool_alloc.h"
#include "kernel/task.h"

extern "C" {

/* Command line buffer */
#define CMD_BUF_SIZE 64

static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

/* ====== Command handlers ====== */

static void put_padded(const char *s, int width)
{
    int len = 0;
    const char *p = s;
    while (*p++) len++;
    uart_puts(s);
    for (int i = len; i < width; i++)
        uart_putc(' ');
}

static const char *state_name(task_state_t state)
{
    switch (state) {
        case TASK_STATE_FREE:    return "free";
        case TASK_STATE_READY:   return "ready";
        case TASK_STATE_RUNNING: return "run";
        case TASK_STATE_BLOCKED: return "block";
        case TASK_STATE_DEAD:    return "dead";
        default:                 return "???";
    }
}

static void cmd_ps(void)
{
    task_tcb_t *pool = sched_get_task_pool();

    uart_puts("ID  State  Ticks  Name\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        if (pool[i].state == TASK_STATE_FREE)
            continue;

        uart_put_dec(pool[i].id);
        uart_puts("   ");
        put_padded(state_name(pool[i].state), 7);
        uart_put_dec(pool[i].ticks_run);
        uart_puts("  ");
        uart_puts(pool[i].name ? pool[i].name : "?");
        uart_puts("\n");
    }
}

static void cmd_mem(void)
{
    uart_puts("Memory pool:\n");
    uart_puts("  Block size:  ");
    uart_put_dec(POOL_BLOCK_SIZE);
    uart_puts(" bytes\n");

    uart_puts("  Total:       ");
    uart_put_dec(POOL_NUM_BLOCKS);
    uart_puts(" blocks (");
    uart_put_dec(POOL_TOTAL_SIZE);
    uart_puts(" bytes)\n");

    uart_puts("  Free:        ");
    uart_put_dec(pool_free_count());
    uart_puts(" blocks\n");

    uart_puts("  Used:        ");
    uart_put_dec(pool_used_count());
    uart_puts(" blocks\n");
}

static void cmd_ticks(void)
{
    uint32_t t = get_tick_count();
    uart_puts("Tick count: ");
    uart_put_dec(t);
    uart_puts(" (");
    uart_put_dec(t / TICK_HZ);
    uart_puts(" seconds)\n");
}

static void cmd_help(void)
{
    uart_puts("OsitoK v" OSITO_VERSION_STRING " shell commands:\n");
    uart_puts("  ps      - list tasks\n");
    uart_puts("  mem     - memory pool status\n");
    uart_puts("  ticks   - uptime in ticks\n");
    uart_puts("  help    - this message\n");
    uart_puts("  reboot  - software reset\n");
}

static void cmd_reboot(void)
{
    uart_puts("Rebooting...\n");
    /* Wait for TX to finish */
    ets_delay_us(10000);
    software_reset();
}

/* ====== Command processing ====== */

static void process_command(const char *cmd)
{
    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;

    if (*cmd == '\0')
        return;

    if (ets_strcmp(cmd, "ps") == 0)
        cmd_ps();
    else if (ets_strcmp(cmd, "mem") == 0)
        cmd_mem();
    else if (ets_strcmp(cmd, "ticks") == 0)
        cmd_ticks();
    else if (ets_strcmp(cmd, "help") == 0)
        cmd_help();
    else if (ets_strcmp(cmd, "reboot") == 0)
        cmd_reboot();
    else {
        uart_puts("unknown command: ");
        uart_puts(cmd);
        uart_puts("\ntype 'help' for commands\n");
    }
}

/* ====== Shell task ====== */

void shell_task(void *arg)
{
    (void)arg;

    uart_puts("\nosito> ");

    for (;;) {
        int c = uart_getc();
        if (c < 0) {
            task_yield();
            continue;
        }

        if (c == '\r' || c == '\n') {
            uart_lock();
            uart_puts("\n");
            cmd_buf[cmd_pos] = '\0';
            process_command(cmd_buf);
            cmd_pos = 0;
            uart_puts("osito> ");
            uart_unlock();
        }
        else if (c == '\b' || c == 127) {
            /* Backspace */
            if (cmd_pos > 0) {
                cmd_pos--;
                uart_puts("\b \b"); /* Erase character on terminal */
            }
        }
        else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = (char)c;
            uart_putc((char)c); /* Echo */
        }
    }
}

} /* extern "C" */
