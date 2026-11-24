/*
 * OsitoK x86-64 — Virtual Terminal Multiplexer
 *
 * Provides Alt+F1/F2/F3 switching between multiple virtual consoles.
 * Each VT has its own text buffer and cursor position.
 * Similar to Linux VT subsystem.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void fb_putc(char c, uint32_t color);
extern void fb_clear(void);

/* ── Virtual Terminal ────────────────────────────────────────── */

#define VT_MAX      4
#define VT_ROWS    50
#define VT_COLS   160

typedef struct {
    char     buf[VT_ROWS][VT_COLS];
    uint32_t colors[VT_ROWS][VT_COLS];
    int      cursor_row;
    int      cursor_col;
    int      scroll_top;
    bool     active;
} vt_t;

static vt_t *vts;   /* Allocated on first init (saves ~160KB BSS) */
static int  current_vt;
static bool vt_initialized;

/* ── Init ────────────────────────────────────────────────────── */

void vt_init(void)
{
    extern void *kmalloc(uint64_t);
    vts = (vt_t *)kmalloc(VT_MAX * sizeof(vt_t));
    if (!vts) { serial_puts("[VT] Failed to allocate\n"); return; }
    for (int i = 0; i < VT_MAX; i++) {
        memset(&vts[i], 0, sizeof(vt_t));
        vts[i].active = true;
    }
    current_vt = 0;
    vt_initialized = true;
    serial_puts("[VT] Virtual terminals initialized (");
    serial_putdec(VT_MAX);
    serial_puts(" VTs)\n");
}

/* ── Switch VT ───────────────────────────────────────────────── */

void vt_switch(int vt_num)
{
    if (!vt_initialized) return;
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    if (vt_num == current_vt) return;

    current_vt = vt_num;
    serial_puts("[VT] Switched to VT ");
    serial_putdec((uint64_t)vt_num);
    serial_puts("\n");

    /* Redraw current VT (in text mode, refresh framebuffer) */
    /* For GUI mode, compositor handles this via terminal surface */
}

int vt_get_current(void) { return current_vt; }

/* ── Write to VT ─────────────────────────────────────────────── */

void vt_putc(int vt_num, char c, uint32_t color)
{
    if (!vt_initialized || vt_num < 0 || vt_num >= VT_MAX) return;
    vt_t *vt = &vts[vt_num];

    if (c == '\n') {
        vt->cursor_col = 0;
        vt->cursor_row++;
    } else if (c == '\r') {
        vt->cursor_col = 0;
    } else if (c == '\b') {
        if (vt->cursor_col > 0) vt->cursor_col--;
    } else {
        if (vt->cursor_col < VT_COLS && vt->cursor_row < VT_ROWS) {
            vt->buf[vt->cursor_row][vt->cursor_col] = c;
            vt->colors[vt->cursor_row][vt->cursor_col] = color;
            vt->cursor_col++;
        }
    }

    /* Line wrap */
    if (vt->cursor_col >= VT_COLS) {
        vt->cursor_col = 0;
        vt->cursor_row++;
    }

    /* Scroll */
    if (vt->cursor_row >= VT_ROWS) {
        for (int r = 0; r < VT_ROWS - 1; r++) {
            memcpy(vt->buf[r], vt->buf[r + 1], VT_COLS);
            memcpy(vt->colors[r], vt->colors[r + 1], VT_COLS * 4);
        }
        memset(vt->buf[VT_ROWS - 1], 0, VT_COLS);
        memset(vt->colors[VT_ROWS - 1], 0, VT_COLS * 4);
        vt->cursor_row = VT_ROWS - 1;
    }

    /* If this is the active VT, also write to framebuffer */
    if (vt_num == current_vt)
        fb_putc(c, color);
}

/* Write string to VT */
void vt_puts(int vt_num, const char *s, uint32_t color)
{
    while (*s) vt_putc(vt_num, *s++, color);
}

/* ── Check for VT switch key (called from keyboard handler) ──── */

/* Returns true if the key was consumed (Alt+Fn switch) */
bool vt_check_switch(uint8_t scancode, bool alt_held)
{
    if (!vt_initialized || !alt_held) return false;

    /* PS/2 scancodes: F1=0x3B, F2=0x3C, F3=0x3D, F4=0x3E */
    int target = -1;
    if (scancode == 0x3B) target = 0;
    else if (scancode == 0x3C) target = 1;
    else if (scancode == 0x3D) target = 2;
    else if (scancode == 0x3E) target = 3;

    if (target >= 0) {
        vt_switch(target);
        return true;
    }
    return false;
}

/* Get VT buffer line (for GUI rendering) */
const char *vt_get_line(int vt_num, int row)
{
    if (!vt_initialized || vt_num < 0 || vt_num >= VT_MAX) return "";
    if (row < 0 || row >= VT_ROWS) return "";
    return vts[vt_num].buf[row];
}
