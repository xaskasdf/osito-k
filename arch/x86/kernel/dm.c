/*
 * OsitoK x86-64 — Device Mapper (dm)
 *
 * Logical volume layer: maps virtual block devices to
 * physical device ranges. Foundation for LVM and dm-crypt.
 *
 * Targets:
 *   dm-linear: simple 1:1 mapping with offset
 *   dm-stripe: striping across multiple devices (future)
 *   dm-crypt:  encrypted block device (stub)
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* Block device layer */
extern int blkdev_read(int dev_idx, uint64_t lba, uint32_t count, void *buf)
    __attribute__((weak));
extern int blkdev_write(int dev_idx, uint64_t lba, uint32_t count, const void *buf)
    __attribute__((weak));

/* ── DM Target Types ────────────────────────────────────────── */

#define DM_TARGET_LINEAR  0
#define DM_TARGET_STRIPE  1
#define DM_TARGET_CRYPT   2

/* ── DM Table Entry ──────────────────────────────────────────── */

#define DM_MAX_TARGETS 8

typedef struct {
    uint8_t  type;           /* DM_TARGET_* */
    uint64_t start_sector;   /* Start of this target's range */
    uint64_t num_sectors;    /* Length of this range */
    /* Linear target: */
    int      phys_dev;       /* blkdev index */
    uint64_t phys_offset;    /* Sector offset on physical device */
} dm_target_t;

/* ── DM Device ───────────────────────────────────────────────── */

#define DM_MAX_DEVICES 4

typedef struct {
    bool         active;
    char         name[32];
    dm_target_t  targets[DM_MAX_TARGETS];
    int          target_count;
    uint64_t     total_sectors;
} dm_device_t;

static dm_device_t dm_devices[DM_MAX_DEVICES];

/* ── Public API ──────────────────────────────────────────────── */

/* Create a new device-mapper device */
int dm_create(const char *name)
{
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        if (!dm_devices[i].active) {
            memset(&dm_devices[i], 0, sizeof(dm_device_t));
            dm_devices[i].active = true;
            int j = 0;
            while (name[j] && j < 31) { dm_devices[i].name[j] = name[j]; j++; }
            dm_devices[i].name[j] = '\0';
            serial_puts("[DM] Created: ");
            serial_puts(name);
            serial_puts("\n");
            return i;
        }
    }
    return -1;
}

/* Add a linear target to a DM device */
int dm_add_linear(int dm_idx, uint64_t start, uint64_t length,
                  int phys_dev, uint64_t phys_offset)
{
    if (dm_idx < 0 || dm_idx >= DM_MAX_DEVICES) return -1;
    dm_device_t *d = &dm_devices[dm_idx];
    if (!d->active || d->target_count >= DM_MAX_TARGETS) return -1;

    dm_target_t *t = &d->targets[d->target_count++];
    t->type = DM_TARGET_LINEAR;
    t->start_sector = start;
    t->num_sectors = length;
    t->phys_dev = phys_dev;
    t->phys_offset = phys_offset;

    d->total_sectors = start + length;
    return 0;
}

/* Read from a DM device */
int dm_read(int dm_idx, uint64_t sector, uint32_t count, void *buf)
{
    if (dm_idx < 0 || dm_idx >= DM_MAX_DEVICES) return -1;
    dm_device_t *d = &dm_devices[dm_idx];
    if (!d->active || !blkdev_read) return -1;

    /* Find the target covering this sector */
    for (int i = 0; i < d->target_count; i++) {
        dm_target_t *t = &d->targets[i];
        if (sector >= t->start_sector &&
            sector < t->start_sector + t->num_sectors) {
            uint64_t offset = sector - t->start_sector;
            return blkdev_read(t->phys_dev, t->phys_offset + offset, count, buf);
        }
    }
    return -1;  /* Sector not mapped */
}

/* Write to a DM device */
int dm_write(int dm_idx, uint64_t sector, uint32_t count, const void *buf)
{
    if (dm_idx < 0 || dm_idx >= DM_MAX_DEVICES) return -1;
    dm_device_t *d = &dm_devices[dm_idx];
    if (!d->active || !blkdev_write) return -1;

    for (int i = 0; i < d->target_count; i++) {
        dm_target_t *t = &d->targets[i];
        if (sector >= t->start_sector &&
            sector < t->start_sector + t->num_sectors) {
            uint64_t offset = sector - t->start_sector;
            return blkdev_write(t->phys_dev, t->phys_offset + offset, count, buf);
        }
    }
    return -1;
}

/* List DM devices */
void dm_list(void)
{
    serial_puts("[DM] Device mapper devices:\n");
    int count = 0;
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        if (!dm_devices[i].active) continue;
        serial_puts("  ");
        serial_puts(dm_devices[i].name);
        serial_puts(": ");
        serial_putdec(dm_devices[i].total_sectors / 2048);
        serial_puts(" MB, ");
        serial_putdec((uint64_t)dm_devices[i].target_count);
        serial_puts(" targets\n");
        count++;
    }
    if (count == 0) serial_puts("  (none)\n");
}

/* Destroy a DM device */
int dm_destroy(int dm_idx)
{
    if (dm_idx < 0 || dm_idx >= DM_MAX_DEVICES) return -1;
    dm_devices[dm_idx].active = false;
    return 0;
}
