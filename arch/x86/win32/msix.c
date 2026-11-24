/*
 * OsitoK Win32 Layer — MSIX / APPX installer. See msix.h.
 */

#include "msix.h"
#include "zip.h"
#include "installer.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

static uint32_t mstrlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* Find `needle` in the first `hl` bytes of `h`; returns index or -1. */
static int substr(const char *h, uint32_t hl, const char *needle)
{
    uint32_t nl = mstrlen(needle);
    if (nl == 0 || nl > hl) return -1;
    for (uint32_t i = 0; i + nl <= hl; i++) {
        uint32_t j = 0;
        while (j < nl && h[i + j] == needle[j]) j++;
        if (j == nl) return (int)i;
    }
    return -1;
}

/* Grab an attribute value: find `attr` (e.g. `Name="`) at or after `from`,
 * then copy up to the next '"'. */
static void grab_attr(const char *h, uint32_t hl, uint32_t from,
                      const char *attr, char *out, int cap)
{
    out[0] = 0;
    if (from >= hl) return;
    int rel = substr(h + from, hl - from, attr);
    if (rel < 0) return;
    uint32_t p = from + (uint32_t)rel + mstrlen(attr);
    int k = 0;
    while (p < hl && h[p] != '"' && k < cap - 1) out[k++] = h[p++];
    out[k] = 0;
}

static int is_skipped(const char *name)
{
    /* package metadata, not payload */
    const char *skips[] = {
        "[Content_Types].xml", "AppxBlockMap.xml", "AppxSignature.p7x",
        "AppxManifest.xml", 0
    };
    for (int i = 0; skips[i]; i++) {
        const char *a = name, *b = skips[i];
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return 1;
    }
    /* AppxMetadata/ prefix */
    const char *pre = "AppxMetadata/";
    const char *a = name, *b = pre;
    while (*b && *a == *b) { a++; b++; }
    if (*b == 0) return 1;
    return 0;
}

int msix_install(const uint8_t *data, uint32_t len, const char *pkg_name)
{
    zip_archive_t *z = zip_open(data, len);
    if (!z) { serial_puts("[MSIX] not a valid zip/OPC container\n"); return -1; }

    char appname[128] = "", appver[64] = "", appexe[160] = "";
    uint32_t msz = 0;
    uint8_t *manifest = zip_extract(z, "AppxManifest.xml", &msz);
    if (manifest) {
        const char *m = (const char *)manifest;
        int idpos = substr(m, msz, "<Identity");
        if (idpos >= 0) {
            grab_attr(m, msz, (uint32_t)idpos, "Name=\"",    appname, sizeof appname);
            grab_attr(m, msz, (uint32_t)idpos, "Version=\"", appver,  sizeof appver);
        }
        int apos = substr(m, msz, "<Application");
        if (apos >= 0)
            grab_attr(m, msz, (uint32_t)apos, "Executable=\"", appexe, sizeof appexe);
        kfree(manifest);
    } else {
        serial_puts("[MSIX] no AppxManifest.xml\n");
    }

    const char *instname = appname[0] ? appname : (pkg_name ? pkg_name : "app");
    installer_manifest_begin(instname);

    int count = zip_entry_count(z), written = 0;
    for (int i = 0; i < count; i++) {
        char name[260]; uint32_t us = 0;
        if (!zip_enum(z, i, name, sizeof name, &us)) continue;

        uint32_t nl = mstrlen(name);
        if (nl == 0 || name[nl - 1] == '/') continue;   /* directory entry */
        if (is_skipped(name)) continue;

        uint32_t bs = 0;
        uint8_t *b = zip_extract(z, name, &bs);
        if (!b) continue;

        /* target = "Program Files\<instname>\<name with / -> \> */
        char target[400];
        int pos = 0;
        const char *base = "Program Files\\";
        for (const char *p = base; *p && pos < 399; p++) target[pos++] = *p;
        for (const char *p = instname; *p && pos < 399; p++) target[pos++] = *p;
        if (pos < 399) target[pos++] = '\\';
        for (const char *p = name; *p && pos < 399; p++)
            target[pos++] = (*p == '/') ? '\\' : *p;
        target[pos] = 0;

        serial_puts("[MSIX] install "); serial_puts(target);
        serial_puts(" ("); serial_putdec(bs); serial_puts(" bytes)\n");
        if (installer_write_file(target, b, bs) == 0) {
            installer_manifest_add(target);
            written++;
        }
        kfree(b);
    }

    installer_manifest_commit();
    zip_close(z);

    serial_puts("[MSIX] package '"); serial_puts(appname[0] ? appname : "?");
    serial_puts("' v"); serial_puts(appver[0] ? appver : "?");
    serial_puts(" exe="); serial_puts(appexe[0] ? appexe : "?");
    serial_puts(" — "); serial_putdec((uint64_t)written); serial_puts(" files\n");
    return written > 0 ? 0 : -1;
}
