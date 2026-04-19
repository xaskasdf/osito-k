/*
 * arch/x86/kernel/io_predict.c — speculative file prefetcher
 *
 * Records open() sequences per kernel (not per-process — simpler and the
 * common patterns are filesystem-wide anyway) and prefetches predicted
 * next files on APs when a trigger file is observed.
 */

#include "../include/io_predict.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void *kmalloc(uint64_t);
extern void  kfree(void *);
extern int   smp_submit_ff(void (*fn)(void *, void *), void *arg, void *result);

/* VFS (fs/vfs.h is not in the kernel's include path without ../fs/...) */
typedef struct {
    uint32_t fs_version;
    uint32_t ino;
    void    *data;
    uint64_t size;
} vfs_node_stub_t;
extern bool vfs_find(const char *path, int mode, void *out_node);
extern int  vfs_read(void *node, uint64_t offset, void *buf, uint64_t len);

#define IO_PATTERN_MAX      64
#define IO_PREDICT_MAX      4
#define IO_NAME_MAX         32
#define IO_PREFETCH_CAP     65536   /* don't prefetch files > 64KB */

typedef struct {
    char     trigger[IO_NAME_MAX];
    char     predicted[IO_PREDICT_MAX][IO_NAME_MAX];
    uint16_t counts[IO_PREDICT_MAX];
    uint16_t total;
    uint32_t last_tick;
} io_pattern_t;

static io_pattern_t patterns[IO_PATTERN_MAX];
static char last_opened[IO_NAME_MAX];

bool io_predict_enabled = true;
extern uint64_t idt_get_ticks(void);

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

typedef struct {
    char path[IO_NAME_MAX];
} prefetch_arg_t;

static void prefetch_worker(void *arg, void *result)
{
    (void)result;
    prefetch_arg_t *a = arg;

    /* vfs_find + small read warms the metadata cache and first blocks */
    vfs_node_stub_t node;
    if (vfs_find(a->path, 0 /* VFS_MODE_NATIVE */, &node)) {
        if (node.size > 0 && node.size <= IO_PREFETCH_CAP) {
            uint8_t scratch[4096];
            uint64_t off = 0;
            uint64_t max = node.size < 16 * 1024 ? node.size : 16 * 1024;
            while (off < max) {
                uint64_t chunk = (max - off) < sizeof(scratch)
                                    ? (max - off) : sizeof(scratch);
                vfs_read(&node, off, scratch, chunk);
                off += chunk;
            }
        }
    }
    kfree(arg);
}

static io_pattern_t *find_or_alloc_pattern(const char *trigger)
{
    int lru = 0;
    uint32_t lru_tick = 0xFFFFFFFFu;
    for (int i = 0; i < IO_PATTERN_MAX; i++) {
        if (patterns[i].trigger[0] == 0) {
            str_copy(patterns[i].trigger, trigger, IO_NAME_MAX);
            patterns[i].total = 0;
            patterns[i].last_tick = idt_get_ticks();
            for (int j = 0; j < IO_PREDICT_MAX; j++) {
                patterns[i].predicted[j][0] = 0;
                patterns[i].counts[j] = 0;
            }
            return &patterns[i];
        }
        if (str_eq(patterns[i].trigger, trigger)) {
            patterns[i].last_tick = idt_get_ticks();
            return &patterns[i];
        }
        if (patterns[i].last_tick < lru_tick) {
            lru_tick = patterns[i].last_tick;
            lru = i;
        }
    }
    /* Evict LRU */
    io_pattern_t *p = &patterns[lru];
    str_copy(p->trigger, trigger, IO_NAME_MAX);
    p->total = 0;
    p->last_tick = idt_get_ticks();
    for (int j = 0; j < IO_PREDICT_MAX; j++) {
        p->predicted[j][0] = 0;
        p->counts[j] = 0;
    }
    return p;
}

static void record_transition(const char *from, const char *to)
{
    io_pattern_t *p = find_or_alloc_pattern(from);

    int min_slot = 0;
    uint16_t min_count = 0xFFFFu;
    int empty_slot = -1;
    for (int j = 0; j < IO_PREDICT_MAX; j++) {
        if (p->predicted[j][0] == 0) {
            if (empty_slot < 0) empty_slot = j;
            continue;
        }
        if (str_eq(p->predicted[j], to)) {
            p->counts[j]++;
            p->total++;
            return;
        }
        if (p->counts[j] < min_count) { min_count = p->counts[j]; min_slot = j; }
    }
    int slot = (empty_slot >= 0) ? empty_slot : min_slot;
    str_copy(p->predicted[slot], to, IO_NAME_MAX);
    p->counts[slot] = 1;
    p->total++;
}

static void dispatch_prefetches(const char *trigger)
{
    for (int i = 0; i < IO_PATTERN_MAX; i++) {
        if (!str_eq(patterns[i].trigger, trigger)) continue;
        for (int j = 0; j < IO_PREDICT_MAX; j++) {
            if (patterns[i].predicted[j][0] == 0) break;
            /* Prefetch only if this prediction accounts for >50% */
            if (patterns[i].counts[j] * 2 < patterns[i].total) continue;

            prefetch_arg_t *a = (prefetch_arg_t *)kmalloc(sizeof *a);
            if (!a) continue;
            str_copy(a->path, patterns[i].predicted[j], IO_NAME_MAX);
            if (smp_submit_ff(prefetch_worker, a, 0) < 0) {
                /* AP busy / not available — run inline (still warms cache) */
                prefetch_worker(a, 0);
            }
        }
        break;
    }
}

void io_predict_observe(const char *path)
{
    if (!io_predict_enabled || !path || !path[0]) return;

    /* Skip long paths — the pattern cache only handles short names */
    int len = 0;
    while (path[len] && len < IO_NAME_MAX) len++;
    if (len == IO_NAME_MAX) return;

    if (last_opened[0]) {
        record_transition(last_opened, path);
    }
    dispatch_prefetches(path);
    str_copy(last_opened, path, IO_NAME_MAX);
}

void io_predict_reset(void)
{
    for (int i = 0; i < IO_PATTERN_MAX; i++) {
        patterns[i].trigger[0] = 0;
        patterns[i].total = 0;
        patterns[i].last_tick = 0;
    }
    last_opened[0] = 0;
}

void io_predict_stats(void)
{
    serial_puts("[IO-PREDICT] patterns:\n");
    int shown = 0;
    for (int i = 0; i < IO_PATTERN_MAX && shown < 10; i++) {
        if (!patterns[i].trigger[0]) continue;
        serial_puts("  ");
        serial_puts(patterns[i].trigger);
        serial_puts(" -> {");
        bool first = true;
        for (int j = 0; j < IO_PREDICT_MAX; j++) {
            if (!patterns[i].predicted[j][0]) continue;
            if (!first) serial_puts(", ");
            first = false;
            serial_puts(patterns[i].predicted[j]);
            serial_puts(" (");
            serial_putdec(patterns[i].counts[j]);
            serial_puts(")");
        }
        serial_puts("}\n");
        shown++;
    }
    if (shown == 0) serial_puts("  (no patterns recorded)\n");
}
