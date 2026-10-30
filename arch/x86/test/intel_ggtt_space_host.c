#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_ggtt_space.h"

static unsigned checks;
#define CHECK(x) do { __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED); if (!(x)) { \
    fprintf(stderr, "FAIL GGTT space line %d: %s\n", __LINE__, #x); abort(); \
} } while (0)

static intel_ggtt_space_t space;
static intel_ggtt_layout_t layout = {.ggtt_bytes = 8ULL << 20,
                                     .gpu_va_bytes = 1ULL << 32};
static uint8_t used[4];
static uint64_t table[64], baseline[64];
static unsigned reads, writes, syncs, fail_read, fail_write, fail_sync;
static bool fail_all_sync, reenter, corrupt_readback;
static uint64_t dma_pages[4] = {0x100001000ULL, 0x70004000, 0x23005000, 0x7ffffff000ULL};

static bool mock_read(void *ctx, uint32_t page, uint64_t *pte)
{
    CHECK(ctx == table && page >= 16 && page < 48);
    if (++reads == fail_read) return false;
    *pte = table[page];
    if (corrupt_readback && syncs) { *pte ^= 4096; corrupt_readback = false; }
    return true;
}

static bool mock_write(void *ctx, uint32_t page, uint64_t pte)
{
    CHECK(ctx == table && page >= 16 && page < 48);
    CHECK(page != 22 && page != 26); /* Firmware entries must never be written. */
    if (reenter) {
        intel_ggtt_binding_t other = {0};
        uint64_t original[4];
        reenter = false;
        CHECK(intel_ggtt_bind(&space, &other, dma_pages, 4, 1, 0, original) == INTEL_GGTT_BIND_BUSY);
        CHECK(!other.phase);
    }
    /* A reported write failure can still reach hardware. */
    table[page] = pte;
    return ++writes != fail_write;
}

static bool mock_sync(void *ctx)
{
    CHECK(ctx == table);
    ++syncs;
    return !fail_all_sync && syncs != fail_sync;
}

static const intel_ggtt_io_t io = {mock_read, mock_write, mock_sync};

static void reset_space(void)
{
    memset(&space, 0, sizeof(space));
    memset(used, 0x55, sizeof(used));
    for (unsigned i = 0; i < 64; i++) table[i] = (uint64_t)i << 40;
    table[22] = 0x9b800001;
    table[26] = 0x9bfe8001;
    memcpy(baseline, table, sizeof(table));
    reads = writes = syncs = fail_read = fail_write = fail_sync = 0;
    fail_all_sync = reenter = corrupt_readback = false;
    CHECK(intel_ggtt_space_init(&space, &layout, 16, 32, used, sizeof(used), &io, table)
          == INTEL_GGTT_BIND_OK);
    CHECK(reads == 32 && !writes && !syncs);
    reads = 0;
}

static void check_clean(void)
{
    CHECK(!space.busy);
    CHECK(!memcmp(table, baseline, sizeof(table)));
    for (unsigned i = 0; i < 32; i++)
        CHECK(!!(used[i / 8] & (1u << (i & 7))) == !!(baseline[i + 16] & 1));
}

