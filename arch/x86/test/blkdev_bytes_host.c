#include "../include/paging.h"
#undef PHYS_TO_VIRT
#define PHYS_TO_VIRT(p) ((void *)(p))
#include "../kernel/blkdev.c"

extern int printf(const char *, ...);
extern void *aligned_alloc(size_t, size_t);
extern void free(void *);

static unsigned checks, failures, reads, writes, allocations, frees;
static unsigned failed_read, reject_over, max_transfer, alloc_attempts;
static uint64_t allocated_size, last_alloc_request;
static void *allocated;
static uint8_t media[512 * 1024];
static uint8_t output[sizeof(media) + 32];

#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void nvme_debug_watch_cpu_write(void *p, uint64_t n, const char *s)
{ (void)p; (void)n; (void)s; }

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    alloc_attempts++;
    last_alloc_request = size;
    CHECK(alignment == 4096 && size && size % 4096 == 0);
    CHECK(!allocated);
    if (reject_over && size > reject_over) return NULL;
    allocated = aligned_alloc(alignment, size);
    CHECK(allocated != NULL);
    allocated_size = size;
    allocations++;
    return allocated;
}

void mem_free_pages(void *p, uint64_t count)
{
    CHECK(p && p == allocated && count * 4096 == allocated_size);
    free(p);
    allocated = NULL;
    allocated_size = 0;
    frees++;
}

static int mock_read(uint64_t lba, uint32_t count, void *buf)
{
    uint64_t size = (uint64_t)count * devices[0].sector_size;
    uint64_t off = lba * devices[0].sector_size;
    reads++;
    CHECK(count && off <= sizeof(media) && size <= sizeof(media) - off);
    if (off > sizeof(media) || size > sizeof(media) - off) return -1;
    if (devices[0].type != BLKDEV_NVME || buf == allocated) {
        CHECK(buf == allocated && size <= allocated_size);
        if (buf != allocated || size > allocated_size) return -1;
    }
    if (size > max_transfer) max_transfer = size;
    if (reads == failed_read) return -1;
    memcpy(buf, media + off, size);
    return 0;
}

static int mock_write(uint64_t lba, uint32_t count, const void *buf)
{
    uint64_t size = (uint64_t)count * devices[0].sector_size;
    uint64_t off = lba * devices[0].sector_size;
    CHECK(off <= sizeof(media) && size <= sizeof(media) - off);
    if (off > sizeof(media) || size > sizeof(media) - off) return -1;
    writes++;
    memcpy(media + off, buf, size);
    return 0;
}

static void setup(uint8_t type, uint32_t sector)
{
    CHECK(!allocated && allocations == frees);
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    reads = writes = allocations = frees = alloc_attempts = 0;
    failed_read = reject_over = max_transfer = 0;
    last_alloc_request = 0;
    CHECK(blkdev_register("test", type, sector, sizeof(media) / sector,
                          mock_read, mock_write) == 0);
    disk_set_active(0);
    for (unsigned i = 0; i < sizeof(media); i++)
        media[i] = (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
    memset(output, 0xcc, sizeof(output));
}

static void check_read(uint64_t off, uint64_t len)
{
    memset(output, 0xcc, sizeof(output));
    CHECK(disk_read_bytes(off, output + 16, len) == 0);
    CHECK(!memcmp(output + 16, media + off, len));
    for (unsigned i = 0; i < 16; i++) {
        CHECK(output[i] == 0xcc);
        CHECK(output[16 + len + i] == 0xcc);
    }
    CHECK(!allocated && allocations == frees);
}

static void usb_boundaries(void)
{
    const uint32_t sectors[] = {512, 4096, 8192};
    const uint32_t lengths[] = {0, 1, 511, 512, 513, 4095, 4096, 8191,
        8192, 8193, 16384, 32768, 65535, 65536, 65537, 131072, 131079};
    const uint32_t offsets[] = {0, 1, 511, 4095, 8191, 65535, 65536};
    for (unsigned s = 0; s < sizeof(sectors) / sizeof(sectors[0]); s++) {
        for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
            for (unsigned n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
                setup(BLKDEV_USB, sectors[s]);
                check_read(offsets[o], lengths[n]);
                CHECK(max_transfer <= 65536);
                if (lengths[n] <= 8192 - offsets[o] % sectors[s])
                    CHECK(last_alloc_request == (lengths[n] ? 8192 : 0));
                if (!offsets[o] && lengths[n] >= 65536) {
                    CHECK(max_transfer == 65536);
                    CHECK(reads == (lengths[n] + 65535) / 65536);
                }
            }
        }
        setup(BLKDEV_USB, sectors[s]);
        check_read(sizeof(media) - 1, 1);
        check_read(sizeof(media) - 131073, 131073);
        check_read(sizeof(media), 0);
    }
}

