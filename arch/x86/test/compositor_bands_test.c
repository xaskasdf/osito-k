/* Exercise the production blitters with a saturated/deferred SMP backend.
 * Unused compositor entry points are removed by --gc-sections. */
#include "../kernel/compositor.c"

extern int printf(const char *, ...);

int ap_worker_count;

static struct {
    void (*func)(void *, void *);
    void *arg;
    void *result;
    int pending;
} jobs[3];
static unsigned fail_mask, submissions, waits;
static int eager, failures, cases;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d, case %d: %s\n", __LINE__, cases, #condition); \
        failures++; \
    } \
} while (0)

int smp_submit_any(void (*func)(void *, void *), void *arg, void *result)
{
    unsigned id = submissions++;
    CHECK(id < 3);
    if (id >= 3 || (fail_mask & (1U << id)))
        return -1;
    jobs[id].func = func;
    jobs[id].arg = arg;
    jobs[id].result = result;
    jobs[id].pending = !eager;
    if (eager)
        func(arg, result);
    return (int)id;
}

void smp_wait(int id)
{
    CHECK(id >= 0 && (unsigned)id < submissions);
    CHECK(!(fail_mask & (1U << id)));
    waits++;
    if (jobs[id].pending) {
        jobs[id].func(jobs[id].arg, jobs[id].result);
        jobs[id].pending = 0;
    }
}

#define SOURCE_PITCH 367U
#define SOURCE_HEIGHT 481U
#define OUTPUT_PITCH 601U
#define OUTPUT_HEIGHT 701U
#define SENTINEL 0xF17ECAFEU

static uint32_t source[SOURCE_PITCH * SOURCE_HEIGHT];
static uint32_t output[OUTPUT_PITCH * OUTPUT_HEIGHT];
static uint32_t expected[OUTPUT_PITCH * OUTPUT_HEIGHT];

static void begin_case(unsigned mask, int immediate)
{
    fail_mask = mask;
    eager = immediate;
    submissions = waits = 0;
    memset(jobs, 0, sizeof(jobs));
    for (unsigned i = 0; i < OUTPUT_PITCH * OUTPUT_HEIGHT; i++)
        output[i] = expected[i] = SENTINEL;
    cases++;
}

static void end_case(unsigned expected_submissions)
{
    CHECK(submissions == expected_submissions);
    CHECK(waits == submissions -
          (unsigned)__builtin_popcount(fail_mask & ((1U << submissions) - 1U)));
    for (unsigned i = 0; i < 3; i++)
        CHECK(!jobs[i].pending);
    for (unsigned i = 0; i < OUTPUT_PITCH * OUTPUT_HEIGHT; i++) {
        if (output[i] != expected[i]) {
            printf("FAIL case %d: pixel (%u,%u) %08x != %08x\n", cases,
                   i % OUTPUT_PITCH, i / OUTPUT_PITCH, output[i], expected[i]);
            failures++;
            break;
        }
    }
}

static void check_window(int x, int y, unsigned height, int clip,
                         unsigned mask, int immediate)
{
    const unsigned screen_w = 579, screen_h = 653;
    window_t win = {
        .x = x, .y = y, .width = 353, .height = height,
        .surface_pitch = SOURCE_PITCH, .surface_height = SOURCE_HEIGHT,
        .pixels = source, .clip_enabled = clip,
        .clip_x = 23, .clip_y = 49, .clip_width = 257, .clip_height = 389
    };
    begin_case(mask, immediate);
    unsigned drawn_rows = 0;
    for (unsigned dy = 0; dy < screen_h; dy++) {
        int row_drawn = 0;
        for (unsigned dx = 0; dx < screen_w; dx++) {
            int sx = (int)dx - x, sy = (int)dy - y;
            if (sx < 0 || sx >= win.width || sy < 0 || sy >= win.height)
                continue;
            if (clip && (dx < 23 || dx >= 280 || dy < 49 || dy >= 438))
                continue;
            expected[dy * OUTPUT_PITCH + dx] = source[sy * SOURCE_PITCH + sx];
            row_drawn = 1;
        }
        drawn_rows += row_drawn;
    }
    blit_window(output, OUTPUT_PITCH, screen_w, screen_h, &win);
    unsigned count = drawn_rows >= 256 ? (unsigned)ap_worker_count : 0;
    end_case(count > 2 ? 2 : count);
}

static void check_scale(unsigned scale, unsigned height,
                        unsigned mask, int immediate)
{
    window_t win = {
        .width = 109, .height = height, .surface_pitch = SOURCE_PITCH,
        .surface_height = SOURCE_HEIGHT, .pixels = source
    };
    fullscreen_layout_t layout = {
        .x = 17, .y = 29, .width = win.width * scale, .height = height * scale
    };
    begin_case(mask, immediate);
    for (unsigned y = 0; y < layout.height; y++)
        for (unsigned x = 0; x < layout.width; x++)
            expected[(layout.y + y) * OUTPUT_PITCH + layout.x + x] =
                source[(y / scale) * SOURCE_PITCH + x / scale];
    blit_scaled_integer(output, OUTPUT_PITCH, &win, &layout);
    unsigned count = height >= 64 ? (unsigned)ap_worker_count : 0;
    end_case(count > 3 ? 3 : count);
}

int main(void)
{
    for (unsigned y = 0; y < SOURCE_HEIGHT; y++)
        for (unsigned x = 0; x < SOURCE_PITCH; x++)
            source[y * SOURCE_PITCH + x] = 0xFF000000U | (y << 10) | x;

    for (ap_worker_count = 0; ap_worker_count <= 4; ap_worker_count++) {
        for (unsigned mask = 0; mask < 8; mask++) {
            for (int immediate = 0; immediate <= 1; immediate++) {
                check_window(11, 19, 479, 0, mask, immediate);
                check_window(-17, -23, 479, 1, mask, immediate);
                check_window(321, 411, 479, 0, mask, immediate);
                check_window(11, 19, 127, 0, mask, immediate);
                check_window(590, 19, 479, 0, mask, immediate);
                for (unsigned scale = 1; scale <= 3; scale++) {
                    check_scale(scale, 197, mask, immediate);
                    check_scale(scale, 31, mask, immediate);
                }
            }
        }
    }
    printf("compositor bands: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
