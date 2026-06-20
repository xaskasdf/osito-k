/*
 * OsitoK x86-64 — GOP Framebuffer Text Console
 *
 * Simple 32bpp text rendering using embedded 8×16 bitmap font.
 * Initialized from UEFI GOP before ExitBootServices.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* ── Framebuffer state ───────────────────────────────────────── */

static uint32_t *fb_base;    /* active drawing target (shadow or vram) */
static uint32_t *fb_vram;    /* physical VRAM (MMIO, slow uncached access) */
static uint32_t *fb_shadow;  /* RAM shadow buffer (fast cached access) */
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch;    /* pixels per scanline */

static uint32_t  text_col;
static uint32_t  text_row;
static uint32_t  max_cols;
static uint32_t  max_rows;

/* Dirty region tracking (pixel rows) */
static uint32_t  dirty_top;
static uint32_t  dirty_bot;

/* Redirect state: when active, fb_base points to an external surface
 * (e.g. an shm terminal surface) and VRAM flushes are suppressed. */
static bool      redirect_active;
static uint32_t  fb_clear_clr = 0x00000000;  /* background fill color */

/* Optional QEMU/virtio scanout mirror. Bare-metal builds keep running
 * through GOP only when these weak symbols are absent. */
extern bool      virtio_gpu_ready(void)      __attribute__((weak));
extern uint32_t *virtio_gpu_get_fb(void)     __attribute__((weak));
extern uint32_t  virtio_gpu_get_width(void)  __attribute__((weak));
extern uint32_t  virtio_gpu_get_height(void) __attribute__((weak));
extern void      virtio_gpu_flush(void)      __attribute__((weak));

#define FONT_W  8
#define FONT_H  16

#define FG_COLOR 0x00CCCCCC   /* Light gray */
#define BG_COLOR 0x00000000   /* Black */
#define HL_COLOR 0x0000FF00   /* Green for highlights */

/* Font data now in gui/gui_text.c (shared across architectures) */
extern const uint8_t gui_font8x16[95][16];
#define font8x16 gui_font8x16

/* ── GUI terminal text buffer (for rendering in desktop terminal window) ── */
#define TERM_BUF_ROWS 200
#define TERM_BUF_COLS 160
static char  term_buf[TERM_BUF_ROWS][TERM_BUF_COLS];
static int   term_cur_row;    /* current write row */
static int   term_cur_col;    /* current write col */
static int   term_total_rows; /* total rows written (for scrollback) */

static void fb_flush_virtio_rows(uint32_t top, uint32_t bot);

/* ── Public API ──────────────────────────────────────────────── */

void fb_init(uint32_t *base, uint32_t width, uint32_t height, uint32_t pitch)
{
    fb_base   = base;
    fb_vram   = base;
    fb_shadow = NULL;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = pitch;
    text_col  = 0;
    text_row  = 0;
    max_cols  = width / FONT_W;
    max_rows  = height / FONT_H;
    dirty_top = 0;
    dirty_bot = 0;
}

/* Enable shadow framebuffer (call after memory allocator is ready) */
void fb_enable_shadow(void *buf)
{
    fb_shadow = (uint32_t *)buf;
    /* Copy current VRAM to shadow (slow UC read, but one-time) */
    uint32_t total = fb_height * fb_pitch;
    for (uint32_t i = 0; i < total; i++)
        fb_shadow[i] = fb_vram[i];
    fb_base = fb_shadow;
    dirty_top = 0;
    dirty_bot = 0;
}

/* Flush dirty region from shadow to VRAM */
void fb_flush(void)
{
    if (redirect_active) return;  /* Surface owner blits to screen; no VRAM flush */
    if (!fb_shadow || dirty_top >= dirty_bot) return;
    /* Clamp to framebuffer bounds */
    if (dirty_bot > fb_height) dirty_bot = fb_height;
    uint32_t top = dirty_top;
    uint32_t bot = dirty_bot;
    /* Copy dirty rows: use 64-bit writes for efficiency on WC memory */
    uint64_t *dst = (uint64_t *)(fb_vram + top * fb_pitch);
    uint64_t *src = (uint64_t *)(fb_shadow + top * fb_pitch);
    uint32_t qwords = (bot - top) * fb_pitch / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];
    fb_flush_virtio_rows(top, bot);
    dirty_top = fb_height;
    dirty_bot = 0;
}

void fb_flush_all(void)
{
    if (redirect_active) return;
    if (!fb_shadow) return;
    uint64_t *dst = (uint64_t *)fb_vram;
    uint64_t *src = (uint64_t *)fb_shadow;
    uint32_t qwords = fb_height * fb_pitch / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];
    fb_flush_virtio_rows(0, fb_height);
}

