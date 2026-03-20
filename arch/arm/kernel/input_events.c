/*
 * OsitoK x86-64 — Raw Input Event System (X-RETINA)
 *
 * High-priority input event queue with TSC timestamps.
 * Supports keyboard and mouse events with event coalescing
 * for compositor consumption.
 *
 * Design:
 *   - Lock-free SPSC ring buffer (ISR writes, compositor reads)
 *   - TSC timestamps for input prediction
 *   - Event coalescing: mouse deltas accumulated between frames
 *   - Keyboard events preserved individually (no loss)
 *
 * This module sits between the PS/2 hardware (keyboard.c) and
 * the compositor. It provides raw events that the Win32 shim
 * (user32_shim.c in exe-reverse) can also consume.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* ── TSC (Time Stamp Counter) ────────────────────────────────── */

static inline uint64_t rdtsc(void)
{
    uint64_t cnt;
    __asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    return cnt;
}

/* ── Event Types ─────────────────────────────────────────────── */

#define INPUT_NONE       0
#define INPUT_KEY_DOWN   1
#define INPUT_KEY_UP     2
#define INPUT_MOUSE_MOVE 3
#define INPUT_MOUSE_BTN  4
#define INPUT_MOUSE_WHEEL 5

/* Mouse button flags */
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* Key event flags */
#define KEY_FLAG_EXTENDED (1 << 0)  /* 0xE0 prefix scancode */
#define KEY_FLAG_SHIFT    (1 << 1)
#define KEY_FLAG_CTRL     (1 << 2)
#define KEY_FLAG_ALT      (1 << 3)

/* ── Input Event Structure ───────────────────────────────────── */

typedef struct {
    uint8_t  type;           /* INPUT_KEY_DOWN, INPUT_MOUSE_MOVE, etc. */
    uint8_t  scancode;       /* PS/2 scancode (for key events) */
    uint8_t  buttons;        /* current mouse button state */
    uint8_t  flags;          /* KEY_FLAG_* modifiers */
    int16_t  dx, dy;         /* mouse delta (for MOUSE_MOVE) */
    int16_t  wheel;          /* mouse wheel delta */
    uint16_t _pad;
    uint64_t timestamp;      /* TSC at event time */
} input_event_t;

_Static_assert(sizeof(input_event_t) == 24, "input_event must be 24 bytes");

/* ── Ring Buffer ─────────────────────────────────────────────── */

#define INPUT_QUEUE_SIZE 256   /* power of 2 */
#define INPUT_QUEUE_MASK (INPUT_QUEUE_SIZE - 1)

static input_event_t input_queue[INPUT_QUEUE_SIZE];
static volatile uint32_t input_head;   /* write index (ISR only) */
static volatile uint32_t input_tail;   /* read index (consumer only) */

/* ── Mouse State ─────────────────────────────────────────────── */

static int32_t  mouse_x, mouse_y;     /* absolute cursor position */
static int32_t  screen_w, screen_h;   /* screen bounds for clamping */
static uint8_t  mouse_buttons;        /* current button state */

/* ── Keyboard Modifier State ─────────────────────────────────── */

static uint8_t kb_modifiers;  /* current KEY_FLAG_* state */

/* ── Enqueue event (called from ISR context) ─────────────────── */

static void input_enqueue(input_event_t *evt)
{
    uint32_t next = (input_head + 1) & INPUT_QUEUE_MASK;
    if (next == input_tail) {
        /* Queue full — drop oldest event */
        input_tail = (input_tail + 1) & INPUT_QUEUE_MASK;
    }
    input_queue[input_head] = *evt;
    input_head = next;
}

/* ── Keyboard Events (called from keyboard IRQ handler) ──────── */

void input_post_key(uint8_t scancode, bool pressed, bool extended)
{
    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = pressed ? INPUT_KEY_DOWN : INPUT_KEY_UP;
    evt.scancode = scancode;
    evt.flags = kb_modifiers;
    if (extended) evt.flags |= KEY_FLAG_EXTENDED;
    evt.timestamp = rdtsc();
    input_enqueue(&evt);

    /* Update modifier tracking */
    switch (scancode) {
    case 0x2A: case 0x36:  /* L/R Shift */
        if (pressed) kb_modifiers |= KEY_FLAG_SHIFT;
        else         kb_modifiers &= ~KEY_FLAG_SHIFT;
        break;
    case 0x1D:              /* Ctrl */
        if (pressed) kb_modifiers |= KEY_FLAG_CTRL;
        else         kb_modifiers &= ~KEY_FLAG_CTRL;
        break;
    case 0x38:              /* Alt */
        if (pressed) kb_modifiers |= KEY_FLAG_ALT;
        else         kb_modifiers &= ~KEY_FLAG_ALT;
        break;
    }
}

/* ── Mouse Events (called from mouse IRQ handler or PS/2 aux) ── */

void input_post_mouse_move(int16_t dx, int16_t dy)
{
    /* Update absolute position with clamping */
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;

    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = INPUT_MOUSE_MOVE;
    evt.dx = dx;
    evt.dy = dy;
    evt.buttons = mouse_buttons;
    evt.timestamp = rdtsc();
    input_enqueue(&evt);
}

