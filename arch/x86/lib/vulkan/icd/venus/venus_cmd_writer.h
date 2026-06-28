/*
 * Helpers for writing Mesa Venus command streams.
 *
 * These streams are submitted directly through virtio-gpu SUBMIT_3D and
 * decoded by virglrenderer's generated venus-protocol code.  They are
 * intentionally separate from the older W3b private command ring.
 */
#ifndef OSITOK_VENUS_CMD_WRITER_H
#define OSITOK_VENUS_CMD_WRITER_H

#include <stdint.h>

struct venus_cmd_writer {
    uint8_t *buf;
    uint32_t off;
    uint32_t cap;
    int err;
};

static inline void vcw_wr_bytes(struct venus_cmd_writer *w,
                                const void *src, uint32_t n) {
    if (w->err) return;
    if (w->off + n > w->cap || w->off + n < w->off) {
        w->err = -12;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w->buf[w->off + i] = ((const uint8_t *)src)[i];
    w->off += n;
}

static inline void vcw_wr_blob_array(struct venus_cmd_writer *w,
                                     const void *src, uint32_t n) {
    uint32_t padded = (n + 3u) & ~3u;

    if (w->err) return;
    if (padded < n || w->off + padded > w->cap || w->off + padded < w->off) {
        w->err = -12;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w->buf[w->off + i] = ((const uint8_t *)src)[i];
    for (uint32_t i = n; i < padded; i++)
        w->buf[w->off + i] = 0;
    w->off += padded;
}

static inline void vcw_wr_u32(struct venus_cmd_writer *w, uint32_t v) {
    vcw_wr_bytes(w, &v, 4);
}

static inline void vcw_wr_i32(struct venus_cmd_writer *w, int32_t v) {
    vcw_wr_bytes(w, &v, 4);
}

static inline void vcw_wr_u64(struct venus_cmd_writer *w, uint64_t v) {
    vcw_wr_bytes(w, &v, 8);
}

static inline void vcw_wr_array_size(struct venus_cmd_writer *w, uint64_t n) {
    vcw_wr_u64(w, n);
}

#endif
