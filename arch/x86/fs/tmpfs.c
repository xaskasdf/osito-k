/*
 * OsitoK x86-64 — tmpfs (RAM-based filesystem)
 *
 * In-memory filesystem for temporary files (/tmp).
 * No persistence — all data lost on reboot.
 * Used for pipe outputs, build artifacts, scratch space.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* ── Configuration ───────────────────────────────────────────── */

#define TMPFS_MAX_FILES   128
#define TMPFS_MAX_NAME    64
#define TMPFS_MAX_SIZE    (64ULL * 1024 * 1024)  /* 64MB total limit */

/* ── File entry ──────────────────────────────────────────────── */

typedef struct {
    char     name[TMPFS_MAX_NAME];
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;   /* allocated buffer size */
    bool     used;
} tmpfs_file_t;

static tmpfs_file_t tmpfs_files[TMPFS_MAX_FILES];
static uint64_t     tmpfs_total_used;
static bool         tmpfs_inited;

/* ── Init ────────────────────────────────────────────────────── */

void tmpfs_init(void)
{
    memset(tmpfs_files, 0, sizeof(tmpfs_files));
    tmpfs_total_used = 0;
    tmpfs_inited = true;
    serial_puts("[TMPFS] Initialized (max ");
    serial_putdec(TMPFS_MAX_FILES);
    serial_puts(" files, ");
    serial_putdec(TMPFS_MAX_SIZE / (1024 * 1024));
    serial_puts(" MB)\n");
}

/* ── Find file by name ───────────────────────────────────────── */

static tmpfs_file_t *tmpfs_lookup(const char *name)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (tmpfs_files[i].used) {
            const char *a = tmpfs_files[i].name, *b = name;
            bool match = true;
            while (*a && *b) {
                if (*a != *b) { match = false; break; }
                a++; b++;
            }
            if (match && *a == *b) return &tmpfs_files[i];
        }
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

/* Create or truncate a file. Returns handle (pointer) or NULL. */
void *tmpfs_create(const char *name)
{
    if (!tmpfs_inited) return NULL;

    /* Check if exists — truncate */
    tmpfs_file_t *f = tmpfs_lookup(name);
    if (f) {
        if (f->data) { tmpfs_total_used -= f->capacity; kfree(f->data); }
        f->data = NULL;
        f->size = 0;
        f->capacity = 0;
        return f;
    }

    /* Find free slot */
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!tmpfs_files[i].used) {
            f = &tmpfs_files[i];
            memset(f, 0, sizeof(*f));
            int j = 0;
            while (name[j] && j < TMPFS_MAX_NAME - 1) { f->name[j] = name[j]; j++; }
            f->name[j] = '\0';
            f->used = true;
            return f;
        }
    }
    return NULL;
}

/* Open existing file. Returns handle or NULL. */
void *tmpfs_open(const char *name)
{
    if (!tmpfs_inited) return NULL;
    return tmpfs_lookup(name);
}

/* Read from file at offset. Returns bytes read. */
int tmpfs_read(void *handle, uint64_t offset, void *buf, uint64_t len)
{
    tmpfs_file_t *f = (tmpfs_file_t *)handle;
    if (!f || !f->used) return -1;
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = f->size - offset;
    memcpy(buf, f->data + offset, len);
    return (int)len;
}

/* Write to file at offset. Grows file if needed. Returns bytes written. */
int tmpfs_write(void *handle, uint64_t offset, const void *buf, uint64_t len)
{
    tmpfs_file_t *f = (tmpfs_file_t *)handle;
    if (!f || !f->used) return -1;

    uint64_t end = offset + len;

    /* Grow buffer if needed */
    if (end > f->capacity) {
        uint64_t new_cap = end < 4096 ? 4096 : end * 2;
        if (tmpfs_total_used + (new_cap - f->capacity) > TMPFS_MAX_SIZE)
            return -1;  /* Out of tmpfs space */
        uint8_t *new_buf = (uint8_t *)kmalloc(new_cap);
        if (!new_buf) return -1;
        if (f->data) {
            memcpy(new_buf, f->data, f->size);
            tmpfs_total_used -= f->capacity;
            kfree(f->data);
        }
        f->data = new_buf;
        f->capacity = new_cap;
        tmpfs_total_used += new_cap;
    }

    /* Zero-fill gap if writing past current end */
    if (offset > f->size)
        memset(f->data + f->size, 0, offset - f->size);

    memcpy(f->data + offset, buf, len);
    if (end > f->size) f->size = end;
    return (int)len;
}

/* Delete file */
int tmpfs_delete(const char *name)
{
    tmpfs_file_t *f = tmpfs_lookup(name);
    if (!f) return -1;
    if (f->data) { tmpfs_total_used -= f->capacity; kfree(f->data); }
    memset(f, 0, sizeof(*f));
    return 0;
}

/* Get file size. Returns -1 if not found. */
int64_t tmpfs_size(const char *name)
{
    tmpfs_file_t *f = tmpfs_lookup(name);
    if (!f) return -1;
    return (int64_t)f->size;
}

/* List all files */
void tmpfs_ls(void)
{
    serial_puts("[TMPFS] Files:\n");
    int count = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (tmpfs_files[i].used) {
            serial_puts("  ");
            serial_puts(tmpfs_files[i].name);
            serial_puts("  (");
            serial_putdec(tmpfs_files[i].size);
            serial_puts(" bytes)\n");
            count++;
        }
    }
    if (count == 0) serial_puts("  (empty)\n");
    serial_puts("[TMPFS] Total: ");
    serial_putdec(tmpfs_total_used / 1024);
    serial_puts(" KB used\n");
}

/* Check if tmpfs is initialized */
bool tmpfs_is_ready(void) { return tmpfs_inited; }

/* Get file count */
int tmpfs_file_count(void)
{
    int n = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        if (tmpfs_files[i].used) n++;
    return n;
}
