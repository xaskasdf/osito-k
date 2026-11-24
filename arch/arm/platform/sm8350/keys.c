/*
 * keys.c -- the ROG Phone 5's Power, Vol-Down and Vol-Up keys, over SPMI
 *
 * From the phone's device tree:
 *   Power     PMK8350 (SID 0) PON_HLOS at 0x1300, "kpdpwr": RT_STS bit 7
 *   Vol-Down  the same peripheral, "resin": RT_STS bit 6
 *   Vol-Up    PM8350 (SID 1) GPIO 6, gpio-keys: an input with pull-up, low
 *             while pressed; its RT_STS bit 0 reads the pin
 * as Linux's pm8941-pwrkey (PON_GEN3_*_N_SET) and pinctrl-spmi-gpio read
 * them. ABL leaves GPIO 6 set up as an input. Their interrupts go through
 * the arbiter, which Osito-K doesn't route, so a task polls every 20 ms and
 * takes a level once two polls in a row agree (the device tree debounces
 * 15 ms).
 *
 * What they do, without the USB console:
 *   Vol-Up / Vol-Down   page the screen back / forward through the console
 *                       log (console_scroll); held, they repeat
 *   both, held 2 s      reboot into fastboot (platform_reboot_bootloader)
 *   Power, under 1 s    a status summary on the console; with the desktop
 *                       up, the next window to the front
 * Holding Power ~10 s still resets the phone: that's the PMIC's own doing.
 * Every press and release also goes, as its PS/2 key (E0 5E power, E0 2E /
 * E0 30 volume), into the GUI's input queue.
 */

#include <stdbool.h>
#include "../../include/hal.h"
#include "../../include/types.h"
#include "pmic_arb.h"
#include "thermal.h"
#include "usb.h"
#include "keys.h"

#define POLL_MS         20
#define RT_STS          0x10        /* in each peripheral */
#define SCROLL_LINES    24          /* a Vol press: about half the text area */
#define REPEAT_AFTER_MS 500
#define REPEAT_MS       150
#define COMBO_MS        2000        /* both volume keys, to reboot */
#define TAP_MS          1000        /* a Power press shorter than this */

void input_post_key(uint8_t scancode, bool pressed, bool extended);

enum { KEY_POWER, KEY_VOLDOWN, KEY_VOLUP };

static const struct key {
    const char *name;
    uint8_t     sid;
    uint16_t    reg;
    uint8_t     bit;
    uint8_t     active_low;
    uint8_t     scancode;           /* PS/2 set 1, after E0 */
} keys[] = {
    [KEY_POWER]   = { "power",    0, 0x1300 + RT_STS, 7, 0, 0x5E },
    [KEY_VOLDOWN] = { "vol-down", 0, 0x1300 + RT_STS, 6, 0, 0x2E },
    [KEY_VOLUP]   = { "vol-up",   1, 0x8D00 + RT_STS, 0, 1, 0x30 },
};
#define NKEYS (int)(sizeof(keys) / sizeof(keys[0]))

static struct {
    int      down;                  /* debounced */
    int      last;                  /* the previous poll, -1 before the first */
    uint32_t presses;
    uint64_t since;                 /* timer_ms of the last press */
    uint64_t repeat;                /* volume: when it scrolls again if held */
    int      err;                   /* pmic_read's error; the key is dropped */
} state[NKEYS];

static int started;
static uint64_t combo_since;        /* both volume keys down since; 0 = not */
static int combo_fired;             /* the reboot was tried: once per hold */

static void status(void)
{
    console_scroll(CONSOLE_LIVE);
    /* timer_ms counts from the phone's boot; the tick count (100 Hz, as
     * `uptime`) from this kernel's */
    serial_puts("[STAT] up ");
    serial_putdec(timer_get_tick_count() / 100);
    serial_puts(" s, ");
    serial_putdec(smp_get_cpu_count());
    serial_puts(" CPUs, ");
    serial_putdec(mem_get_free() >> 20);
    serial_puts(" MB free, USB ");
    int usb = acm_state();
    serial_puts(usb == ACM_PORT_OPEN ? "port open\n" :
                usb == ACM_PORT_CLOSED ? "connected, port closed\n" : "not connected\n");
    serial_puts("[STAT] ");
    thermal_summary();
}

