/* Data-only MSI parser oracle. No package or custom action is executed and
 * every installer write is intercepted. Compile with the host type override
 * used by the other freestanding parser tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define OSITOK_X86_TYPES_H
#include "../win32/msi.h"
#include "../win32/cab.h"
#include "../win32/advapi32_shim.h"

static unsigned allocations, checks, writes, registry, commits;
static int fail_after = -1;
#define CHECK(c) check((c), #c, __LINE__)
static void check(int condition, const char *expression, int line)
{
    checks++;
    if (!condition) { fprintf(stderr, "FAIL %d: %s\n", line, expression); exit(1); }
}
void *kmalloc(uint64_t size)
{
    if (size > 32U * 1024 * 1024 || fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(size ? (size_t)size : 1);
    if (p) allocations++;
    return p;
}
void kfree(void *p) { if (p) { CHECK(allocations != 0); allocations--; free(p); } }
void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t v, int d) { (void)v; (void)d; }
void serial_putdec(uint64_t v) { (void)v; }
#ifndef MSI_DATABASE_REAL_CAB
cab_archive_t *cab_open(const uint8_t *p, uint32_t n) { (void)p; (void)n; return NULL; }
void cab_close(cab_archive_t *c) { (void)c; abort(); }
uint8_t *cab_extract(cab_archive_t *c, const char *name, uint32_t *n)
{ (void)c; (void)name; (void)n; abort(); }
#endif
int installer_write_file(const char *s, const uint8_t *p, uint32_t n)
{ (void)s; (void)p; (void)n; writes++; return -1; }
void installer_manifest_begin(const char *s) { (void)s; }
void installer_manifest_add(const char *s) { (void)s; }
void installer_manifest_commit(void) { commits++; }
void advapi32_reg_install_set(const char *p, const char *n, DWORD t, const void *v, DWORD l)
{ (void)p; (void)n; (void)t; (void)v; (void)l; registry++; }
void advapi32_registry_flush(void) { }

static void hex(const uint8_t *p, uint32_t size)
{ for (uint32_t i = 0; i < size; i++) printf("%02x", p[i]); }
static void inspect(const uint8_t *data, uint32_t size, int dump)
{
    msi_db_t *db = msi_open(data, size);
    if (!db) { CHECK(!dump); return; }
    if (dump) printf("CODEPAGE\t%u\n", msi_codepage(db));
    CHECK(msi_table_name(db, msi_table_count(db)) == NULL);
    CHECK(msi_table_open(db, "not-an-msi-table") == NULL);
    for (uint32_t t = 0; t < msi_table_count(db); t++) {
        const char *name = msi_table_name(db, t);
        msi_table_view_t *table = msi_table_open(db, name);
        if (!table) { CHECK(!dump); continue; }
        uint32_t rows = msi_table_rows(table), columns = msi_table_columns(table);
        if (dump) printf("TABLE\t%s\t%u\t%u\n", name, columns, rows);
        CHECK(msi_table_column_name(table, columns) == NULL);
        CHECK(msi_table_column(table, NULL) == -1);
        msi_value_t value;
        CHECK(!msi_table_value(table, rows, 0, &value));
        CHECK(!msi_table_value(table, 0, columns, &value));
        CHECK(!msi_table_value(table, 0, 0, NULL));
        for (uint32_t c = 0; c < columns; c++) {
            const char *col = msi_table_column_name(table, c);
            CHECK(msi_table_column(table, col) == (int)c);
            if (dump) printf("COLUMN\t%s\t%u\t%s\t%04x\n", name, c, col, msi_table_column_type(table, c));
        }
        for (uint32_t r = 0; r < rows; r++) for (uint32_t c = 0; c < columns; c++) {
            if (!msi_table_value(table, r, c, &value)) { CHECK(!dump); continue; }
            if (dump) printf("CELL\t%s\t%u\t%s\t", name, r, msi_table_column_name(table, c));
            if (value.kind == MSI_VALUE_NULL) { if (dump) printf("null"); }
            else if (value.kind == MSI_VALUE_INTEGER) { if (dump) printf("integer\t%d", value.integer); }
            else if (value.kind == MSI_VALUE_STRING) {
                if (dump) { printf("string\t"); hex((const uint8_t *)value.string, (uint32_t)strlen(value.string)); }
            } else {
                uint32_t bytes = 0;
                uint8_t *binary = msi_table_read_binary(table, r, c, &bytes);
                CHECK(binary != NULL || !dump);
                if (dump) { printf("binary\t"); if (binary) hex(binary, bytes); }
                kfree(binary);
            }
            if (dump) putchar('\n');
        }
        msi_table_close(table);
    }
    msi_close(db);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2 && argc <= 3);
    FILE *f = fopen(argv[1], "rb"); CHECK(f != NULL);
    CHECK(!fseek(f, 0, SEEK_END)); long length = ftell(f); rewind(f);
    CHECK(length > 0 && length < 16 * 1024 * 1024);
    uint8_t *data = malloc((size_t)length); CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)length, f) == (size_t)length); fclose(f);
    inspect(data, (uint32_t)length, 1); CHECK(allocations == 0);
    if (argc == 3 && !strcmp(argv[2], "install-fails")) {
        msi_db_t *db = msi_open(data, (uint32_t)length); CHECK(db != NULL);
        int result = msi_install(db, "audit");
        printf("INSTALL result=%d writes=%u registry=%u commits=%u\n", result, writes, registry, commits);
        CHECK(result < 0); CHECK(writes == 0); CHECK(registry == 0); CHECK(commits == 0);
        msi_close(db); CHECK(allocations == 0);
    }
    if (argc == 3 && !strcmp(argv[2], "mutate")) {
        /* Every truncated prefix has its own allocation for ASan bounds. */
        for (uint32_t cut = 0; cut < (uint32_t)length; cut += cut < 600 ? 1 : 509) {
            uint8_t *copy = malloc(cut ? cut : 1); CHECK(copy != NULL);
            memcpy(copy, data, cut); inspect(copy, cut, 0); free(copy); CHECK(allocations == 0);
        }
        uint32_t random = 0x4712face;
        for (unsigned i = 0; i < 1600; i++) {
            random = random * 1664525U + 1013904223U;
            uint32_t at = i < 512 ? i : random % (uint32_t)length;
            uint8_t saved = data[at]; data[at] ^= (uint8_t)(1U << (i & 7));
            inspect(data, (uint32_t)length, 0); data[at] = saved; CHECK(allocations == 0);
        }
        for (int i = 0; i < 150; i++) {
            fail_after = i; inspect(data, (uint32_t)length, 0); CHECK(allocations == 0);
        }
        fail_after = -1;
    }
    free(data); printf("CHECKS %u allocations=%u\n", checks, allocations);
    return 0;
}
