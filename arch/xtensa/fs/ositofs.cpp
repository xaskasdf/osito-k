/*
 * OsitoFS - Flat filesystem on SPI flash
 *
 * Contiguous allocation, no directories.
 * All flash operations go through ROM SPI functions.
 * A single 4KB sector buffer is used for read-modify-write cycles.
 */

#include "fs/ositofs.h"
#include "drivers/uart.h"
#include "kernel/task.h"

extern "C" {

/* Sector buffer for read-modify-write (shared, not reentrant) */
static uint8_t sec_buf[FS_SECTOR_SIZE] __attribute__((aligned(4)));

/* Mounted flag */
static int mounted = 0;

/* ====== Low-level flash helpers ====== */

static void flash_read(uint32_t addr, void *dst, uint32_t len)
{
    /* SPIRead requires 4-byte aligned destination buffer */
    if (((uint32_t)dst & 3) == 0) {
        SPIRead(addr, dst, len);
    } else {
        uint8_t tmp[64] __attribute__((aligned(4)));
        uint8_t *p = (uint8_t *)dst;
        while (len > 0) {
            uint32_t n = len > 64 ? 64 : len;
            SPIRead(addr, tmp, (n + 3) & ~3u);
            ets_memcpy(p, tmp, n);
            addr += n;
            p += n;
            len -= n;
        }
    }
}

static void flash_erase_sector(uint32_t addr)
{
    SPIEraseSector(addr / FS_SECTOR_SIZE);
}

static void flash_write(uint32_t addr, const void *src, uint32_t len)
{
    /* SPIWrite requires 4-byte aligned source buffer */
    if (((uint32_t)src & 3) == 0) {
        SPIWrite(addr, src, len);
    } else {
        uint8_t tmp[64] __attribute__((aligned(4)));
        const uint8_t *p = (const uint8_t *)src;
        while (len > 0) {
            uint32_t n = len > 64 ? 64 : len;
            ets_memcpy(tmp, p, n);
            SPIWrite(addr, tmp, (n + 3) & ~3u);
            addr += n;
            p += n;
            len -= n;
        }
    }
}

/* ====== String helpers (avoid ROM dependency issues) ====== */

static int fs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

static void fs_strncpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = '\0';
}

/* ====== File table operations ====== */

/* Read full file table into sec_buf */
static void read_table(void)
{
    flash_read(FS_TABLE_ADDR, sec_buf, FS_SECTOR_SIZE);
}

/* Write sec_buf back as file table */
static void write_table(void)
{
    flash_erase_sector(FS_TABLE_ADDR);
    flash_write(FS_TABLE_ADDR, sec_buf, FS_SECTOR_SIZE);
}

/* Get entry pointer within sec_buf (call after read_table) */
static fs_entry_t *table_entry(int i)
{
    return (fs_entry_t *)(sec_buf + i * sizeof(fs_entry_t));
}

/* An entry is unused if name[0] is NUL or 0xFF (erased flash) */
static int entry_free(fs_entry_t *e)
{
    return (e->name[0] == '\0' || (uint8_t)e->name[0] == 0xFF);
}

/* Find entry by name. Returns index or -1. (sec_buf must be loaded) */
static int find_file(const char *name)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_entry_t *e = table_entry(i);
        if (!entry_free(e) && fs_strcmp(e->name, name) == 0)
            return i;
    }
    return -1;
}

/* Find first unused slot. Returns index or -1. */
static int find_free_slot(void)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (entry_free(table_entry(i)))
            return i;
    }
    return -1;
}

/* ====== Sector allocation (bitmap-based) ====== */

/* Bitmap: 1 bit per data sector, 120 bytes for 958 sectors */
#define BITMAP_BYTES ((FS_DATA_SECTORS + 7) / 8)

