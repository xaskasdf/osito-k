/* Layered-window staging copies, with independent colors and guard bytes. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#undef NULL
#define main gdi_screen_contract_main
#include "gdi_screen_host.c"
#undef main

enum { SOURCE_W = 7, SOURCE_H = 5, SOURCE_PITCH = 40, BYTES = 640, PREFIX = 32 };
enum { NATIVE32, NATIVE24, REVERSED32, RGB565, INDEX8 };
static uint32_t input[BYTES / 4], output[BYTES / 4];
static BYTE saved[BYTES], expected[BYTES];
static GDI_PIXEL_FORMAT reversed = {.red = 0xff, .green = 0xff00, .blue = 0xff0000};
static GDI_PIXEL_FORMAT rgb565 = {.red = 0xf800, .green = 0x7e0, .blue = 0x1f};
static GDI_PIXEL_FORMAT indexed = {.color_count = 4,
    .colors = {0xff000000, 0xff17395b, 0xffc29e76, 0xffffffff}};
typedef struct { int x, y, width, height, vx, vy; } LAYER_COPY;

static DWORD source_color(unsigned format, int x, int y)
{
    if (format == INDEX8) return indexed.colors[(x + y) % 4];
    unsigned red = (x * 31 + y * 59) & 255;
    unsigned green = (x * 17 + y * 41) & 255;
    unsigned blue = (x * 53 + y * 13) & 255;
    if (format == RGB565) {
        red = (red & 0xf8) | (red >> 5);
        green = (green & 0xfc) | (green >> 6);
        blue = (blue & 0xf8) | (blue >> 5);
    }
    return (red << 16) | (green << 8) | blue;
}

static BYTE source_alpha(int x, int y)
{
    return (BYTE)((x * 41 + y * 127) & 255);
}

static void discard_layer_dc(HDC handle)
{
    GDI_DC *dc = dc_from_handle(handle);
    CHECK(dc && !dc->paint_clip && !dc->pooled_window && !dc->is_screen);
    if (dc) gdi_memset(dc, 0, sizeof(*dc));
}

static void copy_case(unsigned format, BOOL bottom_up, BOOL per_pixel,
                       BOOL clip, LAYER_COPY copy)
{
    unsigned bytes = format == NATIVE24 ? 3 : format == RGB565 ? 2 : format == INDEX8 ? 1 : 4;
    BYTE *source = (BYTE *)input, *destination = (BYTE *)output;
    for (unsigned i = 0; i < BYTES; i++) source[i] = 0xc7;
    for (int y = 0; y < SOURCE_H; y++) for (int x = 0; x < SOURCE_W; x++) {
        BYTE *pixel = source + PREFIX + (bottom_up ? SOURCE_H - 1 - y : y) * SOURCE_PITCH + x * bytes;
        DWORD color = source_color(format, x, y);
        if (format == INDEX8) pixel[0] = (BYTE)((x + y) % 4);
        else if (format == RGB565) {
            unsigned value = ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
            pixel[0] = (BYTE)value; pixel[1] = (BYTE)(value >> 8);
        } else {
            pixel[0] = (BYTE)(format == REVERSED32 ? color >> 16 : color);
            pixel[1] = (BYTE)(color >> 8);
            pixel[2] = (BYTE)(format == REVERSED32 ? color : color >> 16);
            if (bytes == 4) pixel[3] = source_alpha(x, y);
        }
    }
    for (unsigned i = 0; i < BYTES; i++) {
        saved[i] = source[i]; destination[i] = expected[i] = 0x5a;
    }
    HDC handle = memory_dc(input + PREFIX / 4, SOURCE_W, SOURCE_H, SOURCE_PITCH / 4);
    GDI_DC *dc = dc_from_handle(handle);
    dc->bpp = (int)bytes * 8; dc->bottomup = bottom_up;
    dc->viewport_x = copy.vx; dc->viewport_y = copy.vy;
    if (format == REVERSED32) dc->format = &reversed;
    if (format == RGB565) dc->format = &rgb565;
    if (format == INDEX8) dc->format = &indexed;
    /* Source DC paint/application clips do not crop selected bitmap reads. */
    GDI_RECT rectangle = {3, 2, 4, 3};
    uint64_t token = 0;
    if (clip) {
        CHECK(gdi32_push_paint_clip(handle, &rectangle, &token));
        dc->clip_active = TRUE; dc->clip_region.count = 0;
    }
    BOOL valid = copy.width > 0 && copy.height > 0 && (!per_pixel || bytes == 4);
    if (valid) for (int y = 0; y < copy.height; y++) for (int x = 0; x < copy.width; x++) {
        int64_t sx = (int64_t)copy.x + copy.vx + x;
        int64_t sy = (int64_t)copy.y + copy.vy + y;
        DWORD color = per_pixel ? 0 : 0xff000000u;
        if (sx >= 0 && sx < SOURCE_W && sy >= 0 && sy < SOURCE_H)
            color = source_color(format, (int)sx, (int)sy) |
                    ((DWORD)(per_pixel ? source_alpha((int)sx, (int)sy) : 255) << 24);
        unsigned at = PREFIX + (y * copy.width + x) * 4;
        for (unsigned byte = 0; byte < 4; byte++) expected[at + byte] = (BYTE)(color >> (8 * byte));
    }
    unsigned old_failures = failures, old_dirty = window_dirty, old_presentations = presentations;
    CHECK(gdi32_copy_layered_pixels(handle, copy.x, copy.y, copy.width, copy.height,
                                     output + PREFIX / 4, per_pixel) == valid);
    for (unsigned i = 0; i < BYTES; i++) {
        CHECK(destination[i] == expected[i]); CHECK(source[i] == saved[i]);
    }
    CHECK(window_dirty == old_dirty && presentations == old_presentations);
    if (old_failures != failures)
        printf("case format=%u bottom=%d alpha=%d clip=%d xy=%d,%d size=%d,%d viewport=%d,%d\n",
               format, bottom_up, per_pixel, clip, copy.x, copy.y, copy.width, copy.height, copy.vx, copy.vy);
    if (token) gdi32_pop_paint_clip(handle, token, TRUE);
    discard_layer_dc(handle);
}

