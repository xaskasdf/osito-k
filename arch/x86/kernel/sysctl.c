/*
 * OsitoK x86-64 — sysctl Interface
 *
 * /proc/sys/* readable entries + sysctl() syscall.
 * Kernel parameters queryable and tunable at runtime.
 */

#include "../include/types.h"
#include "../include/sys_caps.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern uint64_t mem_get_free(void);
extern uint64_t mem_get_total(void);

/* ── Sysctl Parameters ──────────────────────────────────────── */

#define SYSCTL_MAX 32

typedef struct {
    const char *name;      /* e.g. "kernel.hostname" */
    uint64_t    value;     /* Integer value */
    char        str_val[64]; /* String value (for string params) */
    bool        is_string;
    bool        writable;
} sysctl_entry_t;

static sysctl_entry_t sysctl_table[SYSCTL_MAX];
static int sysctl_count;

/* ── Init (register default parameters) ──────────────────────── */

void sysctl_init(void)
{
    sysctl_count = 0;

    /* kernel.* */
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "kernel.hostname", .str_val = "osito-k",
        .is_string = true, .writable = true
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "kernel.ostype", .str_val = "OsitoK",
        .is_string = true, .writable = false
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "kernel.osrelease", .str_val = "1.0.0",
        .is_string = true, .writable = false
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "kernel.pid_max", .value = g_sys_caps.max_processes,
        .is_string = false, .writable = true
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "kernel.threads-max", .value = g_sys_caps.max_processes,
        .is_string = false, .writable = false
    };

    /* vm.* */
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "vm.overcommit_memory", .value = 0,
        .is_string = false, .writable = true
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "vm.swappiness", .value = 60,
        .is_string = false, .writable = true
    };

    /* net.* */
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "net.ipv4.ip_forward", .value = 0,
        .is_string = false, .writable = true
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "net.ipv4.tcp_syncookies", .value = 1,
        .is_string = false, .writable = true
    };
    sysctl_table[sysctl_count++] = (sysctl_entry_t){
        .name = "net.core.somaxconn", .value = 32,
        .is_string = false, .writable = true
    };

    serial_puts("[SYSCTL] Initialized (");
    serial_putdec((uint64_t)sysctl_count);
    serial_puts(" parameters)\n");
}

/* ── Lookup ──────────────────────────────────────────────────── */

static sysctl_entry_t *sysctl_find(const char *name)
{
    for (int i = 0; i < sysctl_count; i++) {
        const char *a = sysctl_table[i].name, *b = name;
        bool match = true;
        while (*a && *b) { if (*a++ != *b++) { match = false; break; } }
        if (match && *a == *b) return &sysctl_table[i];
    }
    return NULL;
}

/* ── Read ────────────────────────────────────────────────────── */

int sysctl_read(const char *name, char *buf, int max_len)
{
    sysctl_entry_t *e = sysctl_find(name);
    if (!e) return -1;

    int p = 0;
    if (e->is_string) {
        const char *s = e->str_val;
        while (*s && p < max_len - 1) buf[p++] = *s++;
    } else {
        char tmp[20]; int n = 0;
        uint64_t v = e->value;
        if (v == 0) { buf[p++] = '0'; }
        else {
            while (v) { tmp[n++] = '0' + v % 10; v /= 10; }
            for (int i = n - 1; i >= 0 && p < max_len - 1; i--)
                buf[p++] = tmp[i];
        }
    }
    if (p < max_len - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

/* ── Write ───────────────────────────────────────────────────── */

int sysctl_write(const char *name, const char *value)
{
    sysctl_entry_t *e = sysctl_find(name);
    if (!e || !e->writable) return -1;

    if (e->is_string) {
        int i = 0;
        while (value[i] && i < 63) { e->str_val[i] = value[i]; i++; }
        e->str_val[i] = '\0';
    } else {
        uint64_t v = 0;
        const char *p = value;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        e->value = v;
    }
    return 0;
}

/* ── List all parameters ─────────────────────────────────────── */

void sysctl_list(void)
{
    serial_puts("[SYSCTL] Parameters:\n");
    for (int i = 0; i < sysctl_count; i++) {
        serial_puts("  ");
        serial_puts(sysctl_table[i].name);
        serial_puts(" = ");
        if (sysctl_table[i].is_string) {
            serial_puts(sysctl_table[i].str_val);
        } else {
            serial_putdec(sysctl_table[i].value);
        }
        if (!sysctl_table[i].writable) serial_puts(" (read-only)");
        serial_puts("\n");
    }
}