static void build_bitmap(uint8_t *bmap)
{
    ets_memset(bmap, 0, BITMAP_BYTES);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_entry_t *e = table_entry(i);
        if (entry_free(e)) continue;
        for (int s = 0; s < e->sector_count; s++) {
            int bit = e->start_sector + s;
            if (bit < (int)FS_DATA_SECTORS)
                bmap[bit / 8] |= (1 << (bit % 8));
        }
    }
}

/* Find N contiguous free sectors. Returns start index or -1. */
static int alloc_sectors(uint8_t *bmap, int count)
{
    int run = 0;
    int start = 0;
    for (int i = 0; i < (int)FS_DATA_SECTORS; i++) {
        if (bmap[i / 8] & (1 << (i % 8))) {
            run = 0;
            start = i + 1;
        } else {
            run++;
            if (run >= count)
                return start;
        }
    }
    return -1;
}

/* Count free sectors from bitmap */
static uint32_t count_free(uint8_t *bmap)
{
    uint32_t free = 0;
    for (int i = 0; i < (int)FS_DATA_SECTORS; i++) {
        if (!(bmap[i / 8] & (1 << (i % 8))))
            free++;
    }
    return free;
}

/* ====== Superblock ====== */

static void read_super(fs_super_t *sb)
{
    flash_read(FS_SUPER_ADDR, sb, sizeof(fs_super_t));
}

static void write_super(fs_super_t *sb)
{
    /* Reuse sec_buf to avoid 4KB stack allocation (task stack = 1.5KB) */
    ets_memset(sec_buf, 0xFF, FS_SECTOR_SIZE);
    ets_memcpy(sec_buf, sb, sizeof(fs_super_t));
    flash_erase_sector(FS_SUPER_ADDR);
    flash_write(FS_SUPER_ADDR, sec_buf, FS_SECTOR_SIZE);
}

/* ====== Public API ====== */

int fs_init(void)
{
    fs_super_t sb;
    read_super(&sb);

    if (sb.magic != FS_MAGIC || sb.version != FS_VERSION) {
        uart_puts("fs: no filesystem found (use 'fs format')\n");
        mounted = 0;
        return -1;
    }

    mounted = 1;
    uart_puts("fs: mounted, ");
    uart_put_dec(sb.file_count);
    uart_puts(" files, ");
    uart_put_dec(sb.total_sectors);
    uart_puts(" sectors\n");
    return 0;
}

int fs_format(void)
{
    uart_puts("fs: formatting...\n");

    /* Erase superblock and file table sectors */
    flash_erase_sector(FS_SUPER_ADDR);
    flash_erase_sector(FS_TABLE_ADDR);

    /* Zero-fill file table (erased flash = 0xFF, we need 0x00) */
    ets_memset(sec_buf, 0, FS_SECTOR_SIZE);
    flash_write(FS_TABLE_ADDR, sec_buf, FS_SECTOR_SIZE);

    /* Write superblock */
    fs_super_t sb;
    ets_memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.version = FS_VERSION;
    sb.total_sectors = FS_DATA_SECTORS;
    sb.file_count = 0;
    write_super(&sb);

    mounted = 1;

    uart_puts("fs: formatted, ");
    uart_put_dec(FS_DATA_SECTORS);
    uart_puts(" sectors (");
    uart_put_dec(FS_DATA_SECTORS * FS_SECTOR_SIZE / 1024);
    uart_puts(" KB) available\n");
    return 0;
}

