#ifndef OSITOK_INPUT_EVENTS_H
#define OSITOK_INPUT_EVENTS_H

#include "types.h"

#define INPUT_MOUSE_BUTTON_COUNT 3U

/* A non-destructive snapshot for consumers that must not drain the shared
 * compositor event queue. Counters are monotonic for the current boot. */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    uint8_t reserved[7];
    int64_t motion_x;
    int64_t motion_y;
    uint64_t press_count[INPUT_MOUSE_BUTTON_COUNT];
    uint64_t release_count[INPUT_MOUSE_BUTTON_COUNT];
    int32_t press_x[INPUT_MOUSE_BUTTON_COUNT];
    int32_t press_y[INPUT_MOUSE_BUTTON_COUNT];
    int32_t release_x[INPUT_MOUSE_BUTTON_COUNT];
    int32_t release_y[INPUT_MOUSE_BUTTON_COUNT];
} input_mouse_snapshot_t;

void input_get_mouse_snapshot(input_mouse_snapshot_t *snapshot);

/* Exclusive raw set-1 stream. The owner is an identity token, never
 * dereferenced by IRQ producers. Packets are enqueued atomically. */
bool input_keyboard_acquire(const void *owner);
void input_keyboard_release(const void *owner);
bool input_keyboard_post_set1(const uint8_t *bytes, uint32_t count);
bool input_keyboard_read_set1(const void *owner, uint8_t *byte);
bool input_keyboard_is_owner(const void *owner);

#endif
