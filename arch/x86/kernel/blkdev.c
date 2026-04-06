/*
 * OsitoK x86-64 — Block Device Abstraction Layer
 *
 * Unifies NVMe, AHCI, virtio-blk, and USB mass storage under
 * a common API. Enables automatic filesystem detection and mount.
 *
 * Each block device registers with: read, write, sector_size, sector_count.
 * Probe scans all devices for known filesystem signatures.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* ── Block Device Interface ──────────────────────────────────── */

#define BLKDEV_MAX 8

typedef int (*blk_read_fn)(uint64_t lba, uint32_t count, void *buf);
typedef int (*blk_write_fn)(uint64_t lba, uint32_t count, const void *buf);

typedef struct {
    bool          active;
    char          name[16];          /* "nvme0", "sda", "vda", "usb0" */
    uint32_t      sector_size;       /* 512 or 4096 */
    uint64_t      sector_count;
    blk_read_fn   read;
    blk_write_fn  write;             /* NULL = read-only */
    uint8_t       type;              /* BLKDEV_NVME, _AHCI, _VIRTIO, _USB */
} blkdev_t;

#define BLKDEV_NVME   0
#define BLKDEV_AHCI   1
#define BLKDEV_VIRTIO 2
#define BLKDEV_USB    3

static blkdev_t devices[BLKDEV_MAX];
static int device_count;

/* ── Registration ────────────────────────────────────────────── */

int blkdev_register(const char *name, uint8_t type,
                    uint32_t sector_size, uint64_t sector_count,
                    blk_read_fn read_fn, blk_write_fn write_fn)
{
    if (device_count >= BLKDEV_MAX) return -1;

    blkdev_t *d = &devices[device_count];
    d->active = true;
    d->type = type;
    d->sector_size = sector_size;
    d->sector_count = sector_count;
    d->read = read_fn;
    d->write = write_fn;

    int i = 0;
    while (name[i] && i < 15) { d->name[i] = name[i]; i++; }
    d->name[i] = '\0';

    serial_puts("[BLKDEV] Registered: ");
    serial_puts(d->name);
    serial_puts(" (");
    serial_putdec(sector_count * sector_size / (1024 * 1024));
    serial_puts(" MB, ");
    serial_putdec(sector_size);
    serial_puts("B/sector)\n");

    return device_count++;
}

/* ── Device Access ───────────────────────────────────────────── */

int blkdev_read(int dev_idx, uint64_t lba, uint32_t count, void *buf)
{
    if (dev_idx < 0 || dev_idx >= device_count) return -1;
    if (!devices[dev_idx].active || !devices[dev_idx].read) return -1;
    return devices[dev_idx].read(lba, count, buf);
}

int blkdev_write(int dev_idx, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev_idx < 0 || dev_idx >= device_count) return -1;
    if (!devices[dev_idx].active || !devices[dev_idx].write) return -1;
    return devices[dev_idx].write(lba, count, buf);
}

int blkdev_count(void) { return device_count; }

const char *blkdev_name(int idx)
{
    if (idx < 0 || idx >= device_count) return NULL;
    return devices[idx].name;
}

uint64_t blkdev_size_mb(int idx)
{
    if (idx < 0 || idx >= device_count) return 0;
    return devices[idx].sector_count * devices[idx].sector_size / (1024 * 1024);
}

/* ── Filesystem Signature Detection ──────────────────────────── */

/* Probe a device for known filesystem signatures */
const char *blkdev_detect_fs(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= device_count) return NULL;
    blkdev_t *d = &devices[dev_idx];

    uint8_t buf[4096];

    /* Read sector 0 for FAT/exFAT/NTFS boot sector */
    if (d->read(0, 1, buf) == 0) {
        /* FAT32: bytes_per_sector at offset 11, "FAT32" at offset 82 */
        if (buf[0] == 0xEB || buf[0] == 0xE9) {
            if (buf[82] == 'F' && buf[83] == 'A' && buf[84] == 'T')
                return "fat32";
            if (buf[3] == 'E' && buf[4] == 'X' && buf[5] == 'F' &&
                buf[6] == 'A' && buf[7] == 'T')
                return "exfat";
            if (buf[3] == 'N' && buf[4] == 'T' && buf[5] == 'F' && buf[6] == 'S')
                return "ntfs";
        }
    }

    /* Read sector 2 for ext2/ext4 superblock (byte offset 1024) */
    if (d->read(2, 2, buf) == 0) {
        uint16_t magic = *(uint16_t *)(buf + 56);  /* s_magic at offset 56 in SB */
        if (magic == 0xEF53) return "ext4";
    }

    /* Read LBA 16 for ISO 9660 PVD */
    if (d->sector_count > 16 && d->read(16 * (2048 / d->sector_size), 1, buf) == 0) {
        if (buf[1] == 'C' && buf[2] == 'D' && buf[3] == '0' &&
            buf[4] == '0' && buf[5] == '1')
            return "iso9660";
    }

    /* HFS+ at sector 2 (byte 1024) */
    if (d->read(2, 1, buf) == 0) {
        uint16_t sig = ((uint16_t)buf[0] << 8) | buf[1];
        if (sig == 0x482B || sig == 0x4858) return "hfsplus";
    }

    /* Btrfs at 64KB */
    if (d->sector_count > 128 && d->read(128, 8, buf) == 0) {
        uint64_t magic = *(uint64_t *)(buf + 64);
        if (magic == 0x4D5F53665248425FULL) return "btrfs";
    }

    /* OsitoFS at any GPT partition (check first sector for magic) */
    if (d->read(0, 1, buf) == 0) {
        uint32_t magic = *(uint32_t *)buf;
        if (magic == 0x4F534632) return "ositofs";
    }

    return NULL;  /* Unknown */
}

/* ── List all devices ────────────────────────────────────────── */

void blkdev_list(void)
{
    serial_puts("[BLKDEV] Block devices:\n");
    for (int i = 0; i < device_count; i++) {
        serial_puts("  ");
        serial_puts(devices[i].name);
        serial_puts(": ");
        serial_putdec(blkdev_size_mb(i));
        serial_puts(" MB");
        if (!devices[i].write) serial_puts(" (read-only)");
        const char *fs = blkdev_detect_fs(i);
        if (fs) { serial_puts(" ["); serial_puts(fs); serial_puts("]"); }
        serial_puts("\n");
    }
    if (device_count == 0) serial_puts("  (none)\n");
}
