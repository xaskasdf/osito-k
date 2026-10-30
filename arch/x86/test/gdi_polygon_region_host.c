#define main gdi_screen_contract_main
#include "gdi_screen_host.c"
#undef main
#define POLY_CALL WINAPI
#include "polygon_region_oracle.h"

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

static void polygon_native_masks(void)
{
    for (unsigned i = 0; i < sizeof(poly_cases) / sizeof(poly_cases[0]); i++) {
        const POLY_CASE *test = &poly_cases[i];
        HGDIOBJ handle = test->counts ?
            CreatePolyPolygonRgn((const GDI_POINT *)test->points, test->counts, test->polygons, test->mode) :
            CreatePolygonRgn((const GDI_POINT *)test->points, test->count, test->mode);
        unsigned before = failures;
        CHECK(!!handle == test->created);
        if (handle) {
            POLY_RECT bounds;
            int kind = GetRgnBox(handle, (GDI_RECT *)&bounds);
            BOOL same = kind == test->kind && poly_same_rect(bounds, test->bounds);
            if (!same) printf("bounds=(%d,%d,%d,%d) kind=%d expected=(%d,%d,%d,%d) kind=%d\n",
                bounds.left, bounds.top, bounds.right, bounds.bottom, kind,
                test->bounds.left, test->bounds.top, test->bounds.right, test->bounds.bottom, test->kind);
            CHECK(same);
            CHECK(poly_mask_matches(test, handle, PtInRegion));
            CHECK(DeleteObject(handle));
        }
        if (failures != before)
            printf("case=%s first mismatching pixel=(%d,%d)\n", test->name, poly_fail_x, poly_fail_y);
        CHECK(allocations == 0);
    }
}

static void polygon_allocation_failures(void)
{
    GDI_POINT triangle[] = {{0,0},{257,0},{0,257}};
    BOOL succeeded = FALSE;
    for (int i = 0; i < 16; i++) {
        fail_after = i;
        HGDIOBJ region = CreatePolygonRgn(triangle, 3, 1);
        fail_after = -1;
        if (region) {
            CHECK(PtInRegion(region, 0, 256) && !PtInRegion(region, 1, 256));
            CHECK(DeleteObject(region)); succeeded = TRUE;
        }
        CHECK(allocations == 0);
        for (UINT h = 0; h < MAX_GDI_REGIONS; h++) CHECK(!gdi_regions[h].in_use);
        if (succeeded) break;
    }
    CHECK(succeeded);
    /* Very tall constant bands and collinear degenerates must not iterate
     * over millions of scanlines or allocate a rectangle for each row. */
    GDI_POINT tall[] = {{2,-100000000},{4,-100000000},{4,100000000},{2,100000000}};
    HGDIOBJ region = CreatePolygonRgn(tall, 4, 2);
    GDI_RECT bounds;
    CHECK(region && GetRgnBox(region, &bounds) == GDI_SIMPLEREGION &&
          bounds.left == 2 && bounds.right == 4 && bounds.top == -100000000 && bounds.bottom == 100000000);
    CHECK(DeleteObject(region));
    GDI_POINT line[] = {{0,-100000000},{1,0},{2,100000000}};
    region = CreatePolygonRgn(line, 3, 1);
    CHECK(region && GetRgnBox(region, &bounds) == GDI_NULLREGION);
    CHECK(DeleteObject(region));
    CHECK(allocations == 0);
}

int main(void)
{
    polygon_native_masks();
    polygon_allocation_failures();
    printf("GDI polygon regions: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
