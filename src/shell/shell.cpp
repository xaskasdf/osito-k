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
#include "drivers/adc.h"
#include "drivers/input.h"
#include "drivers/video.h"
#include "mem/pool_alloc.h"
#include "mem/heap.h"
#include "fs/ositofs.h"
#include "kernel/task.h"
#include "kernel/timer_sw.h"
#include "math/fixedpoint.h"
#include "math/matrix3.h"
#include "gfx/wire3d.h"
#include "gfx/ships.h"
#include "game/game.h"

extern "C" {

/* Forth REPL and file runner (from zf_host.cpp) */
void forth_enter(void);
void forth_run(const char *filename);

/* Command line buffer */
#define CMD_BUF_SIZE 128

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

/* ====== Filesystem commands ====== */

static void cmd_fs(const char *args)
{
    while (*args == ' ') args++;

    if (*args == '\0' || ets_strcmp(args, "help") == 0) {
        uart_puts("fs commands:\n");
        uart_puts("  fs format          - create filesystem\n");
        uart_puts("  fs ls              - list files\n");
        uart_puts("  fs df              - free space\n");
        uart_puts("  fs cat NAME        - print file\n");
        uart_puts("  fs write NAME DATA - write text file\n");
        uart_puts("  fs overwrite NAME DATA - overwrite file\n");
        uart_puts("  fs append NAME DATA - append to file\n");
        uart_puts("  fs mv OLD NEW      - rename file\n");
        uart_puts("  fs rm NAME         - delete file\n");
        uart_puts("  fs xxd NAME        - hex dump file\n");
        uart_puts("  fs upload NAME SIZE - binary upload\n");
        return;
    }

    if (ets_strcmp(args, "format") == 0) {
        fs_format();
        return;
    }

    if (ets_strcmp(args, "ls") == 0) {
        fs_list();
        return;
    }

    if (ets_strcmp(args, "df") == 0) {
        if (!fs_mounted()) { uart_puts("fs: not mounted\n"); return; }
        uint32_t free_bytes = fs_free();
        uart_puts("Free: ");
        uart_put_dec(free_bytes / 1024);
        uart_puts(" KB (");
        uart_put_dec(free_bytes);
        uart_puts(" bytes)\n");
        return;
    }

    if (ets_strncmp(args, "cat ", 4) == 0) {
        const char *name = args + 4;
        while (*name == ' ') name++;
        if (*name == '\0') { uart_puts("usage: fs cat <name>\n"); return; }

        int size = fs_stat(name);
        if (size < 0) { uart_puts("not found\n"); return; }
        if (size == 0) { return; }

        /* Read in chunks using heap */
        uint32_t chunk = (uint32_t)size < 512 ? (uint32_t)size : 512;
        uint8_t *buf = (uint8_t *)heap_alloc(chunk);
        if (!buf) { uart_puts("no memory\n"); return; }

        int got = fs_read(name, buf, chunk);
        if (got > 0) {
            for (int i = 0; i < got; i++)
                uart_putc((char)buf[i]);
            if (buf[got - 1] != '\n')
                uart_puts("\n");
        }
        heap_free(buf);
        return;
    }

    if (ets_strncmp(args, "xxd ", 4) == 0) {
        const char *name = args + 4;
        while (*name == ' ') name++;
        if (*name == '\0') { uart_puts("usage: fs xxd <name>\n"); return; }

        int size = fs_stat(name);
        if (size < 0) { uart_puts("not found\n"); return; }

        uint32_t chunk = (uint32_t)size < 256 ? (uint32_t)size : 256;
        uint8_t *buf = (uint8_t *)heap_alloc(chunk);
        if (!buf) { uart_puts("no memory\n"); return; }

        int got = fs_read(name, buf, chunk);
        for (int i = 0; i < got; i++) {
            if (i % 16 == 0) {
                uart_put_hex(i);
                uart_puts(": ");
            }
            /* Print hex byte */
            const char hex[] = "0123456789abcdef";
            uart_putc(hex[(buf[i] >> 4) & 0xF]);
            uart_putc(hex[buf[i] & 0xF]);
            uart_putc(' ');
            if (i % 16 == 15 || i == got - 1)
                uart_puts("\n");
        }
        heap_free(buf);
        return;
    }

    if (ets_strncmp(args, "write ", 6) == 0) {
        const char *rest = args + 6;
        while (*rest == ' ') rest++;

        /* Extract filename (until next space) */
        char name[FS_NAME_LEN];
        int ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            name[ni++] = *rest++;
        name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs write <name> <data>\n"); return; }

        /* Skip space to get data */
        while (*rest == ' ') rest++;
        if (*rest == '\0') { uart_puts("usage: fs write <name> <data>\n"); return; }

        /* Data is the rest of the command line */
        uint32_t len = 0;
        const char *p = rest;
        while (*p++) len++;

        if (fs_create(name, rest, len) == 0) {
            uart_puts("wrote ");
            uart_put_dec(len);
            uart_puts(" bytes to '");
            uart_puts(name);
            uart_puts("'\n");
        }
        return;
    }

    if (ets_strncmp(args, "overwrite ", 10) == 0) {
        const char *rest = args + 10;
        while (*rest == ' ') rest++;

        char name[FS_NAME_LEN];
        int ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            name[ni++] = *rest++;
        name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs overwrite <name> <data>\n"); return; }
        while (*rest == ' ') rest++;
        if (*rest == '\0') { uart_puts("usage: fs overwrite <name> <data>\n"); return; }

        uint32_t len = 0;
        const char *p = rest;
        while (*p++) len++;

        if (fs_overwrite(name, rest, len) == 0) {
            uart_puts("wrote ");
            uart_put_dec(len);
            uart_puts(" bytes to '");
            uart_puts(name);
            uart_puts("'\n");
        }
        return;
    }

    if (ets_strncmp(args, "append ", 7) == 0) {
        const char *rest = args + 7;
        while (*rest == ' ') rest++;

        char name[FS_NAME_LEN];
        int ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            name[ni++] = *rest++;
        name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs append <name> <data>\n"); return; }
        while (*rest == ' ') rest++;
        if (*rest == '\0') { uart_puts("usage: fs append <name> <data>\n"); return; }

        uint32_t len = 0;
        const char *p = rest;
        while (*p++) len++;

        if (fs_append(name, rest, len) == 0) {
            uart_puts("appended ");
            uart_put_dec(len);
            uart_puts(" bytes to '");
            uart_puts(name);
            uart_puts("'\n");
        } else {
            uart_puts("append failed\n");
        }
        return;
    }

    if (ets_strncmp(args, "mv ", 3) == 0) {
        const char *rest = args + 3;
        while (*rest == ' ') rest++;

        char old_name[FS_NAME_LEN];
        int ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            old_name[ni++] = *rest++;
        old_name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs mv <old> <new>\n"); return; }
        while (*rest == ' ') rest++;

        char new_name[FS_NAME_LEN];
        ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            new_name[ni++] = *rest++;
        new_name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs mv <old> <new>\n"); return; }

        if (fs_rename(old_name, new_name) == 0) {
            uart_puts("renamed '");
            uart_puts(old_name);
            uart_puts("' -> '");
            uart_puts(new_name);
            uart_puts("'\n");
        } else {
            uart_puts("rename failed\n");
        }
        return;
    }

    if (ets_strncmp(args, "upload ", 7) == 0) {
        const char *rest = args + 7;
        while (*rest == ' ') rest++;

        /* Parse filename */
        char name[FS_NAME_LEN];
        int ni = 0;
        while (*rest && *rest != ' ' && ni < FS_NAME_LEN - 1)
            name[ni++] = *rest++;
        name[ni] = '\0';

        if (ni == 0) { uart_puts("usage: fs upload <name> <size>\n"); return; }
        while (*rest == ' ') rest++;

        /* Parse size */
        if (*rest < '0' || *rest > '9') {
            uart_puts("usage: fs upload <name> <size>\n");
            return;
        }
        uint32_t total_size = 0;
        while (*rest >= '0' && *rest <= '9') {
            total_size = total_size * 10 + (*rest - '0');
            rest++;
        }
        if (total_size == 0) { uart_puts("size must be > 0\n"); return; }

        /* fs_upload handles everything: alloc, UART read, flash write, ACK */
        fs_upload(name, total_size);
        return;
    }

    if (ets_strncmp(args, "rm ", 3) == 0) {
        const char *name = args + 3;
        while (*name == ' ') name++;
        if (*name == '\0') { uart_puts("usage: fs rm <name>\n"); return; }

        if (fs_delete(name) == 0)
            uart_puts("deleted\n");
        else
            uart_puts("not found\n");
        return;
    }

    uart_puts("unknown fs command (try 'fs help')\n");
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
    uart_puts("  fs      - filesystem commands\n");
    uart_puts("  pri N P - set task N priority to P\n");
    uart_puts("  timer   - test 1s software timer\n");
    uart_puts("  run F   - run .zf Forth script\n");
    uart_puts("  forth   - Forth REPL\n");
    uart_puts("  joy     - joystick live monitor\n");
    uart_puts("  fbtest  - framebuffer test pattern\n");
    uart_puts("  fixtest - fixed-point math test\n");
    uart_puts("  mat3test- 3D matrix/vector test\n");
    uart_puts("  wiretest- wireframe cube (static)\n");
    uart_puts("  wirespin- wireframe cube (anim)\n");
    uart_puts("  ship    - show Elite ship model\n");
    uart_puts("  shipspin- spin all ships (anim)\n");
    uart_puts("  elite   - Elite flight demo\n");
    uart_puts("  uname   - system info\n");
    uart_puts("  help    - this message\n");
    uart_puts("  reboot  - software reset\n");
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

static void cmd_uname(void)
{
    uart_puts("OsitoK v" OSITO_VERSION_STRING " xtensa-lx106 ESP8266 @ ");
    uart_put_dec(CPU_FREQ_HZ / 1000000);
    uart_puts("MHz DRAM:");
    uart_put_dec((DRAM_END - DRAM_START + 1) / 1024);
    uart_puts("KB IRAM:");
    uart_put_dec((IRAM_END - IRAM_START + 1) / 1024);
    uart_puts("KB tick:");
    uart_put_dec(TICK_HZ);
    uart_puts("Hz tasks:");
    uart_put_dec(MAX_TASKS);
    uart_puts("\n");
}

/* ====== Forth run command ====== */

static void cmd_run(const char *args)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        uart_puts("usage: run <file.zf>\n");
        return;
    }
    forth_run(args);
}

