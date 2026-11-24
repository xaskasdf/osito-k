/* Deterministic cooperative contention while production hive I/O is suspended. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
/* BearSSL also includes libc string.h. Keep the freestanding inline libc
 * definitions out of this host translation unit. */
#define OSITOK_X86_TYPES_H
#define __KERNEL_X86__ 1
#include "../include/types.h"
#ifndef REGISTRY_TEST_SOURCE
#define REGISTRY_TEST_SOURCE "../win32/advapi32_shim.c"
#endif
#include REGISTRY_TEST_SOURCE

extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern void registry_context_run(void (*)(void), void (*)(void));
extern void registry_context_yield(void);

#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
static unsigned checks, failures, allocations, yields, writes, publishes;
static int pause_write, pause_read, fail_write, fail_publish, fail_allocate;
static BYTE stored[REG_STORE_MAX_SIZE], staging[REG_STORE_MAX_SIZE];
static uint64_t stored_size, staging_size;
static int compat32;
int *proc_win32_compat32_mode_slot(void) { return &compat32; }

void *kmalloc(uint64_t n)
{
    if (fail_allocate) return NULL;
    void *p = malloc(n);
    if (p) allocations++;
    return p;
}
void kfree(void *p) { if (p) { CHECK(allocations > 0); allocations--; free(p); } }
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return 1000; }
void sched_yield(void) { yields++; registry_context_yield(); }
bool osfs2_is_mounted(void) { return true; }
bool osfs3_is_mounted(void) { return true; }
bool osfs3_directory_exists_ci(const char *path) { (void)path; return true; }
int osfs3_mkdir(const char *path) { (void)path; return 0; }
void *osfs2_find(const char *path) { (void)path; return stored_size ? stored : NULL; }
uint64_t osfs2_file_size(void *file) { CHECK(file == stored); return stored_size; }
int osfs2_read(void *file, uint64_t off, void *buf, uint64_t n)
{
    CHECK(file == stored && !off && n == stored_size);
    if (pause_read) { pause_read = 0; registry_context_yield(); }
    memcpy(buf, stored, n);
    return (int)n;
}
void *osfs2_create(const char *path, uint64_t n)
{
    CHECK(!reg_stricmp(path, REG_STORE_TEMP) && n <= sizeof(staging));
    staging_size = n;
    return staging;
}
int osfs2_delete(const char *path)
{
    CHECK(!reg_stricmp(path, REG_STORE_TEMP));
    staging_size = 0;
    return 0;
}
int osfs2_write(void *file, uint64_t off, const void *buf, uint64_t n)
{
    CHECK(file == staging && !off && n == staging_size && reg_store_lock);
    CHECK(!reg_lock);
    writes++;
    if (pause_write) { pause_write = 0; registry_context_yield(); }
    if (fail_write) return -1;
    memcpy(staging, buf, n);
    return (int)n;
}
int osfs2_truncate(void *file, uint64_t n)
{
    CHECK(file == staging && n == staging_size);
    return 0;
}
int osfs2_rename(const char *from, const char *to, bool replace)
{
    CHECK(!reg_stricmp(from, REG_STORE_TEMP) && !reg_stricmp(to, REG_STORE_PATH) && replace);
    if (fail_publish) return -1;
    memcpy(stored, staging, staging_size);
    stored_size = staging_size;
    publishes++;
    return 0;
}

static void put_value(DWORD value)
{
    CHECK(reg_set_value_view("hkcu\\test", "sequence", REG_DWORD, &value,
                             sizeof(value), REG_VIEW_64) == ERROR_SUCCESS);
    reg_mark_dirty_locked();
}

static void first_writer(void)
{
    put_value(1);
    CHECK(reg_store_save());
    CHECK(reg_dirty_operations != 0); /* The waiting task already mutated the hive. */
}
static void second_writer(void)
{
    put_value(2);
    CHECK(reg_store_save());
}
static void initialize(void) { reg_init(); CHECK(reg_init_state == 2); }

