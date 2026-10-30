/*
 * SM8350 (ROG Phone 5) platform initialization
 */
#include "../../include/hal.h"
#include "../../include/types.h"
#include "sm8350.h"
#include "mmu.h"
#include "usb.h"
#include "pmic_arb.h"
#include "thermal.h"
#include "keys.h"
#include "ufs.h"
#include "touch.h"

/* Page allocator range. DDR bank 0 ends at 0xB5000000 (device memory node;
 * the next bank starts at 0xC0000000): the old 384 MB size ran 176 MB past
 * it, into the hole. The top 2 MB is the DMA window, reserved by mmu.c. */
#define SM8350_RAM_BASE  0xA8000000UL
#define SM8350_RAM_SIZE  (RAM_END - SM8350_RAM_BASE)    /* 208 MB */

uint64_t platform_ram_base(void) { return SM8350_RAM_BASE; }
uint64_t platform_ram_size(void) { return SM8350_RAM_SIZE; }

/* In screen debug mode, pixel rows above this belong to main.c's boot
 * progress bars (to 449), the cpufreq try bar (460-479) and the panic band
 * + cells (500-649); the text console gets the rest of the panel. Out of
 * debug mode it gets all of it (the panic band still goes over it). */
#define DEBUG_CONSOLE_TOP  700

void platform_init(void *dtb) {
    (void)dtb;
    uart_init();
    /* ABL leaves the splash framebuffer scanning out; there is no serial
     * cable, so it doubles as the console. */
    fb_init_platform(SPLASH_FB_BASE, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                     DISPLAY_WIDTH, 32);
    if (screen_debug) {
        fb_set_text_top(DEBUG_CONSOLE_TOP);
    } else {
        fb_set_text_top(0);
        fb_clear();                     /* ABL's splash */
    }
}

/* The panel ABL leaves scanning out: 6.78", 1080 x 2448 at ~395 ppi */
#define PANEL_W_MM  70
#define PANEL_H_MM  158

int platform_panel(uint64_t *base, uint32_t *w, uint32_t *h,
                   uint32_t *w_mm, uint32_t *h_mm) {
    *base = SPLASH_FB_BASE;
    *w = DISPLAY_WIDTH;
    *h = DISPLAY_HEIGHT;
    *w_mm = PANEL_W_MM;
    *h_mm = PANEL_H_MM;
    return 0;
}

/* The console back on the panel after the desktop, as the screen mode
 * wants it, laid out again from the log */
void platform_console_to_panel(void) {
    uint64_t flags = console_lock();
    fb_init_platform(SPLASH_FB_BASE, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_WIDTH, 32);
    fb_fill(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0);
    fb_set_text_top(screen_debug ? DEBUG_CONSOLE_TOP : 0);
    console_unlock(flags);
    console_redraw();
}

/* Shell `screen`: the mode; `screen debug` / `screen normal` switches it,
 * moving the console and laying it out again from the log */
void platform_screen_cmd(int argc, char **argv) {
    if (desktop_active) {
        serial_puts("screen: the desktop has the panel (desktop off)\n");
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "debug") == 0 || strcmp(argv[1], "normal") == 0)) {
        int debug = argv[1][0] == 'd';
        uint64_t flags = console_lock();
        screen_debug = debug;
        console_scroll(CONSOLE_LIVE);
        if (debug)
            fb_fill(0, 0, DISPLAY_WIDTH, DEBUG_CONSOLE_TOP, 0);
        fb_set_text_top(debug ? DEBUG_CONSOLE_TOP : 0);
        console_unlock(flags);
        console_redraw();
    } else if (argc != 1) {
        serial_puts("usage: screen [debug | normal]\n");
        return;
    }
    serial_puts(screen_debug
        ? "screen: debug (heartbeat squares, console from pixel row 700)\n"
        : "screen: normal (the console on the whole panel)\n");
}

void platform_probe_devices(void) {
    usb_probe();
    ufs_mount_at_boot();                /* the OsitoFS on userdata */
}

void platform_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

