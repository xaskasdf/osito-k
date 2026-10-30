/*
 * kload.c -- receive a kernel Image over the USB console and run it
 *
 * The `kload` shell command; the host side is arch/arm/scripts/rog5-kload.ps1
 * (rog5-boot.sh --usb). This replaces flashing boot_a for every test build.
 * `put <name>` receives a file the same way (rog5-put.ps1) into OsitoFS, and
 * `get <name>` sends one back in the same framing (rog5-get.ps1).
 *
 * Protocol, after the device prints "kload: ready": the 4 bytes "OKLD", the
 * Image size and its CRC-32 (u32 little-endian each), then the Image. Bytes
 * before the magic are skipped (e.g. a stray '\n' after the command line).
 *
 * The Image runs where it lands, in freshly allocated pages: the kernel
 * relocates itself and derives its stack, BSS and page-allocator reservation
 * from its own load address (mem_init reserves from the RAM base up to the
 * end of the running kernel). The old kernel's memory is simply abandoned. The
 * entry state matches the shim's: EL1, MMU and D-cache off, DAIF masked,
 * x0 = 0 (Osito-K ignores the DTB).
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "mmu.h"
#include "usb.h"

#define KLOAD_MAX       (32U << 20)
#define PUT_MAX         (1U << 30)      /* received in the high region (models) */

/* fs/ositofs2.c, beyond hal.h */
int  osfs2_write(osfs2_file_t *file, uint64_t offset, const void *buf, uint64_t len);
bool osfs2_is_mounted(void);
#define BYTE_TIMEOUT_MS 5000
#define ARM64_MAGIC_OFF 0x38            /* "ARM\x64" in the Image header */

/* Next byte from the USB console; -1 after BYTE_TIMEOUT_MS of silence. The
 * controller is only serviced when the ring is empty: polling it per byte
 * (an uncached MMIO read each time) held uploads to ~80 KB/s. The gadget is
 * shared with other CPUs' output, hence the console lock, taken per access
 * so they aren't held off for the whole transfer. */
static int read_byte(void)
{
    uint64_t flags = console_lock();
    int c = acm_getc();
    console_unlock(flags);
    if (c >= 0)
        return c;
    for (uint32_t waited_us = 0; waited_us < BYTE_TIMEOUT_MS * 1000; waited_us += 20) {
        flags = console_lock();
        acm_poll();
        c = acm_getc();
        console_unlock(flags);
        if (c >= 0)
            return c;
        udelay(20);
    }
    return -1;
}

static int read_u32(uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int c = read_byte();
        if (c < 0)
            return -1;
        v |= (uint32_t)c << (8 * i);
    }
    *out = v;
    return 0;
}

static uint32_t crc32_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    return crc;
}

static void say(const char *who, const char *what)
{
    serial_puts(who);
    serial_puts(": ");
    serial_puts(what);
}

/* Print "<who>: ready", then take "OKLD" + size + CRC-32 + the bytes into
 * fresh pages: in the first memory region for a kernel (it runs where it
 * lands, and mem_init reserves up to it from that region's base), high for
 * a file. Returns them (*pages to free), or NULL with the reason printed. */
static uint8_t *receive(const char *who, uint32_t min, uint32_t max, int high,
                        uint32_t *size_out, uint64_t *pages_out)
{
    static const char magic[4] = { 'O', 'K', 'L', 'D' };
    uint32_t matched = 0, size, crc_expect;

    say(who, "ready\n");
    while (matched < 4) {
        int c = read_byte();
        if (c < 0) {
            say(who, "timeout waiting for the header\n");
            return (void *)0;
        }
        if (c == magic[matched])
            matched++;
        else
            matched = (c == magic[0]);
    }
    if (read_u32(&size) || read_u32(&crc_expect)) {
        say(who, "timeout in the header\n");
        return (void *)0;
    }
    if (size < min || size > max) {
        say(who, "bad size\n");
        return (void *)0;
    }

    uint64_t pages = (size + 4095) / 4096;
    uint8_t *buf = high ? mem_alloc_aligned_high(size, 4096) : mem_alloc_pages(pages);
    if (!buf) {
        say(who, "out of memory\n");
        return (void *)0;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++) {
        int c = read_byte();
        if (c < 0) {
            say(who, "timeout after ");
            serial_putdec(i);
            serial_puts(" bytes\n");
            mem_free_pages(buf, pages);
            return (void *)0;
        }
        buf[i] = (uint8_t)c;
        crc = crc32_byte(crc, (uint8_t)c);
    }
    crc = ~crc;

    if (crc != crc_expect) {
        say(who, "crc mismatch, got ");
        serial_puthex(crc, 8);
        serial_puts("\n");
        mem_free_pages(buf, pages);
        return (void *)0;
    }
    *size_out = size;
    *pages_out = pages;
    return buf;
}

/* Shell `put <name>`: a file from the host the same way (rog5-put.ps1), into
 * the mounted OsitoFS, replacing one with that name */