static void cmd_joy(void)
{
    uart_puts("Joystick (Ctrl+C to exit)\n");

    for (;;) {
        /* Check for Ctrl+C */
        if (uart_rx_available()) {
            int ch = uart_getc();
            if (ch == 0x03) {
                uart_puts("\n");
                return;
            }
        }

        uint16_t x = adc_read();
        uint32_t state = input_get_state();
        uint8_t btn = (state >> 16) & 1;

        uart_puts("X=");
        uart_put_dec(x);
        uart_puts(" btn=");
        uart_puts(btn ? "DOWN" : "UP  ");

        /* Show recent events */
        input_event_t ev;
        while ((ev = input_poll()) != INPUT_NONE) {
            uart_puts(" ");
            switch (ev) {
                case INPUT_LEFT:    uart_puts("L"); break;
                case INPUT_RIGHT:   uart_puts("R"); break;
                case INPUT_PRESS:   uart_puts("P"); break;
                case INPUT_RELEASE: uart_puts("r"); break;
                default: break;
            }
        }

        uart_puts("\r");
        task_delay_ticks(10);  /* ~100ms update rate */
    }
}

static void cmd_adc(void)
{
    /* Dump I2C registers for SAR ADC */
    adc_debug();

    /* 3 consecutive reads */
    uart_puts("Read 1: ");
    uart_put_dec(adc_read());
    uart_puts("\nRead 2: ");
    uart_put_dec(adc_read());
    uart_puts("\nRead 3: ");
    uart_put_dec(adc_read());
    uart_puts("\n");
}

