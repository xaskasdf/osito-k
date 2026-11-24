/*
 * OsitoK x86-64 — Lock Dependency Validator (lockdep)
 *
 * Detects potential deadlocks by tracking lock acquisition order.
 * If lock A is held while acquiring lock B, records A→B dependency.
 * If later B→A is attempted, reports circular dependency (deadlock).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint32_t proc_current_pid(void);

#define LOCKDEP_MAX_LOCKS   32
#define LOCKDEP_MAX_DEPS    64
#define LOCKDEP_MAX_HELD     8  /* Max locks held simultaneously per CPU */

typedef struct {
    const char *name;
    uint64_t    addr;       /* Lock address (for identification) */
    bool        active;
} lock_class_t;

typedef struct {
    int from;   /* Lock class index held */
    int to;     /* Lock class index being acquired */
} lock_dep_t;

/* Per-CPU held locks stack */
typedef struct {
    int   held[LOCKDEP_MAX_HELD];
    int   depth;
} lock_held_t;

static lock_class_t classes[LOCKDEP_MAX_LOCKS];
static lock_dep_t   deps[LOCKDEP_MAX_DEPS];
static int           class_count;
static int           dep_count;
static lock_held_t   held_stack;  /* Single CPU for now */
static bool          lockdep_enabled;

void lockdep_init(void)
{
    memset(classes, 0, sizeof(classes));
    memset(deps, 0, sizeof(deps));
    class_count = dep_count = 0;
    held_stack.depth = 0;
    lockdep_enabled = true;
    serial_puts("[LOCKDEP] Lock dependency validator enabled\n");
}

/* Register a lock class */
static int lockdep_find_or_create(uint64_t addr, const char *name)
{
    for (int i = 0; i < class_count; i++) {
        if (classes[i].addr == addr) return i;
    }
    if (class_count >= LOCKDEP_MAX_LOCKS) return -1;
    classes[class_count].addr = addr;
    classes[class_count].name = name;
    classes[class_count].active = true;
    return class_count++;
}

/* Check for circular dependency */
static bool lockdep_check_circular(int from, int to)
{
    /* Simple: check if to→from exists (direct cycle) */
    for (int i = 0; i < dep_count; i++) {
        if (deps[i].from == to && deps[i].to == from)
            return true;
    }
    return false;
}

/* Record lock acquisition */
void lockdep_acquire(uint64_t lock_addr, const char *name)
{
    if (!lockdep_enabled) return;

    int cls = lockdep_find_or_create(lock_addr, name);
    if (cls < 0) return;

    /* Check dependencies against all currently held locks */
    for (int i = 0; i < held_stack.depth; i++) {
        int held_cls = held_stack.held[i];

        /* Check for circular dependency */
        if (lockdep_check_circular(held_cls, cls)) {
            serial_puts("\n[LOCKDEP] === DEADLOCK DETECTED ===\n");
            serial_puts("[LOCKDEP] Lock \"");
            serial_puts(name);
            serial_puts("\" acquired while holding \"");
            serial_puts(classes[held_cls].name ? classes[held_cls].name : "?");
            serial_puts("\"\n[LOCKDEP] But reverse order was seen before!\n");
            serial_puts("[LOCKDEP] This WILL deadlock under contention.\n\n");
        }

        /* Record this dependency */
        bool exists = false;
        for (int j = 0; j < dep_count; j++) {
            if (deps[j].from == held_cls && deps[j].to == cls) {
                exists = true; break;
            }
        }
        if (!exists && dep_count < LOCKDEP_MAX_DEPS) {
            deps[dep_count].from = held_cls;
            deps[dep_count].to = cls;
            dep_count++;
        }
    }

    /* Push onto held stack */
    if (held_stack.depth < LOCKDEP_MAX_HELD)
        held_stack.held[held_stack.depth++] = cls;
}

/* Record lock release */
void lockdep_release(uint64_t lock_addr)
{
    if (!lockdep_enabled) return;

    int cls = lockdep_find_or_create(lock_addr, NULL);
    if (cls < 0) return;

    /* Remove from held stack */
    for (int i = held_stack.depth - 1; i >= 0; i--) {
        if (held_stack.held[i] == cls) {
            for (int j = i; j < held_stack.depth - 1; j++)
                held_stack.held[j] = held_stack.held[j + 1];
            held_stack.depth--;
            return;
        }
    }
}

void lockdep_stats(void)
{
    serial_puts("[LOCKDEP] ");
    serial_putdec((uint64_t)class_count);
    serial_puts(" lock classes, ");
    serial_putdec((uint64_t)dep_count);
    serial_puts(" dependencies tracked\n");
}

void lockdep_enable(void) { lockdep_enabled = true; }
void lockdep_disable(void) { lockdep_enabled = false; }
