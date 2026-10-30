/* Exercise production BitBlt with independent pixel snapshots and guarded
 * storage. Reuse the screen harness's kernel/USER32 stubs. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#undef NULL /* The freestanding kernel headers provide their own definition. */
#define main gdi_screen_contract_main
#include "gdi_screen_host.c"
#undef main

enum { BLT_W = 13, BLT_H = 9, BLT_PITCH = 64, BLT_BYTES = COUNT * 4 };
static uint32_t blt_source[COUNT], blt_destination[COUNT];
static BYTE before_source[BLT_BYTES], expected_destination[BLT_BYTES];
static GDI_PIXEL_FORMAT rgb565 = {.red = 0xf800, .green = 0x07e0, .blue = 0x001f};

static BOOL inside(const GDI_RECT *rect, int x, int y)
{
    return x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom;
}

static void discard_memory_dc(HDC handle)
{
    GDI_DC *dc = dc_from_handle(handle);
    CHECK(dc && !dc->paint_clip && !dc->pooled_window && !dc->is_screen);
    /* memory_dc owns only borrowed pixels and a stock brush. Production
     * DeleteDC takes the kernel's IRQ-disabling brush lock, unavailable here. */
    if (dc) gdi_memset(dc, 0, sizeof(*dc));
}

static void blt_case(int bpp, int source_bottom, int destination_bottom,
                     BOOL alias, unsigned clip_kind, int dx, int dy,
                     int width, int height, int sx, int sy)
{
    BYTE *source = (BYTE *)blt_source;
    BYTE *destination = alias ? source : (BYTE *)blt_destination;
    for (unsigned i = 0; i < BLT_BYTES; i++) {
        source[i] = (BYTE)((i * 31 + i / 17) & 255);
        if (!alias) destination[i] = 0x5a;
        before_source[i] = source[i];
        expected_destination[i] = destination[i];
    }
    HDC sh = memory_dc(blt_source + GUARD, BLT_W, BLT_H, BLT_PITCH / 4);
    HDC dh = memory_dc((uint32_t *)destination + GUARD, BLT_W, BLT_H, BLT_PITCH / 4);
    GDI_DC *src = dc_from_handle(sh), *dst = dc_from_handle(dh);
    src->bpp = dst->bpp = bpp;
    src->bottomup = source_bottom;
    dst->bottomup = destination_bottom;
    if (bpp == 16) src->format = dst->format = &rgb565;

    GDI_RECT paint = {2, 1, 11, 8};
    GDI_RECT regions[2] = {{1, 2, 9, 7}, {10, 0, 13, 4}};
    BOOL has_paint = clip_kind == 1 || clip_kind == 3 || clip_kind == 6 || clip_kind == 8;
    BOOL app_clip = clip_kind == 2 || clip_kind == 3 || clip_kind == 4 || clip_kind == 5 || clip_kind == 7;
    unsigned region_count = clip_kind == 5 ? 0 : clip_kind == 4 ? 2 : 1;
    if (clip_kind == 6) paint = (GDI_RECT){3, 3, 3, 6};
    if (clip_kind == 7) regions[0] = (GDI_RECT){-8, -8, -1, -1};
    uint64_t outer_token = 0, inner_token = 0;
    if (has_paint) CHECK(gdi32_push_paint_clip(dh, &paint, &outer_token));
    if (clip_kind == 8) {
        GDI_RECT inner = {4, 3, 12, 9};
        CHECK(gdi32_push_paint_clip(dh, &inner, &inner_token));
        paint = (GDI_RECT){4, 3, 11, 8};
    }
    dst->clip_active = app_clip;
    dst->clip_region.count = region_count;
    for (unsigned i = 0; i < region_count; i++) dst->clip_region.rects[i] = regions[i];

    /* Oracle: each destination pixel comes from the pre-call image, regardless
     * of copying direction, implementation path or overlapping storage. */
    int bytes = bpp / 8;
    for (int y = 0; y < BLT_H; y++) {
        for (int x = 0; x < BLT_W; x++) {
            int64_t u = (int64_t)x - dx, v = (int64_t)y - dy;
            int64_t source_x = (int64_t)sx + u, source_y = (int64_t)sy + v;
            if (u < 0 || v < 0 || u >= width || v >= height ||
                source_x < 0 || source_x >= BLT_W || source_y < 0 || source_y >= BLT_H)
                continue;
            if (has_paint && !inside(&paint, x, y)) continue;
            BOOL visible = !app_clip;
            for (unsigned i = 0; app_clip && i < region_count; i++)
                visible |= inside(&regions[i], x, y);
            if (!visible) continue;
            int source_row = source_bottom ? BLT_H - 1 - (int)source_y : (int)source_y;
            int destination_row = destination_bottom ? BLT_H - 1 - y : y;
            int from = GUARD * 4 + source_row * BLT_PITCH + (int)source_x * bytes;
            int to = GUARD * 4 + destination_row * BLT_PITCH + x * bytes;
            for (int byte = 0; byte < bytes; byte++)
                expected_destination[to + byte] = before_source[from + byte];
            if (bpp == 32 && (has_paint || app_clip)) expected_destination[to + 3] = 255;
        }
    }
    unsigned previous_failures = failures;
    CHECK(BitBlt(dh, dx, dy, width, height, sh, sx, sy, ROP_SRCCOPY));
    for (unsigned i = 0; i < BLT_BYTES; i++) {
        CHECK(destination[i] == expected_destination[i]);
        if (!alias) CHECK(source[i] == before_source[i]);
    }
    if (failures != previous_failures)
        printf("case bpp=%d rows=%d/%d alias=%d clip=%u dst=%d,%d size=%d,%d src=%d,%d\n",
               bpp, source_bottom, destination_bottom, alias, clip_kind, dx, dy, width, height, sx, sy);
    if (inner_token) gdi32_pop_paint_clip(dh, inner_token, TRUE);
    if (outer_token) gdi32_pop_paint_clip(dh, outer_token, TRUE);
    discard_memory_dc(dh);
    discard_memory_dc(sh);
}