static void test_lifetime(void)
{
    reset_space();
    intel_ggtt_binding_t a = {0}, b = {0};
    uint64_t old_a[4], old_b[4];
    reenter = true;
    CHECK(intel_ggtt_bind(&space, &a, dma_pages, 4, 4, 0, old_a) == INTEL_GGTT_BIND_OK);
    CHECK(a.first == 16 && a.gpu_address == 0x10000 && a.count == 4 && a.phase == INTEL_GGTT_BOUND);
    uint64_t old_cookie = a.cookie;
    for (unsigned i = 0; i < 4; i++) {
        CHECK(table[16 + i] == (dma_pages[i] | 1));
        CHECK(old_a[i] == baseline[16 + i]);
    }
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 4, 0, old_b) == INTEL_GGTT_BIND_OK);
    CHECK(b.first == 28); /* Align around both firmware entries and a. */
    CHECK(intel_ggtt_pin(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_pin(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    unsigned before = writes;
    CHECK(intel_ggtt_unbind(&space, &a, a.cookie) == INTEL_GGTT_BIND_BUSY);
    CHECK(writes == before && a.pins == 2);
    CHECK(intel_ggtt_unpin(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_unbind(&space, &a, a.cookie) == INTEL_GGTT_BIND_BUSY);
    CHECK(intel_ggtt_unpin(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_unpin(&space, &a, a.cookie) == INTEL_GGTT_BIND_INVALID);
    intel_ggtt_binding_t copy = a;
    CHECK(intel_ggtt_pin(&space, &copy, a.cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unbind(&space, &copy, a.cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unbind(&space, &a, a.cookie + 100) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unbind(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(!a.phase && !a.space && !a.originals);
    CHECK(intel_ggtt_unbind(&space, &a, old_cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &a, dma_pages, 4, 4, 0, old_a) == INTEL_GGTT_BIND_OK);
    CHECK(a.cookie != old_cookie && a.first == 16);
    CHECK(intel_ggtt_pin(&space, &a, old_cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unpin(&space, &a, old_cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unbind(&space, &a, old_cookie) == INTEL_GGTT_BIND_INVALID);
    a.pins = UINT32_MAX;
    CHECK(intel_ggtt_pin(&space, &a, a.cookie) == INTEL_GGTT_BIND_INVALID);
    a.pins = 0;
    CHECK(intel_ggtt_unbind(&space, &a, a.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_OK);
    check_clean();
}

static void test_transactions(void)
{
    uint64_t old[4];
    for (unsigned fault = 1; fault <= 4; fault++) {
        reset_space();
        intel_ggtt_binding_t b = {0};
        fail_write = fault;
        CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_IO_ERROR);
        CHECK(!b.phase && writes == fault + 4 && syncs == 1);
        check_clean();
    }
    for (unsigned fault = 1; fault <= 8; fault++) {
        reset_space();
        intel_ggtt_binding_t b = {0};
        fail_read = fault; /* preflight or post-write verification */
        CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_IO_ERROR);
        CHECK(!b.phase);
        CHECK(writes == (fault <= 4 ? 0u : 8u));
        check_clean();
    }
    reset_space();
    intel_ggtt_binding_t b = {0};
    fail_sync = 1;
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_IO_ERROR);
    CHECK(!b.phase && writes == 8 && syncs == 2);
    check_clean();
    reset_space(); corrupt_readback = true;
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_IO_ERROR);
    CHECK(!b.phase && writes == 8);
    check_clean();

    reset_space(); fail_all_sync = true;
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_QUARANTINED);
    CHECK(b.phase == INTEL_GGTT_QUARANTINED && b.originals == old && b.cookie);
    CHECK((used[0] & 15) == 15);
    CHECK(intel_ggtt_pin(&space, &b, b.cookie) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_QUARANTINED);
    CHECK((used[0] & 15) == 15);
    fail_all_sync = false;
    intel_ggtt_binding_t other = {0};
    uint64_t other_old[4];
    CHECK(intel_ggtt_bind(&space, &other, dma_pages, 4, 4, 0, other_old) == INTEL_GGTT_BIND_OK);
    CHECK(other.first == 28); /* Quarantined space cannot be recycled. */
    CHECK(intel_ggtt_unbind(&space, &other, other.cookie) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_OK);
    check_clean();

    for (unsigned fault = 1; fault <= 4; fault++) {
        reset_space();
        CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_OK);
        fail_write = writes + fault;
        CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_QUARANTINED);
        CHECK(b.phase == INTEL_GGTT_QUARANTINED && (used[0] & 15) == 15);
        fail_write = 0;
        CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_OK);
        check_clean();
    }
    reset_space();
    baseline[16] = table[16] = 0x12345001; /* External mapping after discovery. */
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_CHANGED);
    CHECK(!writes && !b.phase && (used[0] & 1));
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_OK);
    CHECK(b.first == 17);
    CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_OK);
    check_clean();
}

static void test_inputs(void)
{
    reset_space();
    intel_ggtt_binding_t b = {0};
    uint64_t old[4], bad[4];
    memcpy(bad, dma_pages, sizeof(bad)); bad[1]++;
    CHECK(intel_ggtt_bind(&space, &b, bad, 4, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    bad[1] = INTEL_GGTT_PHYS_LIMIT;
    CHECK(intel_ggtt_bind(&space, &b, bad, 4, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 3, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 0, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, UINT32_MAX, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 1, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, dma_pages) == INTEL_GGTT_BIND_INVALID);
    uint64_t overlapping[5] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000}, saved[5];
    memcpy(saved, overlapping, sizeof(saved));
    CHECK(intel_ggtt_bind(&space, &b, overlapping, 4, 1, 0, overlapping + 1)
          == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, overlapping + 1, 4, 1, 0, overlapping)
          == INTEL_GGTT_BIND_INVALID);
    CHECK(!memcmp(saved, overlapping, sizeof(saved)) && !reads && !writes);
    CHECK(intel_ggtt_bind(NULL, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, NULL, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 19 * 4096, old) == INTEL_GGTT_BIND_NO_SPACE);
    CHECK(!writes && !b.phase);
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 20 * 4096, old) == INTEL_GGTT_BIND_OK);
    CHECK(intel_ggtt_unbind(&space, &b, b.cookie) == INTEL_GGTT_BIND_OK);
    space.next_cookie = UINT64_MAX;
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_INVALID);
    check_clean();

    unsigned previous_reads = reads;
    CHECK(intel_ggtt_space_init(&space, &layout, 16, 32, used, sizeof(used), &io, table)
          == INTEL_GGTT_BIND_BUSY);
    CHECK(reads == previous_reads);
    space = (intel_ggtt_space_t){0};
    CHECK(intel_ggtt_space_init(&space, &layout, 0, 32, used, sizeof(used), &io, table)
          == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_space_init(&space, &layout, 16, 32, used, 3, &io, table)
          == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_space_init(&space, &layout, UINT32_MAX, 32, used, 4, &io, table)
          == INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_space_init(&space, &layout, (1u << 20) - 32, 32, used, 4, &io, table)
          == INTEL_GGTT_BIND_INVALID);
    reads = 0; fail_read = 4;
    CHECK(intel_ggtt_space_init(&space, &layout, 16, 32, used, 4, &io, table)
          == INTEL_GGTT_BIND_IO_ERROR);
    CHECK(!space.ready && !space.busy);
    fail_read = 0;
    CHECK(intel_ggtt_space_init(&space, &layout, 16, 32, used, 4, &io, table)
          == INTEL_GGTT_BIND_OK);
    check_clean();
    memset(used, 255, sizeof(used));
    CHECK(intel_ggtt_bind(&space, &b, dma_pages, 4, 1, 0, old) == INTEL_GGTT_BIND_NO_SPACE);
}

