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
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void mem_free_pages(void *addr, uint64_t count);

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

/* True if device idx is registered, active, and has a non-NULL write
 * callback.  Used by the OsitoFS scan to prefer a writeable backing
 * (USB MSC) over a read-only one (current NVMe driver) when both have
 * OsitoFS.  Without this preference, post-kexec we'd mount NVMe (write
 * NULL) and the FS goes read-only — observed symptom. */
bool blkdev_can_write(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= device_count) return false;
    blkdev_t *d = &devices[dev_idx];
    return d->active && d->write != NULL;
}

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

/* ── Active-disk view (used by fs/ during mount probe) ─────────
 *
 * The filesystem layer (gpt.c, ositofs2.c, ositofs3.c) does not know
 * about NVMe vs USB vs anything else — it just calls disk_read_bytes()
 * to read raw bytes. Before each mount attempt, main.c selects which
 * blkdev is "active" via disk_set_active(); subsequent disk_* calls
 * route through that device's read function.
 *
 * This is simpler than threading a blkdev handle through every fs
 * function and keeps the fs code identical regardless of underlying
 * transport. Active-disk state is single-threaded by construction
 * (only one mount happens at a time during boot). */

static int active_dev = -1;

void disk_set_active(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= device_count) {
        active_dev = -1;
        return;
    }
    active_dev = dev_idx;
}

int disk_active(void) { return active_dev; }

uint32_t disk_lba_size(void)
{
    if (active_dev < 0) return 0;
    return devices[active_dev].sector_size;
}

uint64_t disk_lba_count(void)
{
    if (active_dev < 0) return 0;
    return devices[active_dev].sector_count;
}

/*
 * Write `len` bytes starting at byte_offset on the active device.
 * Read-modify-write at the boundary sectors so partial writes are
 * preserved. Returns 0 on success, -1 on no-active-device, no-write-fn,
 * or any sub-write failure.
 */
int disk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len)
{
    if (active_dev < 0) {
        serial_puts("[BLK] disk_write_bytes: no active dev\n");
        return -1;
    }
    blkdev_t *d = &devices[active_dev];
    if (!d->active || !d->read || !d->write || d->sector_size == 0) {
        serial_puts("[BLK] disk_write_bytes: invalid dev (active=");
        serial_putdec(d->active ? 1 : 0);
        serial_puts(" read=");
        serial_putdec(d->read ? 1 : 0);
        serial_puts(" write=");
        serial_putdec(d->write ? 1 : 0);
        serial_puts(" ssz=");
        serial_putdec(d->sector_size);
        serial_puts(")\n");
        return -1;
    }

    uint64_t device_bytes = d->sector_count * (uint64_t)d->sector_size;
    if ((!buf && len) || byte_offset > device_bytes ||
        len > device_bytes - byte_offset)
        return -1;
    if (!len) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t off  = byte_offset;
    uint64_t left = len;
    uint32_t ssz = d->sector_size;
    if (ssz > 8192) return -1;
    void *tmp_phys = NULL;
    uint8_t *tmp = NULL;
    int result = 0;

    while (left > 0) {
        uint64_t lba   = off / ssz;
        uint32_t intra = (uint32_t)(off % ssz);

        if (d->type == BLKDEV_NVME && intra == 0 && left >= ssz) {
            uint64_t sector_count = left / ssz;
            if (sector_count > UINT32_MAX) sector_count = UINT32_MAX;
            uint64_t direct_bytes = sector_count * ssz;
            if (d->write(lba, (uint32_t)sector_count, src) < 0) {
                serial_puts("[BLK] disk_write_bytes: direct write failed lba=");
                serial_putdec(lba);
                serial_puts("\n");
                result = -1;
                goto out;
            }
            src += direct_bytes;
            off += direct_bytes;
            left -= direct_bytes;
            continue;
        }

        if (!tmp_phys) {
            tmp_phys = mem_alloc_aligned(8192, 4096);
            if (!tmp_phys) return -1;
            tmp = (uint8_t *)PHYS_TO_VIRT(tmp_phys);
        }
        uint32_t want  = (uint32_t)((left < (8192U - intra))
                                    ? left : (8192U - intra));
        uint32_t sectors = (intra + want + ssz - 1) / ssz;

        /* Partial writes require the original boundary-sector bytes. If
         * that read fails, abort rather than manufacturing zero metadata. */
        bool partial = (intra != 0) || ((intra + want) % ssz != 0);
        if (partial) {
            if (d->read(lba, sectors, tmp) < 0) {
                serial_puts("[BLK] disk_write_bytes: prerequisite read failed lba=");
                serial_putdec(lba);
                serial_puts("\n");
                result = -1;
                goto out;
            }
        }

        for (uint32_t i = 0; i < want; i++) tmp[intra + i] = src[i];
        if (d->write(lba, sectors, tmp_phys) < 0) {
            serial_puts("[BLK] disk_write_bytes: write failed lba=");
            serial_putdec(lba);
            serial_puts("\n");
            result = -1;
            goto out;
        }

        src  += want;
        off  += want;
        left -= want;
    }
out:
    if (tmp_phys) mem_free_pages(tmp_phys, 2);
    return result;
}