void platform_put_cmd(int argc, char **argv)
{
    if (argc != 2) {
        serial_puts("usage: put <name>  (the host sends it: rog5-put.ps1)\n");
        return;
    }
    if (!osfs2_is_mounted()) {
        serial_puts("put: no filesystem mounted (ufs mount)\n");
        return;
    }
    uint32_t size;
    uint64_t pages;
    uint8_t *buf = receive("put", 1, PUT_MAX, 1, &size, &pages);
    if (!buf)
        return;
    uint32_t crc_sent = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++)
        crc_sent = crc32_byte(crc_sent, buf[i]);
    if (osfs2_find(argv[1]))
        osfs2_delete(argv[1]);
    osfs2_file_t *f = osfs2_create(argv[1], size);
    int stored = f && osfs2_write(f, 0, buf, size) == 0 && osfs2_sync() == 0;

    /* Read it back from the device into the same pages, and check it */
    int verified = 0;
    if (stored) {
        for (uint32_t i = 0; i < size; i++)
            buf[i] = 0;
        if (osfs2_read(f, 0, buf, size) == (int)size) {
            uint32_t crc = 0xFFFFFFFF;
            for (uint32_t i = 0; i < size; i++)
                crc = crc32_byte(crc, buf[i]);
            verified = crc == crc_sent;
        }
    }
    mem_free_pages(buf, pages);
    serial_puts("put: ");
    serial_puts(argv[1]);
    serial_puts(", ");
    serial_putdec(size);
    serial_puts(!stored ? " bytes, crc ok, but storing it failed\n" :
                !verified ? " bytes, crc ok, stored, but it reads back different\n" :
                " bytes, crc ok, stored and verified\n");
}

/* Raw bytes to the host, a chunk per console lock so other CPUs' output
 * (kept off the link by console_host_hold) isn't held off for long */
static void host_send(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len) {
        uint32_t n = len < 4096 ? len : 4096;
        uint64_t flags = console_lock();
        for (uint32_t i = 0; i < n; i++)
            acm_putc((char)p[i]);
        console_unlock(flags);
        p += n;
        len -= n;
    }
}

/* Shell `get <name>`: a file from the mounted OsitoFS to the host
 * (rog5-get.ps1), framed as put's: "OKLD", size, CRC-32, the bytes */
void platform_get_cmd(int argc, char **argv)
{
    if (argc != 2) {
        serial_puts("usage: get <name>  (the host takes it: rog5-get.ps1)\n");
        return;
    }
    if (!osfs2_is_mounted()) {
        serial_puts("get: no filesystem mounted (ufs mount)\n");
        return;
    }
    osfs2_file_t *f = osfs2_find(argv[1]);
    if (!f) {
        serial_puts("get: no such file\n");
        return;
    }
    if (f->size == 0 || f->size > PUT_MAX) {
        serial_puts("get: bad size\n");
        return;
    }
    uint32_t size = (uint32_t)f->size;
    uint64_t pages = (size + 4095) / 4096;
    uint8_t *buf = mem_alloc_aligned_high(size, 4096);
    if (!buf) {
        serial_puts("get: out of memory\n");
        return;
    }
    if (osfs2_read(f, 0, buf, size) != (int)size) {
        serial_puts("get: read failed\n");
        mem_free_pages(buf, pages);
        return;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++)
        crc = crc32_byte(crc, buf[i]);
    crc = ~crc;

    serial_puts("get: sending\n");
    uint8_t hdr[12] = { 'O', 'K', 'L', 'D' };
    for (int i = 0; i < 4; i++) {
        hdr[4 + i] = (uint8_t)(size >> (8 * i));
        hdr[8 + i] = (uint8_t)(crc >> (8 * i));
    }
    console_host_hold(1);
    host_send(hdr, sizeof(hdr));
    host_send(buf, size);
    console_host_hold(0);
    mem_free_pages(buf, pages);
    serial_puts("get: ");
    serial_puts(argv[1]);
    serial_puts(", ");
    serial_putdec(size);
    serial_puts(" bytes sent\n");
}

int platform_kload(void)
{
    uint32_t size;
    uint64_t pages;
    uint8_t *buf = receive("kload", ARM64_MAGIC_OFF + 4, KLOAD_MAX, 0, &size, &pages);
    if (!buf)
        return -1;
    if (buf[ARM64_MAGIC_OFF] != 'A' || buf[ARM64_MAGIC_OFF + 1] != 'R' ||
        buf[ARM64_MAGIC_OFF + 2] != 'M' || buf[ARM64_MAGIC_OFF + 3] != 0x64) {
        serial_puts("kload: not an ARM64 Image\n");
        mem_free_pages(buf, pages);
        return -1;
    }

    serial_puts("kload: ");
    serial_putdec(size);
    serial_puts(" bytes, crc ok, starting it at ");
    serial_puthex((uint64_t)(uintptr_t)buf, 10);
    serial_puts("\n");
    /* Stop the other CPUs first: they run from memory the new kernel will
     * reuse, and they could print into the gadget being shut down */
    smp_stop_aps();
    for (int i = 0; i < 25000 && !acm_tx_idle(); i++) {     /* up to ~0.5 s */
        acm_poll();
        udelay(20);
    }
    mdelay(200);        /* let the host app read it before the port disappears */
    acm_stop();
    mmu_off_and_jump((uint64_t)(uintptr_t)buf);
}
