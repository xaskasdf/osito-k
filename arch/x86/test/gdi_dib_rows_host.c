/* Unscaled DIB presentation, checked against a logical-color oracle. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#undef NULL
#define main gdi_screen_contract_main
#include "gdi_screen_host.c"
#undef main

enum { IW = 11, IH = 7, OW = 13, OH = 9, STRIDE = 64, STORAGE = 768, PAD = 32 };
enum { RGB24, RGB32, FIELDS32, BGR32, RGB565, INDEX8 };
static uint32_t input_storage[STORAGE / 4], output_storage[STORAGE / 4];
static BYTE saved_input[STORAGE], expected[STORAGE];
static const DWORD palette[4] = {0x000000, 0x17395b, 0xc29e76, 0xffffff};
static GDI_PIXEL_FORMAT format565 = {.red = 0xf800, .green = 0x7e0, .blue = 0x1f};
static GDI_PIXEL_FORMAT format_bgr = {.red = 0xff, .green = 0xff00, .blue = 0xff0000};
typedef struct { GDI_BITMAP_HEADER header; DWORD extra[256]; } DIB_INFO;
typedef struct { int x, y, w, h, sx, sy, sw, sh, vx, vy; UINT start, lines; } COPY_CASE;

static DWORD logical_color(unsigned format, int x, int y)
{
    if (format == INDEX8) return palette[(x + y) % 4];
    unsigned r = (unsigned)(x * 17 + y * 31) & 255;
    unsigned g = (unsigned)(x * 43 + y * 13) & 255;
    unsigned b = (unsigned)(x * 29 + y * 47) & 255;
    if (format == RGB565) {
        r = (r & 0xf8) | (r >> 5);
        g = (g & 0xfc) | (g >> 6);
        b = (b & 0xf8) | (b >> 5);
    }
    return (r << 16) | (g << 8) | b;
}

static unsigned bytes_per_pixel(unsigned format)
{
    return format == RGB24 ? 3 : format == RGB565 ? 2 : format == INDEX8 ? 1 : 4;
}

static void encode_color(BYTE *pixel, unsigned format, DWORD color, BYTE alpha)
{
    if (format == RGB565) {
        unsigned value = ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
        pixel[0] = (BYTE)value; pixel[1] = (BYTE)(value >> 8);
    } else {
        pixel[0] = (BYTE)(format == BGR32 ? color >> 16 : color);
        pixel[1] = (BYTE)(color >> 8);
        pixel[2] = (BYTE)(format == BGR32 ? color : color >> 16);
        if (format != RGB24) pixel[3] = alpha;
    }
}

static void make_info(DIB_INFO *info, unsigned format, int width, int height)
{
    gdi_memset(info, 0, sizeof(*info));
    info->header = (GDI_BITMAP_HEADER){.size = 40, .width = width, .height = height,
        .planes = 1, .bpp = (USHORT)(bytes_per_pixel(format) * 8)};
    if (format == FIELDS32 || format == BGR32 || format == RGB565) {
        info->header.compression = GDI_BI_BITFIELDS;
        info->extra[0] = format == RGB565 ? 0xf800 : format == BGR32 ? 0xff : 0xff0000;
        info->extra[1] = format == RGB565 ? 0x7e0 : 0xff00;
        info->extra[2] = format == RGB565 ? 0x1f : format == BGR32 ? 0xff0000 : 0xff;
    } else if (format == INDEX8) {
        info->header.colors_used = 4;
        for (unsigned i = 0; i < 4; i++) info->extra[i] = palette[i];
    }
}

static BOOL contains(const GDI_RECT *r, int x, int y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

static void discard_dib_dc(HDC handle)
{
    GDI_DC *dc = dc_from_handle(handle);
    CHECK(dc && !dc->paint_clip && !dc->pooled_window && !dc->is_screen);
    if (dc) gdi_memset(dc, 0, sizeof(*dc));
}

static void dib_case(unsigned input_format, unsigned output_format, unsigned orientation,
                      unsigned clipping, BOOL device_api, COPY_CASE c)
{
    unsigned src_bytes = bytes_per_pixel(input_format);
    unsigned dst_bytes = bytes_per_pixel(output_format);
    int src_pitch = (IW * src_bytes + 3) & ~3;
    BOOL top_down = (orientation & 1) != 0, bottom_up = (orientation & 2) != 0;
    BYTE *src = (BYTE *)input_storage, *dst = (BYTE *)output_storage;
    for (unsigned i = 0; i < STORAGE; i++) { src[i] = 0xc7; dst[i] = expected[i] = 0x5a; }
    for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) {
        BYTE *pixel = src + PAD + (top_down ? y : IH - 1 - y) * src_pitch + x * src_bytes;
        if (input_format == INDEX8) pixel[0] = (BYTE)((x + y) % 4);
        else encode_color(pixel, input_format, logical_color(input_format, x, y), (BYTE)(x * 19));
    }
    for (unsigned i = 0; i < STORAGE; i++) saved_input[i] = src[i];
    DIB_INFO info;
    make_info(&info, input_format, IW, top_down ? -IH : IH);
    HDC handle = memory_dc(output_storage + PAD / 4, OW, OH, STRIDE / 4);
    GDI_DC *dc = dc_from_handle(handle);
    dc->bpp = (int)dst_bytes * 8;
    dc->bottomup = bottom_up;
    dc->viewport_x = c.vx; dc->viewport_y = c.vy;
    if (output_format == RGB565) dc->format = &format565;
    if (output_format == BGR32) dc->format = &format_bgr;
    GDI_RECT paint = {1, 2, 10, 8}, region[2] = {{3, 0, 11, 6}, {0, 7, 4, 9}};
    BOOL has_paint = clipping == 1 || clipping == 3 || clipping == 6;
    BOOL app_clip = clipping == 2 || clipping == 3 || clipping == 4 || clipping == 5;
    UINT count = clipping == 4 ? 2 : clipping == 5 ? 0 : 1;
    if (clipping == 6) paint = (GDI_RECT){3, 3, 3, 5};
    uint64_t token = 0;
    if (has_paint) CHECK(gdi32_push_paint_clip(handle, &paint, &token));
    dc->clip_active = app_clip; dc->clip_region.count = count;
    for (UINT i = 0; i < count; i++) dc->clip_region.rects[i] = region[i];

    int dw = c.w, dh = c.h, sw = c.sw, sh = c.sh;
    int64_t source_y = c.sy;
    if (device_api) {
        UINT scans = c.start < IH ? IH - c.start : 0;
        if (scans > c.lines) scans = c.lines;
        if (scans > (UINT)c.h) scans = (UINT)c.h;
        sw = dw; dh = sh = (int)scans; source_y += c.start;
    }
    int aw = dw < 0 ? -dw : dw, ah = dh < 0 ? -dh : dh;
    int asw = sw < 0 ? -sw : sw, ash = sh < 0 ? -sh : sh;
    int64_t left = (int64_t)c.x + c.vx - (dw < 0 ? aw : 0);
    int64_t top = (int64_t)c.y + c.vy - (dh < 0 ? ah : 0);
    int reported = 0;
    for (int y = 0; y < OH; y++) {
        BOOL row_valid = FALSE;
        for (int x = 0; x < OW; x++) {
            int64_t u = x - left, v = y - top;
            if (!aw || !ah || !asw || !ash || u < 0 || v < 0 || u >= aw || v >= ah) continue;
            if (dw < 0) u = aw - 1 - u;
            if (dh < 0) v = ah - 1 - v;
            int64_t ix = (int64_t)c.sx + (sw > 0 ? u * asw / aw : -u * asw / aw - 1);
            int64_t iy = source_y + (sh > 0 ? v * ash / ah : -v * ash / ah - 1);
            if (ix < 0 || iy < 0 || ix >= IW || iy >= IH) continue;
            row_valid = TRUE;
            if (has_paint && !contains(&paint, x, y)) continue;
            BOOL visible = !app_clip;
            for (UINT i = 0; app_clip && i < count; i++) visible |= contains(&region[i], x, y);
            if (!visible) continue;
            BYTE *pixel = expected + PAD + (bottom_up ? OH - 1 - y : y) * STRIDE + x * dst_bytes;
            encode_color(pixel, output_format, logical_color(input_format, (int)ix, (int)iy),
                         output_format == BGR32 ? 0 : 255);
        }
        if (row_valid) reported++;
    }
    unsigned before = failures;
    int rows = device_api ? SetDIBitsToDevice_k32(handle, c.x, c.y, (DWORD)c.w, (DWORD)c.h,
                        c.sx, c.sy, c.start, c.lines, src + PAD, &info, 0)
        : StretchDIBits_k32(handle, c.x, c.y, c.w, c.h, c.sx, c.sy, c.sw, c.sh,
                            src + PAD, &info, 0, ROP_SRCCOPY);
    CHECK(rows == reported);
    for (unsigned i = 0; i < STORAGE; i++) {
        CHECK(dst[i] == expected[i]); CHECK(src[i] == saved_input[i]);
    }
    if (failures != before)
        printf("case src=%u dst=%u orient=%u clip=%u api=%d xy=%d,%d size=%d,%d source=%d,%d scans=%u,%u rows=%d/%d\n",
               input_format, output_format, orientation, clipping, device_api, c.x, c.y,
               c.w, c.h, c.sx, c.sy, c.start, c.lines, rows, reported);
    if (token) gdi32_pop_paint_clip(handle, token, TRUE);
    discard_dib_dc(handle);
}

static void aliased_case(BOOL bottom_up, BOOL shifted)
{
    BYTE *storage = (BYTE *)input_storage;
    unsigned pitch = IW * 4;
    for (unsigned i = 0; i < STORAGE; i++) storage[i] = expected[i] = 0xc7;
    for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) {
        unsigned at = PAD + (bottom_up ? IH - 1 - y : y) * pitch + x * 4;
        encode_color(storage + at, RGB32, logical_color(RGB32, x, y), 0);
    }
    for (unsigned i = 0; i < STORAGE; i++) expected[i] = storage[i];
    /* Read from a later logical row, so these aliases have a snapshot oracle
     * even though the old general DIB path is not overlap-safe in all directions. */
    for (int y = 0; y < 5; y++) for (int x = 0; x < 8; x++) {
        unsigned at = PAD + (bottom_up ? IH - 1 - y : y) * pitch + (x + shifted) * 4;
        encode_color(expected + at, RGB32, logical_color(RGB32, x + 2, y + 1), 255);
    }
    DIB_INFO info;
    make_info(&info, RGB32, IW, bottom_up ? IH : -IH);
    HDC handle = memory_dc(input_storage + PAD / 4 + shifted, IW - shifted, IH, IW);
    dc_from_handle(handle)->bottomup = bottom_up;
    CHECK(StretchDIBits_k32(handle, 0, 0, 8, 5, 2, 1, 8, 5,
                            storage + PAD, &info, 0, ROP_SRCCOPY) == 5);
    for (unsigned i = 0; i < STORAGE; i++) CHECK(storage[i] == expected[i]);
    discard_dib_dc(handle);
}

