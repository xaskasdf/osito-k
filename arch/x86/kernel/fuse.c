/*
 * OsitoK x86-64 — FUSE (Filesystem in Userspace)
 *
 * Allows userspace processes to implement filesystems.
 * Kernel forwards VFS operations to a FUSE daemon via /dev/fuse.
 * Enables sshfs, ntfs-3g, and custom filesystem implementations.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── FUSE Operations ─────────────────────────────────────────── */

#define FUSE_OP_LOOKUP    1
#define FUSE_OP_GETATTR   3
#define FUSE_OP_READ      4
#define FUSE_OP_WRITE     5
#define FUSE_OP_READDIR   6
#define FUSE_OP_OPEN      7
#define FUSE_OP_RELEASE   8
#define FUSE_OP_INIT     26

/* Request/response shared between kernel and userspace daemon */
typedef struct {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;       /* Request ID for matching response */
    uint64_t nodeid;       /* Inode number */
    uint32_t uid, gid, pid;
} fuse_in_header_t;

typedef struct {
    uint32_t len;
    int32_t  error;        /* 0 or -errno */
    uint64_t unique;
} fuse_out_header_t;

/* ── FUSE Mount State ────────────────────────────────────────── */

#define FUSE_MAX_MOUNTS 4
#define FUSE_QUEUE_SIZE 16

typedef struct {
    bool     active;
    char     mountpoint[64];
    uint32_t daemon_pid;
    /* Request queue: kernel → daemon */
    uint8_t  req_buf[FUSE_QUEUE_SIZE][256];
    uint32_t req_len[FUSE_QUEUE_SIZE];
    int      req_head, req_tail, req_count;
    /* Response queue: daemon → kernel */
    uint8_t  resp_buf[FUSE_QUEUE_SIZE][4096];
    uint32_t resp_len[FUSE_QUEUE_SIZE];
    int      resp_head, resp_tail, resp_count;
    uint64_t next_unique;
} fuse_mount_t;

static fuse_mount_t *fuse_mounts;  /* Lazy alloc (saves ~280KB BSS) */

static void fuse_ensure_init(void)
{
    if (fuse_mounts) return;
    extern void *kmalloc(uint64_t);
    fuse_mounts = (fuse_mount_t *)kmalloc(FUSE_MAX_MOUNTS * sizeof(fuse_mount_t));
    if (fuse_mounts) memset(fuse_mounts, 0, FUSE_MAX_MOUNTS * sizeof(fuse_mount_t));
}

/* ── Public API ──────────────────────────────────────────────── */

int fuse_register(const char *mountpoint, uint32_t daemon_pid)
{
    fuse_ensure_init();
    if (!fuse_mounts) return -1;
    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (!fuse_mounts[i].active) {
            fuse_mount_t *m = &fuse_mounts[i];
            memset(m, 0, sizeof(*m));
            m->active = true;
            m->daemon_pid = daemon_pid;
            m->next_unique = 1;
            int j = 0;
            while (mountpoint[j] && j < 63) { m->mountpoint[j] = mountpoint[j]; j++; }
            m->mountpoint[j] = '\0';
            serial_puts("[FUSE] Registered: ");
            serial_puts(mountpoint);
            serial_puts(" (daemon PID ");
            serial_putdec(daemon_pid);
            serial_puts(")\n");
            return i;
        }
    }
    return -1;
}

/* Kernel sends a request to the FUSE daemon */
int fuse_send_request(int mount_idx, uint32_t opcode, uint64_t nodeid,
                      const void *data, uint32_t data_len)
{
    if (mount_idx < 0 || mount_idx >= FUSE_MAX_MOUNTS) return -1;
    fuse_mount_t *m = &fuse_mounts[mount_idx];
    if (!m->active || m->req_count >= FUSE_QUEUE_SIZE) return -1;

    uint8_t *buf = m->req_buf[m->req_head];
    fuse_in_header_t *hdr = (fuse_in_header_t *)buf;
    hdr->opcode = opcode;
    hdr->unique = m->next_unique++;
    hdr->nodeid = nodeid;
    hdr->len = sizeof(fuse_in_header_t) + data_len;
    if (data_len > 0 && data)
        memcpy(buf + sizeof(fuse_in_header_t), data, data_len);
    m->req_len[m->req_head] = hdr->len;
    m->req_head = (m->req_head + 1) % FUSE_QUEUE_SIZE;
    m->req_count++;

    return (int)hdr->unique;
}

/* Daemon reads next pending request (via read(/dev/fuse)) */
int fuse_daemon_read(int mount_idx, void *buf, uint32_t max_len)
{
    if (mount_idx < 0 || mount_idx >= FUSE_MAX_MOUNTS) return -1;
    fuse_mount_t *m = &fuse_mounts[mount_idx];
    if (!m->active || m->req_count == 0) return 0;

    uint32_t len = m->req_len[m->req_tail];
    if (len > max_len) len = max_len;
    memcpy(buf, m->req_buf[m->req_tail], len);
    m->req_tail = (m->req_tail + 1) % FUSE_QUEUE_SIZE;
    m->req_count--;
    return (int)len;
}

/* Daemon writes response (via write(/dev/fuse)) */
int fuse_daemon_write(int mount_idx, const void *buf, uint32_t len)
{
    if (mount_idx < 0 || mount_idx >= FUSE_MAX_MOUNTS) return -1;
    fuse_mount_t *m = &fuse_mounts[mount_idx];
    if (!m->active || m->resp_count >= FUSE_QUEUE_SIZE) return -1;

    uint32_t copy = len < 4096 ? len : 4096;
    memcpy(m->resp_buf[m->resp_head], buf, copy);
    m->resp_len[m->resp_head] = copy;
    m->resp_head = (m->resp_head + 1) % FUSE_QUEUE_SIZE;
    m->resp_count++;
    return (int)copy;
}

/* Check if a path matches a FUSE mountpoint */
int fuse_match_path(const char *path)
{
    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (!fuse_mounts[i].active) continue;
        const char *mp = fuse_mounts[i].mountpoint;
        const char *p = path;
        bool match = true;
        while (*mp) {
            if (*mp != *p) { match = false; break; }
            mp++; p++;
        }
        if (match && (*p == '/' || *p == '\0')) return i;
    }
    return -1;
}

void fuse_list(void)
{
    serial_puts("[FUSE] Mounts:\n");
    int n = 0;
    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (!fuse_mounts[i].active) continue;
        serial_puts("  "); serial_puts(fuse_mounts[i].mountpoint);
        serial_puts(" (pid="); serial_putdec(fuse_mounts[i].daemon_pid);
        serial_puts(")\n");
        n++;
    }
    if (n == 0) serial_puts("  (none)\n");
}
