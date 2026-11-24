/*
 * OsitoK Win32 Layer — package installer dispatch + helpers. See installer.h.
 */

#include "installer.h"
#include "msi.h"
#include "../fs/vfs.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* OsitoFS v2 (flat namespace). */
extern void    *osfs2_find(const char *name);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int      osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern void    *osfs2_create(const char *name, uint64_t size);
extern int      osfs2_delete(const char *name);
extern uint64_t osfs2_file_size(void *file);
extern int      osfs2_is_mounted(void);

/* MSIX engine (zip.c/msix.c). */
extern int msix_install(const uint8_t *data, uint32_t len, const char *pkg_name);

/* ── file writing ────────────────────────────────────────────── */

int installer_write_file(const char *win_path, const uint8_t *data, uint32_t len)
{
    const char *name = vfs_resolve(win_path, VFS_MODE_WIN32);
    if (!name || VFS_IS_VIRTUAL(name)) name = win_path;
    while (*name == '\\' || *name == '/') name++;
    if (!*name) return -1;

    if (osfs2_find(name)) osfs2_delete(name);   /* create rejects duplicates */

    void *f = osfs2_create(name, len ? len : 1);
    if (!f) {
        serial_puts("[INST] create failed: "); serial_puts(name); serial_puts("\n");
        return -1;
    }
    if (len && osfs2_write(f, 0, data, len) != 0) {
        serial_puts("[INST] write failed: "); serial_puts(name); serial_puts("\n");
        return -1;
    }
    return 0;
}

/* ── uninstall manifest ──────────────────────────────────────── */

#define MANIFEST_CAP  (32 * 1024)
static char     manifest_buf[MANIFEST_CAP];
static uint32_t manifest_len;
static char     manifest_pkg[64];

void installer_manifest_begin(const char *pkg_name)
{
    manifest_len = 0;
    int i = 0;
    if (pkg_name)
        for (; pkg_name[i] && i < 63; i++) manifest_pkg[i] = pkg_name[i];
    if (i == 0) { manifest_pkg[i++] = 'p'; }
    manifest_pkg[i] = 0;
}

void installer_manifest_add(const char *osfs_name)
{
    const char *resolved = vfs_resolve(osfs_name, VFS_MODE_WIN32);
    if (!resolved || VFS_IS_VIRTUAL(resolved)) resolved = osfs_name;
    while (*resolved == '\\' || *resolved == '/') resolved++;
    for (const char *p = resolved; *p && manifest_len < MANIFEST_CAP - 2; p++)
        manifest_buf[manifest_len++] = *p;
    if (manifest_len < MANIFEST_CAP - 1) manifest_buf[manifest_len++] = '\n';
}

static void manifest_filename(const char *pkg, char *out, int cap)
{
    int n = 0;
    for (const char *p = pkg; *p && n < cap - 16; p++) out[n++] = *p;
    const char *ext = ".osito-install";
    for (const char *p = ext; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}

void installer_manifest_commit(void)
{
    if (manifest_len == 0) return;

    char fname[80];
    manifest_filename(manifest_pkg, fname, sizeof fname);

    if (osfs2_find(fname)) osfs2_delete(fname);
    void *f = osfs2_create(fname, manifest_len);
    if (f) osfs2_write(f, 0, manifest_buf, manifest_len);
    serial_puts("[INST] manifest written: "); serial_puts(fname); serial_puts("\n");
}

int installer_uninstall(const char *pkg)
{
    if (!osfs2_is_mounted() || !pkg) return -1;

    char fname[80];
    manifest_filename(pkg, fname, sizeof fname);
    void *f = osfs2_find(fname);
    if (!f) { serial_puts("[INST] no manifest: "); serial_puts(fname); serial_puts("\n"); return -1; }

    uint64_t sz = osfs2_file_size(f);
    char *buf = (char *)kmalloc(sz + 1);
    if (!buf) return -1;
    if (osfs2_read(f, 0, buf, sz) < 0) { kfree(buf); return -1; }
    buf[sz] = 0;

    int removed = 0;
    uint64_t i = 0;
    while (i < sz) {
        uint64_t s = i;
        while (i < sz && buf[i] != '\n') i++;
        buf[i] = 0;
        if (buf[s] && osfs2_find(buf + s)) {
            osfs2_delete(buf + s);
            serial_puts("[INST] rm "); serial_puts(buf + s); serial_puts("\n");
            removed++;
        }
        i++;
    }
    kfree(buf);
    osfs2_delete(fname);
    serial_puts("[INST] uninstalled "); serial_putdec((uint64_t)removed); serial_puts(" files\n");
    return removed;
}

/* ── dispatch ────────────────────────────────────────────────── */

int installer_run_buffer(const uint8_t *data, uint32_t len, const char *pkg_name)
{
    if (!data || len < 8) { serial_puts("[INST] package too small\n"); return -1; }

    /* MSI = OLE2 compound file (D0 CF 11 E0 A1 B1 1A E1). */
    if (data[0] == 0xD0 && data[1] == 0xCF && data[2] == 0x11 && data[3] == 0xE0) {
        msi_db_t *db = msi_open(data, len);
        if (!db) { serial_puts("[INST] MSI open failed\n"); return -1; }
        int rc = msi_install(db, pkg_name);
        msi_close(db);
        return (rc < 0) ? -1 : 0;
    }

    /* MSIX = ZIP/OPC (PK\x03\x04 local header, or PK\x05\x06 empty EOCD). */
    if (data[0] == 'P' && data[1] == 'K' &&
        (data[2] == 0x03 || data[2] == 0x05) &&
        (data[3] == 0x04 || data[3] == 0x06)) {
        return msix_install(data, len, pkg_name);
    }

    serial_puts("[INST] unknown package format\n");
    return -1;
}

int installer_run_file(const char *filename)
{
    if (!osfs2_is_mounted()) { serial_puts("[INST] no filesystem\n"); return -1; }
    void *f = osfs2_find(filename);
    if (!f) {
        serial_puts("[INST] file not found: "); serial_puts(filename); serial_puts("\n");
        return -1;
    }
    uint64_t sz = osfs2_file_size(f);
    if (!sz) { serial_puts("[INST] empty file\n"); return -1; }

    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) return -1;
    if (osfs2_read(f, 0, buf, sz) < 0) { kfree(buf); return -1; }

    char pkg[64];
    {
        const char *base = filename;
        for (const char *p = filename; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        int i = 0;
        for (; base[i] && base[i] != '.' && i < 63; i++) pkg[i] = base[i];
        pkg[i] = 0;
    }

    int rc = installer_run_buffer(buf, (uint32_t)sz, pkg);
    kfree(buf);
    return rc;
}
