/*
 * OsitoK — VGA Text Mode Emulation on GOP Framebuffer
 *
 * CGA/VGA text and indexed mode presentation on the GOP framebuffer.
 */

#include "cpu8086.h"
#include "dos_io.h"
#include "dos_vbe.h"
#include "../include/paging.h"

extern uint32_t *fb_get_base(void);
extern uint32_t  fb_get_width(void);
extern uint32_t  fb_get_height(void);
extern uint32_t  fb_get_pitch(void);
extern void      fb_present_rows(uint32_t top, uint32_t bottom);
extern const uint8_t gui_font8x16[95][16];
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);

void dos_vga_flush(dos_vm_t *vm);

static const uint32_t dos_vga_text_palette[16] = {
    0xFF000000U, 0xFF0000AAU, 0xFF00AA00U, 0xFF00AAAAU,
    0xFFAA0000U, 0xFFAA00AAU, 0xFFAA5500U, 0xFFAAAAAAU,
    0xFF555555U, 0xFF5555FFU, 0xFF55FF55U, 0xFF55FFFFU,
    0xFFFF5555U, 0xFFFF55FFU, 0xFFFFFF55U, 0xFFFFFFFFU,
};

/* Timer callbacks may run under the client's private CR3. Keep only guest
 * memory values mapped there; the dos_vm object itself lives on the shell
 * stack and must never be dereferenced from the timer path. */
static uint8_t *dos_vga_bound_memory;
static uint32_t dos_vga_bound_memory_size;
static uint8_t dos_vga_bound_mode;
static dos_vm_t *dos_vga_bound_vm;
static bool dos_vga_vbe_active;
static uint16_t dos_vga_vbe_width;
static uint16_t dos_vga_vbe_height;
static uint16_t dos_vga_vbe_pitch;
static uint16_t dos_vga_vbe_display_x;
static uint16_t dos_vga_vbe_display_y;
static uint8_t dos_vga_vbe_bpp;
static uint8_t dos_vga_vbe_bytes_per_pixel;
static bool dos_vga_clear_surface;
static bool dos_vga_mode13_dirty;
static bool dos_vga_direct_writes;
static bool dos_vga_mode13_reported;
static bool dos_vga_mode13_content_reported;
static bool dos_vga_vbe_reported;
static uint8_t dos_vga_direct_refresh_phase;
static uint16_t dos_vga_mouse_x;
static uint16_t dos_vga_mouse_y;
static bool dos_vga_mouse_visible;

/* Shadow of the virtual VGA DAC palette (6-bit per channel). */
static uint8_t dos_vga_dac[256][3];

void dos_vga_bind_vm(dos_vm_t *vm)
{
    dos_vga_bound_vm = vm;
    dos_vga_bound_memory = vm ? (uint8_t *)PHYS_TO_VIRT(vm->mem) : NULL;
    dos_vga_bound_memory_size = vm ? vm->total_mem_size : 0;
    dos_vga_bound_mode = vm ? vm->vga_mode : 0;
    dos_vga_vbe_active = vm ? vm->vbe_active : false;
    dos_vga_vbe_width = vm ? vm->vbe_width : 0;
    dos_vga_vbe_height = vm ? vm->vbe_height : 0;
    dos_vga_vbe_pitch = vm ? vm->vbe_pitch : 0;
    dos_vga_vbe_display_x = vm ? vm->vbe_display_x : 0;
    dos_vga_vbe_display_y = vm ? vm->vbe_display_y : 0;
    dos_vga_vbe_bpp = vm ? vm->vbe_bpp : 0;
    dos_vga_vbe_bytes_per_pixel = vm ? vm->vbe_bytes_per_pixel : 0;
    dos_vga_clear_surface = vm != NULL;
    dos_vga_mode13_dirty = vm != NULL;
    dos_vga_direct_writes = false;
    dos_vga_mode13_reported = false;
    dos_vga_mode13_content_reported = false;
    dos_vga_vbe_reported = false;
    dos_vga_direct_refresh_phase = 0;
    dos_vga_mouse_x = vm ? vm->mouse_x : 0;
    dos_vga_mouse_y = vm ? vm->mouse_y : 0;
    dos_vga_mouse_visible = vm ? vm->mouse_visible : false;
    if (!vm || !dos_io_copy_dac(vm, dos_vga_dac))
        memset(dos_vga_dac, 0, sizeof(dos_vga_dac));
}

