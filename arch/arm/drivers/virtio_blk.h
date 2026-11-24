/*
 * virtio_blk.h -- VirtIO block device driver (PCI modern transport)
 */

#ifndef OSITO_VIRTIO_BLK_H
#define OSITO_VIRTIO_BLK_H

#include <stdint.h>

/* Initialize VirtIO-blk device found at PCI bus:dev:func.
 * bars[] array contains BAR addresses assigned by PCI scanner. */
int virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func, uint64_t bars[6]);

/* Block I/O — same interface as NVMe on x86 (used by OsitoFS) */
int      virtio_blk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
int      virtio_blk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len);
int      virtio_blk_flush(void);
uint64_t virtio_blk_capacity(void);

#endif /* OSITO_VIRTIO_BLK_H */
