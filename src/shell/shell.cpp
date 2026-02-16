/*
 * OsitoK - Minimal interactive shell
 *
 * Commands:
 *   ps     - list all tasks with state and tick count
 *   mem    - show memory pool statistics
 *   ticks  - show current tick count
 *   gpio   - read/write GPIO pins
 *   help   - show available commands
 *   reboot - software reset
 */

#include "shell/shell.h"
#include "drivers/uart.h"
#include "drivers/gpio.h"
#include "mem/pool_alloc.h"
#include "mem/heap.h"
#include "kernel/task.h"
#include "kernel/mq.h"
#include "kernel/timer_sw.h"

/* IPC queue to heartbeat (defined in main.cpp) */
extern mq_t hb_mq;

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

    uart_puts("ID  Pri  State  Ticks  Name\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        if (pool[i].state == TASK_STATE_FREE)
            continue;

        uart_put_dec(pool[i].id);
        uart_puts("   ");
        uart_put_dec(pool[i].priority);
        uart_puts("    ");
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

static void cmd_heap(const char *args)
{
    while (*args == ' ') args++;

    if (ets_strcmp(args, "test") == 0) {
        /* Allocate, free, show coalescing */
        uart_puts("alloc a=100, b=200, c=50\n");
        void *a = heap_alloc(100);
        void *b = heap_alloc(200);
        void *c = heap_alloc(50);
        uart_puts("  a="); uart_put_hex((uint32_t)a);
        uart_puts("  b="); uart_put_hex((uint32_t)b);
        uart_puts("  c="); uart_put_hex((uint32_t)c);
        uart_puts("\n  used="); uart_put_dec(heap_used_total());
        uart_puts("  free="); uart_put_dec(heap_free_total());
        uart_puts("  frags="); uart_put_dec(heap_frag_count());
        uart_puts("\nfree b...\n");
        heap_free(b);
        uart_puts("  used="); uart_put_dec(heap_used_total());
        uart_puts("  free="); uart_put_dec(heap_free_total());
        uart_puts("  frags="); uart_put_dec(heap_frag_count());
        uart_puts("\nfree a, c...\n");
        heap_free(a);
        heap_free(c);
        uart_puts("  used="); uart_put_dec(heap_used_total());
        uart_puts("  free="); uart_put_dec(heap_free_total());
        uart_puts("  frags="); uart_put_dec(heap_frag_count());
        uart_puts("\n");
        return;
    }

    /* Default: show heap stats */
    uart_puts("Heap:\n");
    uart_puts("  Total:      ");
    uart_put_dec(HEAP_SIZE);
    uart_puts(" bytes\n");
    uart_puts("  Free:       ");
    uart_put_dec(heap_free_total());
    uart_puts(" bytes\n");
    uart_puts("  Used:       ");
    uart_put_dec(heap_used_total());
    uart_puts(" bytes\n");
    uart_puts("  Largest:    ");
    uart_put_dec(heap_largest_free());
    uart_puts(" bytes\n");
    uart_puts("  Fragments:  ");
    uart_put_dec(heap_frag_count());
    uart_puts("\n");
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
    uart_puts("  heap    - heap allocator status\n");
    uart_puts("  ticks   - uptime in ticks\n");
    uart_puts("  gpio    - read/write GPIO pins\n");
    uart_puts("  pri N P - set task N priority to P\n");
    uart_puts("  ping    - send IPC message to heartbeat\n");
    uart_puts("  timer   - test 1s software timer\n");
    uart_puts("  help    - this message\n");
    uart_puts("  reboot  - software reset\n");
}

static uint32_t ping_counter = 0;

static void cmd_ping(void)
{
    /* Send a message to heartbeat through the IPC queue */
    typedef struct { uint32_t value; } hb_msg_t;
    hb_msg_t msg;
    msg.value = ping_counter++;

    if (mq_trysend(&hb_mq, &msg) == 0) {
        uart_puts("ping ");
        uart_put_dec(msg.value);
        uart_puts(" -> heartbeat (queued: ");
        uart_put_dec(mq_count(&hb_mq));
        uart_puts(")\n");
    } else {
        uart_puts("queue full!\n");
    }
}

/* Software timer demo: fires once after 1 second */
static swtimer_t demo_timer;
static volatile uint32_t demo_timer_fired = 0;

static void demo_timer_cb(void *arg)
{
    (void)arg;
    demo_timer_fired++;
}

static void cmd_timer(void)
{
    swtimer_init(&demo_timer, demo_timer_cb, nullptr);
    demo_timer_fired = 0;
    swtimer_start(&demo_timer, 100, SWTIMER_ONESHOT);
    uart_puts("timer: armed 1s one-shot... ");

    /* Wait for it to fire (busy-wait is fine for demo) */
    uint32_t start = get_tick_count();
    while (!demo_timer_fired && (get_tick_count() - start) < 200) {
        task_yield();
    }

    if (demo_timer_fired) {
        uart_puts("FIRED! (");
        uart_put_dec(get_tick_count() - start);
        uart_puts(" ticks)\n");
    } else {
        uart_puts("timeout!\n");
    }
    uart_puts("active timers: ");
    uart_put_dec(swtimer_active_count());
    uart_puts("\n");
}

/* ====== GPIO command ====== */

static int parse_pin(const char *s, uint8_t *pin)
{
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return -1;
    uint8_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    if (val > 16) return -1;
    *pin = val;
    return 0;
}

static void cmd_gpio(const char *args)
{
    while (*args == ' ') args++;

    if (*args == '\0') {
        /* Show all safe pin states */
        struct { uint8_t pin; const char *label; } pins[] = {
            { 0, "D3"}, { 2, "D4/LED"}, { 4, "D2"}, { 5, "D1"},
            {12, "D6"}, {13, "D7"}, {14, "D5"}, {15, "D8"}, {16, "D0"},
        };
        uart_puts("Pin  Dir  Val  Wemos\n");
        for (int i = 0; i < 9; i++) {
            uint8_t p = pins[i].pin;
            if (p < 10) uart_puts(" ");
            uart_put_dec(p);
            uart_puts("   ");
            /* Direction */
            if (p == 16)
                uart_puts((RTC_GPIO_ENABLE & 1) ? "out" : "in ");
            else
                uart_puts((GPIO_ENABLE & (1 << p)) ? "out" : "in ");
            uart_puts("  ");
            uart_put_dec(gpio_read(p));
            uart_puts("    ");
            uart_puts(pins[i].label);
            uart_puts("\n");
        }
        return;
    }

    uint8_t pin;

    if (ets_strncmp(args, "read ", 5) == 0) {
        if (parse_pin(args + 5, &pin) < 0) {
            uart_puts("usage: gpio read <0-16>\n"); return;
        }
        uart_puts("GPIO");
        uart_put_dec(pin);
        uart_puts(" = ");
        uart_put_dec(gpio_read(pin));
        uart_puts("\n");
    }
    else if (ets_strncmp(args, "high ", 5) == 0) {
        if (parse_pin(args + 5, &pin) < 0) {
            uart_puts("usage: gpio high <0-16>\n"); return;
        }
        gpio_mode(pin, GPIO_MODE_OUTPUT);
        gpio_write(pin, 1);
        uart_puts("GPIO");
        uart_put_dec(pin);
        uart_puts(" -> HIGH\n");
    }
    else if (ets_strncmp(args, "low ", 4) == 0) {
        if (parse_pin(args + 4, &pin) < 0) {
            uart_puts("usage: gpio low <0-16>\n"); return;
        }
        gpio_mode(pin, GPIO_MODE_OUTPUT);
        gpio_write(pin, 0);
        uart_puts("GPIO");
        uart_put_dec(pin);
        uart_puts(" -> LOW\n");
    }
    else if (ets_strcmp(args, "blink") == 0) {
        uart_puts("blinking LED (GPIO2) 5x...\n");
        gpio_mode(2, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 5; i++) {
            gpio_write(2, 0);  /* LED on (active low) */
            task_delay_ticks(25);
            gpio_write(2, 1);  /* LED off */
            task_delay_ticks(25);
        }
        uart_puts("done\n");
    }
    else {
        uart_puts("gpio commands:\n");
        uart_puts("  gpio          - show all pins\n");
        uart_puts("  gpio read N   - read pin\n");
        uart_puts("  gpio high N   - set output high\n");
        uart_puts("  gpio low N    - set output low\n");
        uart_puts("  gpio blink    - blink LED (GPIO2)\n");
    }
}

static void cmd_pri(const char *args)
{
    while (*args == ' ') args++;

    uint8_t tid;
    if (parse_pin(args, &tid) < 0 || tid >= MAX_TASKS) {
        uart_puts("usage: pri <task_id> <priority>\n");
        return;
    }

    /* Skip past task id digits */
    while (*args >= '0' && *args <= '9') args++;
    while (*args == ' ') args++;

    uint8_t pri;
    if (*args < '0' || *args > '9') {
        uart_puts("usage: pri <task_id> <priority>\n");
        return;
    }
    pri = 0;
    while (*args >= '0' && *args <= '9') {
        pri = pri * 10 + (*args - '0');
        args++;
    }

    task_tcb_t *pool = sched_get_task_pool();
    if (pool[tid].state == TASK_STATE_FREE) {
        uart_puts("task not found\n");
        return;
    }

    uint8_t old = pool[tid].priority;
    pool[tid].priority = pri;
    uart_puts(pool[tid].name ? pool[tid].name : "?");
    uart_puts(": priority ");
    uart_put_dec(old);
    uart_puts(" -> ");
    uart_put_dec(pri);
    uart_puts("\n");
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
    else if (ets_strncmp(cmd, "heap", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0'))
        cmd_heap(cmd + 4);
    else if (ets_strcmp(cmd, "ticks") == 0)
        cmd_ticks();
    else if (ets_strcmp(cmd, "help") == 0)
        cmd_help();
    else if (ets_strncmp(cmd, "gpio", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0'))
        cmd_gpio(cmd + 4);
    else if (ets_strncmp(cmd, "pri ", 4) == 0)
        cmd_pri(cmd + 4);
    else if (ets_strcmp(cmd, "ping") == 0)
        cmd_ping();
    else if (ets_strcmp(cmd, "timer") == 0)
        cmd_timer();
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