void dos_vga_unbind_vm(dos_vm_t *vm)
{
    if (vm == dos_vga_bound_vm)
        dos_vga_bind_vm(NULL);
}

void dos_vga_set_mode(uint8_t mode)
{
    if (dos_vga_bound_memory) {
        dos_vga_bound_mode = mode;
        dos_vga_vbe_active = false;
        dos_vga_clear_surface = true;
        dos_vga_mode13_dirty = true;
        dos_vga_mode13_reported = false;
        dos_vga_mode13_content_reported = false;
        dos_vga_vbe_reported = false;
        dos_vga_direct_refresh_phase = 0;
    }
}

void dos_vga_vbe_update(dos_vm_t *vm)
{
    if (!vm || vm != dos_vga_bound_vm) return;
    bool geometry_changed = dos_vga_vbe_active != vm->vbe_active ||
        dos_vga_vbe_width != vm->vbe_width ||
        dos_vga_vbe_height != vm->vbe_height ||
        dos_vga_vbe_pitch != vm->vbe_pitch ||
        dos_vga_vbe_bpp != vm->vbe_bpp ||
        dos_vga_vbe_bytes_per_pixel != vm->vbe_bytes_per_pixel;
    dos_vga_bound_mode = vm->vga_mode;
    dos_vga_vbe_active = vm->vbe_active;
    dos_vga_vbe_width = vm->vbe_width;
    dos_vga_vbe_height = vm->vbe_height;
    dos_vga_vbe_pitch = vm->vbe_pitch;
    dos_vga_vbe_display_x = vm->vbe_display_x;
    dos_vga_vbe_display_y = vm->vbe_display_y;
    dos_vga_vbe_bpp = vm->vbe_bpp;
    dos_vga_vbe_bytes_per_pixel = vm->vbe_bytes_per_pixel;
    dos_vga_mode13_dirty = true;
    if (geometry_changed) {
        dos_vga_clear_surface = true;
        dos_vga_vbe_reported = false;
        dos_vga_direct_refresh_phase = 0;
    }
}

void dos_vga_dac_update(dos_vm_t *vm, uint8_t index,
                        uint8_t component, uint8_t value)
{
    if (vm == dos_vga_bound_vm && component < 3U) {
        dos_vga_dac[index][component] = value & 0x3FU;
        dos_vga_mode13_dirty = true;
    }
}

void dos_vga_set_direct_writes(dos_vm_t *vm, bool enabled)
{
    if (vm != dos_vga_bound_vm)
        return;
    dos_vga_direct_writes = enabled;
    if (!enabled)
        dos_vga_mode13_dirty = true;
}

void dos_vga_mouse_update(dos_vm_t *vm)
{
    if (!vm || vm != dos_vga_bound_vm)
        return;

    bool changed = dos_vga_mouse_x != vm->mouse_x ||
                   dos_vga_mouse_y != vm->mouse_y ||
                   dos_vga_mouse_visible != vm->mouse_visible;
    dos_vga_mouse_x = vm->mouse_x;
    dos_vga_mouse_y = vm->mouse_y;
    dos_vga_mouse_visible = vm->mouse_visible;
    if (!changed) return;

    if (dos_vga_bound_mode == 0x13 || dos_vga_vbe_active) {
        dos_vga_mode13_dirty = true;
    } else if (dos_vga_bound_mode == 0x03) {
        dos_vga_flush(vm);
    }
}

static bool dos_vga_arrow_inside(int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= 14 || y >= 14) return false;
    if (x == 0 || (y <= 10 && x <= y)) return true;
    if (y > 10 && x <= 13 - y) return true;
    if (y >= 3 && y <= 10 && x >= 3 && x <= 10) {
        int32_t diagonal = x + y - 13;
        return diagonal >= -1 && diagonal <= 1;
    }
    return false;
}

