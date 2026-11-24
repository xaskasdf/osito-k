/*
 * wdbg.c — Win32 binary-debugging toolkit implementation.
 *
 * See wdbg.h for design rationale.
 *
 * All state is static / fixed-size — safe to use from INT2E context
 * before kmalloc is ready and without taking locks. Output is
 * line-buffered through serial_puts.
 */

#include "wdbg.h"
#include "compat32.h"
#include "dllloader.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t proc_current_cr3(void);

/* ── Module table ───────────────────────────────────────────── */

#define WDBG_MAX_MODULES 16

typedef struct {
    char     name[24];
    uint32_t base;
    uint32_t size;
    uint8_t  used;
} wdbg_module_t;

static wdbg_module_t g_modules[WDBG_MAX_MODULES];
static int g_module_count = 0;

static void wdbg_strncpy(char *dst, const char *src, int n)
{
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int wdbg_register_module(const char *name, uint32_t base, uint32_t size)
{
    if (g_module_count >= WDBG_MAX_MODULES) return -1;
    /* dedupe: same base → overwrite */
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].used && g_modules[i].base == base) {
            wdbg_strncpy(g_modules[i].name, name, sizeof g_modules[i].name);
            g_modules[i].size = size;
            return i;
        }
    }
    int idx = g_module_count++;
    wdbg_strncpy(g_modules[idx].name, name, sizeof g_modules[idx].name);
    g_modules[idx].base = base;
    g_modules[idx].size = size;
    g_modules[idx].used = 1;
    return idx;
}

static void hex_to_str(uint32_t v, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    out[10] = 0;
}

static const char *format_module_offset(const char *name, uint32_t base,
                                        uint32_t va, char *buf, int bufsz)
{
    uint32_t off = va - base;
    int p = 0;
    while (name && name[p] && p < bufsz - 12) {
        buf[p] = name[p];
        p++;
    }
    buf[p++] = '+';
    buf[p++] = '0';
    buf[p++] = 'x';
    static const char hex[] = "0123456789ABCDEF";
    int started = 0;
    for (int j = 7; j >= 0; j--) {
        uint8_t nibble = (off >> (j * 4)) & 0xF;
        if (nibble || started || j == 0) {
            buf[p++] = hex[nibble];
            started = 1;
        }
    }
    buf[p] = 0;
    return buf;
}

const char *wdbg_symbolize(uint32_t va, char *buf, int bufsz)
{
    if (bufsz < 16) { if (bufsz > 0) buf[0] = 0; return buf; }

    LOADED_MODULE *loaded =
        dll_find_module_by_address((PVOID)(uintptr_t)va);
    if (loaded) {
        uint32_t base = (uint32_t)(uintptr_t)loaded->image.ImageBase;
        return format_module_offset(loaded->name, base, va, buf, bufsz);
    }

    for (int i = 0; i < g_module_count; i++) {
        wdbg_module_t *m = &g_modules[i];
        if (!m->used) continue;
        if (va >= m->base && va - m->base < m->size)
            return format_module_offset(m->name, m->base, va, buf, bufsz);
    }
    hex_to_str(va, buf);
    return buf;
}

/* ── Address-site hooks ─────────────────────────────────────── */

#define WDBG_MAX_HOOKS 32

typedef struct {
    uint32_t      va_start;
    uint32_t      va_end;
    wdbg_hook_fn  cb;
    const char   *name;
    uint32_t      hit_count;
    uint8_t       used;
} wdbg_hook_t;

static wdbg_hook_t g_hooks[WDBG_MAX_HOOKS];
static int g_hook_count = 0;

int wdbg_addr_hook(uint32_t va_start, uint32_t va_end,
                   wdbg_hook_fn cb, const char *name)
{
    if (g_hook_count >= WDBG_MAX_HOOKS) return -1;
    if (va_end < va_start) va_end = va_start;
    int idx = g_hook_count++;
    g_hooks[idx].va_start = va_start;
    g_hooks[idx].va_end   = va_end;
    g_hooks[idx].cb       = cb;
    g_hooks[idx].name     = name;
    g_hooks[idx].hit_count = 0;
    g_hooks[idx].used     = 1;
    return idx;
}