int main(int argc, char **argv)
{
    (void)argv;
    /* Disabling nonexistent handle caches cannot initialize or block on I/O. */
    reg_store_lock = 1;
    for (unsigned i = 0; i < 256; i++) CHECK(RegDisablePredefinedCache() == ERROR_SUCCESS);
    CHECK(!reg_init_state && !reg_lock && !allocations && !writes && !yields);
    reg_store_lock = 0;
    reg_init();
    static const char directx_path[] = "hklm\\software\\microsoft\\directx";
    static const char directx_version[] = "4.09.00.0904";
    for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
        REG_VALUE *entry = reg_find_value(directx_path, "Version", view);
        CHECK(entry && entry->type == REG_SZ &&
              entry->data_len == sizeof(directx_version) &&
              !memcmp(entry->data, directx_version, sizeof(directx_version)));
    }
    CHECK(!writes); /* Initialization itself does not publish the hive. */
    if (argc > 1) {
        put_value(2);
        CHECK(reg_store_save());
    } else {
        pause_write = 1;
        registry_context_run(first_writer, second_writer);
        CHECK(writes == 2 && publishes == 2 && yields > 0);
    }
    CHECK(!reg_store_lock && !reg_lock && !allocations && !reg_dirty_operations);
    yields = 0; reg_init_state = 0; pause_read = 1;
    memset(reg_keys, 0, sizeof(reg_keys));
    memset(reg_values, 0, sizeof(reg_values));
    registry_context_run(initialize, initialize);
    CHECK(yields > 0 && reg_init_state == 2 && !allocations);
    DWORD value = 0;
    REG_VALUE *sequence = reg_find_value("hkcu\\test", "sequence", REG_VIEW_64);
    if (sequence) memcpy(&value, sequence->data, sizeof(value));
    CHECK(sequence && value == 2);
    for (int stage = 0; stage < 3; stage++) {
        put_value(3 + stage);
        fail_allocate = stage == 0; fail_write = stage == 1; fail_publish = stage == 2;
        CHECK(!reg_store_save());
        CHECK(reg_dirty_operations && !reg_store_lock && !reg_lock && !allocations);
        fail_allocate = fail_write = fail_publish = 0;
        CHECK(reg_store_save());
        CHECK(!reg_dirty_operations && !reg_store_lock && !allocations);
    }
    /* Installed runtimes exceed the former 128-key / 256-value hive limits.
     * Preserve and reload distinct data in both redirected views. */
    for (unsigned i = 0; i < 512; i++) {
        char path[96];
        snprintf(path, sizeof(path), "hklm\\software\\osito-capacity\\key%u", i);
        for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
            DWORD data = i | ((DWORD)view << 24);
            CHECK(reg_set_value_view(path, "sequence", REG_DWORD, &data,
                                    sizeof(data), view) == ERROR_SUCCESS);
        }
    }
    reg_mark_dirty_locked();
    CHECK(reg_store_save());
    const REG_STORE_HEADER *header = (const REG_STORE_HEADER *)stored;
    CHECK(header->version == 1 && header->key_count >= 1024 &&
          header->value_count >= 1024);
    reg_init_state = 0;
    memset(reg_keys, 0, sizeof(reg_keys));
    memset(reg_values, 0, sizeof(reg_values));
    reg_init();
    CHECK(reg_init_state == 2 && !reg_dirty_operations && !allocations);
    for (unsigned i = 0; i < 512; i++) {
        char path[96];
        snprintf(path, sizeof(path), "hklm\\software\\osito-capacity\\key%u", i);
        for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
            REG_VALUE *entry = reg_find_value(path, "sequence", view);
            DWORD data = 0;
            if (entry) memcpy(&data, entry->data, sizeof(data));
            CHECK(entry && entry->type == REG_DWORD &&
                  entry->data_len == sizeof(data) &&
                  data == (i | ((DWORD)view << 24)));
        }
    }
    /* A persisted override, including an empty value or a different type,
     * belongs to its caller. Fill only the other view's missing metadata. */
    DWORD override = 0x12345678;
    CHECK(reg_set_value_view(directx_path, "Version", REG_DWORD, &override,
                             sizeof(override), REG_VIEW_32) == ERROR_SUCCESS);
    REG_VALUE *version64 = reg_find_value(directx_path, "Version", REG_VIEW_64);
    if (version64) version64->used = 0;
    reg_mark_dirty_locked();
    CHECK(reg_store_save());
    reg_init_state = 0;
    reg_init();
    REG_VALUE *version32 = reg_find_value(directx_path, "Version", REG_VIEW_32);
    version64 = reg_find_value(directx_path, "Version", REG_VIEW_64);
    CHECK(version32 && version32->type == REG_DWORD &&
          version32->data_len == sizeof(override) &&
          !memcmp(version32->data, &override, sizeof(override)));
    CHECK(version64 && version64->type == REG_SZ &&
          version64->data_len == sizeof(directx_version) &&
          !memcmp(version64->data, directx_version, sizeof(directx_version)));
    CHECK(reg_dirty_operations != 0);
    CHECK(reg_set_value_view(directx_path, "Version", REG_SZ, NULL, 0,
                             REG_VIEW_64) == ERROR_SUCCESS);
    reg_mark_dirty_locked();
    CHECK(reg_store_save());
    reg_init_state = 0;
    reg_init();
    version64 = reg_find_value(directx_path, "Version", REG_VIEW_64);
    CHECK(version64 && version64->type == REG_SZ && !version64->data_len);
    CHECK(!reg_dirty_operations && !allocations);

    /* A legacy shared value must not be shadowed by per-view defaults. */
    version32 = reg_find_value(directx_path, "Version", REG_VIEW_32);
    if (version32) version32->used = 0;
    if (version64) version64->used = 0;
    static const char shared_version[] = "existing shared version";
    CHECK(reg_set_value_view(directx_path, "Version", REG_SZ, shared_version,
                             sizeof(shared_version), REG_VIEW_SHARED) == ERROR_SUCCESS);
    reg_mark_dirty_locked();
    CHECK(reg_store_save());
    reg_init_state = 0;
    reg_init();
    for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
        REG_VALUE *entry = reg_find_value(directx_path, "Version", view);
        CHECK(entry && entry->view == REG_VIEW_SHARED &&
              entry->data_len == sizeof(shared_version) &&
              !memcmp(entry->data, shared_version, sizeof(shared_version)));
    }
    CHECK(!reg_dirty_operations && !allocations);
    /* Preserve the input encoding across a hive round trip, including the
     * untagged bytes written by an older kernel. Disk record sizes stay v1. */
    static const BYTE encoding_data[] = { 'A', 0, 0x2D, 0x4E, 0, 0 };
    for (BYTE encoding = REG_DATA_LEGACY; encoding <= REG_DATA_UTF16; encoding++) {
        CHECK(reg_set_value_view("hkcu\\test", "encoding", REG_SZ,
                                 encoding_data, sizeof(encoding_data),
                                 REG_VIEW_64) == ERROR_SUCCESS);
        REG_VALUE *entry = reg_find_value("hkcu\\test", "encoding", REG_VIEW_64);
        CHECK(entry != NULL);
        if (entry) entry->encoding = encoding;
        reg_mark_dirty_locked();
        CHECK(reg_store_save());
        reg_init_state = 0;
        reg_init();
        entry = reg_find_value("hkcu\\test", "encoding", REG_VIEW_64);
        CHECK(entry && entry->encoding == encoding &&
              entry->data_len == sizeof(encoding_data) &&
              !memcmp(entry->data, encoding_data, sizeof(encoding_data)));
        CHECK(!reg_dirty_operations && !allocations);
    }
    printf("registry-store: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
