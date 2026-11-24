/*
 * OsitoK x86-64 — Wayland Display Protocol Stub
 *
 * Minimal Wayland-like protocol for window management.
 * Applications communicate via shared memory buffers and
 * a simple message protocol instead of direct SHM manipulation.
 *
 * This is NOT full Wayland — it's a compatible subset that lets
 * programs create surfaces, attach buffers, and receive input
 * without knowing OsitoK compositor internals.
 *
 * Protocol messages (via /dev/wayland):
 *   REQUEST_DISPLAY      → returns display info (size, format)
 *   CREATE_SURFACE(w,h)  → returns surface_id + shm_handle
 *   ATTACH_BUFFER(sid)   → marks surface as ready for composite
 *   COMMIT(sid)          → signals compositor to redraw
 *   DESTROY_SURFACE(sid) → cleanup
 *   GET_INPUT(sid)       → returns pending input events
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* Compositor integration */
extern uint32_t compositor_create_window(uint32_t shm, int16_t x, int16_t y,
    uint16_t w, uint16_t h, uint32_t pid, const char *title)
    __attribute__((weak));
extern void compositor_destroy_window(uint32_t wid) __attribute__((weak));
extern void compositor_signal_dirty(uint32_t wid) __attribute__((weak));

/* SHM */
extern uint32_t shm_create(uint64_t size, uint32_t flags) __attribute__((weak));
extern void    *shm_map(uint32_t handle) __attribute__((weak));

extern uint32_t display_get_width(void) __attribute__((weak));
extern uint32_t display_get_height(void) __attribute__((weak));

/* ── Protocol Message Types ──────────────────────────────────── */

#define WL_MSG_DISPLAY_INFO     1
#define WL_MSG_CREATE_SURFACE   2
#define WL_MSG_COMMIT           3
#define WL_MSG_DESTROY          4
#define WL_MSG_GET_INPUT        5

/* Request/response structures */
typedef struct __attribute__((packed)) {
    uint32_t type;       /* WL_MSG_* */
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} wl_request_t;

typedef struct __attribute__((packed)) {
    int32_t  status;     /* 0 = success, <0 = error */
    uint32_t surface_id; /* For CREATE_SURFACE response */
    uint32_t shm_handle; /* SHM buffer handle */
    uint32_t width;
    uint32_t height;
    uint32_t format;     /* Pixel format (ARGB8888 = 0) */
} wl_response_t;

/* ── Surface Tracking ────────────────────────────────────────── */

#define MAX_WL_SURFACES 16

typedef struct {
    bool     active;
    uint32_t wid;         /* Compositor window ID */
    uint32_t shm_handle;
    void    *pixels;
    uint16_t width, height;
    uint32_t owner_pid;
} wl_surface_t;

static wl_surface_t wl_surfaces[MAX_WL_SURFACES];

/* ── Public API ──────────────────────────────────────────────── */

/* Process a Wayland protocol message. Called from syscall layer
 * when a process writes to /dev/wayland. */
int wayland_process_message(const wl_request_t *req, wl_response_t *resp,
                            uint32_t pid)
{
    memset(resp, 0, sizeof(*resp));

    switch (req->type) {
    case WL_MSG_DISPLAY_INFO:
        resp->status = 0;
        resp->width = display_get_width ? display_get_width() : 1024;
        resp->height = display_get_height ? display_get_height() : 768;
        resp->format = 0;  /* ARGB8888 */
        return 0;

    case WL_MSG_CREATE_SURFACE: {
        if (!shm_create || !shm_map || !compositor_create_window) {
            resp->status = -1;
            return -1;
        }

        /* Find free slot */
        int slot = -1;
        for (int i = 0; i < MAX_WL_SURFACES; i++) {
            if (!wl_surfaces[i].active) { slot = i; break; }
        }
        if (slot < 0) { resp->status = -1; return -1; }

        uint16_t w = (uint16_t)req->width;
        uint16_t h = (uint16_t)req->height;
        uint32_t shm = shm_create((uint64_t)w * h * 4, 3);
        void *px = shm_map(shm);
        if (!px) { resp->status = -1; return -1; }

        uint32_t wid = compositor_create_window(shm, 100, 100, w, h,
                                                 pid, "Wayland Surface");

        wl_surfaces[slot].active = true;
        wl_surfaces[slot].wid = wid;
        wl_surfaces[slot].shm_handle = shm;
        wl_surfaces[slot].pixels = px;
        wl_surfaces[slot].width = w;
        wl_surfaces[slot].height = h;
        wl_surfaces[slot].owner_pid = pid;

        resp->status = 0;
        resp->surface_id = (uint32_t)slot;
        resp->shm_handle = shm;
        resp->width = w;
        resp->height = h;

        serial_puts("[WL] Surface created: ");
        serial_putdec(w);
        serial_puts("x");
        serial_putdec(h);
        serial_puts(" sid=");
        serial_putdec((uint64_t)slot);
        serial_puts("\n");
        return 0;
    }

    case WL_MSG_COMMIT: {
        uint32_t sid = req->surface_id;
        if (sid >= MAX_WL_SURFACES || !wl_surfaces[sid].active) {
            resp->status = -1;
            return -1;
        }
        if (compositor_signal_dirty)
            compositor_signal_dirty(wl_surfaces[sid].wid);
        resp->status = 0;
        return 0;
    }

    case WL_MSG_DESTROY: {
        uint32_t sid = req->surface_id;
        if (sid >= MAX_WL_SURFACES || !wl_surfaces[sid].active) {
            resp->status = -1;
            return -1;
        }
        if (compositor_destroy_window)
            compositor_destroy_window(wl_surfaces[sid].wid);
        wl_surfaces[sid].active = false;
        resp->status = 0;
        serial_puts("[WL] Surface destroyed: sid=");
        serial_putdec(sid);
        serial_puts("\n");
        return 0;
    }

    default:
        resp->status = -1;
        return -1;
    }
}

/* Cleanup all surfaces owned by a process */
void wayland_cleanup_process(uint32_t pid)
{
    for (int i = 0; i < MAX_WL_SURFACES; i++) {
        if (wl_surfaces[i].active && wl_surfaces[i].owner_pid == pid) {
            if (compositor_destroy_window)
                compositor_destroy_window(wl_surfaces[i].wid);
            wl_surfaces[i].active = false;
        }
    }
}
