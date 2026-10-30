#ifndef OSITO_TERMINAL_CONTROL_H
#define OSITO_TERMINAL_CONTROL_H
#include "../include/types.h"

/* A foreground backend must reject generations other than its current launch.
 * Requests run on a service task; the input/IRQ path only publishes a key. */
typedef bool (*terminal_signal_fn)(uint32_t generation, int signal);
uint32_t terminal_control_bind(terminal_signal_fn request);
void terminal_control_release(uint32_t generation);
bool terminal_control_key(unsigned char key);
void terminal_control_poll(void);
#endif
