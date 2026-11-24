/*
 * Host-side validation harness for the OsitoK MSI/MSIX installer engine.
 * Links the real ole2/msi/cab/zip/msix/zlib parsers against libc stubs for
 * kmalloc/serial/installer/advapi32, so we can exercise them on a real
 * test.msi / test.msix without booting QEMU.
 *
 * Build (mingw clang):
 *   clang -O1 -w -o host_test.exe \
 *     scripts/host_test_installer.c win32/ole2.c win32/cab.c win32/msi.c \
 *     win32/zip.c win32/msix.c kernel/zlib.c
 */

#include "../win32/msi.h"
#include "../win32/msix.h"
#include <stdio.h>
#include <stdlib.h>

/* ── stubs the parsers expect ─────────────────────────────────── */

void serial_puts(const char *s) { fputs(s, stdout); }
void serial_puthex(uint64_t v, int d) { (void)d; printf("%llx", (unsigned long long)v); }
void serial_putdec(uint64_t v) { printf("%llu", (unsigned long long)v); }
void *kmalloc(uint64_t s) { return malloc(s ? s : 1); }
void kfree(void *p) { free(p); }

int installer_write_file(const char *win_path, const uint8_t *data, uint32_t len)
{
    printf("  [WRITE] %s (%u bytes)\n", win_path, len);
    char fn[600]; int j = 0;
    const char *pre = "out/";
    for (const char *p = pre; *p; p++) fn[j++] = *p;
    for (const char *p = win_path; *p && j < 590; p++)
        fn[j++] = (*p == '\\' || *p == '/') ? '_' : *p;
    fn[j] = 0;
    FILE *f = fopen(fn, "wb");
    if (f) { if (len) fwrite(data, 1, len, f); fclose(f); }
    return 0;
}
void installer_manifest_begin(const char *p) { printf("  [MANIFEST begin] %s\n", p ? p : ""); }
void installer_manifest_add(const char *n)   { printf("  [MANIFEST +] %s\n", n); }
void installer_manifest_commit(void)         { printf("  [MANIFEST commit]\n"); }

void advapi32_reg_install_set(const char *path, const char *name,
                              uint32_t type, const void *data, uint32_t len)
{
    (void)len;
    printf("  [REG] %s : \"%s\" = ", path, name);
    if (type == 4) printf("(dword) %u\n", *(const unsigned *)data);
    else           printf("(sz) \"%s\"\n", (const char *)data);
}

/* ── main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <pkg>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    printf("== %s (%ld bytes) ==\n", argv[1], sz);
    if (buf[0] == 0xD0 && buf[1] == 0xCF) {
        msi_db_t *db = msi_open(buf, (uint32_t)sz);
        if (!db) { printf("msi_open FAILED\n"); return 1; }
        int n = msi_install(db, "test");
        msi_close(db);
        printf("== msi_install -> %d files ==\n", n);
    } else if (buf[0] == 'P' && buf[1] == 'K') {
        int rc = msix_install(buf, (uint32_t)sz, "test");
        printf("== msix_install -> %d ==\n", rc);
    } else {
        printf("unknown format\n");
    }
    free(buf);
    return 0;
}