static void vol_scroll(int k)
{
    console_scroll(k == KEY_VOLUP ? SCROLL_LINES : -SCROLL_LINES);
    state[k].repeat = timer_ms() + REPEAT_MS;
}

/* After a poll: what the keys held now mean */
static void act(void)
{
    uint64_t now = timer_ms();
    int up = state[KEY_VOLUP].down, dn = state[KEY_VOLDOWN].down;

    if (up && dn) {
        if (!combo_since) {
            combo_since = now;
            console_scroll(CONSOLE_LIVE);
            serial_puts("[KEY ] both volume keys: hold 2 s to reboot into fastboot\n");
        } else if (!combo_fired && now - combo_since >= COMBO_MS) {
            combo_fired = 1;
            platform_reboot_bootloader();   /* returns only if it can't */
        }
        return;
    }
    if (combo_since) {
        /* Nothing scrolls until both are up again */
        if (!up && !dn)
            combo_since = combo_fired = 0;
        return;
    }
    for (int k = KEY_VOLDOWN; k <= KEY_VOLUP; k++)
        if (state[k].down && now >= state[k].repeat)
            vol_scroll(k);
}

static void edge(int k, int down)
{
    state[k].down = down;
    input_post_key(keys[k].scancode, down, true);
    if (down) {
        state[k].presses++;
        state[k].since = timer_ms();
        if (k != KEY_POWER && !combo_since) {
            vol_scroll(k);
            state[k].repeat = state[k].since + REPEAT_AFTER_MS;
        }
    } else if (k == KEY_POWER && timer_ms() - state[k].since < TAP_MS) {
        if (desktop_active)
            desktop_cycle_window();
        else
            status();
    }
}

static void keys_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int k = 0; k < NKEYS; k++) {
            if (state[k].err)
                continue;
            uint8_t v;
            int err = pmic_read(keys[k].sid, keys[k].reg, &v);
            if (err) {
                state[k].err = err;
                serial_puts("[KEY ] ");
                serial_puts(keys[k].name);
                serial_puts(": can't read it, dropped\n");
                continue;
            }
            int down = ((v >> keys[k].bit) & 1) != keys[k].active_low;
            if (down == state[k].last && down != state[k].down)
                edge(k, down);
            state[k].last = down;
        }
        act();
        task_delay_ms(POLL_MS);
    }
}

void keys_start(void)
{
    for (int k = 0; k < NKEYS; k++)
        state[k].last = -1;
    started = task_create_on("keys", keys_task, (void *)0, 1, TASK_ANY_CPU) >= 0;
    if (started)
        serial_puts("[KEY ] Vol-Up/Down: console history; both 2 s: fastboot; "
                    "Power: status\n");
}

/* Shell `keys`: each key now (read afresh) and since boot */
void platform_keys_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int k = 0; k < NKEYS; k++) {
        uint8_t v;
        int err = pmic_read(keys[k].sid, keys[k].reg, &v);
        serial_puts(keys[k].name);
        serial_puts(k == KEY_POWER ? "     " : k == KEY_VOLDOWN ? "  " : "    ");
        if (err) {
            serial_puts("can't read it\n");
            continue;
        }
        serial_puts(((v >> keys[k].bit) & 1) != keys[k].active_low ? "down" : "up  ");
        serial_puts("  pressed ");
        serial_putdec(state[k].presses);
        serial_puts("x");
        if (state[k].err)
            serial_puts(", not polled (read failed)");
        serial_puts("\n");
    }
    if (!started)
        serial_puts("(no polling task)\n");
    serial_puts("Vol-Up/Down page the screen through the console log; both held "
                "2 s reboot into fastboot; Power under 1 s prints a status\n");
}
