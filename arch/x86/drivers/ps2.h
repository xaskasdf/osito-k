#ifndef OSITOK_PS2_H
#define OSITOK_PS2_H

#include "../include/types.h"

typedef struct {
    bool controller, ready, synaptics;
    uint8_t device_id, packet_size;
    uint32_t identity;
    uint64_t irq12, aux_bytes, packets, delivered, dropped, bad_bytes, resyncs;
    uint64_t timeouts, resends;
    const char *status;
} ps2_stats_t;

void ps2_init(void);
void ps2_irq(void);
void ps2_aux_irq(void);
void ps2_poll(void);
void ps2_get_stats(ps2_stats_t *out);

#endif
