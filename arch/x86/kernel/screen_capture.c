/* One immutable, bounded RGB snapshot per authenticated SSH session. */
#include "screen_capture.h"
#include "crypto.h"
#include "zlib.h"

static const screen_capture_ops_t *capture;
static unsigned char *pixels;
static unsigned long long size;
static bool greeted;

static void put32(unsigned char *p, unsigned v)
{
    for (unsigned i = 4; i; v >>= 8) p[--i] = (unsigned char)v;
}

static void put64(unsigned char *p, unsigned long long v)
{
    for (unsigned i = 8; i; v >>= 8) p[--i] = (unsigned char)v;
}

static unsigned long long get64(const unsigned char *p)
{
    unsigned long long v = 0;
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void release_snapshot(void)
{
    if (pixels) {
        volatile unsigned char *p = pixels;
        for (unsigned long long i = 0; i < size; i++) p[i] = 0;
        capture->release(pixels);
    }
    pixels = NULL;
    size = 0;
}

void screen_capture_reset(void *arg)
{
    (void)arg;
    release_snapshot();
    greeted = false;
}

void screen_capture_init(const screen_capture_ops_t *ops)
{
    screen_capture_reset(NULL);
    capture = ops;
}

static int snapshot(unsigned char *metadata)
{
    screen_view_t view = {0}, after = {0};
    if (pixels) return SCREEN_BUSY;
    int rc = capture->view(&view);
    if (rc) return rc;
    if (!view.pixels || !view.width || !view.height || view.pitch < view.width ||
        view.width > SCREEN_MAX_DIMENSION || view.height > SCREEN_MAX_DIMENSION ||
        (unsigned long long)view.width * view.height > SCREEN_MAX_PIXELS ||
        ((unsigned long long)(view.height - 1) * view.pitch + view.width) * 4 > view.bytes)
        return SCREEN_INVALID;
    if (view.pixel_format > 1) return SCREEN_UNSUPPORTED;
    size = (unsigned long long)view.width * view.height * 3;
    pixels = capture->allocate(size);
    if (!pixels) { size = 0; return SCREEN_NOMEM; }
    unsigned long long started = capture->now_ms();
    unsigned long long slice_started = started;
    sha256_ctx hash;
    sha256_init(&hash);
    for (unsigned y = 0; y < view.height; y++) {
        if (capture->cancelled()) {
            release_snapshot();
            return SCREEN_CANCELLED;
        }
        if (capture->now_ms() - started > SCREEN_CAPTURE_TIMEOUT_MS) {
            release_snapshot();
            return SCREEN_TIMEOUT;
        }
        unsigned char *row = pixels + (unsigned long long)y * view.width * 3;
        for (unsigned x = 0; x < view.width; x++) {
            unsigned v = view.pixels[(unsigned long long)y * view.pitch + x];
            row[x * 3] = (unsigned char)(v >> (view.pixel_format ? 16 : 0));
            row[x * 3 + 1] = (unsigned char)(v >> 8);
            row[x * 3 + 2] = (unsigned char)(v >> (view.pixel_format ? 0 : 16));
        }
        sha256_update(&hash, row, view.width * 3);
        /* Row-count yielding makes a fast copy wait behind every runnable
         * application dozens of times. Yield by elapsed work instead; the
         * scheduler can still preempt this ordinary kernel task. */
        if (y + 1 < view.height &&
            capture->now_ms() - slice_started >= SCREEN_CAPTURE_SLICE_MS) {
            capture->yield();
            slice_started = capture->now_ms();
        }
    }
    if (capture->cancelled() ||
        capture->now_ms() - started > SCREEN_CAPTURE_TIMEOUT_MS) {
        rc = capture->cancelled() ? SCREEN_CANCELLED : SCREEN_TIMEOUT;
        release_snapshot();
        return rc;
    }
    rc = capture->view(&after);
    if (rc || after.pixels != view.pixels || after.bytes != view.bytes ||
        after.width != view.width || after.height != view.height ||
        after.pitch != view.pitch || after.pixel_format != view.pixel_format) {
        release_snapshot();
        return SCREEN_CHANGED;
    }
    put32(metadata, view.width);
    put32(metadata + 4, view.height);
    put32(metadata + 8, 1);       /* RGB8, tightly packed, top-down */
    put32(metadata + 12, 1);      /* source: primary GOP */
    put32(metadata + 16, 1);      /* non-atomic read of a live scanout */
    put32(metadata + 20, 0);      /* reserved */
    put64(metadata + 24, size);
    put64(metadata + 32, started);
    sha256_final(&hash, metadata + 40);
    return SCREEN_OK;
}

int screen_capture_request(void *arg, const unsigned char *p, unsigned n,
                           unsigned char *out, unsigned capacity, int *finished)
{
    (void)arg;
    if (!finished) return -1;
    *finished = 0;
    if (!capture || !capture->view || !capture->allocate || !capture->release ||
        !capture->now_ms || !capture->cancelled || !capture->yield ||
        !p || !n || n > SCREEN_FRAME || !out || capacity < SCREEN_FRAME)
        return -1;
    unsigned op = *p++, bytes = 0;
    int rc = SCREEN_INVALID;
    n--;
    if (!greeted && op != 'V') goto result;
    switch (op) {
    case 'V':
        if (n || greeted) break;
        greeted = true;
        memcpy(out + 1, "OSCR0001", 8);
        put32(out + 9, SCREEN_BLOCK);
        bytes = 12; rc = SCREEN_OK;
        break;
    case 'C':
        if (n) break;
        rc = snapshot(out + 1);
        if (!rc) bytes = SCREEN_METADATA;
        break;
    case 'Z':
    case 'R': {
        if (n != 12 || !pixels) break;
        unsigned long long offset = get64(p);
        unsigned count = (unsigned)p[8] << 24 | (unsigned)p[9] << 16 |
                         (unsigned)p[10] << 8 | p[11];
        if (!count || count > SCREEN_BLOCK || offset > size || count > size - offset)
            break;
        if (op == 'Z') {
            /* Independent zlib blocks keep CPU/stack and decoder output bounded.
             * Incompressible input uses the original bytes, never a larger frame. */
            unsigned compressed = count;
            bool encoded = !zlib_deflate(pixels + offset, count, out + 2, &compressed) &&
                           compressed < count;
            out[1] = encoded ? 1 : 0;
            if (!encoded) memcpy(out + 2, pixels + offset, count);
            bytes = 1 + (encoded ? compressed : count);
        } else {
            memcpy(out + 1, pixels + offset, count);
            bytes = count;
        }
        rc = SCREEN_OK;
        break;
    }
    case 'X':
        if (n) break;
        release_snapshot(); rc = SCREEN_OK;
        break;
    case 'Q':
        if (n) break;
        screen_capture_reset(NULL); *finished = 1; rc = SCREEN_OK;
        break;
    }
result:
    out[0] = (unsigned char)rc;
    return rc ? 1 : (int)bytes + 1;
}