static void window_notifications(void)
{
    uint32_t pixels[20];
    for (unsigned i = 0; i < 20; i++) pixels[i] = 0x00123456;
    DIB_INFO info;
    make_info(&info, RGB32, 5, -4);
    window_ready = 1;
    HDC handle = gdi32_alloc_window_dc(WINDOW);
    unsigned old_dirty = window_dirty, old_presentations = presentations;
    CHECK(StretchDIBits_k32(handle, 0, 0, 5, 4, 0, 0, 5, 4,
                            pixels, &info, 0, ROP_SRCCOPY) == 4);
    CHECK(window_dirty == old_dirty + 1 && presentations == old_presentations);
    for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 5; x++)
        CHECK(window_pixels[GUARD + y * 8 + x] == 0xff123456);
    GDI_RECT empty = {2, 1, 2, 3};
    uint64_t token;
    CHECK(gdi32_push_paint_clip(handle, &empty, &token));
    pixels[0] = 0x00abcdef;
    CHECK(SetDIBitsToDevice_k32(handle, 0, 0, 5, 4, 0, 0, 0, 4,
                                pixels, &info, 0) == 4);
    CHECK(window_dirty == old_dirty + 2 && presentations == old_presentations);
    CHECK(window_pixels[GUARD] == 0xff123456);
    gdi32_pop_paint_clip(handle, token, TRUE);
    /* Window DC lifetime/IRQ locking is exercised by the in-OS PE fixtures. */
    window_ready = 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void benchmark(int width, int height, BOOL tile, BOOL device_api)
{
    SIZE_T size = (SIZE_T)width * height * 4;
    uint32_t *source = malloc(size), *destination = malloc(size);
    if (!source || !destination) abort();
    for (SIZE_T i = 0; i < size / 4; i++) source[i] = (uint32_t)i & 0xffffff;
    HDC handle = memory_dc(destination, width, height, width);
    DIB_INFO info;
    make_info(&info, RGB32, width, -height);
    GDI_RECT rect = tile ? (GDI_RECT){40, 40, 104, 104} : (GDI_RECT){0, 0, width, height};
    uint64_t token;
    CHECK(gdi32_push_paint_clip(handle, &rect, &token));
    int (WINAPI *volatile stretch)(HDC, int, int, int, int, int, int, int, int,
                                    PCVOID, PCVOID, UINT, DWORD) = StretchDIBits_k32;
    int (WINAPI *volatile device)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
                                   PCVOID, PCVOID, UINT) = SetDIBitsToDevice_k32;
    /* Five warmups exclude the first four full-frame diagnostic hashes. */
    for (int sample = -1; sample < 3; sample++) {
        unsigned frames = sample < 0 ? 5 : 30;
        uint64_t start = monotonic_ns();
        for (unsigned frame = 0; frame < frames; frame++) {
            int rows = device_api
                ? device(handle, 0, 0, width, height, 0, 0, 0, height, source, &info, 0)
                : stretch(handle, 0, 0, width, height, 0, 0, width, height,
                          source, &info, 0, ROP_SRCCOPY);
            CHECK(rows == height);
        }
        uint64_t elapsed = monotonic_ns() - start;
        CHECK(destination[(SIZE_T)rect.top * width + rect.left] ==
              (0xff000000 | source[(SIZE_T)rect.top * width + rect.left]));
        if (sample >= 0)
            printf("BENCH api=%s width=%d height=%d tile=%d sample=%d ns_per_dib=%llu\n",
                   device_api ? "SetDIBitsToDevice" : "StretchDIBits", width, height,
                   tile, sample, (unsigned long long)(elapsed / frames));
    }
    gdi32_pop_paint_clip(handle, token, TRUE);
    discard_dib_dc(handle);
    free(destination);
    free(source);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        for (BOOL device = FALSE; device <= TRUE; device++) {
            benchmark(700, 440, FALSE, device);
            benchmark(1920, 1080, FALSE, device);
            benchmark(1920, 1080, TRUE, device);
        }
        return failures ? 1 : 0;
    }
    if (gdi_screen_contract_main()) return 1;
    static const unsigned outputs[] = {RGB24, RGB32, RGB565, BGR32};
    static const COPY_CASE copies[] = {
        {.w = IW, .h = IH, .sw = IW, .sh = IH, .lines = IH},
        {.x = -3, .y = -2, .w = 15, .h = 12, .sw = 15, .sh = 12, .lines = 12},
        {.x = 2, .y = 1, .w = 15, .h = 12, .sx = -3, .sy = -2, .sw = 15, .sh = 12, .lines = 12},
        {.x = 2, .y = 1, .w = 7, .h = 4, .sx = 1, .sy = 2, .sw = 7, .sh = 4,
         .vx = -1, .vy = 2, .start = 2, .lines = 3},
    };
    for (unsigned input = RGB24; input <= INDEX8; input++)
        for (unsigned output = 0; output < sizeof(outputs) / sizeof(outputs[0]); output++)
            for (unsigned orientation = 0; orientation < 4; orientation++)
                for (unsigned clip = 0; clip < 7; clip++)
                    for (unsigned c = 0; c < sizeof(copies) / sizeof(copies[0]); c++)
                        for (BOOL device = FALSE; device <= TRUE; device++)
                            dib_case(input, outputs[output], orientation, clip, device, copies[c]);

    static const COPY_CASE stretch_edges[] = {
        {.x = 10, .y = 7, .w = -10, .h = -7, .sw = 10, .sh = 7},
        {.w = 10, .h = 7, .sx = 10, .sy = 7, .sw = -10, .sh = -7},
        {.x = 10, .y = 7, .w = -10, .h = -7, .sx = 10, .sy = 7, .sw = -10, .sh = -7},
        {.w = 12, .h = 8, .sw = 6, .sh = 4},
        {.x = 1, .y = 2, .w = 5, .h = 3, .sw = 11, .sh = 7},
        {.w = 0, .h = 7, .sw = 11, .sh = 7},
        {.w = 11, .h = 7, .sw = 0, .sh = 7},
        {.x = 2147483647, .w = 11, .h = 7, .sw = 11, .sh = 7, .vx = 1},
        {.y = -2147483647 - 1, .w = 11, .h = 7, .sw = 11, .sh = 7, .vy = -1},
        {.w = 11, .h = 7, .sx = 2147483647, .sw = 11, .sh = 7},
        {.w = 11, .h = 7, .sy = -2147483647 - 1, .sw = 11, .sh = 7},
        {.x = -2147483647, .w = 2147483647, .h = 7, .sw = 2147483647, .sh = 7, .vx = 2},
        {.w = 2147483647, .h = 2147483647, .sw = 2147483647, .sh = 2147483647},
    };
    static const COPY_CASE device_edges[] = {
        {.w = 11, .h = 7, .start = 7, .lines = 7},
        {.w = 11, .h = 7, .start = 1, .lines = 0},
        {.w = 11, .h = 1, .start = 2, .lines = 7},
        {.w = 11, .h = 7, .sy = 2147483647, .start = 2, .lines = 7},
        {.x = 2147483647, .w = 11, .h = 7, .vx = 1, .lines = 7},
        {.y = -2147483647 - 1, .w = 11, .h = 7, .vy = -1, .lines = 7},
    };
    for (unsigned orientation = 0; orientation < 4; orientation++)
        for (unsigned clip = 0; clip < 7; clip++) {
            for (unsigned c = 0; c < sizeof(stretch_edges) / sizeof(stretch_edges[0]); c++)
                dib_case(RGB32, RGB32, orientation, clip, FALSE, stretch_edges[c]);
            for (unsigned c = 0; c < sizeof(device_edges) / sizeof(device_edges[0]); c++)
                dib_case(RGB32, RGB32, orientation, clip, TRUE, device_edges[c]);
        }
    for (BOOL bottom = FALSE; bottom <= TRUE; bottom++)
        for (BOOL shifted = FALSE; shifted <= TRUE; shifted++) aliased_case(bottom, shifted);
    window_notifications();
    printf("GDI DIB rows: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
