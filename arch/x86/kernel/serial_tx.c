#include "serial_tx.h"

#define SERIAL_POLL_BYTES 64u

bool serial_tx_init(serial_tx_t *tx, serial_tx_slot_t *slots, uint32_t capacity,
                    uint32_t fifo_depth, bool present,
                    const serial_tx_ops_t *ops, void *opaque)
{
    if (!tx || !slots || !capacity || (capacity & (capacity - 1)) ||
        (fifo_depth != 1 && fifo_depth != 16) || !ops ||
        !ops->status || !ops->write || !ops->now_ns)
        return false;
    memset(tx, 0, sizeof(*tx));
    memset(slots, 0, sizeof(*slots) * capacity);
    tx->slots = slots;
    tx->capacity = capacity;
    tx->fifo_depth = fifo_depth;
    tx->present = present;
    tx->ops = ops;
    tx->opaque = opaque;
    return true;
}

bool serial_tx_enqueue(serial_tx_t *tx, const char *data, uint32_t length)
{
    if (!length) return true;
    if (!data || !tx->present || length > tx->capacity) goto full;
    uint64_t head;
    for (;;) {
        uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
        head = __atomic_load_n(&tx->head, __ATOMIC_ACQUIRE);
        if (head - tail > tx->capacity - length) goto full;
        uint64_t next = head + length;
        if (__atomic_compare_exchange_n(&tx->head, &head, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
    for (uint32_t i = 0; i < length; i++) {
        serial_tx_slot_t *slot = &tx->slots[(head + i) & (tx->capacity - 1)];
        slot->value = (uint8_t)data[i];
        __atomic_store_n(&slot->ready, 1, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&tx->accepted, length, __ATOMIC_RELAXED);
    return true;
full:
    __atomic_fetch_add(&tx->dropped, length, __ATOMIC_RELAXED);
    return false;
}

static bool tx_acquire(serial_tx_t *tx)
{
    if (__atomic_test_and_set(&tx->draining, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&tx->busy, 1, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

static void tx_release(serial_tx_t *tx)
{
    __atomic_clear(&tx->draining, __ATOMIC_RELEASE);
}

/* Called only by the consumer. THRE means the entire FIFO is empty, not
 * just that it has one vacant position (PC16550D section 8.6.3). */
static uint32_t tx_batch(serial_tx_t *tx, uint64_t target, uint32_t limit)
{
    uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_RELAXED);
    if (tail == target || !limit) return 0;
    serial_tx_slot_t *slot = &tx->slots[tail & (tx->capacity - 1)];
    if (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) return 0;
    uint8_t status = tx->ops->status(tx->opaque);
    if (status == 0xff || !(status & SERIAL_TX_THRE)) return 0;
    uint32_t count = 0;
    while (tail != target && count < tx->fifo_depth && count < limit) {
        slot = &tx->slots[tail & (tx->capacity - 1)];
        if (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) break;
        tx->ops->write(tx->opaque, slot->value);
        __atomic_store_n(&slot->ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&tx->tail, ++tail, __ATOMIC_RELEASE);
        count++;
    }
    __atomic_fetch_add(&tx->sent, count, __ATOMIC_RELAXED);
    return count;
}

void serial_tx_poll(serial_tx_t *tx)
{
    serial_tx_poll_limit(tx, SERIAL_POLL_BYTES);
}

void serial_tx_poll_limit(serial_tx_t *tx, uint32_t max_bytes)
{
    if (!max_bytes || !tx->present || !tx_acquire(tx)) return;
    uint64_t target = __atomic_load_n(&tx->head, __ATOMIC_ACQUIRE);
    uint32_t remaining = max_bytes < SERIAL_POLL_BYTES
        ? max_bytes : SERIAL_POLL_BYTES;
    while (remaining) {
        uint32_t sent = tx_batch(tx, target, remaining);
        if (!sent) break;
        remaining -= sent;
    }
    tx_release(tx);
}

bool serial_tx_flush(serial_tx_t *tx, uint64_t timeout_ns)
{
    if (!tx->present || !tx_acquire(tx)) return false;
    uint64_t target = __atomic_load_n(&tx->head, __ATOMIC_ACQUIRE);
    uint64_t start = tx->ops->now_ns(tx->opaque);
    uint32_t remaining = SERIAL_POLL_BYTES;
    bool complete = false;
    for (;;) {
        uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_RELAXED);
        if (tail == target) {
            uint8_t status = tx->ops->status(tx->opaque);
            if (status != 0xff && (status & SERIAL_TX_TEMT)) {
                complete = true;
                break;
            }
        } else if (!__atomic_load_n(&tx->slots[tail & (tx->capacity - 1)].ready,
                                    __ATOMIC_ACQUIRE)) {
            /* Waiting here could deadlock the interrupted producer. */
            break;
        }
        if (start && timeout_ns) {
            uint64_t now = tx->ops->now_ns(tx->opaque);
            if (!now || now - start >= timeout_ns) {
                __atomic_fetch_add(&tx->timeouts, 1, __ATOMIC_RELAXED);
                break;
            }
        }
        uint32_t sent = tx_batch(tx, target, remaining);
        if (!start || !timeout_ns) {
            remaining -= sent;
            if (!sent || !remaining) break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    tx_release(tx);
    return complete;
}

void serial_tx_stats(serial_tx_t *tx, serial_stats_t *stats)
{
    stats->accepted = __atomic_load_n(&tx->accepted, __ATOMIC_RELAXED);
    stats->sent = __atomic_load_n(&tx->sent, __ATOMIC_RELAXED);
    stats->dropped = __atomic_load_n(&tx->dropped, __ATOMIC_RELAXED);
    stats->timeouts = __atomic_load_n(&tx->timeouts, __ATOMIC_RELAXED);
    stats->busy = __atomic_load_n(&tx->busy, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
    stats->pending = __atomic_load_n(&tx->head, __ATOMIC_ACQUIRE) - tail;
    if (stats->pending > tx->capacity) stats->pending = tx->capacity;
    stats->capacity = tx->capacity;
    stats->fifo_depth = tx->fifo_depth;
    stats->present = tx->present;
}
