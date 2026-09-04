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

#endif