/* The restart reason ABL reads on the next boot: the PMK8350's (SID 0)
 * SDAM at 0x7100, byte 0x48, bits 7:1 (restart@48 under sdam@7100 in the
 * phone's device tree, the qcom,reboot-reason nvmem cell). Android's kernel
 * writes 2 there for `reboot bootloader`; it lives in the PMIC, so it
 * survives whatever reset PSCI does. */
#define PMK8350_SID             0
#define SDAM_RESTART_REASON     0x7148
#define RESTART_REASON_SHIFT    1
#define RESTART_BOOTLOADER      2

static const char *pmic_err(int err) {
    switch (err) {
    case PMIC_ERR_VERSION:    return "arbiter isn't v5";
    case PMIC_ERR_NO_CHANNEL: return "no arbiter channel for it";
    case PMIC_ERR_STATUS:     return "arbiter reported an error";
    case PMIC_ERR_TIMEOUT:    return "arbiter timed out";
    }
    return "?";
}

/* Reboot into ABL's fastboot mode (shell `reboot bootloader`), so a kernel
 * can be flashed without the buttons. Returns only if the reason couldn't
 * be set. */
void platform_reboot_bootloader(void) {
    uint8_t old, now;
    int err = pmic_read(PMK8350_SID, SDAM_RESTART_REASON, &old);
    if (!err)
        err = pmic_write(PMK8350_SID, SDAM_RESTART_REASON,
                         (old & 1) | (RESTART_BOOTLOADER << RESTART_REASON_SHIFT));
    if (!err)
        err = pmic_read(PMK8350_SID, SDAM_RESTART_REASON, &now);
    if (err || (now >> RESTART_REASON_SHIFT) != RESTART_BOOTLOADER) {
        serial_puts("reboot: could not set the restart reason: ");
        serial_puts(err ? pmic_err(err) : "it didn't stick");
        serial_puts("\n");
        return;
    }
    serial_puts("reboot: restart reason ");
    serial_puthex(old, 2);
    serial_puts(" -> ");
    serial_puthex(now, 2);
    serial_puts(", rebooting to fastboot\n");
    /* Let the host read that before the port goes away */
    for (int i = 0; i < 25000 && !acm_tx_idle(); i++) {
        uint64_t flags = console_lock();
        acm_poll();
        console_unlock(flags);
        udelay(20);
    }
    mdelay(200);
    platform_reboot();
}

/* Once the scheduler exists: the thermal limit, the keys, the touchscreen */
void platform_start_tasks(void) {
    thermal_start();
    keys_start();
    touch_start();
}

/* Shell `pmic`: the arbiter version and the restart reason; `pmic <sid>
 * <addr>` reads any register (hex addr, e.g. 7148); `pmic list <sid>` the
 * PMIC's peripherals and which EE 0 may write. Read-only. */
void platform_pmic_cmd(int argc, char **argv) {
    uint32_t sid = PMK8350_SID, addr = SDAM_RESTART_REASON;
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        pmic_list((uint8_t)(argv[2][0] - '0'));
        return;
    }
    if (argc == 3) {
        sid = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            sid = sid * 10 + (uint32_t)(*p - '0');
        addr = 0;
        for (const char *p = argv[2]; *p; p++) {
            char c = *p;
            int d = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            addr = addr * 16 + (uint32_t)d;
        }
    } else if (argc != 1) {
        serial_puts("usage: pmic [<sid> <hex addr> | list <sid>]\n");
        return;
    }
    serial_puts("arbiter version ");
    serial_puthex(pmic_arb_version(), 8);
    serial_puts("\n");
    uint8_t v;
    int err = pmic_read((uint8_t)sid, (uint16_t)addr, &v);
    serial_puts("sid ");
    serial_putdec(sid);
    serial_puts(" addr ");
    serial_puthex(addr, 4);
    serial_puts(": ");
    if (err) {
        serial_puts(pmic_err(err));
    } else {
        serial_puthex(v, 2);
    }
    serial_puts("\n");
}

uint64_t platform_gicd_base(void) { return 0x17A00000UL; }
uint64_t platform_gicr_base(void) { return 0x17A60000UL; }
