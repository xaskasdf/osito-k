/*
 * OsitoK - Input subsystem (joystick + button)
 *
 * Hardware: KY-023 analog joystick
 *   VRx → A0 (TOUT, 10-bit ADC)
 *   SW  → GPIO12 (D6), active low with pull-up
 *   VRy → not connected (ESP8266 has only 1 ADC channel)
 *
 * Events are queued in a 16-entry ring buffer.
 * Polling runs from a dedicated task at ~50Hz.
 */
#ifndef OSITO_INPUT_H
#define OSITO_INPUT_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Input event types */
typedef enum {
    INPUT_NONE    = 0,
    INPUT_LEFT    = 1,
    INPUT_RIGHT   = 2,
    INPUT_PRESS   = 3,
    INPUT_RELEASE = 4
} input_event_t;

/* Dead zone thresholds for X axis (0-1023 range) */
#define INPUT_DEAD_LOW   400
#define INPUT_DEAD_HIGH  600

/* Button pin (GPIO12 = D6 on Wemos D1) */
#define INPUT_BTN_PIN    12

/* Debounce ticks (3 ticks = 30ms at 100Hz) */
#define INPUT_DEBOUNCE   3

/* Event queue size (power of 2) */
#define INPUT_QUEUE_SIZE 16

/* Initialize input subsystem (ADC + GPIO12 with pull-up) */
void input_init(void);

/* Poll hardware and generate events. Called from input_task at ~50Hz. */
void input_update(void);

/* Get next event from queue (returns INPUT_NONE if empty) */
input_event_t input_poll(void);

/*
 * Get current raw state:
 *   bits [15:0]  = X axis raw ADC value (0-1023)
 *   bit  [16]    = button state (1=pressed, 0=released)
 */
uint32_t input_get_state(void);

/* Input task entry point (for task_create) */
void input_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_INPUT_H */