static void cmd_fbtest(void)
{
    uart_puts("fb: drawing test pattern...\n");

    fb_clear();

    /* Border */
    fb_line(0, 0, FB_WIDTH - 1, 0);
    fb_line(FB_WIDTH - 1, 0, FB_WIDTH - 1, FB_HEIGHT - 1);
    fb_line(FB_WIDTH - 1, FB_HEIGHT - 1, 0, FB_HEIGHT - 1);
    fb_line(0, FB_HEIGHT - 1, 0, 0);

    /* Title text at top */
    fb_text_puts(2, 1, "OsitoK v0.1");

    /* Info text */
    fb_text_puts(2, 3, "128x64 framebuffer");
    fb_text_puts(2, 4, "4x6 font 32x10 grid");
    fb_text_puts(2, 6, "ABCDEFGHIJ0123456789");
    fb_text_puts(2, 8, "Ready.");

    fb_flush();
    uart_puts("fb: flushed 1028 bytes\n");
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
    else if (ets_strncmp(cmd, "fs", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0'))
        cmd_fs(cmd + 2);
    else if (ets_strncmp(cmd, "pri ", 4) == 0)
        cmd_pri(cmd + 4);
    else if (ets_strcmp(cmd, "timer") == 0)
        cmd_timer();
    else if (ets_strncmp(cmd, "run", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0'))
        cmd_run(cmd + 3);
    else if (ets_strcmp(cmd, "joy") == 0)
        cmd_joy();
    else if (ets_strcmp(cmd, "adc") == 0)
        cmd_adc();
    else if (ets_strcmp(cmd, "fbtest") == 0)
        cmd_fbtest();
    else if (ets_strcmp(cmd, "forth") == 0)
        forth_enter();
    else if (ets_strcmp(cmd, "fixtest") == 0)
        fix_test();
    else if (ets_strcmp(cmd, "mat3test") == 0)
        mat3_test();
    else if (ets_strcmp(cmd, "wiretest") == 0)
        wire_test();
    else if (ets_strcmp(cmd, "wirespin") == 0)
        wire_spin();
    else if (ets_strncmp(cmd, "ship", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0'))
        cmd_ship(cmd + 4);
    else if (ets_strcmp(cmd, "shipspin") == 0)
        cmd_shipspin();
    else if (ets_strcmp(cmd, "elite") == 0)
        game_elite();
    else if (ets_strcmp(cmd, "uname") == 0)
        cmd_uname();
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
