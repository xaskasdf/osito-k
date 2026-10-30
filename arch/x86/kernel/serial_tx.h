#ifndef OSITOK_SERIAL_TX_H
#define OSITOK_SERIAL_TX_H

#include "../include/types.h"

#define SERIAL_TX_THRE 0x20
#define SERIAL_TX_TEMT 0x40

typedef struct {
    uint8_t value;
    uint8_t ready;
} serial_tx_slot_t;

typedef struct {
    uint8_t (*status)(void *opaque);
    void (*write)(void *opaque, uint8_t value);
    /* Zero means that an elapsed-time clock is not available yet. */
    uint64_t (*now_ns)(void *opaque);
} serial_tx_ops_t;

typedef struct {
    uint64_t accepted, sent, dropped, timeouts, busy, pending;
    uint32_t capacity, fifo_depth;
    bool present;
} serial_stats_t;

/* Private UART queue. Producers reserve whole records, then publish bytes.
 * A preempted producer can delay transmission, but cannot be overwritten.
 * The sole consumer never waits for another consumer or unpublished data. */
typedef struct {
    serial_tx_slot_t *slots;
    const serial_tx_ops_t *ops;
    void *opaque;
    uint64_t head, tail;
    uint64_t accepted, sent, dropped, timeouts, busy;
    uint32_t capacity, fifo_depth;
    uint8_t draining;
    bool present;
} serial_tx_t;

bool serial_tx_init(serial_tx_t *tx, serial_tx_slot_t *slots, uint32_t capacity,
                    uint32_t fifo_depth, bool present,
                    const serial_tx_ops_t *ops, void *opaque);
bool serial_tx_enqueue(serial_tx_t *tx, const char *data, uint32_t length);
void serial_tx_poll(serial_tx_t *tx);
void serial_tx_poll_limit(serial_tx_t *tx, uint32_t max_bytes);
/* Bounded by elapsed time; false retains unsent data for the next poll.
 * Without a clock, flush only attempts a nonblocking poll. */
bool serial_tx_flush(serial_tx_t *tx, uint64_t timeout_ns);
/* Approximate while writers are active; exact after they have quiesced. */
void serial_tx_stats(serial_tx_t *tx, serial_stats_t *stats);

#endif