static void alias_case(BOOL reverse, BOOL per_pixel)
{
    for (unsigned i = 0; i < BYTES / 4; i++) input[i] = 0x10203040u + i * 0x01030507u;
    BYTE *source = (BYTE *)input;
    for (unsigned i = 0; i < BYTES; i++) expected[i] = source[i];
    unsigned src = PREFIX / 4 + reverse, dst = PREFIX / 4 + !reverse;
    for (unsigned x = 0; x < 5; x++) {
        DWORD color = input[src + x + 2];
        if (!per_pixel) color |= 0xff000000u;
        for (unsigned byte = 0; byte < 4; byte++)
            expected[(dst + x) * 4 + byte] = (BYTE)(color >> (8 * byte));
    }
    HDC handle = memory_dc(input + src, SOURCE_W, SOURCE_H, SOURCE_PITCH / 4);
    CHECK(gdi32_copy_layered_pixels(handle, 2, 0, 5, 1, input + dst, per_pixel));
    for (unsigned i = 0; i < BYTES; i++) CHECK(source[i] == expected[i]);
    discard_layer_dc(handle);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void benchmark(int width, int height, BOOL per_pixel)
{
    SIZE_T size = (SIZE_T)width * height * 4;
    uint32_t *source = malloc(size), *destination = malloc(size);
    if (!source || !destination) abort();
    for (SIZE_T i = 0; i < size / 4; i++) source[i] = 0x80201008u;
    HDC handle = memory_dc(source, width, height, width);
    BOOL (*volatile copy)(HDC, int, int, int, int, uint32_t *, BOOL) = gdi32_copy_layered_pixels;
    for (int sample = -1; sample < 3; sample++) {
        unsigned frames = sample < 0 ? 3 : 30;
        uint64_t start = monotonic_ns();
        for (unsigned frame = 0; frame < frames; frame++)
            CHECK(copy(handle, 0, 0, width, height, destination, per_pixel));
        uint64_t elapsed = monotonic_ns() - start;
        CHECK(destination[0] == (per_pixel ? 0x80201008u : 0xff201008u));
        CHECK(destination[size / 4 - 1] == destination[0]);
        if (sample >= 0)
            printf("BENCH width=%d height=%d alpha=%d sample=%d ns_per_copy=%llu\n",
                   width, height, per_pixel, sample, (unsigned long long)(elapsed / frames));
    }
    discard_layer_dc(handle);
    free(destination); free(source);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        for (BOOL alpha = FALSE; alpha <= TRUE; alpha++) {
            benchmark(700, 440, alpha);
            benchmark(1920, 1080, alpha);
        }
        return failures ? 1 : 0;
    }
    if (gdi_screen_contract_main()) return 1;
    static const LAYER_COPY copies[] = {
        {0, 0, 7, 5, 0, 0}, {-2, -1, 10, 8, 0, 0}, {2, 3, 10, 8, 0, 0},
        {3, 2, 4, 3, -2, 1}, {0, 0, 10, 8, -9, -7}, {0, 0, 10, 8, 9, 7},
        {0, 0, 0, 8, 0, 0}, {0, 0, 10, -1, 0, 0},
        {2147483647, 0, 10, 8, 1, 0}, {-2147483647 - 1, 0, 10, 8, -1, 0},
        {0, 2147483647, 10, 8, 0, 1}, {0, -2147483647 - 1, 10, 8, 0, -1},
    };
    for (unsigned format = NATIVE32; format <= INDEX8; format++)
        for (BOOL bottom = FALSE; bottom <= TRUE; bottom++)
            for (BOOL alpha = FALSE; alpha <= TRUE; alpha++)
                for (BOOL clip = FALSE; clip <= TRUE; clip++)
                    for (unsigned c = 0; c < sizeof(copies) / sizeof(copies[0]); c++)
                        copy_case(format, bottom, alpha, clip, copies[c]);
    for (BOOL reverse = FALSE; reverse <= TRUE; reverse++)
        for (BOOL alpha = FALSE; alpha <= TRUE; alpha++) alias_case(reverse, alpha);
    CHECK(!gdi32_copy_layered_pixels((HDC)(ULONG_PTR)0xbad0, 0, 0, 1, 1, output, FALSE));
    HDC handle = memory_dc(input, 1, 1, 1);
    CHECK(!gdi32_copy_layered_pixels(handle, 0, 0, 1, 1, NULL, FALSE));
    discard_layer_dc(handle);
    printf("GDI layered copy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