static void *mapping_worker(void *argument)
{
    uint64_t dma[2] = {0x50000000 + (uint64_t)(uintptr_t)argument * 0x2000,
                       0x60000000 + (uint64_t)(uintptr_t)argument * 0x2000};
    uint64_t old[2];
    intel_ggtt_binding_t b = {0};
    for (unsigned i = 0; i < 400; i++) {
        enum intel_ggtt_bind_status r;
        do { r = intel_ggtt_bind(&space, &b, dma, 2, 2, 0, old); if (r == INTEL_GGTT_BIND_BUSY) sched_yield(); }
        while (r == INTEL_GGTT_BIND_BUSY);
        CHECK(r == INTEL_GGTT_BIND_OK);
        do { r = intel_ggtt_pin(&space, &b, b.cookie); if (r == INTEL_GGTT_BIND_BUSY) sched_yield(); }
        while (r == INTEL_GGTT_BIND_BUSY);
        CHECK(r == INTEL_GGTT_BIND_OK);
        do { r = intel_ggtt_unpin(&space, &b, b.cookie); if (r == INTEL_GGTT_BIND_BUSY) sched_yield(); }
        while (r == INTEL_GGTT_BIND_BUSY);
        CHECK(r == INTEL_GGTT_BIND_OK);
        do { r = intel_ggtt_unbind(&space, &b, b.cookie); if (r == INTEL_GGTT_BIND_BUSY) sched_yield(); }
        while (r == INTEL_GGTT_BIND_BUSY);
        CHECK(r == INTEL_GGTT_BIND_OK);
    }
    return NULL;
}

static void test_concurrent(void)
{
    reset_space();
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++)
        CHECK(pthread_create(&threads[i], NULL, mapping_worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < 4; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    check_clean();
}

static void capacity_cases(void)
{
    reset_space();
    uint32_t count=0xaaaa;
    CHECK(intel_ggtt_space_capacity(&space,3,1,0,&count)==INTEL_GGTT_BIND_OK && count==9);
    CHECK(intel_ggtt_space_capacity(&space,23,1,0,&count)==INTEL_GGTT_BIND_OK && !count);
    CHECK(intel_ggtt_space_capacity(&space,0,1,0,&count)==INTEL_GGTT_BIND_INVALID);
    CHECK(intel_ggtt_space_capacity(&space,1,0,4097,&count)==INTEL_GGTT_BIND_INVALID);
    space.busy=1;
    CHECK(intel_ggtt_space_capacity(&space,3,1,0,&count)==INTEL_GGTT_BIND_BUSY); space.busy=0;
    /* Exhaustive small-window fragmentation against a first-fit allocator
     * oracle, including one page kept for HWSP and address clipping. No MMIO. */
    for (unsigned mask=0;mask<4096;mask++) for (unsigned size=1;size<=6;size++)
        for (unsigned spare=0;spare<=3;spare++) for (unsigned clip=0;clip<2;clip++) {
            used[0]=(uint8_t)mask; used[1]=(uint8_t)(0xf0|(mask>>8)); used[2]=used[3]=255;
            unsigned n=clip ? 8 : 12,free=0,expected=0;
            bool busy[12]; for (unsigned i=0;i<n;i++) { busy[i]=!!(mask&(1u<<i)); free+=!busy[i]; }
            while (free>=spare && free-spare>=size) {
                unsigned at=0;
                for (;at+size<=n;at++) {
                    unsigned j=0; while (j<size && !busy[at+j]) j++;
                    if (j==size) break;
                }
                if (at+size>n) break;
                for (unsigned j=0;j<size;j++) busy[at+j]=true;
                expected++; free-=size;
            }
            CHECK(intel_ggtt_space_capacity(&space,size,spare,clip ? 24*4096 : 0,&count)==INTEL_GGTT_BIND_OK);
            CHECK(count==expected && !space.busy);
        }
    CHECK(!reads && !writes && !syncs);
    reset_space(); check_clean();
}
int main(void)
{
    capacity_cases();
    test_inputs();
    test_lifetime();
    test_transactions();
    test_concurrent();
    printf("GGTT mapping lifetime: %u checks, 0 failures (mock hardware)\n", checks);
    return 0;
}