int fs_create(const char *name, const void *data, uint32_t size)
{
    if (!mounted) return -1;
    if (name[0] == '\0' || size == 0) return -1;

    uint32_t ps = irq_save();
    read_table();

    /* Check if file already exists */
    if (find_file(name) >= 0) {
        irq_restore(ps);
        uart_puts("fs: file exists\n");
        return -1;
    }

    /* Find free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        irq_restore(ps);
        uart_puts("fs: file table full\n");
        return -1;
    }

    /* Calculate sectors needed */
    uint16_t nsec = (uint16_t)((size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE);

    /* Build bitmap and allocate */
    uint8_t bmap[BITMAP_BYTES];
    build_bitmap(bmap);

    int start = alloc_sectors(bmap, nsec);
    if (start < 0) {
        irq_restore(ps);
        uart_puts("fs: no space\n");
        return -1;
    }

    /* Write data to flash */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = size;
    for (int s = 0; s < nsec; s++) {
        uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + s) * FS_SECTOR_SIZE;
        flash_erase_sector(addr);

        uint32_t chunk = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
        /* Pad to 4-byte alignment for SPIWrite */
        uint32_t write_len = (chunk + 3) & ~3u;
        flash_write(addr, src, write_len);
        src += chunk;
        remaining -= chunk;
    }

    /* Update file table entry */
    fs_entry_t *e = table_entry(slot);
    ets_memset(e, 0, sizeof(fs_entry_t));
    fs_strncpy(e->name, name, FS_NAME_LEN);
    e->size = size;
    e->start_sector = (uint16_t)start;
    e->sector_count = nsec;
    write_table();

    /* Update superblock */
    fs_super_t sb;
    read_super(&sb);
    sb.file_count++;
    write_super(&sb);

    irq_restore(ps);
    return 0;
}

int fs_read(const char *name, void *buf, uint32_t max_size)
{
    if (!mounted) return -1;

    read_table();
    int idx = find_file(name);
    if (idx < 0) return -1;

    fs_entry_t *e = table_entry(idx);
    uint32_t to_read = e->size < max_size ? e->size : max_size;

    /* Round up to 4-byte boundary for SPIRead */
    uint32_t read_len = (to_read + 3) & ~3u;
    uint32_t addr = FS_DATA_ADDR + (uint32_t)e->start_sector * FS_SECTOR_SIZE;
    flash_read(addr, buf, read_len);

    return (int)to_read;
}

int fs_delete(const char *name)
{
    if (!mounted) return -1;

    uint32_t ps = irq_save();
    read_table();

    int idx = find_file(name);
    if (idx < 0) {
        irq_restore(ps);
        return -1;
    }

    /* Clear the entry */
    ets_memset(table_entry(idx), 0, sizeof(fs_entry_t));
    write_table();

    /* Update superblock */
    fs_super_t sb;
    read_super(&sb);
    if (sb.file_count > 0) sb.file_count--;
    write_super(&sb);

    irq_restore(ps);
    return 0;
}

int fs_stat(const char *name)
{
    if (!mounted) return -1;

    read_table();
    int idx = find_file(name);
    if (idx < 0) return -1;

    return (int)table_entry(idx)->size;
}

void fs_list(void)
{
    if (!mounted) {
        uart_puts("fs: not mounted\n");
        return;
    }

    read_table();

    uart_puts("Name                     Size  Sec\n");
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_entry_t *e = table_entry(i);
        if (entry_free(e)) continue;

        /* Print name padded to 25 chars */
        const char *p = e->name;
        int len = 0;
        while (p[len]) len++;
        uart_puts(e->name);
        for (int j = len; j < 25; j++) uart_putc(' ');

        uart_put_dec(e->size);
        uart_puts("  ");
        uart_put_dec(e->sector_count);
        uart_puts("\n");
        count++;
    }

    if (count == 0)
        uart_puts("(empty)\n");
}

uint32_t fs_free(void)
{
    if (!mounted) return 0;

    read_table();
    uint8_t bmap[BITMAP_BYTES];
    build_bitmap(bmap);
    return count_free(bmap) * FS_SECTOR_SIZE;
}

