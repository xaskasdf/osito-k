#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/screen_capture.h"

#define WIDTH 801u
#define HEIGHT 601u
#define PITCH 808u
static unsigned framebuffer[PITCH * HEIGHT];
static screen_view_t view;
static unsigned live, failures, checks, view_calls;
static int allocation_fails, cancel, change_view;
static unsigned long long allocation_size, clock_ms;
static unsigned clock_step, yields, yield_delay, cancel_on_yield;

#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static int get_view(screen_view_t *out)
{
    *out = view;
    if (change_view && view_calls++) out->pitch++;
    return SCREEN_OK;
}
static void *allocate(unsigned long long size)
{
    if (allocation_fails) return NULL;
    CHECK(!live);
    void *p = malloc((size_t)size);
    if (p) { live++; allocation_size = size; }
    return p;
}
static void release(void *p)
{
    CHECK(live == 1);
    for (unsigned long long i = 0; i < allocation_size; i++) {
        if (((unsigned char *)p)[i]) { CHECK(0); break; }
    }
    live--; allocation_size = 0; free(p);
}
static unsigned long long now(void) { clock_ms += clock_step; return clock_ms; }
static int cancelled(void) { return cancel; }
static void yield(void)
{
    yields++;
    clock_ms += yield_delay;
    if (cancel_on_yield) cancel = 1;
}
static const screen_capture_ops_t ops = {get_view, allocate, release, now, cancelled, yield};

void screen_capture_host_init(void)
{
    screen_capture_init(&ops);
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < PITCH; x++) {
            framebuffer[y * PITCH + x] = x < WIDTH ?
                0xa5000000u | ((x * 3 & 255) << 16) | ((y * 5 & 255) << 8) | ((x ^ y) & 255) :
                0xdeadbeef;
        }
    }
    view = (screen_view_t){framebuffer, sizeof(framebuffer), WIDTH, HEIGHT, PITCH, 1};
    clock_ms = 12345;
    allocation_fails = cancel = change_view = 0;
    clock_step = yields = yield_delay = cancel_on_yield = 0;
    view_calls = 0;
}