static void failures_and_fallback(void)
{
    setup(BLKDEV_USB, 512);
    CHECK(disk_read_bytes(0, NULL, 1) < 0);
    CHECK(disk_read_bytes(sizeof(media), output, 1) < 0);
    CHECK(disk_read_bytes(UINT64_MAX, output, 1) < 0);
    CHECK(disk_read_bytes(1, output, UINT64_MAX) < 0);
    CHECK(!disk_read_bytes(0, NULL, 0));
    CHECK(!reads && !alloc_attempts);
    disk_set_active(-1);
    CHECK(disk_read_bytes(0, output, 1) < 0);
    disk_set_active(0);
    devices[0].read = NULL;
    CHECK(disk_read_bytes(0, output, 1) < 0);
    devices[0].read = mock_read;
    devices[0].active = false;
    CHECK(disk_read_bytes(0, output, 1) < 0);
    devices[0].active = true;
    devices[0].sector_size = 0;
    CHECK(disk_read_bytes(0, output, 1) < 0);

    setup(BLKDEV_USB, 512);
    reject_over = 8192;
    check_read(0, 131072);
    CHECK(alloc_attempts == 2 && allocations == 1);
    CHECK(max_transfer == 8192 && reads == 16);

    setup(BLKDEV_USB, 512);
    reject_over = 1;
    CHECK(disk_read_bytes(0, output + 16, 65536) < 0);
    CHECK(alloc_attempts == 2 && !reads && !allocated && !allocations);
    CHECK(output[16] == 0xcc);

    setup(BLKDEV_USB, 512);
    failed_read = 1;
    CHECK(disk_read_bytes(0, output + 16, 131072) < 0);
    CHECK(reads == 1 && allocations == frees);
    CHECK(output[16] == 0xcc);

    setup(BLKDEV_USB, 512);
    failed_read = 2;
    CHECK(disk_read_bytes(0, output + 16, 131072) < 0);
    CHECK(reads == 2 && allocations == frees);
    CHECK(!memcmp(output + 16, media, 65536));
    CHECK(output[16 + 65536] == 0xcc && output[16 + 131072] == 0xcc);
}

static void other_paths(void)
{
    const uint8_t types[] = {BLKDEV_AHCI, BLKDEV_VIRTIO};
    for (unsigned i = 0; i < sizeof(types); i++) {
        setup(types[i], 512);
        check_read(0, 65536);
        CHECK(max_transfer == 8192 && reads == 8 && allocations == 1);
    }
    setup(BLKDEV_NVME, 512);
    check_read(0, 131072);
    CHECK(reads == 1 && max_transfer == 131072 && !allocations);
    check_read(1, 65537);
    CHECK(allocations == 1);

    setup(BLKDEV_USB, 512);
    memset(output, 0x69, 65537);
    uint8_t before = media[510], after = media[511 + 65537];
    CHECK(!disk_write_bytes(511, output, 65537));
    CHECK(!memcmp(media + 511, output, 65537));
    CHECK(media[510] == before && media[511 + 65537] == after);
    CHECK(writes == 9 && allocations == frees);
    setup(BLKDEV_USB, 512);
    before = media[0];
    failed_read = 1;
    CHECK(disk_write_bytes(1, output, 1) < 0 && !writes);
    CHECK(media[0] == before && allocations == frees);
}

int main(void)
{
    usb_boundaries();
    failures_and_fallback();
    other_paths();
    printf("Block byte I/O: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
