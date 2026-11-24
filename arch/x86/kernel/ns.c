/*
 * OsitoK x86-64 — Namespaces (Container Isolation)
 *
 * PID, mount, and network namespace support.
 * Enables running isolated process groups (containers).
 * Created via clone(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Namespace Types ─────────────────────────────────────────── */

#define NS_PID   (1 << 0)   /* PID namespace: process sees its own PID 1 */
#define NS_MNT   (1 << 1)   /* Mount namespace: private mount table */
#define NS_NET   (1 << 2)   /* Network namespace: private network stack */
#define NS_UTS   (1 << 3)   /* UTS namespace: private hostname */
#define NS_IPC   (1 << 4)   /* IPC namespace: private System V IPC */

/* ── Namespace Instance ──────────────────────────────────────── */

#define NS_MAX 16

typedef struct {
    bool     active;
    uint32_t type;           /* NS_PID | NS_MNT | NS_NET | ... */
    uint32_t id;             /* Unique namespace ID */
    uint32_t creator_pid;    /* PID that created this namespace */
    int      ref_count;      /* Number of processes using this NS */

    /* PID namespace: mapping from global PID → local PID */
    uint32_t pid_offset;     /* local_pid = global_pid - pid_offset */
    uint32_t next_local_pid; /* Next PID to assign in this NS */

    /* UTS namespace: hostname */
    char     hostname[64];

    /* Mount namespace: could point to private VFS mount table (future) */
    /* Net namespace: could point to private network stack (future) */
} namespace_t;

static namespace_t namespaces[NS_MAX];
static uint32_t ns_next_id = 1;

/* Global (init) namespace — all processes start here */
static namespace_t init_ns = {
    .active = true,
    .type = NS_PID | NS_MNT | NS_NET | NS_UTS | NS_IPC,
    .id = 0,
    .creator_pid = 0,
    .ref_count = 1,
    .hostname = "osito-k",
};

/* ── Public API ──────────────────────────────────────────────── */

/* Create a new namespace of the given type(s).
 * Returns namespace ID or -1 on failure. */
int ns_create(uint32_t type, uint32_t creator_pid)
{
    for (int i = 0; i < NS_MAX; i++) {
        if (!namespaces[i].active) {
            namespace_t *ns = &namespaces[i];
            memset(ns, 0, sizeof(*ns));
            ns->active = true;
            ns->type = type;
            ns->id = ns_next_id++;
            ns->creator_pid = creator_pid;
            ns->ref_count = 1;
            ns->next_local_pid = 1;

            /* Copy hostname from init NS */
            memcpy(ns->hostname, init_ns.hostname, 64);

            serial_puts("[NS] Created namespace ");
            serial_putdec(ns->id);
            serial_puts(" type=0x");
            char hex[5];
            hex[0] = "0123456789abcdef"[(type >> 12) & 0xF];
            hex[1] = "0123456789abcdef"[(type >> 8) & 0xF];
            hex[2] = "0123456789abcdef"[(type >> 4) & 0xF];
            hex[3] = "0123456789abcdef"[type & 0xF];
            hex[4] = 0;
            serial_puts(hex);
            serial_puts("\n");
            return (int)ns->id;
        }
    }
    return -1;
}

/* Attach a process to a namespace */
int ns_attach(uint32_t ns_id, uint32_t pid)
{
    (void)pid;
    for (int i = 0; i < NS_MAX; i++) {
        if (namespaces[i].active && namespaces[i].id == ns_id) {
            namespaces[i].ref_count++;
            return 0;
        }
    }
    return -1;
}

/* Detach from a namespace (decrement refcount) */
int ns_detach(uint32_t ns_id)
{
    for (int i = 0; i < NS_MAX; i++) {
        if (namespaces[i].active && namespaces[i].id == ns_id) {
            namespaces[i].ref_count--;
            if (namespaces[i].ref_count <= 0)
                namespaces[i].active = false;
            return 0;
        }
    }
    return -1;
}

/* Translate global PID to namespace-local PID */
uint32_t ns_translate_pid(uint32_t ns_id, uint32_t global_pid)
{
    if (ns_id == 0) return global_pid;  /* Init namespace = identity */
    for (int i = 0; i < NS_MAX; i++) {
        if (namespaces[i].active && namespaces[i].id == ns_id &&
            (namespaces[i].type & NS_PID)) {
            return global_pid - namespaces[i].pid_offset;
        }
    }
    return global_pid;
}

/* Set hostname for a UTS namespace */
int ns_set_hostname(uint32_t ns_id, const char *name, int len)
{
    namespace_t *ns = &init_ns;
    if (ns_id > 0) {
        for (int i = 0; i < NS_MAX; i++) {
            if (namespaces[i].active && namespaces[i].id == ns_id) {
                ns = &namespaces[i];
                break;
            }
        }
    }
    if (!(ns->type & NS_UTS)) return -1;
    int copy = len < 63 ? len : 63;
    memcpy(ns->hostname, name, copy);
    ns->hostname[copy] = '\0';
    return 0;
}

const char *ns_get_hostname(uint32_t ns_id)
{
    if (ns_id == 0) return init_ns.hostname;
    for (int i = 0; i < NS_MAX; i++) {
        if (namespaces[i].active && namespaces[i].id == ns_id)
            return namespaces[i].hostname;
    }
    return init_ns.hostname;
}

/* List namespaces */
void ns_list(void)
{
    serial_puts("[NS] Namespaces:\n");
    serial_puts("  0: init (");
    serial_puts(init_ns.hostname);
    serial_puts(")\n");
    for (int i = 0; i < NS_MAX; i++) {
        if (!namespaces[i].active) continue;
        serial_puts("  ");
        serial_putdec(namespaces[i].id);
        serial_puts(": type=0x");
        serial_putdec(namespaces[i].type);
        serial_puts(" refs=");
        serial_putdec((uint64_t)namespaces[i].ref_count);
        serial_puts(" host=");
        serial_puts(namespaces[i].hostname);
        serial_puts("\n");
    }
}
