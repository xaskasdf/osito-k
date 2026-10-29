/*
 * kload.c -- receive a kernel Image over the USB console and run it
 *
 * The `kload` shell command; the host side is arch/arm/scripts/rog5-kload.ps1
 * (rog5-boot.sh --usb). This replaces flashing boot_a for every test build.
 *
 * Protocol, after the device prints "kload: ready": the 4 bytes "OKLD", the
 * Image size and its CRC-32 (u32 little-endian each), then the Image. Bytes
 * before the magic are skipped (e.g. a stray '\n' after the command line).
 *
 * The Image runs where it lands, in freshly allocated pages: the kernel is
 * PC-relative and derives its stack, BSS and page-allocator reservation from
 * its own load address (mem_init reserves from the RAM base up to the end
 * of the running kernel). The old kernel's memory is simply abandoned. The
 * entry state matches the shim's: EL1, MMU and D-cache off, DAIF masked,
 * x0 = 0 (Osito-K ignores the DTB).
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "usb.h"

#define KLOAD_MAX       (32U << 20)
#define BYTE_TIMEOUT_MS 5000
#define ARM64_MAGIC_OFF 0x38            /* "ARM\x64" in the Image header */

/* Next byte from the USB console; -1 after BYTE_TIMEOUT_MS of silence. The
 * controller is only serviced when the ring is empty: polling it per byte
 * (an uncached MMIO read each time) held uploads to ~80 KB/s. */
static int read_byte(void)
{
    int c = acm_getc();
    if (c >= 0)
        return c;
    for (uint32_t waited_us = 0; waited_us < BYTE_TIMEOUT_MS * 1000; waited_us += 20) {
        acm_poll();
        c = acm_getc();
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

static void __attribute__((noreturn)) jump_to(uint64_t entry)
{
    register uint64_t target __asm__("x1") = entry;
    __asm__ volatile(
        "msr  daifset, #0xf\n"
        "msr  cntv_ctl_el0, xzr\n"      /* stop the scheduler tick */
        "ic   iallu\n"                  /* the new code was written as data */
        "dsb  sy\n"
        "isb\n"
        "mov  x0, xzr\n"
        "br   x1\n"
        : : "r"(target) : "x0", "memory");
    __builtin_unreachable();
}

int platform_kload(void)
{
    static const char magic[4] = { 'O', 'K', 'L', 'D' };
    uint32_t matched = 0, size, crc_expect;

    serial_puts("kload: ready\n");
    while (matched < 4) {
        int c = read_byte();
        if (c < 0) {
            serial_puts("kload: timeout waiting for the header\n");
            return -1;
        }
        if (c == magic[matched])
            matched++;
        else
            matched = (c == magic[0]);
    }
    if (read_u32(&size) || read_u32(&crc_expect)) {
        serial_puts("kload: timeout in the header\n");
        return -1;
    }
    if (size < ARM64_MAGIC_OFF + 4 || size > KLOAD_MAX) {
        serial_puts("kload: bad size\n");
        return -1;
    }

    uint64_t pages = (size + 4095) / 4096;
    uint8_t *buf = mem_alloc_pages(pages);
    if (!buf) {
        serial_puts("kload: out of memory\n");
        return -1;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++) {
        int c = read_byte();
        if (c < 0) {
            serial_puts("kload: timeout after ");
            serial_putdec(i);
            serial_puts(" bytes\n");
            mem_free_pages(buf, pages);
            return -1;
        }
        buf[i] = (uint8_t)c;
        crc = crc32_byte(crc, (uint8_t)c);
    }
    crc = ~crc;

    if (crc != crc_expect) {
        serial_puts("kload: crc mismatch, got ");
        serial_puthex(crc, 8);
        serial_puts("\n");
        mem_free_pages(buf, pages);
        return -1;
    }
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
    for (int i = 0; i < 25000 && !acm_tx_idle(); i++) {     /* up to ~0.5 s */
        acm_poll();
        udelay(20);
    }
    mdelay(200);        /* let the host app read it before the port disappears */
    acm_stop();
    jump_to((uint64_t)(uintptr_t)buf);
}
