/*
 * arch/x86/include/self_optimize.h — runtime kernel self-modification
 *
 * Strategy #1 (v1): static branch patching. Callers register sites
 * where a runtime check becomes permanently fixed after boot (e.g.
 * `if (tensor_has_avx2())` is always true once CPUID ran). After boot
 * stabilizes (tick_count > 1000), self_opt_apply() rewrites those
 * branches into unconditional JMP / NOP to eliminate the per-call
 * branch cost.
 *
 * THIS V1 IS DRY-RUN ONLY: it records what would be patched and prints
 * it at `self_opt stats`. The actual .text rewrite is gated behind
 * SELF_OPT_PATCH=1 and requires paging_text_make_writable/readonly
 * helpers that don't yet exist.
 *
 * Strategies #2-#4 (function cloning, proctab reorder, ML-guided) are
 * follow-up work.
 */
#ifndef OSITOK_SELF_OPTIMIZE_H
#define OSITOK_SELF_OPTIMIZE_H

#include "types.h"
#include "stdint.h"

/* Register a branch site. Description is a static string literal. */
void self_opt_register_branch(void *branch_site, const char *description);

/* Trigger patching. No-op until tick_count > 1000 and SELF_OPT_PATCH=1. */
void self_opt_apply(void);

/* Undo all patches. Always runnable — good for emergency during boot. */
void self_opt_undo_all(void);

/* Shell: list registered sites + patch state. */
void self_opt_stats(void);

/* Enable/disable globally. */
extern bool self_opt_enabled;

#endif /* OSITOK_SELF_OPTIMIZE_H */
