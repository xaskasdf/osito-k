/*
 * OsitoK x86-64 — Input Event Device (evdev)
 *
 * Unified input interface: /dev/input/event0 (keyboard),
 * /dev/input/event1 (mouse). Linux-compatible struct input_event.
 * Apps read events via standard read() syscall.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* ── Linux input_event Structure ─────────────────────────────── */

/* Matches linux/input.h exactly for binary compatibility */
typedef struct __attribute__((packed)) {
    uint64_t time_sec;
    uint64_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t_ev;

/* Event types */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03

/* Relative axes */
#define REL_X   0x00
#define REL_Y   0x01
#define REL_WHEEL 0x08

/* ── Event Ring Buffer ───────────────────────────────────────── */

#define EVDEV_MAX_DEVICES 4
#define EVDEV_RING_SIZE   64

typedef struct {
    bool            active;
    char            name[32];
    input_event_t_ev ring[EVDEV_RING_SIZE];
    uint32_t        head;
    uint32_t        tail;
    uint32_t        count;
} evdev_device_t;

static evdev_device_t evdevs[EVDEV_MAX_DEVICES];

/* ── Init ────────────────────────────────────────────────────── */

void evdev_init(void)
{
    memset(evdevs, 0, sizeof(evdevs));

    /* Device 0: keyboard */
    evdevs[0].active = true;
    const char *n0 = "keyboard";
    for (int i = 0; n0[i]; i++) evdevs[0].name[i] = n0[i];

    /* Device 1: mouse */
    evdevs[1].active = true;
    const char *n1 = "mouse";
    for (int i = 0; n1[i]; i++) evdevs[1].name[i] = n1[i];

    serial_puts("[EVDEV] Input devices registered (keyboard, mouse)\n");
}

/* ── Post Events (called from ISR/drivers) ───────────────────── */

static void evdev_post(int dev_idx, uint16_t type, uint16_t code, int32_t value)
{
    if (dev_idx < 0 || dev_idx >= EVDEV_MAX_DEVICES) return;
    evdev_device_t *d = &evdevs[dev_idx];
    if (!d->active || d->count >= EVDEV_RING_SIZE) return;

    input_event_t_ev *ev = &d->ring[d->head];
    uint64_t ticks = idt_get_ticks();
    ev->time_sec = ticks / 100;
    ev->time_usec = (ticks % 100) * 10000;
    ev->type = type;
    ev->code = code;
    ev->value = value;

    d->head = (d->head + 1) % EVDEV_RING_SIZE;
    d->count++;
}

/* Post a key event */
void evdev_post_key(uint16_t keycode, int32_t value)
{
    evdev_post(0, EV_KEY, keycode, value);
    evdev_post(0, EV_SYN, 0, 0);  /* SYN_REPORT */
}

/* Post a mouse movement */
void evdev_post_rel(int32_t dx, int32_t dy)
{
    if (dx) evdev_post(1, EV_REL, REL_X, dx);
    if (dy) evdev_post(1, EV_REL, REL_Y, dy);
    evdev_post(1, EV_SYN, 0, 0);
}

/* Post a mouse button */
void evdev_post_button(uint16_t button, int32_t value)
{
    evdev_post(1, EV_KEY, button, value);
    evdev_post(1, EV_SYN, 0, 0);
}

/* ── Read Interface (for syscall layer) ──────────────────────── */

/* Read events from a device. Returns bytes read (multiple of 24). */
int evdev_read(int dev_idx, void *buf, uint32_t max_bytes)
{
    if (dev_idx < 0 || dev_idx >= EVDEV_MAX_DEVICES) return -1;
    evdev_device_t *d = &evdevs[dev_idx];
    if (!d->active) return -1;

    uint32_t ev_size = sizeof(input_event_t_ev);
    uint32_t max_events = max_bytes / ev_size;
    uint32_t read_count = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (read_count < max_events && d->count > 0) {
        memcpy(dst, &d->ring[d->tail], ev_size);
        d->tail = (d->tail + 1) % EVDEV_RING_SIZE;
        d->count--;
        dst += ev_size;
        read_count++;
    }

    return (int)(read_count * ev_size);
}

/* Check if events are available (for poll/epoll) */
bool evdev_has_events(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= EVDEV_MAX_DEVICES) return false;
    return evdevs[dev_idx].count > 0;
}

int evdev_device_count(void)
{
    int n = 0;
    for (int i = 0; i < EVDEV_MAX_DEVICES; i++)
        if (evdevs[i].active) n++;
    return n;
}
