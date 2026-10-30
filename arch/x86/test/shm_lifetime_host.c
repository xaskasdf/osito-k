/* Real SHM implementation with host memory and concurrent surface owners. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OSITOK_X86_TYPES_H
#include "../include/paging.h"
#undef PHYS_TO_VIRT
#undef VIRT_TO_PHYS
#define PHYS_TO_VIRT(p) ((void *)(p))
#define VIRT_TO_PHYS(p) ((uint64_t)(uintptr_t)(p))
#define SHM_HOST_TEST
#ifndef SHM_TEST_SOURCE
#define SHM_TEST_SOURCE "../kernel/shm.c"
#endif
#include SHM_TEST_SOURCE

static unsigned checks, failures;
#define CHECK(value) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); \
    if (!(value)) { unsigned failure = __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); \
        if (failure < 20) fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)

enum { BLOCKS = SHM_MAX_REGIONS + 256, WORKERS = 6, LOOPS = 1000 };
static struct { void *base; uint64_t pages; } allocations[BLOCKS];
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned allocation_count, free_count;
static bool fail_allocation, synchronize_allocation, probe_allocator;
static pthread_barrier_t allocation_barrier;
static bool compositor_live;
static uint32_t direct_pixels[16], *active_scanout;
static uint32_t destroy_during_flush;
static unsigned flushes, dirty_signals;

void serial_puts(const char *text) { (void)text; }
void serial_puthex(uint64_t value, int digits) { (void)value; (void)digits; }
void serial_putdec(uint64_t value) { (void)value; }
uint32_t proc_exec_pid(void) { return 42; }
const char *proc_current_name(void) { return "surface test"; }

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    if (probe_allocator) (void)shm_get_active_count();
    if (__atomic_exchange_n(&fail_allocation, false, __ATOMIC_RELAXED)) return NULL;
    if (synchronize_allocation) pthread_barrier_wait(&allocation_barrier);
    void *memory = NULL;
    if (!size || size > 1u << 24 || posix_memalign(&memory, alignment, size)) return NULL;
    memset(memory, 0x5a, size);
    pthread_mutex_lock(&allocation_lock);
    unsigned slot;
    for (slot = 0; slot < BLOCKS && allocations[slot].base; slot++) {}
    CHECK(slot < BLOCKS);
    if (slot < BLOCKS) {
        allocations[slot].base = memory;
        allocations[slot].pages = size / 4096;
        allocation_count++;
    }
    pthread_mutex_unlock(&allocation_lock);
    return memory;
}

void mem_free_pages(void *memory, uint64_t pages)
{
    if (probe_allocator) (void)shm_get_active_count();
    pthread_mutex_lock(&allocation_lock);
    unsigned slot;
    for (slot = 0; slot < BLOCKS && allocations[slot].base != memory; slot++) {}
    CHECK(slot < BLOCKS);
    if (slot < BLOCKS) {
        CHECK(allocations[slot].pages == pages);
        allocations[slot].base = NULL;
        free_count++;
        free(memory);
    }
    pthread_mutex_unlock(&allocation_lock);
}

uint32_t compositor_create_window(uint32_t handle, int16_t x, int16_t y,
                                 uint16_t width, uint16_t height, uint32_t pid, const char *title)
{
    (void)x; (void)y;
    CHECK(width == 2 && height == 2 && pid == 42 && !strcmp(title, "surface test"));
    CHECK(shm_get_size(handle) == 4096);
    CHECK(shm_map(handle) != NULL);
    return handle;
}
void compositor_set_fullscreen(uint32_t window, bool full) { CHECK(window && full); }
void compositor_signal_dirty(uint32_t window) { CHECK(window); dirty_signals++; }
uint32_t compositor_find_window_by_shm(uint32_t handle) { return handle; }
bool compositor_is_running(void) { return compositor_live; }
uint32_t *fb_get_base(void)
{
    if (destroy_during_flush) {
        shm_destroy(destroy_during_flush);
        /* A direct flush must hold its own reference while it reads pixels. */
        CHECK(shm_get_size(destroy_during_flush) == 4096);
        destroy_during_flush = 0;
    }
    return active_scanout;
}
uint32_t fb_get_width(void) { return 2; }
uint32_t fb_get_height(void) { return 2; }
uint32_t fb_get_pitch(void) { return 2; }
void fb_flush_all(void) { flushes++; }
uint64_t idt_get_ticks(void) { return 100; }

