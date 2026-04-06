/*
 * OsitoK x86-64 — Unified Device Model (kobject/kset)
 *
 * Hierarchical device tree for driver management.
 * Each device is a kobject with parent/child relationships.
 * Foundation for sysfs real implementation and udev.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── kobject ─────────────────────────────────────────────────── */

#define KOBJ_MAX    64
#define KOBJ_NAME   32

typedef struct kobject {
    char     name[KOBJ_NAME];
    int      parent;        /* Index of parent kobject (-1 = root) */
    int      refcount;
    bool     active;
    uint8_t  type;          /* 0=generic, 1=device, 2=driver, 3=bus */
    void    *private_data;  /* Driver-specific data */
} kobject_t;

static kobject_t kobjects[KOBJ_MAX];
static int kobj_count;

int kobject_create(const char *name, int parent, uint8_t type);

/* ── Init ────────────────────────────────────────────────────── */

void kobject_init(void)
{
    memset(kobjects, 0, sizeof(kobjects));
    kobj_count = 0;

    /* Create root kobjects */
    kobject_create("devices", -1, 0);   /* /sys/devices */
    kobject_create("bus", -1, 3);       /* /sys/bus */
    kobject_create("class", -1, 0);     /* /sys/class */
    kobject_create("block", 2, 0);      /* /sys/class/block */
    kobject_create("net", 2, 0);        /* /sys/class/net */

    serial_puts("[KOBJ] Device model initialized (");
    serial_putdec((uint64_t)kobj_count);
    serial_puts(" root objects)\n");
}

/* ── Create ──────────────────────────────────────────────────── */

int kobject_create(const char *name, int parent, uint8_t type)
{
    if (kobj_count >= KOBJ_MAX) return -1;

    kobject_t *k = &kobjects[kobj_count];
    k->active = true;
    k->parent = parent;
    k->type = type;
    k->refcount = 1;
    int i = 0;
    while (name[i] && i < KOBJ_NAME - 1) { k->name[i] = name[i]; i++; }
    k->name[i] = '\0';

    return kobj_count++;
}

/* ── Reference Counting ──────────────────────────────────────── */

void kobject_get(int idx)
{
    if (idx >= 0 && idx < kobj_count && kobjects[idx].active)
        kobjects[idx].refcount++;
}

void kobject_put(int idx)
{
    if (idx >= 0 && idx < kobj_count && kobjects[idx].active) {
        kobjects[idx].refcount--;
        if (kobjects[idx].refcount <= 0)
            kobjects[idx].active = false;
    }
}

/* ── Queries ─────────────────────────────────────────────────── */

int kobject_find(const char *name, int parent)
{
    for (int i = 0; i < kobj_count; i++) {
        if (!kobjects[i].active) continue;
        if (kobjects[i].parent != parent && parent != -2) continue;
        const char *a = kobjects[i].name, *b = name;
        bool match = true;
        while (*a && *b) { if (*a++ != *b++) { match = false; break; } }
        if (match && *a == *b) return i;
    }
    return -1;
}

const char *kobject_name(int idx)
{
    if (idx >= 0 && idx < kobj_count && kobjects[idx].active)
        return kobjects[idx].name;
    return NULL;
}

void kobject_set_data(int idx, void *data)
{
    if (idx >= 0 && idx < kobj_count)
        kobjects[idx].private_data = data;
}

void *kobject_get_data(int idx)
{
    if (idx >= 0 && idx < kobj_count)
        return kobjects[idx].private_data;
    return NULL;
}

/* ── Tree Display ────────────────────────────────────────────── */

static void kobject_print_tree(int parent, int depth)
{
    for (int i = 0; i < kobj_count; i++) {
        if (!kobjects[i].active || kobjects[i].parent != parent) continue;
        for (int d = 0; d < depth; d++) serial_puts("  ");
        serial_puts(kobjects[i].name);
        serial_puts(" (ref=");
        serial_putdec((uint64_t)kobjects[i].refcount);
        serial_puts(")\n");
        kobject_print_tree(i, depth + 1);
    }
}

void kobject_dump(void)
{
    serial_puts("[KOBJ] Device tree:\n");
    kobject_print_tree(-1, 1);
}

int kobject_count(void) { return kobj_count; }
