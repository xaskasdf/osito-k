#include "polygon_region_cases.h"
static int poly_fail_x, poly_fail_y;

static int poly_expected(const POLY_CASE *test, int x, int y)
{
    for (unsigned i = 0; i < test->rectangles; i++) {
        POLY_RECT r = test->rects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return 1;
    }
    return 0;
}

static int poly_sample(const POLY_CASE *test, void *handle, int x, int y,
                         int (POLY_CALL *point)(void *, int, int))
{
    if (!!point(handle, x, y) == poly_expected(test, x, y)) return 1;
    poly_fail_x = x; poly_fail_y = y;
    return 0;
}

static int poly_mask_matches(const POLY_CASE *test, void *handle,
                               int (POLY_CALL *point)(void *, int, int))
{
    POLY_RECT b = test->bounds;
    int step = b.right - b.left > 64 ? (b.right - b.left) / 23 : 1;
    /* Every pixel of small masks; for large masks, every scanline plus both
     * sides of every native band boundary and deterministic interior samples. */
    for (int y = b.top - 1; y <= b.bottom; y++)
        for (int x = b.left - 1; x <= b.right; x += step)
            if (!poly_sample(test, handle, x, y, point)) return 0;
    for (unsigned i = 0; i < test->rectangles; i++) {
        POLY_RECT r = test->rects[i];
        int xs[] = {r.left - 1, r.left, (r.left + r.right) / 2, r.right - 1, r.right};
        int ys[] = {r.top - 1, r.top, (r.top + r.bottom) / 2, r.bottom - 1, r.bottom};
        for (unsigned x = 0; x < 5; x++) for (unsigned y = 0; y < 5; y++)
            if (!poly_sample(test, handle, xs[x], ys[y], point)) return 0;
    }
    return 1;
}

static int poly_same_rect(POLY_RECT a, POLY_RECT b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