/*
 * Flush controller-side write cache to durable storage. Required after
 * a series of writes if the caller intends to physically remove the
 * device — without this, USB sticks may report "complete" while the
 * controller still holds the data in its DRAM cache.
 *
 * USB MSC and NVMe expose transport-specific cache flush commands.
 */
int disk_flush(void)
{
    if (active_dev < 0) return -1;
    blkdev_t *d = &devices[active_dev];
    if (!d->active) return -1;
    extern int nvme_flush(void) __attribute__((weak));
    extern int usb_storage_flush(void) __attribute__((weak));
    if (d->type == BLKDEV_NVME && nvme_flush)
        return nvme_flush();
    if (d->type == 3 /* BLKDEV_USB */ && usb_storage_flush)
        return usb_storage_flush();
    return 0;
}

/*
 * Read `len` bytes starting at byte_offset on the active device.
 * Translates byte coordinates to LBA + intra-sector offset. Aligned NVMe
 * spans use the caller buffer directly; partial boundary sectors use a
 * temporary aligned buffer. Returns 0 on success, -1 on any sub-read
 * failure or when no device is active.
 */
int disk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len)
{
    if (active_dev < 0) return -1;
    blkdev_t *d = &devices[active_dev];
    if (!d->active || !d->read || d->sector_size == 0) return -1;

    uint64_t device_bytes = d->sector_count * (uint64_t)d->sector_size;
    if ((!buf && len) || byte_offset > device_bytes ||
        len > device_bytes - byte_offset)
        return -1;
    if (!len) return 0;

    uint8_t *dst   = (uint8_t *)buf;
    uint64_t off   = byte_offset;
    uint64_t left  = len;
    /* Use physically backed direct-map memory for DMA. Kernel stacks are
     * not guaranteed to be physically contiguous across two pages. */
    uint32_t ssz   = d->sector_size;
    if (ssz > 8192) return -1;
    void *tmp_phys = NULL;
    uint8_t *tmp = NULL;
    int result = 0;

    while (left > 0) {
        uint64_t lba    = off / ssz;
        uint32_t intra  = (uint32_t)(off % ssz);

        if (d->type == BLKDEV_NVME && intra == 0 && left >= ssz) {
            uint64_t sector_count = left / ssz;
            if (sector_count > UINT32_MAX) sector_count = UINT32_MAX;
            uint64_t direct_bytes = sector_count * ssz;
            if (d->read(lba, (uint32_t)sector_count, dst) < 0) {
                result = -1;
                break;
            }
            extern void nvme_debug_watch_cpu_write(void *, uint64_t,
                                                    const char *);
            nvme_debug_watch_cpu_write(dst, direct_bytes,
                                       "disk_read_bytes-direct");
            dst += direct_bytes;
            off += direct_bytes;
            left -= direct_bytes;
            continue;
        }

        if (!tmp_phys) {
            tmp_phys = mem_alloc_aligned(8192, 4096);
            if (!tmp_phys) return -1;
            tmp = (uint8_t *)PHYS_TO_VIRT(tmp_phys);
        }
        uint32_t want   = (uint32_t)((left < (8192U - intra))
                                     ? left : (8192U - intra));
        uint32_t sectors = (intra + want + ssz - 1) / ssz;

        if (d->read(lba, sectors, tmp) < 0) {
            result = -1;
            break;
        }

        extern void nvme_debug_watch_cpu_write(void *, uint64_t, const char *);
        nvme_debug_watch_cpu_write(dst, want, "disk_read_bytes");
        for (uint32_t i = 0; i < want; i++) dst[i] = tmp[intra + i];
        dst  += want;
        off  += want;
        left -= want;
    }
    if (tmp_phys) mem_free_pages(tmp_phys, 2);
    return result;
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