#ifdef SCREEN_CAPTURE_UNIT_TEST
extern int zlib_deflate(const unsigned char *, unsigned, unsigned char *, unsigned *);
extern int zlib_inflate(const unsigned char *, unsigned, unsigned char *, unsigned *);
static unsigned char output[SCREEN_FRAME];
static int finished;
static int request(const void *p, unsigned n)
{
    memset(output, 0xcc, sizeof(output));
    return screen_capture_request(NULL, p, n, output, sizeof(output), &finished);
}
static void begin(void)
{
    screen_capture_host_init();
    CHECK(request("V", 1) == 13 && !output[0] && !memcmp(output + 1, "OSCR0001", 8));
}
int main(void)
{
    int f;
    CHECK(screen_capture_request(NULL, (const unsigned char *)"V", 1, output, sizeof(output), &f) == -1);
    begin();
    CHECK(request("V", 1) == 1 && output[0] == SCREEN_INVALID);
    CHECK(request("Cextra", 6) == 1 && output[0] == SCREEN_INVALID && !live);
    CHECK(request("C", 1) == 73 && !output[0] && live);
    CHECK(output[1] == 0 && output[3] == 3 && output[4] == 33);
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_BUSY);
    unsigned char read[13] = {'R'};
    read[12] = 6;
    CHECK(request(read, sizeof(read)) == 7 && !memcmp(output + 1, "\0\0\0\3\0\1", 6));
    /* Snapshot remains stable if the scanout changes after acquisition. */
    framebuffer[0] = 0xffffffff;
    CHECK(request(read, sizeof(read)) == 7 && output[1] == 0);
    read[0] = 'Z';
    CHECK(request(read, sizeof(read)) == 8 && output[1] == 0 && output[2] == 0);
    read[0] = 'R';
    memset(read + 1, 0xff, 8);
    CHECK(request(read, sizeof(read)) == 1 && output[0] == SCREEN_INVALID);
    CHECK(request("R", 1) == 1 && output[0] == SCREEN_INVALID);
    CHECK(request("X", 1) == 1 && !output[0] && !live);
    CHECK(request("Q", 1) == 1 && !output[0] && finished);
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_INVALID);

    for (unsigned format = 0; format < 2; format++) {
        begin();
        view = (screen_view_t){framebuffer, 32, 3, 2, 5, format};
        framebuffer[0] = 0xa5112233;
        framebuffer[5] = 0xbb445566;
        CHECK(request("C", 1) == 73 && !output[0]);
        memset(read + 1, 0, 12); read[12] = 18;
        CHECK(request(read, sizeof(read)) == 19);
        CHECK(output[1] == (format ? 0x11 : 0x33) && output[2] == 0x22);
        CHECK(output[10] == (format ? 0x44 : 0x66) && output[11] == 0x55);
        screen_capture_reset(NULL);
        CHECK(!live);
    }
    for (unsigned bad = 0; bad < 8; bad++) {
        begin();
        switch (bad) {
        case 0: view.width = 0; break;
        case 1: view.height = 0; break;
        case 2: view.pitch = view.width - 1; break;
        case 3: view.bytes = 3; break;
        case 4: view.width = 0xffffffff; break;
        case 5: view.height = 0xffffffff; break;
        case 6: view.pixels = NULL; break;
        case 7: view.width = view.height = view.pitch = 8192; break;
        }
        CHECK(request("C", 1) == 1 && output[0] == SCREEN_INVALID && !live);
    }
    begin(); view.pixel_format = 2;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_UNSUPPORTED && !live);
    begin(); allocation_fails = 1;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_NOMEM && !live);
    begin(); cancel = 1;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_CANCELLED && !live);
    begin(); clock_step = 1; yield_delay = SCREEN_CAPTURE_TIMEOUT_MS + 1;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_TIMEOUT && !live && yields == 1);
    begin(); clock_step = 1; cancel_on_yield = 1;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_CANCELLED && !live && yields == 1);
    /* Sub-slice work must not incur an arbitrary long ready-queue rotation. */
    begin(); yield_delay = 6000;
    CHECK(request("C", 1) == 73 && !output[0] && live && !yields);
    screen_capture_reset(NULL);
    begin(); clock_step = 1;
    CHECK(request("C", 1) == 73 && !output[0] && live && yields > 0);
    screen_capture_reset(NULL);
    begin(); change_view = 1;
    CHECK(request("C", 1) == 1 && output[0] == SCREEN_CHANGED && !live);
    begin();
    CHECK(screen_capture_request(NULL, NULL, 1, output, sizeof(output), &finished) == -1);
    CHECK(screen_capture_request(NULL, (const unsigned char *)"V", 1, output, 1, &finished) == -1);
    for (unsigned i = 0; i < 12; i++) {
        CHECK(request("C", 1) == 73);
        CHECK(request("X", 1) == 1 && !live);
    }
    CHECK(request("C", 1) == 73);
    screen_capture_reset(NULL);
    CHECK(!live);
    begin();
    memset(framebuffer, 0, sizeof(framebuffer));
    CHECK(request("C", 1) == 73);
    memset(read, 0, sizeof(read)); read[0] = 'Z'; read[11] = 128;
    int encoded = request(read, sizeof(read));
    CHECK(encoded > 2 && encoded < 1024 && output[1] == 1);
    unsigned char decoded[32768], tiny[16];
    unsigned length = sizeof(decoded);
    CHECK(!zlib_inflate(output + 2, encoded - 2, decoded, &length) && length == sizeof(decoded));
    CHECK(!memcmp(decoded, framebuffer, length));
    for (unsigned cap = 0; cap <= sizeof(tiny); cap++) {
        memset(tiny, 0xa5, sizeof(tiny)); length = cap;
        CHECK(zlib_deflate((const unsigned char *)framebuffer, sizeof(decoded), tiny, &length) < 0);
        for (unsigned i = cap; i < sizeof(tiny); i++) CHECK(tiny[i] == 0xa5);
    }
    screen_capture_reset(NULL);
    printf("screen-capture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
#endif