static void fb_mark_dirty(uint32_t pixel_top, uint32_t pixel_bot)
{
    if (pixel_top < dirty_top) dirty_top = pixel_top;
    if (pixel_bot > dirty_bot) dirty_bot = pixel_bot;
}

static void fb_flush_virtio_rows(uint32_t top, uint32_t bot)
{
    static uint64_t dbg_flush_count;

    if (!virtio_gpu_ready || !virtio_gpu_get_fb || !virtio_gpu_get_width ||
        !virtio_gpu_get_height || !virtio_gpu_flush) {
        return;
    }
    if (!virtio_gpu_ready() || !fb_base) {
        return;
    }

    uint32_t *dst = virtio_gpu_get_fb();
    if (!dst) {
        return;
    }

    uint32_t vw = virtio_gpu_get_width();
    uint32_t vh = virtio_gpu_get_height();
    if (!vw || !vh || top >= fb_height || top >= vh) {
        return;
    }
    if (bot > fb_height) {
        bot = fb_height;
    }
    if (bot > vh) {
        bot = vh;
    }
    if (top >= bot) {
        return;
    }

    uint32_t copy_w = fb_width < vw ? fb_width : vw;
    for (uint32_t y = top; y < bot; y++) {
        memcpy(dst + y * vw, fb_base + y * fb_pitch,
               (uint64_t)copy_w * sizeof(uint32_t));
    }

    dbg_flush_count++;
    if (dbg_flush_count <= 16 ||
        (dbg_flush_count & (dbg_flush_count - 1)) == 0) {
        uint32_t hash = 2166136261u;
        uint32_t step_y = (bot - top) > 32 ? (bot - top) / 32 : 1;
        uint32_t step_x = copy_w > 64 ? copy_w / 64 : 1;
        for (uint32_t y = top; y < bot; y += step_y) {
            for (uint32_t x = 0; x < copy_w; x += step_x) {
                hash ^= dst[y * vw + x];
                hash *= 16777619u;
            }
        }
        serial_puts("[FBV] flush#");
        serial_putdec(dbg_flush_count);
        serial_puts(" rows=");
        serial_putdec(top);
        serial_puts("-");
        serial_putdec(bot);
        serial_puts(" hash=0x");
        serial_puthex(hash, 8);
        serial_puts("\n");
    }

    virtio_gpu_flush();
}

void fb_clear(void)
{
    if (!fb_base) return;
    uint32_t fill = fb_clear_clr;
    uint64_t fill64 = (uint64_t)fill | ((uint64_t)fill << 32);
    uint64_t *p = (uint64_t *)fb_base;
    uint32_t qwords = fb_height * fb_pitch / 2;
    for (uint32_t i = 0; i < qwords; i++)
        p[i] = fill64;
    text_col = 0;
    text_row = 0;
    if (redirect_active) {
        dirty_top = fb_height;
        dirty_bot = 0;
        return;
    }
    /* Flush to VRAM if shadow buffer is active */
    if (fb_shadow) {
        uint64_t *dst = (uint64_t *)fb_vram;
        uint64_t *src = (uint64_t *)fb_shadow;
        for (uint32_t i = 0; i < qwords; i++)
            dst[i] = src[i];
    }
    fb_flush_virtio_rows(0, fb_height);
    dirty_top = fb_height;
    dirty_bot = 0;
}

static void fb_putchar_at(uint32_t col, uint32_t row, char c, uint32_t fg)
{
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = font8x16[c - 32];
    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;

    for (uint32_t y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_W; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? fg : fb_clear_clr;
            uint32_t sx = px + x;
            uint32_t sy = py + y;
            if (sx < fb_width && sy < fb_height)
                fb_base[sy * fb_pitch + sx] = color;
        }
    }
    fb_mark_dirty(py, py + FONT_H);
}

static void fb_scroll(void)
{
    /* Scroll up by one row */
    uint32_t row_pixels = FONT_H * fb_pitch;
    uint32_t total_pixels = fb_height * fb_pitch;

    /* Move rows up (works on shadow if available, else VRAM) */
    uint64_t *dst = (uint64_t *)fb_base;
    uint64_t *src = (uint64_t *)(fb_base + row_pixels);
    uint32_t qwords = (total_pixels - row_pixels) / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];

    /* Clear last row with background color */
    uint32_t fill = fb_clear_clr;
    uint64_t fill64 = (uint64_t)fill | ((uint64_t)fill << 32);
    uint64_t *last = (uint64_t *)(fb_base + (total_pixels - row_pixels));
    uint32_t clear_qwords = row_pixels / 2;
    for (uint32_t i = 0; i < clear_qwords; i++)
        last[i] = fill64;

    /* Mark entire screen dirty */
    fb_mark_dirty(0, fb_height);
    fb_flush();
}