void input_post_mouse_button(uint8_t buttons)
{
    uint8_t changed = buttons ^ mouse_buttons;
    if (!changed) return;

    mouse_buttons = buttons;

    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = INPUT_MOUSE_BTN;
    evt.buttons = buttons;
    evt.timestamp = rdtsc();
    input_enqueue(&evt);
}

void input_post_mouse_wheel(int16_t delta)
{
    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = INPUT_MOUSE_WHEEL;
    evt.wheel = delta;
    evt.buttons = mouse_buttons;
    evt.timestamp = rdtsc();
    input_enqueue(&evt);
}

/* ── Event Coalescing (called by compositor before each frame) ── */

/* Drain all pending events with coalescing:
 * - Mouse move deltas are accumulated into a single dx,dy
 * - Key events are preserved individually
 * Returns total number of key events written to key_out. */

int input_drain_coalesced(int16_t *out_mouse_dx, int16_t *out_mouse_dy,
                          uint8_t *out_buttons, int16_t *out_wheel,
                          input_event_t *key_out, int key_max)
{
    int key_count = 0;
    int32_t mdx = 0, mdy = 0;
    int32_t wheel_accum = 0;
    *out_buttons = mouse_buttons;

    while (input_tail != input_head) {
        input_event_t *e = &input_queue[input_tail & INPUT_QUEUE_MASK];
        input_tail = (input_tail + 1) & INPUT_QUEUE_MASK;

        switch (e->type) {
        case INPUT_MOUSE_MOVE:
            /* Coalesce: accumulate deltas */
            mdx += e->dx;
            mdy += e->dy;
            break;
        case INPUT_MOUSE_BTN:
            *out_buttons = e->buttons;
            break;
        case INPUT_KEY_DOWN:
        case INPUT_KEY_UP:
            /* Preserve every key event */
            if (key_count < key_max) {
                key_out[key_count++] = *e;
            }
            break;
        case INPUT_MOUSE_WHEEL:
            /* Coalesce: accumulate wheel deltas between frames */
            wheel_accum += e->wheel;
            break;
        }
    }

    *out_mouse_dx = (int16_t)mdx;
    *out_mouse_dy = (int16_t)mdy;
    *out_wheel = (int16_t)wheel_accum;
    return key_count;
}

/* ── Input Prediction (X-RETINA) ─────────────────────────────── */

/* Predict cursor position for the next frame based on velocity.
 * This hides input latency by extrapolating cursor movement.
 * Call with the time until next frame display (in TSC ticks). */

static int32_t prev_mouse_x, prev_mouse_y;
static uint64_t prev_prediction_tsc;

void input_predict_cursor(int32_t *predicted_x, int32_t *predicted_y,
                          uint64_t frame_tsc_budget)
{
    uint64_t now = rdtsc();
    uint64_t dt = now - prev_prediction_tsc;

    if (dt == 0 || prev_prediction_tsc == 0) {
        *predicted_x = mouse_x;
        *predicted_y = mouse_y;
        prev_mouse_x = mouse_x;
        prev_mouse_y = mouse_y;
        prev_prediction_tsc = now;
        return;
    }

    /* Velocity in pixels per TSC tick */
    int32_t vx = mouse_x - prev_mouse_x;
    int32_t vy = mouse_y - prev_mouse_y;

    /* Extrapolate by frame_tsc_budget / dt */
    int32_t px = mouse_x + (int32_t)((int64_t)vx * (int64_t)frame_tsc_budget / (int64_t)dt);
    int32_t py = mouse_y + (int32_t)((int64_t)vy * (int64_t)frame_tsc_budget / (int64_t)dt);

    /* Clamp to screen */
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= screen_w) px = screen_w - 1;
    if (py >= screen_h) py = screen_h - 1;

    *predicted_x = px;
    *predicted_y = py;

    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;
    prev_prediction_tsc = now;
}

/* ── Query Functions ─────────────────────────────────────────── */

void input_get_cursor(int32_t *x, int32_t *y)
{
    *x = mouse_x;
    *y = mouse_y;
}

uint8_t input_get_buttons(void) { return mouse_buttons; }
uint8_t input_get_modifiers(void) { return kb_modifiers; }

bool input_has_events(void)
{
    return input_head != input_tail;
}

bool input_pop_event(input_event_t *out_evt)
{
    if (input_head == input_tail) return false;
    *out_evt = input_queue[input_tail & INPUT_QUEUE_MASK];
    input_tail = (input_tail + 1) & INPUT_QUEUE_MASK;
    return true;
}

uint32_t input_queue_count(void)
{
    return (input_head - input_tail) & INPUT_QUEUE_MASK;
}

/* ── Initialize ──────────────────────────────────────────────── */

void input_events_init(uint32_t scr_width, uint32_t scr_height)
{
    input_head = 0;
    input_tail = 0;
    mouse_x = (int32_t)scr_width / 2;
    mouse_y = (int32_t)scr_height / 2;
    screen_w = (int32_t)scr_width;
    screen_h = (int32_t)scr_height;
    mouse_buttons = 0;
    kb_modifiers = 0;
    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;
    prev_prediction_tsc = 0;

    serial_puts("[INPUT] Event system initialized (");
    serial_putdec(scr_width);
    serial_puts("x");
    serial_putdec(scr_height);
    serial_puts(", queue=");
    serial_putdec(INPUT_QUEUE_SIZE);
    serial_puts(")\n");
}
