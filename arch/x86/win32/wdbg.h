/*
 * wdbg.h — Win32 binary-debugging toolkit for compat32 layer.
 *
 * Reusable primitives for "who called X, what was the engine state,
 * which FName/UObject was the argument?" — questions that keep
 * recurring while debugging UT99/GTAV/etc. PE32 binaries on OsitoK.
 *
 * All primitives are safe to call from INT 0x2E context (no heap, no
 * locks, fixed-size static state, output via serial_puts).
 */

#ifndef WDBG_H
#define WDBG_H

#include <stdint.h>

/* ── Address-site hooks ─────────────────────────────────────────
 *
 * Register a callback to fire when a PE32 instruction at `va` is
 * about to be entered (detected via ret_addr == va on the next INT2E).
 *
 * Use-case: "every time the throw helper @0x1014BD10 fires, dump the
 * caller frame + the FFileException argument". One add_hook call,
 * persistent across the run, output prefixed [WDBG/<name>].
 *
 * Caveat: relies on the target being called from a thunk path, so the
 * next INT2E observes ret_addr ∈ target's basic block. For functions
 * that never trigger an INT2E, use wdbg_install_int_trampoline()
 * (TODO) — not yet implemented.
 */
typedef void (*wdbg_hook_fn)(uint32_t va,
                              uint32_t esp,
                              uint32_t ebp,
                              const uint32_t *stack_args);

/* Returns hook id ≥ 0, or -1 on failure (table full). */
int wdbg_addr_hook(uint32_t va_start,
                   uint32_t va_end,    /* inclusive; pass va_start to hook single addr */
                   wdbg_hook_fn cb,
                   const char *name);

/* Called from compat32_dispatch every INT2E. ret_addr is the PE32
 * caller's return address (stack_args[-1]). Dispatches any matching
 * hooks. No-op when no hooks registered. */
void wdbg_check_caller(uint32_t ret_addr, uint32_t *stack_args);

/* ── FName resolver ─────────────────────────────────────────────
 *
 * Given an FName index, returns the engine-side ANSI string name.
 * Walks FName::Names TArray @ WDBG_FNAME_NAMES_VA.
 *
 * Returns "<null>" if Data == NULL, "<oob>" if idx >= Num,
 * "<bad>" if the entry pointer is unreadable. */
const char *wdbg_fname_resolve(uint32_t idx);

/* Override the TArray VA (default 0x10295D30 = UT99 Core.dll). */
void wdbg_fname_set_array(uint32_t tarray_va);

/* ── UObject inspector ──────────────────────────────────────────
 *
 * Given a 32-bit UObject*, dump one line to serial:
 *   [WDBG/UObj] <label> va=0x... cls=ClassName name=NameStr outer=...
 *
 * UE1 UObject layout assumed (override via wdbg_uobject_set_offsets):
 *   +0   vtbl
 *   +4   Index
 *   +8   HashNext
 *   +12  StateFrame
 *   +16  Outer
 *   +20  Name (FName index)
 *   +24  Class
 *   +28  ObjectFlags
 */
void wdbg_uobject_dump(uint32_t obj_va, const char *label);

/* Override UObject member offsets if engine differs from UE1 default. */
void wdbg_uobject_set_offsets(int outer, int name, int klass);

/* ── Stack walker ───────────────────────────────────────────────
 *
 * Walk EBP frame chain `depth` levels deep, print each return
 * address symbolized via wdbg_symbolize. Output prefix [WDBG/stk].
 *
 * Caveat: requires the PE32 binary to compile with frame pointers.
 * Epic/UE1 typically omits frame pointers in Release builds — use
 * wdbg_stack_scan instead in that case.
 */
void wdbg_stack_walk(uint32_t ebp, int depth, const char *label);

/* ── Stack scanner (no frame pointer required) ──────────────────
 *
 * Walks `depth` dwords up from `esp`. For each dword that looks
 * like a return address (i.e., falls inside a registered module
 * range AND the 5 bytes before it look like `E8 ?? ?? ?? ??` —
 * a CALL rel32), print it symbolized.
 *
 * Heuristic isn't perfect — false positives from data dwords that
 * happen to point into code. But for finding upstream callers in
 * a binary built without frame pointers, this is the only option.
 */
void wdbg_stack_scan(uint32_t esp, int depth, const char *label);

/* ── Module + symbolization ─────────────────────────────────────
 *
 * Register a PE32 module's load range so wdbg_symbolize can print
 * "Core.dll+0x4BD3F" instead of just "0x1014BD3F". Auto-called by
 * pe.c after each pe_load(). Manual registration also possible.
 *
 * Module table has fixed capacity (16 entries — UT99 loads 8-10).
 */
int wdbg_register_module(const char *name, uint32_t base, uint32_t size);

/* Returns module-relative string ("Core.dll+0x4BD3F") for `va`, or
 * the raw "0x10141234" if no module matches. Writes into `buf`. */
const char *wdbg_symbolize(uint32_t va, char *buf, int bufsz);

/* ── Init ───────────────────────────────────────────────────────
 *
 * Called once from win32_init(). Sets up default hooks (currently:
 * the Core.dll throw helper @0x1014BD10 with caller-logging callback).
 */
void wdbg_init(void);

/* ── Quick string-dump utility ──────────────────────────────────
 *
 * Read a string from PE memory and print to serial, safely handling
 * NULL / unreadable / unterminated. Uses 256-byte cap.
 */
void wdbg_print_ansi(uint32_t va);
void wdbg_print_wide(uint32_t va);  /* UTF-16LE; transcoded to ASCII */

#endif /* WDBG_H */
