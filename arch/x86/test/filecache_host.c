#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OSITOK_X86_TYPES_H
#define FILECACHE_HOST_TEST
#include "../win32/filecache.c"

#define CHECK(x) do { __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED); if (!(x)) { \
    printf("FAIL %d: %s\n", __LINE__, #x); \
    __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); } } while (0)

static unsigned checks, failures;
static _Thread_local unsigned locked;
static _Thread_local uint64_t irq_flags = 0x202;
static struct {
    uint8_t bytes[4 * NT_FILE_CACHE_BYTES];
    uint64_t size, revision, calls, bytes_read;
    unsigned fail_ahead, fail_all, short_read, mutate_read, mutate_revision;
} disk;
static uint8_t buffer[2 * NT_FILE_CACHE_BYTES];
static FILE_OBJECT file;

static uint64_t read_cache_lock_irqsave(void)
{
    if (locked) abort();
    locked = 1;
    spin_lock(&g_read_cache_lock);
    uint64_t flags = irq_flags;
    irq_flags &= ~0x200ULL;
    return flags;
}

static void read_cache_unlock_irqrestore(uint64_t flags)
{
    if (!locked) abort();
    spin_unlock(&g_read_cache_lock);
    locked = 0;
    irq_flags = flags;
}

uint64_t osfs2_file_revision(void *identity)
{
    CHECK(identity == &disk && !locked);
    if (disk.mutate_revision && --disk.mutate_revision == 0) {
        disk.revision += 2;
        disk.bytes[0]++;
    }
    return disk.revision;
}

int osfs2_read(void *identity, uint64_t offset, void *dst, uint64_t length)
{
    CHECK(identity == &disk && !locked);
    __atomic_add_fetch(&disk.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&disk.bytes_read, length, __ATOMIC_RELAXED);
    if (disk.fail_all || (disk.fail_ahead && length > 2048) ||
        offset > disk.size || length > disk.size - offset)
        return -1;
    memcpy(dst, disk.bytes + offset, length);
    if (disk.mutate_read && --disk.mutate_read == 0) {
        disk.revision += 2;
        disk.bytes[0]++;
    }
    return (int)length - (disk.short_read ? 1 : 0);
}

static void reset(ULONG options)
{
    nt_file_cache_close(&file);
    memset(&disk, 0, sizeof(disk));
    disk.size = sizeof(disk.bytes);
    disk.revision = 2;
    for (unsigned i = 0; i < sizeof(disk.bytes); i++)
        disk.bytes[i] = (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
    memset(&file, 0, sizeof(file));
    file.flags = FILE_OBJ_DISK_FILE;
    file.osfs_file = &disk;
    file.create_options = options;
    nt_file_cache_open(&file);
}

static int read_at(uint64_t offset, unsigned length, unsigned small_buffer)
{
    unsigned capacity = nt_file_read_capacity(&file, offset, length, disk.size);
    CHECK(capacity >= length && capacity <= sizeof(buffer));
    if (small_buffer) capacity = length;
    uint64_t flags = irq_flags;
    memset(buffer, 0xcc, sizeof(buffer));
    int result = nt_file_cached_read(&file, offset, buffer, length, capacity,
                                     disk.size);
    CHECK(irq_flags == flags && !locked);
    if (result == (int)length)
        for (unsigned i = 0; i < length; i++)
            CHECK(buffer[i] == disk.bytes[offset + i]);
    return result;
}

static unsigned occupied(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < NT_FILE_CACHE_SLOTS; i++)
        count += g_read_windows[i].key.cookie != 0;
    return count;
}

static void *reader(void *argument)
{
    unsigned stream = (unsigned)(uintptr_t)argument;
    uint8_t *data = malloc(NT_FILE_CACHE_BYTES);
    CHECK(data != NULL);
    if (!data) return NULL;
    for (unsigned round = 0; round < 24; round++) {
        FILE_OBJECT own = {0};
        own.osfs_file = &disk;
        nt_file_cache_open(&own);
        for (unsigned i = 0; i < 64; i++) {
            unsigned off = ((i + stream * 8) % 128) * 2048;
            unsigned capacity = nt_file_read_capacity(&own, off, 2048, disk.size);
            CHECK(capacity >= 2048 && capacity <= NT_FILE_CACHE_BYTES);
            CHECK(nt_file_cached_read(&own, off, data, 2048, capacity, disk.size) == 2048);
            CHECK(!memcmp(data, disk.bytes + off, 2048));
            CHECK(!locked && irq_flags == 0x202);
        }
        nt_file_cache_close(&own);
    }
    free(data);
    return NULL;
}