static void dos_vga_draw_mode13_mouse(uint32_t *fb, uint32_t pitch,
                                      uint32_t origin_x, uint32_t origin_y)
{
    if (!dos_vga_mouse_visible) return;

    int32_t cursor_x = (int32_t)(dos_vga_mouse_x / 2U);
    int32_t cursor_y = (int32_t)dos_vga_mouse_y;
    for (int pass = 0; pass < 2; pass++) {
        int32_t shift = pass == 0 ? 1 : 0;
        uint32_t color = pass == 0 ? 0xFF000000U : 0xFFFFFFFFU;
        for (int32_t y = 0; y < 14; y++) {
            int32_t py = cursor_y + y + shift;
            if (py < 0 || py >= 200) continue;
            for (int32_t x = 0; x < 14; x++) {
                if (!dos_vga_arrow_inside(x, y)) continue;
                int32_t px = cursor_x + x + shift;
                if (px < 0 || px >= 320) continue;
                fb[(origin_y + (uint32_t)py) * pitch + origin_x +
                   (uint32_t)px] = color;
            }
        }
    }
}

static uint8_t dos_vga_expand_5(uint32_t value)
{
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t dos_vga_expand_6(uint32_t value)
{
    return (uint8_t)((value << 2) | (value >> 4));
}

static uint32_t dos_vga_vbe_pixel(const uint8_t *source)
{
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;

    if (dos_vga_vbe_bpp == 8u) {
        uint8_t index = source[0];
        red = dos_vga_expand_6(dos_vga_dac[index][0]);
        green = dos_vga_expand_6(dos_vga_dac[index][1]);
        blue = dos_vga_expand_6(dos_vga_dac[index][2]);
    } else if (dos_vga_vbe_bpp == 15u) {
        uint16_t pixel = (uint16_t)(source[0] |
                                    ((uint16_t)source[1] << 8));
        red = dos_vga_expand_5((pixel >> 10) & 0x1Fu);
        green = dos_vga_expand_5((pixel >> 5) & 0x1Fu);
        blue = dos_vga_expand_5(pixel & 0x1Fu);
    } else if (dos_vga_vbe_bpp == 16u) {
        uint16_t pixel = (uint16_t)(source[0] |
                                    ((uint16_t)source[1] << 8));
        red = dos_vga_expand_5((pixel >> 11) & 0x1Fu);
        green = dos_vga_expand_6((pixel >> 5) & 0x3Fu);
        blue = dos_vga_expand_5(pixel & 0x1Fu);
    } else if (dos_vga_vbe_bpp == 24u) {
        blue = source[0];
        green = source[1];
        red = source[2];
    }
    return 0xFF000000u | (red << 16) | (green << 8) | blue;
}

static void dos_vga_draw_vbe_mouse(uint32_t *fb, uint32_t pitch,
                                   uint32_t origin_x, uint32_t origin_y,
                                   uint32_t display_width,
                                   uint32_t display_height)
{
    if (!dos_vga_mouse_visible || !dos_vga_vbe_width ||
        !dos_vga_vbe_height || !display_width || !display_height)
        return;

    int32_t cursor_x = (int32_t)((uint64_t)dos_vga_mouse_x *
                                 display_width / dos_vga_vbe_width);
    int32_t cursor_y = (int32_t)((uint64_t)dos_vga_mouse_y *
                                 display_height / dos_vga_vbe_height);
    for (int pass = 0; pass < 2; pass++) {
        int32_t shift = pass == 0 ? 1 : 0;
        uint32_t color = pass == 0 ? 0xFF000000u : 0xFFFFFFFFu;
        for (int32_t y = 0; y < 14; y++) {
            int32_t py = cursor_y + y + shift;
            if (py < 0 || (uint32_t)py >= display_height) continue;
            for (int32_t x = 0; x < 14; x++) {
                if (!dos_vga_arrow_inside(x, y)) continue;
                int32_t px = cursor_x + x + shift;
                if (px < 0 || (uint32_t)px >= display_width) continue;
                fb[(origin_y + (uint32_t)py) * pitch + origin_x +
                   (uint32_t)px] = color;
            }
        }
    }
}

static void dos_vga_vbe_present(void)
{
    if (!dos_vga_bound_memory || !dos_vga_vbe_active ||
        !dos_vga_vbe_width || !dos_vga_vbe_height ||
        !dos_vga_vbe_pitch || !dos_vga_vbe_bytes_per_pixel ||
        dos_vga_bound_memory_size < DOS_VBE_FB_BASE + DOS_VBE_FB_SIZE ||
        (!dos_vga_mode13_dirty && !dos_vga_direct_writes))
        return;

    if (!dos_vga_mode13_dirty && dos_vga_direct_writes) {
        /* Native writes bypass dirty tracking. Limit conversion work to one
         * third of the timer/service cadence. */
        dos_vga_direct_refresh_phase++;
        if (dos_vga_direct_refresh_phase < 3u) return;
    }
    dos_vga_direct_refresh_phase = 0;

    uint32_t logical_width =
        dos_vga_vbe_pitch / dos_vga_vbe_bytes_per_pixel;
    uint32_t maximum_lines = DOS_VBE_FB_SIZE / dos_vga_vbe_pitch;
    if ((uint32_t)dos_vga_vbe_display_x + dos_vga_vbe_width >
            logical_width ||
        (uint32_t)dos_vga_vbe_display_y + dos_vga_vbe_height >
            maximum_lines)
        return;

    uint32_t *fb = fb_get_base();
    uint32_t fw = fb_get_width();
    uint32_t fh = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    if (!fb || !fw || !fh || pitch < fw) return;

    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t display_width;
    uint32_t display_height;
    dos_vbe_fit_rect(dos_vga_vbe_width, dos_vga_vbe_height, fw, fh,
                     &origin_x, &origin_y, &display_width, &display_height);
    if (!display_width || !display_height) return;

    uint32_t damage_top = origin_y;
    uint32_t damage_bottom = origin_y + display_height;
    if (dos_vga_clear_surface) {
        for (uint32_t y = 0; y < fh; y++) {
            uint32_t *row = fb + y * pitch;
            for (uint32_t x = 0; x < fw; x++)
                row[x] = 0xFF000000u;
        }
        dos_vga_clear_surface = false;
        damage_top = 0;
        damage_bottom = fh;
    }

    const uint8_t *vram = dos_vga_bound_memory + DOS_VBE_FB_BASE;
    for (uint32_t y = 0; y < display_height; y++) {
        uint32_t source_y = dos_vga_vbe_display_y +
            (uint32_t)((uint64_t)y * dos_vga_vbe_height / display_height);
        const uint8_t *source_row = vram + source_y * dos_vga_vbe_pitch;
        uint32_t *destination =
            fb + (origin_y + y) * pitch + origin_x;
        for (uint32_t x = 0; x < display_width; x++) {
            uint32_t source_x = dos_vga_vbe_display_x +
                (uint32_t)((uint64_t)x * dos_vga_vbe_width /
                           display_width);
            destination[x] = dos_vga_vbe_pixel(
                source_row + source_x * dos_vga_vbe_bytes_per_pixel);
        }
    }
    dos_vga_draw_vbe_mouse(fb, pitch, origin_x, origin_y,
                           display_width, display_height);

    if (!dos_vga_vbe_reported) {
        serial_puts("[DOS/VBE] GOP presenter ");
        serial_putdec(dos_vga_vbe_width);
        serial_puts("x");
        serial_putdec(dos_vga_vbe_height);
        serial_puts("x");
        serial_putdec(dos_vga_vbe_bpp);
        serial_puts(" -> ");
        serial_putdec(display_width);
        serial_puts("x");
        serial_putdec(display_height);
        serial_puts("\n");
        dos_vga_vbe_reported = true;
    }
    fb_present_rows(damage_top, damage_bottom);
    dos_vga_mode13_dirty = false;
}

/* Present indexed VGA or VBE video to the GOP framebuffer. This symbol keeps
 * its historical name because both the interpreter and timer call it. */
void dos_vga_mode13_present(void)
{
    if (dos_vga_vbe_active) {
        dos_vga_vbe_present();
        return;
    }
    if (!dos_vga_bound_memory || dos_vga_bound_mode != 0x13 ||
        dos_vga_bound_memory_size < DOS_CONV_TOP + 320U * 200U ||
        (!dos_vga_mode13_dirty && !dos_vga_direct_writes))
        return;

    uint32_t *fb = fb_get_base();
    uint32_t  fw = fb_get_width();
    uint32_t  fh = fb_get_height();
    uint32_t  pp = fb_get_pitch();
    if (!fb || fw < 320 || fh < 200) return;

    const uint8_t *vram = &dos_vga_bound_memory[DOS_CONV_TOP];
    uint32_t ox = (fw - 320) / 2;
    uint32_t oy = (fh - 200) / 2;
    uint32_t damage_top = oy;
    uint32_t damage_bottom = oy + 200U;
    bool has_content = false;
    uint8_t first_index = 0;
    if (dos_vga_clear_surface) {
        for (uint32_t y = 0; y < fh; y++) {
            uint32_t *row = fb + y * pp;
            for (uint32_t x = 0; x < fw; x++)
                row[x] = 0xFF000000U;
        }
        dos_vga_clear_surface = false;
        damage_top = 0;
        damage_bottom = fh;
    }
    for (uint32_t y = 0; y < 200; y++) {
        uint32_t *dst = fb + (oy + y) * pp + ox;
        const uint8_t *src = vram + y * 320;
        for (uint32_t x = 0; x < 320; x++) {
            uint8_t v = src[x];
            if (!has_content && v != 0) {
                has_content = true;
                first_index = v;
            }
            uint32_t r = (uint32_t)((dos_vga_dac[v][0] << 2) |
                                    (dos_vga_dac[v][0] >> 4));
            uint32_t g = (uint32_t)((dos_vga_dac[v][1] << 2) |
                                    (dos_vga_dac[v][1] >> 4));
            uint32_t b = (uint32_t)((dos_vga_dac[v][2] << 2) |
                                    (dos_vga_dac[v][2] >> 4));
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    dos_vga_draw_mode13_mouse(fb, pp, ox, oy);
    if (!dos_vga_mode13_reported) {
        serial_puts("[DOS/VGA] mode 13h presenter active, DAC[1]=");
        serial_puthex(dos_vga_dac[1][0], 2);
        serial_puts(",");
        serial_puthex(dos_vga_dac[1][1], 2);
        serial_puts(",");
        serial_puthex(dos_vga_dac[1][2], 2);
        serial_puts("\n");
        dos_vga_mode13_reported = true;
    }
    if (has_content && !dos_vga_mode13_content_reported) {
        serial_puts("[DOS/VGA] mode 13h first nonzero palette index ");
        serial_puthex(first_index, 2);
        serial_puts("\n");
        dos_vga_mode13_content_reported = true;
    }
    fb_present_rows(damage_top, damage_bottom);
    dos_vga_mode13_dirty = false;
}

void dos_vga_invalidate_text(dos_vm_t *vm)
{
    if (!vm) return;
    memset(vm->vga_dirty, 0xFF, sizeof(vm->vga_dirty));
    vm->vga_render_valid = false;
}

static void dos_vga_dirty_cell(dos_vm_t *vm, uint32_t cell)
{
    if (cell < 80U * 25U)
        vm->vga_dirty[cell >> 3] |= (uint8_t)(1U << (cell & 7U));
}

static void dos_vga_draw_pixel(uint32_t *target, uint32_t offset,
                               uint32_t color)
{
    target[offset] = color;
}

/* Flush dirty cells from the active 80x25 text page. */
void dos_vga_flush(dos_vm_t *vm)
{
    if (!vm || !vm->mem || vm->vga_mode != 0x03)
        return;

    uint32_t *target = fb_get_base();
    uint32_t width = fb_get_width();
    uint32_t height = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    if (!target || width < 640U || height < 400U || pitch < width)
        return;

    uint8_t page = vm->vga_page & 7U;
    bool full_redraw = !vm->vga_render_valid || vm->vga_render_page != page;
    if (full_redraw) {
        memset(vm->vga_dirty, 0xFF, sizeof(vm->vga_dirty));
    } else {
        dos_vga_dirty_cell(vm,
            (uint32_t)vm->vga_render_cursor_row * 80U +
            vm->vga_render_cursor_col);
        dos_vga_dirty_cell(vm,
            (uint32_t)vm->cursor_row * 80U + vm->cursor_col);
        if (vm->vga_render_mouse_visible)
            dos_vga_dirty_cell(vm,
                (uint32_t)(vm->vga_render_mouse_y / 8U) * 80U +
                vm->vga_render_mouse_x / 8U);
        if (vm->mouse_visible)
            dos_vga_dirty_cell(vm,
                (uint32_t)(vm->mouse_y / 8U) * 80U + vm->mouse_x / 8U);
    }

    uint32_t origin_x = (width - 640U) / 2U;
    uint32_t origin_y = (height - 400U) / 2U;
    uint32_t page_base = DOS_VRAM_BASE + (uint32_t)page * 4096U;
    bool cursor_enabled = (vm->cursor_start & 0x20U) == 0;
    uint8_t cursor_first = vm->cursor_start & 0x1FU;
    uint8_t cursor_last = vm->cursor_end & 0x1FU;
    uint32_t damage_top = height;
    uint32_t damage_bottom = 0;
    if (full_redraw) {
        for (uint32_t y = 0; y < height; y++) {
            uint32_t *row = target + y * pitch;
            for (uint32_t x = 0; x < width; x++)
                row[x] = 0xFF000000U;
        }
        damage_top = 0;
        damage_bottom = height;
    }

    for (uint32_t cell = 0; cell < 80U * 25U; cell++) {
        uint8_t mask = (uint8_t)(1U << (cell & 7U));
        if (!(vm->vga_dirty[cell >> 3] & mask))
            continue;
        vm->vga_dirty[cell >> 3] &= (uint8_t)~mask;

        uint8_t ch = dos_mem_read8(vm, page_base + cell * 2U);
        uint8_t attr = dos_mem_read8(vm, page_base + cell * 2U + 1U);
        const uint8_t *glyph = ch >= 32U && ch <= 126U
            ? gui_font8x16[ch - 32U]
            : gui_font8x16['?' - 32];
        uint32_t fg = dos_vga_text_palette[attr & 0x0FU];
        uint32_t bg = dos_vga_text_palette[(attr >> 4) & 0x07U];
        bool mouse_cursor = vm->mouse_visible &&
            cell == (uint32_t)(vm->mouse_y / 8U) * 80U + vm->mouse_x / 8U;
        if (mouse_cursor) {
            uint32_t swap = fg;
            fg = bg;
            bg = swap;
        }
        uint32_t x0 = origin_x + (cell % 80U) * 8U;
        uint32_t y0 = origin_y + (cell / 80U) * 16U;
        if (y0 < damage_top) damage_top = y0;
        if (y0 + 16U > damage_bottom) damage_bottom = y0 + 16U;
        bool cursor = cursor_enabled && cursor_first <= cursor_last &&
                      cell == (uint32_t)vm->cursor_row * 80U +
                              vm->cursor_col;

        for (uint32_t y = 0; y < 16U; y++) {
            uint8_t bits = glyph[y];
            bool cursor_line = cursor && y >= cursor_first && y <= cursor_last;
            uint32_t row = (y0 + y) * pitch + x0;
            for (uint32_t x = 0; x < 8U; x++) {
                uint32_t color = cursor_line || (bits & (0x80U >> x))
                    ? fg : bg;
                dos_vga_draw_pixel(target, row + x, color);
            }
        }
    }

    if (damage_top < damage_bottom)
        fb_present_rows(damage_top, damage_bottom);

    vm->vga_render_page = page;
    vm->vga_render_cursor_row = vm->cursor_row;
    vm->vga_render_cursor_col = vm->cursor_col;
    vm->vga_render_mouse_x = vm->mouse_x;
    vm->vga_render_mouse_y = vm->mouse_y;
    vm->vga_render_mouse_visible = vm->mouse_visible;
    vm->vga_render_valid = true;
}

/* Mark a cell as dirty when VGA memory is written */
void dos_vga_mark_dirty(dos_vm_t *vm, uint32_t addr)
{
    if (!vm) return;
    if (addr >= DOS_VBE_FB_BASE &&
        addr < DOS_VBE_FB_BASE + DOS_VBE_FB_SIZE) {
        if (vm == dos_vga_bound_vm && dos_vga_vbe_active)
            dos_vga_mode13_dirty = true;
        return;
    }
    if (addr >= DOS_CONV_TOP && addr < DOS_CONV_TOP + 320U * 200U) {
        if (vm == dos_vga_bound_vm && dos_vga_bound_mode == 0x13)
            dos_vga_mode13_dirty = true;
        return;
    }
    if (addr < DOS_VRAM_BASE || addr >= DOS_VRAM_BASE + DOS_VRAM_SIZE) return;
    uint32_t offset = addr - DOS_VRAM_BASE;
    uint8_t page = (uint8_t)(offset / 4096U);
    if (page != (vm->vga_page & 7U)) return;
    dos_vga_dirty_cell(vm, (offset % 4096U) / 2U);
}
