/*
 * OsitoK x86-64 — Pseudo-Terminal (PTY) Subsystem
 *
 * Implements /dev/ptmx (master) + /dev/pts/N (slave) pairs.
 * Required for ssh, screen, tmux, and terminal multiplexing.
 *
 * Usage:
 *   1. Process opens /dev/ptmx → gets master FD + pts number
 *   2. Process opens /dev/pts/N → gets slave FD
 *   3. Write to master → appears as input on slave (and vice versa)
 *   4. Slave acts as a terminal device (line discipline)
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── PTY Pair ────────────────────────────────────────────────── */

#define PTY_MAX_PAIRS  8
#define PTY_BUF_SIZE   4096

typedef struct {
    bool     active;
    /* Master → Slave buffer (master writes, slave reads) */
    uint8_t  m2s_buf[PTY_BUF_SIZE];
    uint32_t m2s_head, m2s_tail, m2s_count;
    /* Slave → Master buffer (slave writes, master reads) */
    uint8_t  s2m_buf[PTY_BUF_SIZE];
    uint32_t s2m_head, s2m_tail, s2m_count;
    /* State */
    bool     master_open;
    bool     slave_open;
    uint32_t owner_pid;
} pty_pair_t;

static pty_pair_t pty_pairs[PTY_MAX_PAIRS];

/* ── Buffer Operations ───────────────────────────────────────── */

static int pty_buf_write(uint8_t *buf, uint32_t *head, uint32_t *count,
                         const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    while (written < len && *count < PTY_BUF_SIZE) {
        buf[*head] = src[written++];
        *head = (*head + 1) % PTY_BUF_SIZE;
        (*count)++;
    }
    return (int)written;
}

static int pty_buf_read(uint8_t *buf, uint32_t *tail, uint32_t *count,
                        void *data, uint32_t len)
{
    uint8_t *dst = (uint8_t *)data;
    uint32_t nread = 0;
    while (nread < len && *count > 0) {
        dst[nread++] = buf[*tail];
        *tail = (*tail + 1) % PTY_BUF_SIZE;
        (*count)--;
    }
    return (int)nread;
}

/* ── Public API ──────────────────────────────────────────────── */

/* Allocate a new PTY pair. Returns pts number (0-7) or -1. */
int pty_alloc(uint32_t pid)
{
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!pty_pairs[i].active) {
            pty_pair_t *p = &pty_pairs[i];
            memset(p, 0, sizeof(*p));
            p->active = true;
            p->master_open = true;
            p->owner_pid = pid;
            serial_puts("[PTY] Allocated pts/");
            serial_putdec((uint64_t)i);
            serial_puts("\n");
            return i;
        }
    }
    return -1;
}

/* Open the slave side */
int pty_open_slave(int pts_num)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return -1;
    if (!pty_pairs[pts_num].active) return -1;
    pty_pairs[pts_num].slave_open = true;
    return 0;
}

/* Write to master (data goes to slave's input) */
int pty_master_write(int pts_num, const void *data, uint32_t len)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return -1;
    pty_pair_t *p = &pty_pairs[pts_num];
    if (!p->active) return -1;
    return pty_buf_write(p->m2s_buf, &p->m2s_head, &p->m2s_count, data, len);
}

/* Read from master (gets slave's output) */
int pty_master_read(int pts_num, void *data, uint32_t len)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return -1;
    pty_pair_t *p = &pty_pairs[pts_num];
    if (!p->active) return -1;
    if (p->s2m_count == 0) return 0;  /* No data */
    return pty_buf_read(p->s2m_buf, &p->s2m_tail, &p->s2m_count, data, len);
}

/* Write to slave (data goes to master's input) */
int pty_slave_write(int pts_num, const void *data, uint32_t len)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return -1;
    pty_pair_t *p = &pty_pairs[pts_num];
    if (!p->active) return -1;
    return pty_buf_write(p->s2m_buf, &p->s2m_head, &p->s2m_count, data, len);
}

/* Read from slave (gets master's output) */
int pty_slave_read(int pts_num, void *data, uint32_t len)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return -1;
    pty_pair_t *p = &pty_pairs[pts_num];
    if (!p->active) return -1;
    if (p->m2s_count == 0) {
        if (!p->master_open) return 0;  /* EOF — master closed */
        return -11;  /* EAGAIN */
    }
    return pty_buf_read(p->m2s_buf, &p->m2s_tail, &p->m2s_count, data, len);
}

/* Close master side */
void pty_close_master(int pts_num)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return;
    pty_pairs[pts_num].master_open = false;
    if (!pty_pairs[pts_num].slave_open)
        pty_pairs[pts_num].active = false;
}

/* Close slave side */
void pty_close_slave(int pts_num)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return;
    pty_pairs[pts_num].slave_open = false;
    if (!pty_pairs[pts_num].master_open)
        pty_pairs[pts_num].active = false;
}

/* Check if data available on master (for poll/epoll) */
bool pty_master_has_data(int pts_num)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return false;
    return pty_pairs[pts_num].s2m_count > 0;
}

/* Check if data available on slave */
bool pty_slave_has_data(int pts_num)
{
    if (pts_num < 0 || pts_num >= PTY_MAX_PAIRS) return false;
    return pty_pairs[pts_num].m2s_count > 0;
}
