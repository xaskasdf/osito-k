/*
 * OsitoK x86-64 — Capability-Based Security + Basic cgroups
 *
 * Linux-compatible capabilities (CAP_NET_RAW, CAP_SYS_ADMIN, etc.)
 * and basic cgroup resource limits (CPU time, memory).
 *
 * Capabilities: per-process bitmask checked before privileged operations.
 * cgroups: per-group resource quotas enforced by scheduler and allocator.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Linux Capabilities ──────────────────────────────────────── */

#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW          13
#define CAP_SYS_CHROOT       18
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_TIME         25
#define CAP_MKNOD            27
#define CAP_LAST             37

/* Per-process capability sets */
typedef struct {
    uint64_t effective;    /* Currently active capabilities */
    uint64_t permitted;    /* Maximum capabilities this process can have */
    uint64_t inheritable;  /* Capabilities passed to children */
} cap_set_t;

/* Default: all capabilities (bare-metal, single-user) */
#define CAP_ALL  0x3FFFFFFFFFULL  /* Bits 0-37 */

static cap_set_t default_caps = { CAP_ALL, CAP_ALL, CAP_ALL };

/* Check if current process has a capability */
bool cap_check(const cap_set_t *caps, int cap)
{
    if (cap < 0 || cap > CAP_LAST) return false;
    return (caps->effective & (1ULL << cap)) != 0;
}

/* Drop a capability */
void cap_drop(cap_set_t *caps, int cap)
{
    if (cap < 0 || cap > CAP_LAST) return;
    caps->effective &= ~(1ULL << cap);
    caps->permitted &= ~(1ULL << cap);
}

/* Get default caps for new processes */
cap_set_t *cap_get_default(void) { return &default_caps; }

/* ── Basic cgroups ───────────────────────────────────────────── */

#define CGROUP_MAX 8

typedef struct {
    bool     active;
    char     name[32];
    /* Resource limits */
    uint64_t cpu_quota_ticks;    /* Max CPU ticks per 100-tick period (0=unlimited) */
    uint64_t mem_limit_bytes;    /* Max memory usage (0=unlimited) */
    /* Usage tracking */
    uint64_t cpu_used_ticks;     /* CPU ticks used in current period */
    uint64_t mem_used_bytes;     /* Current memory usage */
    uint64_t period_start;       /* Tick when current period started */
    /* Members */
    uint32_t pids[16];           /* Process IDs in this cgroup */
    int      pid_count;
} cgroup_t;

static cgroup_t cgroups[CGROUP_MAX];

/* ── cgroup API ──────────────────────────────────────────────── */

int cgroup_create(const char *name)
{
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!cgroups[i].active) {
            memset(&cgroups[i], 0, sizeof(cgroup_t));
            cgroups[i].active = true;
            int j = 0;
            while (name[j] && j < 31) { cgroups[i].name[j] = name[j]; j++; }
            cgroups[i].name[j] = '\0';
            serial_puts("[CGROUP] Created: ");
            serial_puts(cgroups[i].name);
            serial_puts("\n");
            return i;
        }
    }
    return -1;
}

int cgroup_set_cpu_limit(int cg_idx, uint64_t quota_percent)
{
    if (cg_idx < 0 || cg_idx >= CGROUP_MAX || !cgroups[cg_idx].active) return -1;
    /* quota_percent: 100 = full CPU, 50 = half, 0 = unlimited */
    cgroups[cg_idx].cpu_quota_ticks = quota_percent;  /* ticks per 100-tick period */
    return 0;
}

int cgroup_set_mem_limit(int cg_idx, uint64_t limit_bytes)
{
    if (cg_idx < 0 || cg_idx >= CGROUP_MAX || !cgroups[cg_idx].active) return -1;
    cgroups[cg_idx].mem_limit_bytes = limit_bytes;
    return 0;
}

int cgroup_add_pid(int cg_idx, uint32_t pid)
{
    if (cg_idx < 0 || cg_idx >= CGROUP_MAX || !cgroups[cg_idx].active) return -1;
    cgroup_t *cg = &cgroups[cg_idx];
    if (cg->pid_count >= 16) return -1;
    cg->pids[cg->pid_count++] = pid;
    return 0;
}

/* Check if a process in a cgroup is allowed more CPU time */
bool cgroup_check_cpu(uint32_t pid)
{
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!cgroups[i].active || cgroups[i].cpu_quota_ticks == 0) continue;
        for (int j = 0; j < cgroups[i].pid_count; j++) {
            if (cgroups[i].pids[j] == pid) {
                return cgroups[i].cpu_used_ticks < cgroups[i].cpu_quota_ticks;
            }
        }
    }
    return true;  /* No cgroup limit */
}

/* Check if a memory allocation is allowed for a cgroup member */
bool cgroup_check_mem(uint32_t pid, uint64_t alloc_bytes)
{
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!cgroups[i].active || cgroups[i].mem_limit_bytes == 0) continue;
        for (int j = 0; j < cgroups[i].pid_count; j++) {
            if (cgroups[i].pids[j] == pid) {
                return (cgroups[i].mem_used_bytes + alloc_bytes) <=
                       cgroups[i].mem_limit_bytes;
            }
        }
    }
    return true;
}

/* Tick accounting (called from scheduler) */
void cgroup_tick(uint32_t pid)
{
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!cgroups[i].active) continue;
        for (int j = 0; j < cgroups[i].pid_count; j++) {
            if (cgroups[i].pids[j] == pid) {
                cgroups[i].cpu_used_ticks++;
                return;
            }
        }
    }
}

/* Period reset (call every 100 ticks = 1 second) */
void cgroup_reset_period(void)
{
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (cgroups[i].active)
            cgroups[i].cpu_used_ticks = 0;
    }
}

/* List cgroups */
void cgroup_list(void)
{
    serial_puts("[CGROUP] Groups:\n");
    int count = 0;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!cgroups[i].active) continue;
        serial_puts("  ");
        serial_puts(cgroups[i].name);
        serial_puts(": cpu=");
        serial_putdec(cgroups[i].cpu_quota_ticks);
        serial_puts("% mem=");
        serial_putdec(cgroups[i].mem_limit_bytes / (1024 * 1024));
        serial_puts("MB pids=");
        serial_putdec((uint64_t)cgroups[i].pid_count);
        serial_puts("\n");
        count++;
    }
    if (count == 0) serial_puts("  (none)\n");
}