/* ── ANSI CSI escape sequence parser ────────────────────────── */

#define ANSI_STATE_NORMAL  0
#define ANSI_STATE_ESC     1   /* received ESC */
#define ANSI_STATE_CSI     2   /* received ESC [ */
#define ANSI_STATE_QMARK   3   /* received ESC [ ? */

static int    ansi_state;
static int    ansi_params[8];
static int    ansi_nparams;
static int    ansi_cur_param;
static uint32_t ansi_fg_color = FG_COLOR;
static uint32_t ansi_bg_color = 0;  /* default bg = black */
static int      ansi_bold = 0;
static int      ansi_reverse = 0;

/* Basic ANSI color palette (SGR 30-37) */
static const uint32_t ansi_colors[8] = {
    0x00000000, /* 0: black */
    0x00CC0000, /* 1: red */
    0x0000CC00, /* 2: green */
    0x00CCCC00, /* 3: yellow */
    0x000000CC, /* 4: blue */
    0x00CC00CC, /* 5: magenta */
    0x0000CCCC, /* 6: cyan */
    0x00CCCCCC, /* 7: white/gray */
};

/* Bright ANSI colors (SGR 90-97) */
static const uint32_t ansi_bright[8] = {
    0x00666666, /* 0: bright black (dark gray) */
    0x00FF4444, /* 1: bright red */
    0x0044FF44, /* 2: bright green */
    0x00FFFF44, /* 3: bright yellow */
    0x004444FF, /* 4: bright blue */
    0x00FF44FF, /* 5: bright magenta */
    0x0044FFFF, /* 6: bright cyan */
    0x00FFFFFF, /* 7: bright white */
};

static uint32_t ansi_256_color(int n)
{
    if (n < 0) return 0;
    if (n < 8)  return ansi_colors[n];
    if (n < 16) return ansi_bright[n - 8];
    if (n < 232) {
        n -= 16;
        uint32_t r = (n / 36) * 51, g = ((n / 6) % 6) * 51, b = (n % 6) * 51;
        return (r << 16) | (g << 8) | b;
    }
    uint32_t v = (uint32_t)(n - 232) * 10 + 8;
    return (v << 16) | (v << 8) | v;
}

static void fb_clear_line_from(uint32_t row, uint32_t col)
{
    /* Clear from (col, row) to end of line */
    for (uint32_t c = col; c < max_cols; c++)
        fb_putchar_at(c, row, ' ', BG_COLOR);
}

static void fb_clear_line(uint32_t row)
{
    fb_clear_line_from(row, 0);
}

