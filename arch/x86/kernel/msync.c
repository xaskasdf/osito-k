/*
 * OsitoK x86-64 — msync + MAP_SHARED Coherence
 *
 * Writeback support for memory-mapped files.
 * MAP_SHARED pages are tracked; msync() flushes dirty pages to disk.
 * Enables memory-mapped I/O for databases (SQLite) and IPC.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* OsitoFS write */
extern int osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len)
    __attribute__((weak));

/* ── Shared Mapping Tracking ─────────────────────────────────── */

#define MSYNC_MAX_MAPS 32

typedef struct {
    bool     active;
    uint64_t vaddr;          /* Virtual address of mapping */
    uint64_t length;         /* Mapping length */
    void    *file;           /* File handle (osfs2_file_t *) */
    uint64_t file_offset;    /* Offset in file */
    uint8_t  fd_type;        /* FD_TYPE_FILE, FD_TYPE_TMPFS, etc. */
    bool     dirty;          /* Has been written to */
} shared_map_t;

static shared_map_t shared_maps[MSYNC_MAX_MAPS];

/* Forward declaration */
int msync_flush(uint64_t addr, uint64_t length, int flags);

/* ── Registration ────────────────────────────────────────────── */

/* Called when mmap(MAP_SHARED) creates a file-backed mapping */
int msync_register(uint64_t vaddr, uint64_t length, void *file,
                   uint64_t file_offset, uint8_t fd_type)
{
    for (int i = 0; i < MSYNC_MAX_MAPS; i++) {
        if (!shared_maps[i].active) {
            shared_maps[i].active = true;
            shared_maps[i].vaddr = vaddr;
            shared_maps[i].length = length;
            shared_maps[i].file = file;
            shared_maps[i].file_offset = file_offset;
            shared_maps[i].fd_type = fd_type;
            shared_maps[i].dirty = false;
            return i;
        }
    }
    return -1;
}

/* Called when the mapping is unmapped */
void msync_unregister(uint64_t vaddr)
{
    for (int i = 0; i < MSYNC_MAX_MAPS; i++) {
        if (shared_maps[i].active && shared_maps[i].vaddr == vaddr) {
            /* Flush before unmap if dirty */
            if (shared_maps[i].dirty)
                msync_flush(vaddr, shared_maps[i].length, 0);
            shared_maps[i].active = false;
            return;
        }
    }
}

/* ── Mark Dirty (called from page fault handler on write) ────── */

void msync_mark_dirty(uint64_t fault_addr)
{
    for (int i = 0; i < MSYNC_MAX_MAPS; i++) {
        if (shared_maps[i].active &&
            fault_addr >= shared_maps[i].vaddr &&
            fault_addr < shared_maps[i].vaddr + shared_maps[i].length) {
            shared_maps[i].dirty = true;
            return;
        }
    }
}

/* ── Flush (msync syscall) ───────────────────────────────────── */

#define MS_ASYNC   1
#define MS_SYNC    4
#define MS_INVALIDATE 2

int msync_flush(uint64_t addr, uint64_t length, int flags)
{
    (void)flags;

    for (int i = 0; i < MSYNC_MAX_MAPS; i++) {
        if (!shared_maps[i].active) continue;
        if (addr < shared_maps[i].vaddr + shared_maps[i].length &&
            addr + length > shared_maps[i].vaddr) {
            /* Overlapping region — write back to file */
            shared_map_t *m = &shared_maps[i];

            if (m->fd_type == 2 /* FD_TYPE_FILE */ && osfs2_write) {
                uint64_t write_off = m->file_offset;
                uint64_t write_len = m->length;
                if (write_len > length) write_len = length;

                int ret = osfs2_write(m->file, write_off,
                                      (const void *)m->vaddr, write_len);
                if (ret >= 0) {
                    m->dirty = false;
                    serial_puts("[MSYNC] Flushed ");
                    serial_putdec(write_len);
                    serial_puts(" bytes to file\n");
                }
                return ret;
            }

            /* tmpfs: already in memory, nothing to flush */
            m->dirty = false;
            return 0;
        }
    }
    return 0;  /* No matching mapping — success (POSIX allows this) */
}

/* ── Info ─────────────────────────────────────────────────────── */

void msync_list(void)
{
    serial_puts("[MSYNC] Shared mappings:\n");
    int n = 0;
    for (int i = 0; i < MSYNC_MAX_MAPS; i++) {
        if (!shared_maps[i].active) continue;
        serial_puts("  0x");
        serial_puthex(shared_maps[i].vaddr, 12);
        serial_puts(" len=");
        serial_putdec(shared_maps[i].length);
        serial_puts(shared_maps[i].dirty ? " DIRTY" : " clean");
        serial_puts("\n");
        n++;
    }
    if (n == 0) serial_puts("  (none)\n");
}