void wdbg_check_caller(uint32_t ret_addr, uint32_t *stack_args)
{
    if (g_hook_count == 0 || !stack_args) return;
    /* ret_addr is the instruction AFTER the call → check va range
     * with small backward window since CALL is 5 bytes. */
    for (int i = 0; i < g_hook_count; i++) {
        wdbg_hook_t *h = &g_hooks[i];
        if (!h->used) continue;
        /* The hook is "we're entering this function" — i.e., the
         * caller pushed `ret_addr` and jumped to h->va_start. So we
         * don't compare ret_addr with va_start; we compare with
         * va_start..va_end. But the test we actually want is:
         * "was the called function h->va_start?". The caller's PC
         * isn't directly visible here — only the return address is.
         *
         * Reinterpretation: we hook on ret_addr in [va_start, va_end].
         * I.e. "right after a CALL inside this region returned". For
         * the throw helper case (CxxThrowException is NORETURN, so
         * ret_addr is never observed for the throw itself), what we
         * want is upstream: hook the GetPackageLinker function range,
         * and on any INT2E originating from within it, dump state.
         */
        if (ret_addr >= h->va_start && ret_addr <= h->va_end) {
            h->hit_count++;
            uint32_t esp = (uint32_t)(uintptr_t)stack_args;
            uint32_t ebp = compat32_get_last_user_ebp();
            h->cb(ret_addr, esp, ebp, stack_args);
        }
    }
}

static int va_readable(uint32_t va, int bytes)
{
    if (!va || bytes <= 0) return 0;
    uint32_t end = va + (uint32_t)bytes - 1U;
    if (end < va) return 0;

    uint64_t cr3 = proc_current_cr3();
    if (!cr3) return 0;
    uint64_t page = (uint64_t)va & ~0xFFFULL;
    uint64_t last = (uint64_t)end & ~0xFFFULL;
    for (;;) {
        if (paging_translate_in_cr3(cr3, page) == UINT64_MAX) return 0;
        if (page == last) break;
        page += 0x1000ULL;
    }
    return 1;
}

/* ── Stack walker ──────────────────────────────────────────── */

void wdbg_stack_walk(uint32_t ebp, int depth, const char *label)
{
    char sym[64];
    serial_puts("[WDBG/stk] ");
    if (label) { serial_puts(label); serial_puts(" "); }
    serial_puts("ebp="); serial_puthex(ebp, 8); serial_puts("\n");
    if (depth > 16) depth = 16;
    uint32_t fp = ebp;
    for (int i = 0; i < depth; i++) {
        if (!va_readable(fp, 8)) {
            serial_puts("  [#"); serial_putdec((uint64_t)i);
            serial_puts("] <unread fp="); serial_puthex(fp, 8); serial_puts(">\n");
            return;
        }
        uint32_t saved_ebp = *(volatile uint32_t *)(uintptr_t)fp;
        uint32_t ret_va    = *(volatile uint32_t *)(uintptr_t)(fp + 4);
        if (ret_va == 0) return;
        serial_puts("  [#"); serial_putdec((uint64_t)i);
        serial_puts("] "); serial_puts(wdbg_symbolize(ret_va, sym, sizeof sym));
        serial_puts(" (ebp="); serial_puthex(fp, 8);
        serial_puts(" ret="); serial_puthex(ret_va, 8); serial_puts(")\n");
        if (saved_ebp <= fp || saved_ebp - fp > 0x100000) return;  /* frame chain broken */
        fp = saved_ebp;
    }
}

/* ── Stack scanner ─────────────────────────────────────────── */

/* Returns 1 if `va` looks like a CALL-return address: it's in a
 * loaded or manually registered module AND the bytes immediately before it are
 * `E8 ?? ?? ?? ??` (relative CALL) or `FF ?? ...` (indirect CALL). */