static void reference_contract(void)
{
    uint32_t handle = shm_create_surface(2, 2, SHM_SURFACE_FLAGS);
    CHECK(handle && shm_get_active_count() == 1);
    CHECK(shm_get_flags(handle) == SHM_SURFACE_FLAGS && shm_get_size(handle) == 4096);
    uint32_t *one = shm_map(handle), *two = shm_map(handle);
    CHECK(one && one == two && shm_get_phys(handle) == (uint64_t)(uintptr_t)one);
    if (!one) return;
    bool zero = true;
    for (unsigned i = 0; i < 1024; i++) if (one[i]) zero = false;
    CHECK(zero);
    shm_destroy(handle);
    shm_destroy(handle); /* Repeated creator release cannot consume a mapping. */
    shm_unmap(handle);
    CHECK(shm_get_size(handle) == 4096);
    if (shm_get_size(handle)) {
        two[0] = 0x12345678;
        uint32_t *three = shm_map(handle);
        CHECK(three == two && three[0] == 0x12345678);
        shm_unmap(handle);
    }
    shm_unmap(handle);
    CHECK(!shm_get_size(handle) && !shm_map(handle) && !shm_get_active_count());
    shm_destroy(handle); shm_unmap(handle);
    CHECK(!shm_get_flags(handle) && !shm_get_phys(handle));
}

static void *create_once(void *out)
{
    *(uint32_t *)out = shm_create(4096, SHM_SURFACE_FLAGS);
    return NULL;
}

static void simultaneous_creators(void)
{
    pthread_t threads[2];
    uint32_t handles[2] = {0};
    CHECK(!pthread_barrier_init(&allocation_barrier, NULL, 2));
    synchronize_allocation = true;
    for (unsigned i = 0; i < 2; i++) CHECK(!pthread_create(&threads[i], NULL, create_once, &handles[i]));
    for (unsigned i = 0; i < 2; i++) CHECK(!pthread_join(threads[i], NULL));
    synchronize_allocation = false;
    pthread_barrier_destroy(&allocation_barrier);
    CHECK(handles[0] && handles[1] && handles[0] != handles[1]);
    void *one = shm_map(handles[0]), *two = shm_map(handles[1]);
    CHECK(one && two && one != two);
    if (one) shm_unmap(handles[0]);
    if (two) shm_unmap(handles[1]);
    shm_destroy(handles[0]); shm_destroy(handles[1]);
    CHECK(shm_get_active_count() == 0);
}

static void capacity_and_cleanup(void)
{
    /* The compositor admits 256 windows. Keep that many one-page surfaces
     * alive while allocating a replacement; the older 64-slot table cannot
     * satisfy this public desktop workload even when every owner is live. */
    uint32_t desktop[256];
    for (unsigned i = 0; i < 256; i++) {
        desktop[i] = shm_create_surface(16, 16, 3);
        CHECK(desktop[i] && shm_map(desktop[i]));
    }
    uint32_t replacement = shm_create_surface(32, 32, 3);
    CHECK(replacement != 0);
    shm_destroy(replacement);
    for (unsigned i = 0; i < 256; i++) {
        shm_destroy(desktop[i]);
        if (desktop[i]) CHECK(shm_get_size(desktop[i]) == 4096);
        shm_unmap(desktop[i]);
    }
    CHECK(!shm_get_active_count());
    fail_allocation = true;
    CHECK(!shm_create(4096, 3) && !shm_get_active_count());
    CHECK(!shm_create(0, 3));
    CHECK(!shm_create(UINT64_MAX, 3));
    uint32_t handles[SHM_MAX_REGIONS];
    for (unsigned i = 0; i < SHM_MAX_REGIONS; i++) {
        handles[i] = shm_create(1, 3);
        CHECK(handles[i] && shm_get_size(handles[i]) == 4096);
        shm_set_owner(handles[i], i & 1 ? 42 : 43);
        CHECK(shm_map(handles[i]) != NULL);
    }
    CHECK(shm_get_active_count() == SHM_MAX_REGIONS && !shm_create(4096, 3));
    shm_cleanup_process(42); shm_cleanup_process(42);
    CHECK(shm_get_active_count() == SHM_MAX_REGIONS);
    for (unsigned i = 0; i < SHM_MAX_REGIONS; i++) {
        shm_unmap(handles[i]);
        CHECK((shm_get_size(handles[i]) != 0) == !(i & 1));
    }
    CHECK(shm_get_active_count() == SHM_MAX_REGIONS / 2);
    shm_cleanup_process(43);
    CHECK(shm_get_active_count() == 0);
    CHECK(!shm_create_surface(0, 2, 3) && !shm_create_surface(2, 65536, 3));
    uint32_t scanout = shm_create_surface(2, 2, SHM_SCANOUT_FLAGS);
    CHECK(scanout != 0);
    shm_cleanup_process(42);
    CHECK(shm_get_size(scanout) == 4096);
    shm_unmap(scanout);
    CHECK(!shm_get_active_count());
}

static uint32_t shared_handle;
static void *mapping_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < LOOPS; i++) {
        const uint32_t *pixels = shm_map(shared_handle);
        CHECK(pixels && pixels[0] == 0x11223344);
        if (pixels) shm_unmap(shared_handle);
    }
    return NULL;
}

