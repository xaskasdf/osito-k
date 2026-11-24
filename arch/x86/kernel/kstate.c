/*
 * OsitoK x86-64 — Kernel State Preservation Across kexec
 *
 * Reserves a physical memory region that survives kexec.
 * Stores: GGUF model pointer, KV cache, network state.
 * New kernel detects preserved state on boot and resumes.
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* Fixed physical address for state header (above kernel, below temp) */
#define KSTATE_PHYS  0x4000000ULL   /* 64MB mark */
#define KSTATE_MAGIC 0x4F534B53     /* "OSKS" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;
    uint32_t _pad;

    /* GGUF model: already in RAM, just record location */
    uint64_t gguf_mmap_phys;     /* physical address of mmap'd file */
    uint64_t gguf_mmap_size;     /* total bytes */

    /* Inference: KV cache location + position */
    uint64_t kv_cache_phys;      /* physical address of kv_cache array */
    uint32_t kv_pos;             /* current sequence position */
    uint32_t kv_n_layers;
    uint32_t kv_dim;
    uint32_t kv_max_seq;

    /* Network: IP + connections */
    uint32_t ip_addr;            /* host byte order */
    uint32_t gateway;
    uint64_t net_state_tick;     /* tick when state was saved */

    /* Boot tick offset for uptime continuity */
    uint64_t prev_uptime_ticks;
} kstate_header_t;

static uint32_t kstate_checksum(kstate_header_t *ks)
{
    uint32_t sum = 0;
    uint32_t *p = (uint32_t *)ks;
    /* Skip magic/version/checksum fields (first 12 bytes = 3 dwords) */
    for (uint64_t i = 3; i < sizeof(*ks) / 4; i++)
        sum += p[i];
    return sum ^ 0xDEADBEEF;
}

/* Save kernel state before kexec */
void kstate_save(void)
{
    kstate_header_t *ks = (kstate_header_t *)PHYS_TO_VIRT(KSTATE_PHYS);

    ks->magic   = KSTATE_MAGIC;
    ks->version = 1;

    /* Record GGUF model location if loaded */
    extern void *gguf_get_mmap_base(void);
    extern uint64_t gguf_get_mmap_size(void);
    void *base = gguf_get_mmap_base();
    if (base) {
        ks->gguf_mmap_phys = VIRT_TO_PHYS(base);
        ks->gguf_mmap_size = gguf_get_mmap_size();
    }

    /* Record uptime for continuity */
    ks->prev_uptime_ticks = idt_get_ticks();

    ks->checksum = kstate_checksum(ks);

    serial_puts("[KSTATE] Saved: GGUF ");
    serial_putdec(ks->gguf_mmap_size / (1024 * 1024));
    serial_puts(" MB, uptime ");
    serial_putdec(ks->prev_uptime_ticks / 100);
    serial_puts("s\n");
}

/* Check for preserved state on boot. Returns 1 if warm boot. */
int kstate_restore(void)
{
    kstate_header_t *ks = (kstate_header_t *)PHYS_TO_VIRT(KSTATE_PHYS);

    if (ks->magic != KSTATE_MAGIC || ks->version != 1)
        return 0;

    if (kstate_checksum(ks) != ks->checksum) {
        serial_puts("[KSTATE] Checksum mismatch, cold boot\n");
        ks->magic = 0;
        return 0;
    }

    serial_puts("[KSTATE] Warm boot detected!\n");

    if (ks->gguf_mmap_phys && ks->gguf_mmap_size) {
        serial_puts("[KSTATE] Model data preserved: ");
        serial_putdec(ks->gguf_mmap_size / (1024 * 1024));
        serial_puts(" MB at phys 0x");
        serial_puthex(ks->gguf_mmap_phys, 16);
        serial_puts("\n");
        /* The caller (main.c) can skip gguf_load() and point directly
         * to the preserved physical pages. */
    }

    if (ks->prev_uptime_ticks > 0) {
        serial_puts("[KSTATE] Previous uptime: ");
        serial_putdec(ks->prev_uptime_ticks / 100);
        serial_puts("s\n");
    }

    /* Invalidate so next cold boot doesn't use stale data */
    ks->magic = 0;

    return 1;
}

/* Accessors for main.c to check preserved model */
uint64_t kstate_get_gguf_phys(void)
{
    kstate_header_t *ks = (kstate_header_t *)PHYS_TO_VIRT(KSTATE_PHYS);
    return (ks->magic == KSTATE_MAGIC) ? ks->gguf_mmap_phys : 0;
}

uint64_t kstate_get_gguf_size(void)
{
    kstate_header_t *ks = (kstate_header_t *)PHYS_TO_VIRT(KSTATE_PHYS);
    return (ks->magic == KSTATE_MAGIC) ? ks->gguf_mmap_size : 0;
}