int fs_overwrite(const char *name, const void *data, uint32_t size)
{
    if (!mounted) return -1;
    if (name[0] == '\0' || size == 0) return -1;

    uint32_t ps = irq_save();
    read_table();

    int idx = find_file(name);
    if (idx < 0) {
        /* File doesn't exist — create it */
        irq_restore(ps);
        return fs_create(name, data, size);
    }

    fs_entry_t *e = table_entry(idx);
    uint16_t new_nsec = (uint16_t)((size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE);

    if (new_nsec <= e->sector_count) {
        /* Fits in existing sectors — erase and rewrite in place */
        uint16_t start = e->start_sector;

        /* Erase all old sectors */
        for (int s = 0; s < e->sector_count; s++) {
            uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + s) * FS_SECTOR_SIZE;
            flash_erase_sector(addr);
        }

        /* Write new data */
        const uint8_t *src = (const uint8_t *)data;
        uint32_t remaining = size;
        for (int s = 0; s < new_nsec; s++) {
            uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + s) * FS_SECTOR_SIZE;
            uint32_t chunk = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
            uint32_t write_len = (chunk + 3) & ~3u;
            flash_write(addr, src, write_len);
            src += chunk;
            remaining -= chunk;
        }

        /* Update entry: new size & sector count */
        e->size = size;
        e->sector_count = new_nsec;
        write_table();

        irq_restore(ps);
        return 0;
    }

    /* Doesn't fit — delete and recreate */
    ets_memset(e, 0, sizeof(fs_entry_t));
    write_table();

    fs_super_t sb;
    read_super(&sb);
    if (sb.file_count > 0) sb.file_count--;
    write_super(&sb);

    irq_restore(ps);
    return fs_create(name, data, size);
}

int fs_append(const char *name, const void *data, uint32_t size)
{
    if (!mounted || size == 0) return -1;

    uint32_t ps = irq_save();
    read_table();

    int idx = find_file(name);
    if (idx < 0) {
        irq_restore(ps);
        return -1;
    }

    fs_entry_t *e = table_entry(idx);
    uint32_t old_size = e->size;
    uint32_t new_total = old_size + size;
    uint16_t start = e->start_sector;
    uint16_t nsec = e->sector_count;

    uint16_t need_nsec = (uint16_t)((new_total + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE);
    if (need_nsec > nsec) {
        irq_restore(ps);
        uart_puts("fs: append won't fit in allocated sectors\n");
        return -1;
    }

    /* Save entry info — we'll reuse sec_buf for data ops */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = size;
    uint32_t write_pos = old_size;

    /* Handle partial last sector: read-modify-write */
    uint32_t offset_in_sec = write_pos % FS_SECTOR_SIZE;
    if (offset_in_sec != 0) {
        uint32_t sec_idx = write_pos / FS_SECTOR_SIZE;
        uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + sec_idx) * FS_SECTOR_SIZE;

        flash_read(addr, sec_buf, FS_SECTOR_SIZE);

        uint32_t space = FS_SECTOR_SIZE - offset_in_sec;
        uint32_t chunk = remaining < space ? remaining : space;
        ets_memcpy(sec_buf + offset_in_sec, src, chunk);

        flash_erase_sector(addr);
        flash_write(addr, sec_buf, FS_SECTOR_SIZE);

        src += chunk;
        remaining -= chunk;
        write_pos += chunk;
    }

    /* Write remaining full sectors */
    while (remaining > 0) {
        uint32_t sec_idx = write_pos / FS_SECTOR_SIZE;
        uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + sec_idx) * FS_SECTOR_SIZE;
        uint32_t chunk = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;

        flash_erase_sector(addr);
        uint32_t write_len = (chunk + 3) & ~3u;
        flash_write(addr, src, write_len);

        src += chunk;
        remaining -= chunk;
        write_pos += chunk;
    }

    /* Reload table and update size */
    read_table();
    table_entry(idx)->size = new_total;
    write_table();

    irq_restore(ps);
    return 0;
}

int fs_rename(const char *old_name, const char *new_name)
{
    if (!mounted) return -1;
    if (old_name[0] == '\0' || new_name[0] == '\0') return -1;

    uint32_t ps = irq_save();
    read_table();

    int idx = find_file(old_name);
    if (idx < 0) {
        irq_restore(ps);
        return -1;
    }

    /* Check new name doesn't already exist */
    if (find_file(new_name) >= 0) {
        irq_restore(ps);
        uart_puts("fs: target name exists\n");
        return -1;
    }

    fs_strncpy(table_entry(idx)->name, new_name, FS_NAME_LEN);
    write_table();

    irq_restore(ps);
    return 0;
}