static void *surface_worker(void *arg)
{
    uint32_t pattern = (uint32_t)(uintptr_t)arg;
    for (unsigned i = 0; i < LOOPS; i++) {
        uint32_t handle = shm_create_surface(4, 4, 3);
        CHECK(handle);
        uint32_t *pixels = shm_map(handle);
        CHECK(pixels);
        if (pixels) {
            for (unsigned n = 0; n < 16; n++) pixels[n] = pattern;
            shm_destroy(handle);
            sched_yield();
            bool matches = true;
            for (unsigned n = 0; n < 16; n++) if (pixels[n] != pattern) matches = false;
            CHECK(matches);
            shm_unmap(handle);
        } else shm_destroy(handle);
    }
    return NULL;
}

static void concurrency_contract(void)
{
    pthread_t threads[WORKERS];
    shared_handle = shm_create_surface(4, 4, 3);
    uint32_t *anchor = shm_map(shared_handle);
    CHECK(anchor);
    if (!anchor) return;
    anchor[0] = 0x11223344;
    shm_destroy(shared_handle);
    for (unsigned i = 0; i < WORKERS; i++) CHECK(!pthread_create(&threads[i], NULL, mapping_worker, NULL));
    for (unsigned i = 0; i < WORKERS; i++) CHECK(!pthread_join(threads[i], NULL));
    shm_unmap(shared_handle);
    CHECK(!shm_get_active_count());
    for (unsigned i = 0; i < WORKERS; i++)
        CHECK(!pthread_create(&threads[i], NULL, surface_worker, (void *)(uintptr_t)(i + 1)));
    for (unsigned i = 0; i < WORKERS; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!shm_get_active_count());
}

static void flush_contract(void)
{
    uint32_t handle = shm_create_surface(2, 2, 3);
    uint32_t *pixels = shm_map(handle);
    CHECK(pixels);
    if (!pixels) return;
    for (unsigned i = 0; i < 4; i++) pixels[i] = 0x10203040 + i;
    shm_unmap(handle);
    compositor_live = true;
    shm_flush_surface(handle);
    CHECK(dirty_signals == 1 && !flushes && shm_get_active_count() == 1);
    compositor_live = false;
    active_scanout = direct_pixels;
    destroy_during_flush = handle;
    shm_flush_surface(handle);
    CHECK(flushes == 1 && !shm_get_active_count());
    for (unsigned i = 0; i < 4; i++) CHECK(direct_pixels[i] == 0x10203040 + i);
    shm_flush_surface(handle);
    CHECK(flushes == 1);
}

static void *destroy_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < LOOPS; i++) {
        shm_destroy(shared_handle);
        shm_cleanup_process(42);
    }
    return NULL;
}

static void cleanup_race(void)
{
    pthread_t threads[WORKERS];
    shared_handle = shm_create_surface(4, 4, 3);
    uint32_t *anchor = shm_map(shared_handle);
    CHECK(anchor);
    if (!anchor) return;
    anchor[0] = 0x11223344;
    shm_set_owner(shared_handle, 42);
    for (unsigned i = 0; i < WORKERS; i++)
        CHECK(!pthread_create(&threads[i], NULL, i & 1 ? destroy_worker : mapping_worker, NULL));
    for (unsigned i = 0; i < WORKERS; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(shm_get_size(shared_handle) == 4096 && anchor[0] == 0x11223344);
    shm_unmap(shared_handle);
    CHECK(!shm_get_active_count());
}

static void limits_contract(void)
{
    uint32_t first = shm_create(4096, 3);
    /* Simulate counter wrap/collision without billions of allocations. */
    shm_next_handle = first;
    uint32_t second = shm_create(4096, 3);
    CHECK(first && second && first != second);
    shm_next_handle = UINT32_MAX;
    uint32_t last = shm_create(4096, 3), wrapped = shm_create(4096, 3);
    CHECK(last == UINT32_MAX && wrapped && wrapped != first && wrapped != second);
    shm_region_t *region = shm_find(first); /* Single-threaded boundary fixture. */
    CHECK(region);
    if (region) {
        region->refcount = 0x7fffffff;
        CHECK(!shm_map(first));
        CHECK(region->refcount == 0x7fffffff);
        region->refcount = 1;
    }
    shm_destroy(first); shm_destroy(second); shm_destroy(last); shm_destroy(wrapped);
    CHECK(!shm_get_active_count());
    uint32_t handle = shm_create_surface(2, 2, 3);
    active_scanout = NULL;
    destroy_during_flush = handle;
    shm_flush_surface(handle);
    CHECK(!shm_get_active_count() && flushes == 1);
}

int main(int argc, char **argv)
{
    (void)argv;
    shm_init();
    probe_allocator = true;
    reference_contract();
    simultaneous_creators();
    if (argc == 1) {
        capacity_and_cleanup();
        concurrency_contract();
        cleanup_race();
        flush_contract();
        limits_contract();
    }
    CHECK(allocation_count == free_count);
    /* Reclaim orphaned host allocations after reporting a baseline failure. */
    for (unsigned i = 0; i < BLOCKS; i++) if (allocations[i].base) free(allocations[i].base);
    printf("SHM lifetime: %u checks, %u failures; allocated=%u freed=%u\n",
           checks, failures, allocation_count, free_count);
    return failures ? 1 : 0;
}
