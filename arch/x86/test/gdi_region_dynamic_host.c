/* Ownership, exact coverage and allocation-failure transactions for large
 * production regions. The allocator wrappers affect only this executable. */
#define main gdi_screen_contract_main
#include "gdi_screen_host.c"
#undef main

extern void *__real_malloc(size_t);
extern void __real_free(void *);
static long allocations, fail_after = -1;

void *__wrap_malloc(size_t bytes)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = __real_malloc(bytes);
    if (p) allocations++;
    return p;
}

void __wrap_free(void *p)
{
    if (p) allocations--;
    __real_free(p);
}

static BOOL sparse_point(int x, int y)
{
    return x >= 0 && x < 480 && x % 3 == 0 && y == (x / 3 % 9) * 3;
}

static void sparse_matches(HGDIOBJ region, int dx, int dy)
{
    for (int y = -1; y < 30; y++) for (int x = -1; x < 485; x++)
        CHECK(!!PtInRegion(region, x + dx, y + dy) == sparse_point(x, y));
}

static void region_dynamic_contract(void)
{
    long baseline = allocations;
    GDI_REGION_SCOPE(source);
    GDI_REGION_SCOPE(snapshot);
    for (int i = 0; i < 160; i++) {
        GDI_RECT part = {i * 3, (i % 9) * 3, i * 3 + 1, (i % 9) * 3 + 1};
        CHECK(region_append_rect(&source, part));
    }
    CHECK(source.count == 160 && source.storage && allocations == baseline + 1);
    HGDIOBJ original = CreateRectRgn(0, 0, 0, 0);
    HGDIOBJ copy = CreateRectRgn(0, 0, 0, 0);
    HGDIOBJ destination = CreateRectRgn(700, 700, 710, 710);
    HGDIOBJ empty = CreateRectRgn(0, 0, 0, 0);
    CHECK(gdi32_region_data_write(original, &source) == GDI_COMPLEXREGION);
    CHECK(gdi32_region_data_read(original, &snapshot));
    CHECK(gdi32_region_data_write(copy, &snapshot) == GDI_COMPLEXREGION);
    CHECK(allocations == baseline + 1); /* Immutable copies share the backing. */
    sparse_matches(original, 0, 0);

    fail_after = 0;
    CHECK(OffsetRgn(original, 5, -4) == GDI_RGN_ERROR);
    CHECK(CombineRgn(destination, original, empty, GDI_RGN_DIFF) == GDI_RGN_ERROR);
    fail_after = -1;
    GDI_RECT bounds;
    CHECK(GetRgnBox(destination, &bounds) == GDI_SIMPLEREGION &&
          bounds.left == 700 && bounds.top == 700 && bounds.right == 710 && bounds.bottom == 710);
    sparse_matches(original, 0, 0); /* Both failed mutations are transactional. */
    CHECK(OffsetRgn(original, 5, -4) == GDI_COMPLEXREGION);
    sparse_matches(original, 5, -4);
    sparse_matches(copy, 0, 0);
    CHECK(DeleteObject(original));

    HDC hdc = memory_dc(window_pixels + GUARD, 5, 4, 8);
    GDI_DC *dc = dc_from_handle(hdc);
    CHECK(SelectClipRgn(hdc, copy) == GDI_COMPLEXREGION);
    uint64_t token = 0;
    CHECK(gdi32_push_paint_region(hdc, &snapshot, &token));
    CHECK(SetRectRgn(copy, 700, 700, 710, 710));
    CHECK(DeleteObject(copy));
    gdi32_region_data_clear(&source);
    gdi32_region_data_clear(&snapshot);
    CHECK(dc->clip_region.count == 160 && dc->paint_clip->region.count == 160);
    CHECK(gdi_dc_point_visible(dc, 0, 0) && !gdi_dc_point_visible(dc, 1, 0));
    CHECK(gdi_dc_point_visible(dc, 477, 18) && !gdi_dc_point_visible(dc, 478, 18));
    CHECK(SelectClipRgn(hdc, NULL) == GDI_SIMPLEREGION);
    CHECK(gdi_dc_point_visible(dc, 477, 18));
    gdi32_pop_paint_clip(hdc, token, TRUE);
    gdi_memset(dc, 0, sizeof(*dc));
    CHECK(DeleteObject(destination) && DeleteObject(empty));
    CHECK(allocations == baseline);

    /* Exhaust each allocation in a >64-rectangle union. Every failure must
     * keep the old destination; every partial result must release storage. */
    for (int i = 0; i < 160; i++)
        CHECK(region_append_rect(&source, (GDI_RECT){i * 3, 0, i * 3 + 1, 1}));
    original = CreateRectRgn(0, 0, 0, 0);
    CHECK(gdi32_region_data_write(original, &source) == GDI_COMPLEXREGION);
    HGDIOBJ second = CreateRectRgn(0, 2, 480, 3);
    BOOL succeeded = FALSE;
    for (int fail = 0; fail < 12; fail++) {
        destination = CreateRectRgn(700, 700, 710, 710);
        long before = allocations;
        fail_after = fail;
        int result = CombineRgn(destination, original, second, GDI_RGN_OR);
        fail_after = -1;
        if (!result) {
            CHECK(allocations == before);
            CHECK(GetRgnBox(destination, &bounds) == GDI_SIMPLEREGION && bounds.left == 700);
        } else {
            for (int x = 0; x < 480; x++) for (int y = 0; y < 4; y++)
                CHECK(!!PtInRegion(destination, x, y) == (y == 2 || (y == 0 && x % 3 == 0)));
            succeeded = TRUE;
        }
        CHECK(DeleteObject(destination));
        if (succeeded) break;
    }
    CHECK(succeeded);
    CHECK(DeleteObject(second));
    gdi32_region_data_clear(&source);
    /* Process exit releases live handles, while a retained snapshot survives. */
    CHECK(gdi32_region_data_read(original, &snapshot));
    gdi_region_release_process(1);
    CHECK(region_from_handle(original) == NULL && snapshot.count == 160);
    CHECK(GDI_REGION_RECTS(&snapshot)[159].left == 477);
    gdi32_region_data_clear(&snapshot);
    CHECK(allocations == baseline);
}

int main(void)
{
    region_dynamic_contract();
    CHECK(allocations == 0);
    printf("GDI dynamic regions: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