static void conversion_case(void)
{
    uint32_t source[8] = {0x002211ee, 0x00112233, 0x00cc8844, 0x00445566};
    uint32_t destination[8];
    for (unsigned i = 0; i < 8; i++) destination[i] = SENTINEL;
    GDI_PIXEL_FORMAT reversed = {.red = 0xff, .green = 0xff00, .blue = 0xff0000};
    HDC sh = memory_dc(source, 4, 1, 8);
    HDC dh = memory_dc(destination, 4, 1, 8);
    dc_from_handle(sh)->format = &reversed;
    GDI_RECT rect = {1, 0, 3, 1};
    uint64_t token;
    CHECK(gdi32_push_paint_clip(dh, &rect, &token));
    CHECK(BitBlt(dh, 0, 0, 4, 1, sh, 0, 0, ROP_SRCCOPY));
    CHECK(destination[0] == SENTINEL && destination[3] == SENTINEL);
    CHECK(destination[1] == 0xff332211 && destination[2] == 0xff4488cc);
    for (unsigned i = 4; i < 8; i++) CHECK(destination[i] == SENTINEL);
    gdi32_pop_paint_clip(dh, token, TRUE);
    discard_memory_dc(sh);
    discard_memory_dc(dh);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void benchmark(int width, int height, BOOL tile)
{
    SIZE_T size = (SIZE_T)width * height * 4;
    uint32_t *source = malloc(size), *destination = malloc(size);
    if (!source || !destination) abort();
    for (SIZE_T i = 0; i < size / 4; i++) source[i] = (uint32_t)i & 0xffffff;
    HDC sh = memory_dc(source, width, height, width);
    HDC dh = memory_dc(destination, width, height, width);
    GDI_RECT rect = tile ? (GDI_RECT){40, 40, 104, 104} : (GDI_RECT){0, 0, width, height};
    uint64_t token;
    CHECK(gdi32_push_paint_clip(dh, &rect, &token));
    BOOL (WINAPI *volatile blit)(HDC, int, int, int, int, HDC, int, int, DWORD) = BitBlt;
    for (int warm = 0; warm < 3; warm++) CHECK(blit(dh, 0, 0, width, height, sh, 0, 0, ROP_SRCCOPY));
    for (unsigned sample = 0; sample < 3; sample++) {
        uint64_t start = monotonic_ns();
        for (int frame = 0; frame < 30; frame++)
            CHECK(blit(dh, 0, 0, width, height, sh, 0, 0, ROP_SRCCOPY));
        uint64_t elapsed = monotonic_ns() - start;
        CHECK(destination[(SIZE_T)rect.top * width + rect.left] ==
              (0xff000000 | source[(SIZE_T)rect.top * width + rect.left]));
        printf("BENCH width=%d height=%d tile=%d sample=%u ns_per_blt=%llu\n",
               width, height, tile, sample, (unsigned long long)(elapsed / 30));
    }
    gdi32_pop_paint_clip(dh, token, TRUE);
    discard_memory_dc(sh);
    discard_memory_dc(dh);
    free(destination);
    free(source);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        benchmark(700, 440, FALSE);
        benchmark(1920, 1080, FALSE);
        benchmark(1920, 1080, TRUE);
        return failures ? 1 : 0;
    }
    if (gdi_screen_contract_main()) return 1;
    static const int formats[] = {8, 16, 24, 32};
    static const int copies[][6] = {
        {0, 0, BLT_W, BLT_H, 0, 0}, {-3, -2, 15, 12, 0, 0},
        {2, 1, 15, 12, -3, -2}, {2, 3, 7, 4, 1, 2},
        {0, 0, 0, 8, 0, 0}, {0, 0, 13, -1, 0, 0},
        {2147483647, 0, 13, 9, 0, 0}, {0, 0, 13, 9, -2147483647 - 1, 0},
        {-2147483647 - 1, 0, 2147483647, 9, 0, 0},
        {0, 0, 2147483647, 2147483647, 0, 0}
    };
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
        for (int sb = 0; sb < 2; sb++)
            for (int db = 0; db < 2; db++)
                for (unsigned clip = 0; clip < 9; clip++)
                    for (unsigned i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
                        const int *c = copies[i];
                        blt_case(formats[f], sb, db, FALSE, clip, c[0], c[1], c[2], c[3], c[4], c[5]);
                    }
    /* Same-layout overlap in every direction, including bottom-up storage. */
    for (int bpp = 24; bpp <= 32; bpp += 8)
        for (int bottom = 0; bottom < 2; bottom++)
            for (unsigned clip = 0; clip < 4; clip++)
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                        blt_case(bpp, bottom, bottom, TRUE, clip,
                                 2 + dx, 2 + dy, 8, 5, 2, 2);
    conversion_case();
    printf("GDI clipped blit: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
