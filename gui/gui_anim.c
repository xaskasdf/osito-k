/*
 * OsitoK GUI — Animation Engine (Phase 3)
 *
 * Drives smooth property animations using easing curves.
 * All values are int32_t; easing functions use 16.16 fixed-point (0..0x10000).
 * No floating point — runs in kernel context.
 *
 * Usage:
 *   gui_anim_start(&win->x, target_x, 300, gui_ease_out_cubic, NULL, NULL);
 *   // each frame:
 *   gui_anim_tick(now_ms);
 */

#include "gui.h"

/* ── Fixed-point helpers ──────────────────────────────────── */

#define FP16 0x10000  /* 1.0 in 16.16 */

/* Clamp t to [0, FP16] */
static inline int32_t fp_clamp(int32_t t)
{
    if (t < 0) return 0;
    if (t > FP16) return FP16;
    return t;
}

/* Multiply two 16.16 fixed-point values */
static inline int32_t fp_mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

/* ── Easing functions ──────────────────────────────────────── */

int32_t gui_ease_linear(int32_t t)
{
    return fp_clamp(t);
}

int32_t gui_ease_out_cubic(int32_t t)
{
    /* 1 - (1-t)^3 */
    int32_t inv = FP16 - fp_clamp(t);
    int32_t inv2 = fp_mul(inv, inv);
    int32_t inv3 = fp_mul(inv2, inv);
    return FP16 - inv3;
}

int32_t gui_ease_out_quad(int32_t t)
{
    /* 1 - (1-t)^2 */
    int32_t inv = FP16 - fp_clamp(t);
    return FP16 - fp_mul(inv, inv);
}

int32_t gui_ease_in_out_quad(int32_t t)
{
    /* Smooth step: 3t^2 - 2t^3 */
    int32_t tc = fp_clamp(t);
    int32_t t2 = fp_mul(tc, tc);
    int32_t t3 = fp_mul(t2, tc);
    /* 3*t2 - 2*t3 in 16.16 */
    return fp_clamp(3 * t2 - 2 * fp_mul(t2, tc) + fp_mul(t3, 0));  /* simplified */
}

int32_t gui_ease_spring(int32_t t)
{
    /* Slight overshoot: out-cubic with 10% overshoot, then settle.
     * Uses a simple sin-based approach approximated with fixed-point:
     * f(t) = 1 + (1 - t)^2 * -sin(t * pi * 2.5)
     * Approximation: cubic out + small kick at t=0.7 */
    int32_t base = gui_ease_out_cubic(t);
    /* Add slight overshoot using triangle wave around t=0.7 */
    int32_t tc = fp_clamp(t);
    int32_t kick_t = tc - 0xB333;  /* 0.7 * FP16 */
    if (kick_t > 0 && kick_t < 0x4CCC) {  /* 0.3 * FP16 */
        /* Triangle: 0→peak→0 in [0.7, 1.0] */
        int32_t k = kick_t * 2 - 0x4CCC;  /* signed relative to midpoint */
        if (k < 0) k = -k;
        k = 0x4CCC - k;  /* inverted triangle: 0 at edges, peak at 0.85 */
        /* Scale to ~8% overshoot */
        int32_t kick = fp_mul(k, 0x1476);  /* *0.08 in 16.16 */
        base = fp_clamp(base + kick);
    }
    return base;
}

/* ── Animation slots ──────────────────────────────────────── */

#define GUI_ANIM_MAX 16

typedef struct {
    bool         active;
    int32_t     *target;
    int32_t      start_val;
    int32_t      end_val;
    uint64_t     start_ms;
    uint64_t     duration_ms;
    gui_ease_fn  ease;
    void       (*on_complete)(void *ctx);
    void        *ctx;
} gui_anim_t;

static gui_anim_t anim_slots[GUI_ANIM_MAX];

/* ── Public API ───────────────────────────────────────────── */

int gui_anim_start(int32_t *target, int32_t end_val, uint32_t duration_ms,
                   gui_ease_fn ease,
                   void (*on_complete)(void *ctx), void *ctx)
{
    /* Cancel any existing animation on this target */
    gui_anim_cancel(target);

    /* Find a free slot */
    for (int i = 0; i < GUI_ANIM_MAX; i++) {
        if (!anim_slots[i].active) {
            anim_slots[i].active      = true;
            anim_slots[i].target      = target;
            anim_slots[i].start_val   = *target;
            anim_slots[i].end_val     = end_val;
            anim_slots[i].start_ms    = 0;  /* set on first tick */
            anim_slots[i].duration_ms = duration_ms;
            anim_slots[i].ease        = ease ? ease : gui_ease_out_cubic;
            anim_slots[i].on_complete = on_complete;
            anim_slots[i].ctx         = ctx;
            return i;
        }
    }
    /* No slot available: jump directly to end value */
    *target = end_val;
    return -1;
}

void gui_anim_cancel(int32_t *target)
{
    for (int i = 0; i < GUI_ANIM_MAX; i++) {
        if (anim_slots[i].active && anim_slots[i].target == target) {
            anim_slots[i].active = false;
            return;
        }
    }
}

/* Update all animations.  Returns true if any animation is still active. */
bool gui_anim_tick(uint64_t now_ms)
{
    bool any = false;

    for (int i = 0; i < GUI_ANIM_MAX; i++) {
        gui_anim_t *a = &anim_slots[i];
        if (!a->active) continue;

        /* Capture start time on first tick */
        if (a->start_ms == 0)
            a->start_ms = now_ms;

        uint64_t elapsed = now_ms - a->start_ms;

        if (elapsed >= a->duration_ms) {
            /* Animation complete */
            *a->target = a->end_val;
            a->active  = false;
            if (a->on_complete)
                a->on_complete(a->ctx);
            continue;
        }

        /* Progress t in 16.16: elapsed / duration */
        int32_t t = (int32_t)((elapsed * FP16) / a->duration_ms);
        int32_t eased = a->ease(t);

        /* Interpolate: start + (end - start) * eased */
        int32_t delta = a->end_val - a->start_val;
        *a->target = a->start_val + (int32_t)(((int64_t)delta * eased) >> 16);

        any = true;
    }

    return any;
}

bool gui_anim_any_active(void)
{
    for (int i = 0; i < GUI_ANIM_MAX; i++)
        if (anim_slots[i].active) return true;
    return false;
}