static void ansi_execute(char cmd)
{
    int p0 = (ansi_nparams > 0) ? ansi_params[0] : 0;
    int p1 = (ansi_nparams > 1) ? ansi_params[1] : 0;

    switch (cmd) {
    case 'A': /* Cursor Up */
        if (p0 == 0) p0 = 1;
        text_row = (text_row >= (uint32_t)p0) ? text_row - p0 : 0;
        break;
    case 'B': /* Cursor Down */
        if (p0 == 0) p0 = 1;
        text_row += p0;
        if (text_row >= max_rows) text_row = max_rows - 1;
        break;
    case 'C': /* Cursor Forward */
        if (p0 == 0) p0 = 1;
        text_col += p0;
        if (text_col >= max_cols) text_col = max_cols - 1;
        break;
    case 'D': /* Cursor Back */
        if (p0 == 0) p0 = 1;
        text_col = (text_col >= (uint32_t)p0) ? text_col - p0 : 0;
        break;
    case 'H': /* Cursor Position (row;col, 1-based) */
    case 'f':
        text_row = (p0 > 0) ? (uint32_t)(p0 - 1) : 0;
        text_col = (p1 > 0) ? (uint32_t)(p1 - 1) : 0;
        if (text_row >= max_rows) text_row = max_rows - 1;
        if (text_col >= max_cols) text_col = max_cols - 1;
        break;
    case 'J': /* Erase in Display */
        if (p0 == 0) {
            /* Clear from cursor to end of screen */
            fb_clear_line_from(text_row, text_col);
            for (uint32_t r = text_row + 1; r < max_rows; r++)
                fb_clear_line(r);
        } else if (p0 == 2 || p0 == 3) {
            /* Clear entire screen */
            fb_clear();
        }
        break;
    case 'K': /* Erase in Line */
        if (p0 == 0) {
            fb_clear_line_from(text_row, text_col);
        } else if (p0 == 2) {
            fb_clear_line(text_row);
        }
        break;
    case 'm': /* SGR — Select Graphic Rendition */
        if (ansi_nparams == 0) {
            ansi_fg_color = FG_COLOR; ansi_bg_color = 0;
            ansi_bold = 0; ansi_reverse = 0;
        }
        for (int i = 0; i < ansi_nparams; i++) {
            int p = ansi_params[i];
            if (p == 0) { ansi_fg_color = FG_COLOR; ansi_bg_color = 0; ansi_bold = 0; ansi_reverse = 0; }
            else if (p == 1) ansi_bold = 1;
            else if (p == 7) ansi_reverse = 1;
            else if (p == 22) ansi_bold = 0;
            else if (p == 27) ansi_reverse = 0;
            else if (p >= 30 && p <= 37) ansi_fg_color = ansi_bold ? ansi_bright[p-30] : ansi_colors[p-30];
            else if (p == 39) ansi_fg_color = FG_COLOR;
            else if (p >= 40 && p <= 47) ansi_bg_color = ansi_colors[p - 40];
            else if (p == 49) ansi_bg_color = 0;
            else if (p >= 90 && p <= 97) ansi_fg_color = ansi_bright[p - 90];
            else if (p >= 100 && p <= 107) ansi_bg_color = ansi_bright[p - 100];
            else if (p == 38 && i + 2 < ansi_nparams && ansi_params[i+1] == 5) {
                ansi_fg_color = ansi_256_color(ansi_params[i+2]); i += 2;
            }
            else if (p == 48 && i + 2 < ansi_nparams && ansi_params[i+1] == 5) {
                ansi_bg_color = ansi_256_color(ansi_params[i+2]); i += 2;
            }
            else if (p == 38 && i + 4 < ansi_nparams && ansi_params[i+1] == 2) {
                ansi_fg_color = ((uint32_t)ansi_params[i+2]<<16)|((uint32_t)ansi_params[i+3]<<8)|ansi_params[i+4]; i += 4;
            }
            else if (p == 48 && i + 4 < ansi_nparams && ansi_params[i+1] == 2) {
                ansi_bg_color = ((uint32_t)ansi_params[i+2]<<16)|((uint32_t)ansi_params[i+3]<<8)|ansi_params[i+4]; i += 4;
            }
        }
        break;
    case 'n': /* Device Status Report */
        /* 6n = cursor position report: we can't send back to the app
         * from framebuffer. The syscall layer handles this via keyboard
         * injection. Just ignore here. */
        break;
    case 'l': /* Reset Mode (used for ?25l = hide cursor) */
    case 'h': /* Set Mode (used for ?25h = show cursor) */
        /* Cursor visibility — we don't draw a cursor, so ignore */
        break;
    }
}

static void term_buf_newline(void)
{
    if (term_cur_row < TERM_BUF_ROWS - 1) {
        term_cur_row++;
    } else {
        /* Shift rows up to make room (after ~200 lines) */
        for (int i = 0; i < TERM_BUF_ROWS - 1; i++)
            for (int j = 0; j < TERM_BUF_COLS; j++)
                term_buf[i][j] = term_buf[i + 1][j];
    }
    term_total_rows++;
    term_cur_col = 0;
    term_buf[term_cur_row][0] = '\0';
}