int fs_upload(const char *name, uint32_t total_size)
{
    if (!mounted) return -1;
    if (name[0] == '\0' || total_size == 0) return -1;

    uint32_t ps = irq_save();

    /* Delete existing file if any */
    read_table();
    int old_idx = find_file(name);
    if (old_idx >= 0) {
        ets_memset(table_entry(old_idx), 0, sizeof(fs_entry_t));
        write_table();
        fs_super_t sb;
        read_super(&sb);
        if (sb.file_count > 0) sb.file_count--;
        write_super(&sb);
        /* Reload table after superblock write (write_super uses sec_buf) */
        read_table();
    }

    /* Find free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        irq_restore(ps);
        uart_puts("fs: file table full\n");
        return -1;
    }

    /* Allocate sectors */
    uint16_t nsec = (uint16_t)((total_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE);
    uint8_t bmap[BITMAP_BYTES];
    build_bitmap(bmap);

    int start = alloc_sectors(bmap, nsec);
    if (start < 0) {
        irq_restore(ps);
        uart_puts("fs: no space\n");
        return -1;
    }

    /* Create file table entry NOW (so sectors are reserved) */
    fs_entry_t *e = table_entry(slot);
    ets_memset(e, 0, sizeof(fs_entry_t));
    fs_strncpy(e->name, name, FS_NAME_LEN);
    e->size = total_size;
    e->start_sector = (uint16_t)start;
    e->sector_count = nsec;
    write_table();

    /* Update superblock */
    fs_super_t sb;
    read_super(&sb);
    sb.file_count++;
    write_super(&sb);

    irq_restore(ps);

    /* Signal PC: ready to receive */
    uart_puts("READY\n");

    /* Receive data sector by sector */
    uint16_t crc = 0xFFFF;
    uint32_t received = 0;

    for (uint16_t sec = 0; sec < nsec; sec++) {
        uint32_t chunk = total_size - received;
        if (chunk > FS_SECTOR_SIZE) chunk = FS_SECTOR_SIZE;

        /* Read chunk bytes from UART into sec_buf */
        uint32_t got = 0;
        uint32_t timeout_start = get_tick_count();
        while (got < chunk) {
            int c = uart_getc();
            if (c >= 0) {
                sec_buf[got++] = (uint8_t)c;
                timeout_start = get_tick_count();
            } else {
                task_yield();
                if (get_tick_count() - timeout_start > 10 * TICK_HZ) {
                    /* Timeout — delete the partial file */
                    fs_delete(name);
                    uart_puts("ERR timeout\n");
                    return -1;
                }
            }
        }

        /* Update CRC */
        for (uint32_t i = 0; i < got; i++) {
            crc ^= (uint16_t)sec_buf[i] << 8;
            for (int b = 0; b < 8; b++) {
                if (crc & 0x8000)
                    crc = (crc << 1) ^ 0x1021;
                else
                    crc <<= 1;
            }
        }

        received += got;

        /* Pad remaining bytes in sector to 0xFF */
        for (uint32_t i = got; i < FS_SECTOR_SIZE; i++)
            sec_buf[i] = 0xFF;

        /* Write sector to flash */
        uint32_t addr = FS_DATA_ADDR + (uint32_t)(start + sec) * FS_SECTOR_SIZE;
        flash_erase_sector(addr);
        flash_write(addr, sec_buf, FS_SECTOR_SIZE);

        /* ACK this sector — PC waits for '#' before sending next chunk */
        uart_putc('#');
    }

    /* Done */
    uart_puts("\nOK ");
    uart_put_hex(crc);
    uart_puts("\n");

    return (int)crc;
}

int fs_mounted(void)
{
    return mounted;
}

uint16_t fs_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

} /* extern "C" */