static int looks_like_retaddr(uint32_t va)
{
    int in_module =
        dll_find_module_by_address((PVOID)(uintptr_t)va) != NULL;
    for (int i = 0; i < g_module_count; i++) {
        wdbg_module_t *m = &g_modules[i];
        if (m->used && va >= m->base && va - m->base < m->size) {
            in_module = 1;
            break;
        }
    }
    if (!in_module) return 0;
    /* Need to read [va-5..va-1] safely. */
    if (va < 5 || !va_readable(va - 5, 5)) return 0;
    uint8_t b5 = *(volatile uint8_t *)(uintptr_t)(va - 5);
    if (b5 == 0xE8) return 1;  /* CALL rel32 — 5 bytes total */
    /* CALL [r/m32] via 0xFF — usually 2-6 bytes; check b2..b1 for
     * the most common 2-byte form (FF 15 disp32 = 6 bytes total) */
    if (va >= 6 && va_readable(va - 6, 6)) {
        uint8_t b6 = *(volatile uint8_t *)(uintptr_t)(va - 6);
        if (b6 == 0xFF) return 1;
    }
    /* 2-byte indirect: FF D? / FF E? / FF 1? */
    if (va >= 2 && va_readable(va - 2, 2)) {
        uint8_t b2 = *(volatile uint8_t *)(uintptr_t)(va - 2);
        if (b2 == 0xFF) return 1;
    }
    /* 3-byte indirect: FF /r modrm with disp8 */
    if (va >= 3 && va_readable(va - 3, 3)) {
        uint8_t b3 = *(volatile uint8_t *)(uintptr_t)(va - 3);
        if (b3 == 0xFF) return 1;
    }
    return 0;
}

void wdbg_stack_scan(uint32_t esp, int depth, const char *label)
{
    char sym[64];
    serial_puts("[WDBG/scan] ");
    if (label) { serial_puts(label); serial_puts(" "); }
    serial_puts("esp="); serial_puthex(esp, 8);
    serial_puts(" depth="); serial_putdec((uint64_t)depth);
    serial_puts("\n");
    if (depth > 128) depth = 128;
    int found = 0;
    for (int i = 0; i < depth; i++) {
        uint32_t a = esp + i * 4;
        if (!va_readable(a, 4)) break;
        uint32_t v = *(volatile uint32_t *)(uintptr_t)a;
        if (looks_like_retaddr(v)) {
            serial_puts("  [+"); serial_putdec((uint64_t)(i * 4));
            serial_puts("] "); serial_puts(wdbg_symbolize(v, sym, sizeof sym));
            serial_puts(" (=0x"); serial_puthex(v, 8); serial_puts(")\n");
            found++;
        }
    }
    if (found == 0) {
        serial_puts("  (no return addresses found in window)\n");
    }
}

/* ── String dump utilities ─────────────────────────────────── */

void wdbg_print_ansi(uint32_t va)
{
    if (va == 0) { serial_puts("<null>"); return; }
    if (!va_readable(va, 1)) { serial_puts("<unread>"); return; }
    serial_puts("\"");
    char buf[256];
    int i = 0;
    while (i < 255) {
        if (!va_readable(va + i, 1)) break;
        char c = *(volatile char *)(uintptr_t)(va + i);
        if (c == 0) break;
        buf[i++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    buf[i] = 0;
    serial_puts(buf);
    serial_puts("\"");
}

void wdbg_print_wide(uint32_t va)
{
    if (va == 0) { serial_puts("<null>"); return; }
    if (!va_readable(va, 2)) { serial_puts("<unread>"); return; }
    serial_puts("L\"");
    char buf[256];
    int i = 0;
    while (i < 255) {
        if (!va_readable(va + i * 2, 2)) break;
        uint16_t w = *(volatile uint16_t *)(uintptr_t)(va + i * 2);
        if (w == 0) break;
        buf[i++] = (w >= 0x20 && w < 0x7F) ? (char)w : '?';
    }
    buf[i] = 0;
    serial_puts(buf);
    serial_puts("\"");
}

void wdbg_init(void)
{
    for (int i = 0; i < WDBG_MAX_MODULES; i++)
        g_modules[i].used = 0;
    for (int i = 0; i < WDBG_MAX_HOOKS; i++)
        g_hooks[i].used = 0;
    g_module_count = 0;
    g_hook_count = 0;
    serial_puts("[WDBG] ready: loader-backed symbols, no default hooks\n");
}