void fb_putc(char c, uint32_t color)
{
    if (!fb_base) return;

    /* ANSI state machine */
    switch (ansi_state) {
    case ANSI_STATE_ESC:
        if (c == '[') {
            ansi_state = ANSI_STATE_CSI;
            ansi_nparams = 0;
            ansi_cur_param = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
            return;
        }
        ansi_state = ANSI_STATE_NORMAL;
        /* Fall through to render the character */
        break;
    case ANSI_STATE_CSI:
    case ANSI_STATE_QMARK:
        if (c == '?') {
            ansi_state = ANSI_STATE_QMARK;
            return;
        }
        if (c >= '0' && c <= '9') {
            ansi_cur_param = ansi_cur_param * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (ansi_nparams < 8)
                ansi_params[ansi_nparams++] = ansi_cur_param;
            ansi_cur_param = 0;
            return;
        }
        /* Command character — finalize params and execute */
        if (ansi_nparams < 8)
            ansi_params[ansi_nparams++] = ansi_cur_param;
        ansi_execute(c);
        ansi_state = ANSI_STATE_NORMAL;
        return;
    }

    /* Normal character processing */
    if (c == 27) {  /* ESC */
        ansi_state = ANSI_STATE_ESC;
        return;
    }

    /* Use ANSI fg color if no explicit color override */
    uint32_t fg = (color == FG_COLOR) ? ansi_fg_color : color;

    if (c == '\n') {
        text_col = 0;
        text_row++;
        term_buf[term_cur_row][term_cur_col] = '\0';
        term_buf_newline();
    } else if (c == '\r') {
        text_col = 0;
        term_cur_col = 0;
    } else if (c == '\b') {
        if (text_col > 0) text_col--;
        if (term_cur_col > 0) {
            term_cur_col--;
            term_buf[term_cur_row][term_cur_col] = ' ';
        }
    } else if (c == '\t') {
        text_col = (text_col + 4) & ~3;
        int nc = (term_cur_col + 4) & ~3;
        while (term_cur_col < nc && term_cur_col < TERM_BUF_COLS - 1)
            term_buf[term_cur_row][term_cur_col++] = ' ';
        if (term_cur_col < TERM_BUF_COLS)
            term_buf[term_cur_row][term_cur_col] = '\0';
    } else {
        fb_putchar_at(text_col, text_row, c, fg);
        text_col++;
        if (c >= 32 && c <= 126 && term_cur_col < TERM_BUF_COLS - 1) {
            term_buf[term_cur_row][term_cur_col++] = c;
            term_buf[term_cur_row][term_cur_col] = '\0';
        }
    }

    if (text_col >= max_cols) {
        text_col = 0;
        text_row++;
        term_buf[term_cur_row][term_cur_col] = '\0';
        term_buf_newline();
    }
    if (text_row >= max_rows) {
        fb_scroll();
        text_row = max_rows - 1;
    }
}

/* ── Query framebuffer dimensions ──────────────────────────── */

uint32_t fb_get_cols(void) { return max_cols; }
uint32_t fb_get_rows(void) { return max_rows; }
uint32_t *fb_get_base(void)   { return fb_base; }
uint32_t *fb_get_vram(void)   { return fb_vram; }
uint32_t  fb_get_width(void)  { return fb_width; }
uint32_t  fb_get_height(void) { return fb_height; }
uint32_t  fb_get_pitch(void)  { return fb_pitch; }

uint32_t fb_get_text_col(void) { return text_col; }
uint32_t fb_get_text_row(void) { return text_row; }

/* ── Surface redirect (for compositor terminal window) ──────── */

/* Redirect fb_putc output to an external pixel surface.
 * Called once from shell.c:desktop after creating the terminal shm surface.
 * w/h are in pixels; pitch = w (tightly packed, no padding). */
void fb_redirect(uint32_t *new_base, uint32_t w, uint32_t h, uint32_t pitch)
{
    fb_base        = new_base;
    fb_width       = w;
    fb_height      = h;
    fb_pitch       = pitch;
    max_cols       = w / FONT_W;
    max_rows       = h / FONT_H;
    /* Clamp to term_buf dimensions so scrollback stays in bounds */
    if (max_cols > TERM_BUF_COLS) max_cols = TERM_BUF_COLS;
    if (max_rows > TERM_BUF_ROWS) max_rows = TERM_BUF_ROWS;
    text_col       = 0;
    text_row       = 0;
    term_cur_col   = 0;
    term_cur_row   = 0;
    redirect_active = true;
    /* Caller is responsible for clearing the surface if needed */
}

void fb_set_clear_color(uint32_t color) { fb_clear_clr = color; }

/* ── GUI terminal text buffer getters ───────────────────────── */

const char *fb_get_term_line(int row)
{
    if (row < 0 || row >= TERM_BUF_ROWS) return "";
    return term_buf[row];
}
int fb_get_term_rows(void)       { return term_cur_row; }
int fb_get_term_cursor_col(void) { return term_cur_col; }

void fb_puts(const char *s)
{
    while (*s) fb_putc(*s++, FG_COLOR);
    fb_flush();
}

void fb_puts_color(const char *s, uint32_t color)
{
    while (*s) fb_putc(*s++, color);
    fb_flush();
}

void fb_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    fb_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        fb_putc(hex[(val >> i) & 0xF], FG_COLOR);
}

void fb_putdec(uint64_t val)
{
    char buf[20];
    int i = 0;
    if (val == 0) { fb_putc('0', FG_COLOR); return; }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0) fb_putc(buf[i], FG_COLOR);
}

void fb_putchar(char c)
{
    fb_putc(c, FG_COLOR);
    fb_flush();
}
