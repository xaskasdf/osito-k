/*
 * arch/x86/kernel/self_optimize.c — runtime branch patching (v1 dry-run)
 *
 * Registers hot static branches and — when enabled — rewrites them to
 * unconditional jumps after boot stabilizes.
 *
 * v1 SAFETY: actual .text modification is gated behind SELF_OPT_PATCH=1
 * at compile time. By default this module operates in dry-run mode: it
 * accepts registrations and logs what it WOULD patch, without touching
 * code. This lets the API surface settle and the site list grow while
 * the text-write plumbing is validated separately.
 */

#include "../include/self_optimize.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int d);
extern uint64_t idt_get_ticks(void);

#define SELF_OPT_MAX_SITES 64

typedef struct {
    void        *addr;
    const char  *desc;
    bool         patched;
    uint64_t     registered_at_tick;
} self_opt_site_t;

static self_opt_site_t sites[SELF_OPT_MAX_SITES];
static int             site_count;

bool self_opt_enabled = false;      /* must be explicitly enabled from shell */

void self_opt_register_branch(void *branch_site, const char *description)
{
    if (site_count >= SELF_OPT_MAX_SITES) {
        serial_puts("[SELF-OPT] site table full, dropping registration\n");
        return;
    }
    sites[site_count].addr    = branch_site;
    sites[site_count].desc    = description ? description : "(anon)";
    sites[site_count].patched = false;
    sites[site_count].registered_at_tick = idt_get_ticks();
    site_count++;
}

void self_opt_apply(void)
{
    if (!self_opt_enabled) return;
    if (idt_get_ticks() < 1000) {
        serial_puts("[SELF-OPT] boot not yet stable (tick<1000), skipping\n");
        return;
    }

#ifdef SELF_OPT_PATCH
    /* Real patching path — requires paging_text_make_writable helper that
     * does not exist yet in paging.c. Once it's added, implement:
     *   1. paging_text_make_writable();
     *   2. for each unpatched site: memcpy(site->addr, replacement, N);
     *   3. mfence; lfence; IPI broadcast to flush pipelines on APs;
     *   4. paging_text_make_readonly();
     * For v1 this branch is gated so the build doesn't regress. */
    serial_puts("[SELF-OPT] PATCH mode: real rewriting not implemented yet\n");
#else
    /* Dry-run: just report */
    int would = 0;
    for (int i = 0; i < site_count; i++) {
        if (!sites[i].patched) would++;
    }
    serial_puts("[SELF-OPT] dry-run: would patch ");
    serial_putdec(would);
    serial_puts(" site(s)\n");
    for (int i = 0; i < site_count; i++) {
        if (sites[i].patched) continue;
        serial_puts("  - ");
        serial_puts(sites[i].desc);
        serial_puts(" @ 0x");
        serial_puthex((uint64_t)sites[i].addr, 12);
        serial_puts("\n");
    }
#endif
}

void self_opt_undo_all(void)
{
#ifdef SELF_OPT_PATCH
    /* TODO: restore original bytes for each patched site */
#endif
    for (int i = 0; i < site_count; i++)
        sites[i].patched = false;
    serial_puts("[SELF-OPT] all sites marked unpatched\n");
}

void self_opt_stats(void)
{
    serial_puts("[SELF-OPT] ");
    serial_putdec((uint64_t)site_count);
    serial_puts(" sites registered, enabled=");
    serial_puts(self_opt_enabled ? "YES" : "NO");
    serial_puts("\n");
    for (int i = 0; i < site_count; i++) {
        serial_puts("  [");
        serial_putdec(i);
        serial_puts("] ");
        serial_puts(sites[i].patched ? "PATCHED  " : "pending  ");
        serial_puts(sites[i].desc);
        serial_puts(" @ 0x");
        serial_puthex((uint64_t)sites[i].addr, 12);
        serial_puts("\n");
    }
}
