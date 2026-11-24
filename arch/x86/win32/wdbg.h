/*
 * wdbg.h — Win32 binary-debugging toolkit for compat32 layer.
 *
 * Reusable primitives for inspecting callers, stacks, and application-owned
 * object layouts while debugging PE32 binaries on OsitoK.
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
 * Hooks are opt-in and accept runtime-discovered address ranges. One
 * registration persists for the current boot and is prefixed [WDBG/<name>].
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

/* ── Stack walker ───────────────────────────────────────────────
 *
 * Walk EBP frame chain `depth` levels deep, print each return
 * address symbolized via wdbg_symbolize. Output prefix [WDBG/stk].
 *
 * Caveat: requires the PE32 binary to compile with frame pointers. Use
 * wdbg_stack_scan for optimized binaries that omit them.
 */
void wdbg_stack_walk(uint32_t ebp, int depth, const char *label);

/* ── Stack scanner (no frame pointer required) ──────────────────
 *
 * Walks `depth` dwords up from `esp`. For each dword that looks
 * like a return address (i.e., falls inside a loaded or registered module
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
 * The process loader is queried automatically so wdbg_symbolize can print a
 * module-relative address. Manual registration remains available for mapped
 * executable ranges that are not represented by the PE loader.
 *
 * The manual module table has a fixed capacity of 16 entries.
 */
int wdbg_register_module(const char *name, uint32_t base, uint32_t size);

/* Returns a module-relative string for `va`, or the raw hexadecimal address
 * if no module matches. Writes into `buf`. */
const char *wdbg_symbolize(uint32_t va, char *buf, int bufsz);

/* ── Init ───────────────────────────────────────────────────────
 *
 * Called once from win32_init(). Resets the toolkit. It installs no hooks or
 * application addresses unless an explicit diagnostic profile is selected.
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
