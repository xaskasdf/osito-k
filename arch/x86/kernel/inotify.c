/*
 * OsitoK x86-64 — inotify (Filesystem Event Notification)
 *
 * Monitors file/directory changes. Used by build systems,
 * editors, and file managers for live updates.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

#define IN_ACCESS        0x001
#define IN_MODIFY        0x002
#define IN_CREATE        0x100
#define IN_DELETE        0x200
#define IN_OPEN          0x020
#define IN_CLOSE_WRITE   0x008
#define IN_MOVED_FROM    0x040
#define IN_MOVED_TO      0x080

#define INOTIFY_MAX_INSTANCES 8
#define INOTIFY_MAX_WATCHES   32
#define INOTIFY_EVENT_BUF     64

typedef struct {
    int32_t  wd;       /* Watch descriptor */
    uint32_t mask;     /* Events */
    uint32_t cookie;
    uint32_t len;      /* Name length */
    char     name[64];
} inotify_event_t;

typedef struct {
    bool     active;
    char     path[64];
    uint32_t mask;
    int      wd;
} inotify_watch_t;

typedef struct {
    bool             active;
    inotify_watch_t  watches[INOTIFY_MAX_WATCHES];
    int              watch_count;
    inotify_event_t  events[INOTIFY_EVENT_BUF];
    int              ev_head, ev_tail, ev_count;
    int              next_wd;
} inotify_instance_t;

static inotify_instance_t instances[INOTIFY_MAX_INSTANCES];

int inotify_init(void)
{
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!instances[i].active) {
            memset(&instances[i], 0, sizeof(inotify_instance_t));
            instances[i].active = true;
            instances[i].next_wd = 1;
            return i;
        }
    }
    return -1;
}

int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    if (fd < 0 || fd >= INOTIFY_MAX_INSTANCES || !instances[fd].active) return -1;
    inotify_instance_t *in = &instances[fd];
    if (in->watch_count >= INOTIFY_MAX_WATCHES) return -1;

    inotify_watch_t *w = &in->watches[in->watch_count++];
    w->active = true;
    w->mask = mask;
    w->wd = in->next_wd++;
    int i = 0;
    while (path[i] && i < 63) { w->path[i] = path[i]; i++; }
    w->path[i] = '\0';
    return w->wd;
}

int inotify_rm_watch(int fd, int wd)
{
    if (fd < 0 || fd >= INOTIFY_MAX_INSTANCES) return -1;
    inotify_instance_t *in = &instances[fd];
    for (int i = 0; i < in->watch_count; i++) {
        if (in->watches[i].wd == wd) {
            in->watches[i].active = false;
            return 0;
        }
    }
    return -1;
}

/* Post an event (called by filesystem operations) */
void inotify_post_event(const char *path, uint32_t event_mask)
{
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!instances[i].active) continue;
        for (int w = 0; w < instances[i].watch_count; w++) {
            if (!instances[i].watches[w].active) continue;
            if (!(instances[i].watches[w].mask & event_mask)) continue;

            /* Simple path prefix match */
            const char *wp = instances[i].watches[w].path;
            const char *fp = path;
            bool match = true;
            while (*wp) { if (*wp++ != *fp++) { match = false; break; } }
            if (!match) continue;

            /* Post event */
            if (instances[i].ev_count >= INOTIFY_EVENT_BUF) continue;
            inotify_event_t *ev = &instances[i].events[instances[i].ev_head];
            ev->wd = instances[i].watches[w].wd;
            ev->mask = event_mask;
            ev->cookie = 0;
            /* Extract filename from path */
            const char *name = path;
            for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;
            int nl = 0;
            while (name[nl] && nl < 63) { ev->name[nl] = name[nl]; nl++; }
            ev->name[nl] = '\0';
            ev->len = (uint32_t)nl;

            instances[i].ev_head = (instances[i].ev_head + 1) % INOTIFY_EVENT_BUF;
            instances[i].ev_count++;
        }
    }
}

/* Read events (for syscall layer) */
int inotify_read(int fd, void *buf, uint32_t max_len)
{
    if (fd < 0 || fd >= INOTIFY_MAX_INSTANCES) return -1;
    inotify_instance_t *in = &instances[fd];
    if (!in->active || in->ev_count == 0) return 0;

    uint32_t copied = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (in->ev_count > 0 && copied + sizeof(inotify_event_t) <= max_len) {
        memcpy(dst + copied, &in->events[in->ev_tail], sizeof(inotify_event_t));
        in->ev_tail = (in->ev_tail + 1) % INOTIFY_EVENT_BUF;
        in->ev_count--;
        copied += sizeof(inotify_event_t);
    }
    return (int)copied;
}
