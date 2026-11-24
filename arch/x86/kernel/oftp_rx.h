#ifndef OSITOK_OFTP_RX_H
#define OSITOK_OFTP_RX_H

#include "../include/types.h"

/* The caller serializes access and owns the buffer for the whole transfer. */
typedef struct {
    uint8_t *buf;
    uint32_t capacity;
    uint32_t total;
    uint32_t received;  /* Contiguous prefix, also the OFTN resume offset. */
    bool done;
    bool error;
} oftp_rx_t;

static inline uint32_t oftp_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Returns true only for new transfer information, progress or a fatal error.
 * Out-of-order data beyond a hole is discarded, then requested again with
 * OFTN. This needs no bitmap and accepts the server's variable chunk sizes. */
static inline bool oftp_rx_packet(oftp_rx_t *rx, const void *data, uint32_t len)
{
    if (rx->done || rx->error || !rx->buf || len < 4) return false;
    const uint8_t *p = data;
    if (memcmp(p, "OFTE", 4) == 0) {
        rx->error = true;
        return true;
    }
    if (memcmp(p, "OFTD", 4) != 0 || len < 16) return false;

    uint32_t total = oftp_be32(p + 4);
    uint32_t offset = oftp_be32(p + 8);
    uint32_t chunk = oftp_be32(p + 12);
    /* Subtraction checks avoid overflow on malformed wire lengths. */
    if (offset > total || chunk > total - offset || chunk != len - 16 ||
        (!chunk && offset != total)) return false;
    if (!total || total > rx->capacity || (rx->total && total != rx->total)) {
        rx->error = true;
        return true;
    }

    bool changed = rx->total == 0;
    rx->total = total;
    /* An EOF marker is not proof that the preceding datagrams arrived. */
    if (!chunk || offset > rx->received) return changed;
    uint32_t end = offset + chunk;
    if (end <= rx->received) return changed;

    uint32_t skip = rx->received - offset;
    memcpy(rx->buf + rx->received, p + 16 + skip, chunk - skip);
    rx->received = end;
    rx->done = rx->received == rx->total;
    return true;
}

#endif