int main(void)
{
    reset(0);
    for (unsigned off = 0; off < disk.size; off += 2048)
        CHECK(read_at(off, 2048, 0) == 2048);
    CHECK(disk.calls == 5 && disk.bytes_read == disk.size + 2048);
    CHECK(occupied() == 1);

    reset(FILE_SEQUENTIAL_ONLY);
    CHECK(nt_file_read_capacity(&file, 0, 2048, disk.size) == 65536);
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 1);
    CHECK(read_at(511, 12345, 0) == 12345 && disk.calls == 1);
    CHECK(read_at(65535, 2, 0) == 2 && disk.calls == 2);
    CHECK(read_at(65537, 4096, 0) == 4096 && disk.calls == 3);
    CHECK(occupied() == 1);

    /* Revision changes invalidate data from every writer, not just this open. */
    reset(FILE_SEQUENTIAL_ONLY);
    CHECK(read_at(0, 2048, 0) == 2048);
    disk.bytes[0] ^= 0x55;
    disk.revision += 2;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 2);
    disk.revision++;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 3);
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 4);
    disk.revision++;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 5);

    reset(FILE_SEQUENTIAL_ONLY);
    CHECK(read_at(0, 2048, 0) == 2048);
    disk.size = 513;
    disk.revision += 2;
    CHECK(read_at(0, 513, 0) == 513 && disk.calls == 2);
    CHECK(read_at(500, 13, 0) == 13 && disk.calls == 2);
    disk.size = sizeof(disk.bytes);
    disk.bytes[1000] = 0x99;
    disk.revision += 2;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 3);

    reset(FILE_NO_INTERMEDIATE_BUFFERING | FILE_SEQUENTIAL_ONLY);
    CHECK(nt_file_read_capacity(&file, 0, 2048, disk.size) == 2048);
    for (unsigned i = 0; i < 4; i++) CHECK(read_at(0, 2048, 0) == 2048);
    CHECK(disk.calls == 4 && !occupied());

    reset(FILE_RANDOM_ACCESS | FILE_SEQUENTIAL_ONLY);
    CHECK(read_at(0, 2048, 0) == 2048);
    CHECK(read_at(2048, 2048, 0) == 2048);
    CHECK(read_at(2048, 2048, 0) == 2048);
    CHECK(disk.calls == 2 && disk.bytes_read == 4096);

    /* Low-memory capacity falls back to ordinary I/O, without error or leak. */
    reset(FILE_SEQUENTIAL_ONLY);
    CHECK(read_at(0, 2048, 1) == 2048 && disk.bytes_read == 2048);
    CHECK(read_at(2048, 2048, 1) == 2048 && disk.bytes_read == 4096);
    CHECK(read_at(4096, 2048, 0) == 2048 && disk.bytes_read == 69632);

    reset(FILE_SEQUENTIAL_ONLY);
    disk.fail_ahead = 1;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 2);
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 2);
    disk.fail_all = 1;
    CHECK(read_at(2048, 2048, 0) == -1 && disk.calls == 4);
    CHECK(!file.read_cache.sequential);
    disk.fail_all = disk.fail_ahead = 0;
    disk.short_read = 1;
    CHECK(read_at(2048, 2048, 0) == 2047 && disk.calls == 6);
    disk.short_read = 0;
    CHECK(read_at(2048, 2048, 0) == 2048 && disk.calls == 7);

    /* A writer during a speculative fill or cache hit must not publish stale data. */
    reset(FILE_SEQUENTIAL_ONLY);
    disk.mutate_read = 1;
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 2);
    disk.mutate_revision = 3; /* capacity key, read key, post-copy check */
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 3);
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 3);

    reset(0);
    CHECK(read_at(0, 2 * NT_FILE_CACHE_BYTES, 0) == 2 * NT_FILE_CACHE_BYTES);
    CHECK(disk.calls == 1 && !occupied());
    CHECK(nt_file_cached_read(&file, UINT64_MAX - 8, buffer, 16, 16,
                              UINT64_MAX) == -1);
    CHECK(nt_file_cached_read(&file, 0, buffer, 2, 1, disk.size) == -1);
    CHECK(nt_file_cached_read(&file, 0, NULL, 2, 2, disk.size) == -1);
    CHECK(nt_file_cached_read(&file, 0, buffer, 0x80000000U, 0x80000000U,
                              UINT64_MAX) == -1);

    /* No per-open heap allocation, loading reservation or stale cookie survives. */
    reset(FILE_SEQUENTIAL_ONLY);
    CHECK(read_at(0, 2048, 0) == 2048);
    NT_READ_KEY old = { file.read_cache.cookie, &disk, disk.revision,
                        disk.size, 0, 2048 };
    nt_file_cache_close(&file);
    CHECK(!occupied());
    nt_file_cache_open(&file);
    CHECK(file.read_cache.cookie != old.cookie);
    read_cache_store(&old, buffer); /* Late publication from a retired open. */
    CHECK(read_at(0, 2048, 0) == 2048 && disk.calls == 2);
    nt_file_cache_close(&file);
    memset(g_read_windows, 0, sizeof(g_read_windows));

    FILE_OBJECT opens[NT_FILE_CACHE_SLOTS + 8] = {0};
    for (unsigned i = 0; i < NT_FILE_CACHE_SLOTS + 8; i++) {
        opens[i].osfs_file = &disk;
        nt_file_cache_open(&opens[i]);
        CHECK(nt_file_cached_read(&opens[i], 0, buffer, 1, 1, disk.size) == 1);
    }
    CHECK(occupied() == NT_FILE_CACHE_SLOTS);
    for (unsigned i = 0; i < NT_FILE_CACHE_SLOTS + 8; i++)
        nt_file_cache_close(&opens[i]);
    CHECK(!occupied());

    reset(0);
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; i++)
        CHECK(pthread_create(&workers[i], NULL, reader, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < 8; i++) CHECK(pthread_join(workers[i], NULL) == 0);
    CHECK(!occupied());

    reset(FILE_SEQUENTIAL_ONLY);
    irq_flags = 2;
    CHECK(read_at(0, 2048, 0) == 2048 && irq_flags == 2);
    CHECK(read_at(0, 2048, 0) == 2048 && irq_flags == 2);
    nt_file_cache_close(&file);
    CHECK(!occupied() && irq_flags == 2);
    g_read_cookie = UINT64_MAX;
    nt_file_cache_open(&file);
    CHECK(!file.read_cache.cookie);
    CHECK(read_at(0, 2048, 0) == 2048 && !occupied());
    printf("filecache: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
