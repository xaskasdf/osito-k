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

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

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

const char *wdbg_symbolize(uint32_t va, char *buf, int bufsz)
{
    if (bufsz < 16) { if (bufsz > 0) buf[0] = 0; return buf; }
    for (int i = 0; i < g_module_count; i++) {
        wdbg_module_t *m = &g_modules[i];
        if (!m->used) continue;
        if (va >= m->base && va < m->base + m->size) {
            uint32_t off = va - m->base;
            int p = 0;
            while (m->name[p] && p < bufsz - 12) { buf[p] = m->name[p]; p++; }
            buf[p++] = '+';
            buf[p++] = '0'; buf[p++] = 'x';
            static const char hex[] = "0123456789ABCDEF";
            int started = 0;
            for (int j = 7; j >= 0; j--) {
                uint8_t nibble = (off >> (j * 4)) & 0xF;
                if (nibble || started || j == 0) { buf[p++] = hex[nibble]; started = 1; }
            }
            buf[p] = 0;
            return buf;
        }
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

/* Forward decl — values populated by int2e_stub.S each entry */
extern uint64_t g_int2e_user_rbp;

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
            uint32_t ebp = (uint32_t)g_int2e_user_rbp;
            h->cb(ret_addr, esp, ebp, stack_args);
        }
    }
}

/* ── FName resolver ─────────────────────────────────────────── */

static uint32_t g_fname_tarray_va = 0x10295D30;  /* UT99 default */
static int g_fname_entry_name_off = 8;            /* FNameEntry.Name @+8 */

void wdbg_fname_set_array(uint32_t va) { g_fname_tarray_va = va; }

static int va_readable(uint32_t va, int bytes)
{
    /* Coarse readability check: must be inside known module ranges
     * or known heap/IAT zones. For now, accept anything in
     * [0x10000, 0x80000000) — bare-metal compat32 mapping. */
    if (va < 0x10000) return 0;
    if (va + bytes < va) return 0;
    if (va + bytes >= 0x80000000u) return 0;
    return 1;
}

const char *wdbg_fname_resolve(uint32_t idx)
{
    if (!va_readable(g_fname_tarray_va, 12)) return "<no-arr>";
    volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)g_fname_tarray_va;
    uint32_t data = t[0];
    uint32_t num  = t[1];
    if (data == 0) return "<null>";
    if (idx >= num) return "<oob>";
    if (!va_readable(data + idx * 4, 4)) return "<bad-arr>";
    uint32_t entry = *(volatile uint32_t *)(uintptr_t)(data + idx * 4);
    if (entry == 0) return "<nullent>";
    if (!va_readable(entry + g_fname_entry_name_off, 1)) return "<bad-ent>";
    return (const char *)(uintptr_t)(entry + g_fname_entry_name_off);
}

/* ── UObject inspector ──────────────────────────────────────── */

static int g_uobj_off_outer = 16;
static int g_uobj_off_name  = 20;
static int g_uobj_off_class = 24;

void wdbg_uobject_set_offsets(int outer, int name, int klass)
{
    g_uobj_off_outer = outer;
    g_uobj_off_name  = name;
    g_uobj_off_class = klass;
}

void wdbg_uobject_dump(uint32_t obj_va, const char *label)
{
    serial_puts("[WDBG/UObj] ");
    if (label) { serial_puts(label); serial_puts(" "); }
    if (obj_va == 0) { serial_puts("<null>\n"); return; }
    if (!va_readable(obj_va, 32)) {
        serial_puts("<unread va=0x");
        serial_puthex(obj_va, 8);
        serial_puts(">\n");
        return;
    }
    serial_puts("va="); serial_puthex(obj_va, 8);

    uint32_t name_idx = *(volatile uint32_t *)(uintptr_t)(obj_va + g_uobj_off_name);
    uint32_t klass    = *(volatile uint32_t *)(uintptr_t)(obj_va + g_uobj_off_class);
    uint32_t outer    = *(volatile uint32_t *)(uintptr_t)(obj_va + g_uobj_off_outer);

    serial_puts(" name="); serial_puts(wdbg_fname_resolve(name_idx));
    serial_puts(" (idx=");  serial_putdec((uint64_t)name_idx);
    serial_puts(") cls="); serial_puthex(klass, 8);
    if (klass && va_readable(klass + g_uobj_off_name, 4)) {
        uint32_t cls_name_idx = *(volatile uint32_t *)(uintptr_t)(klass + g_uobj_off_name);
        serial_puts("("); serial_puts(wdbg_fname_resolve(cls_name_idx)); serial_puts(")");
    }
    serial_puts(" outer="); serial_puthex(outer, 8);
    serial_puts("\n");
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

/* ── Default hooks ─────────────────────────────────────────── */

/*
 * Throw-helper region hook — Core.dll @0x1014BD10.
 *
 * Since the throw is NORETURN, we cannot observe a ret_addr equal to
 * the throw helper itself. Instead, we hook the *enclosing* function
 * (GetPackageLinker or its caller — UE1 typical layout puts the
 * package lookup in a ~0x200 byte function). The first time an
 * INT2E originates from somewhere in this range, dump full state.
 *
 * Range chosen: 0x1014BB00..0x1014BE00 — empirical estimate for the
 * function containing the throw at 0x1014BD10. Adjust after first
 * run shows the actual ret_addrs.
 */
static int g_throw_hook_fired = 0;

static void throw_caller_dump(uint32_t va, uint32_t esp, uint32_t ebp,
                              const uint32_t *stack_args)
{
    /* Throttle: dump first 5 hits with full detail, then count silently */
    g_throw_hook_fired++;
    if (g_throw_hook_fired > 5) {
        if ((g_throw_hook_fired % 1000) == 0) {
            serial_puts("[WDBG/throw] still firing: hits=");
            serial_putdec((uint64_t)g_throw_hook_fired);
            serial_puts("\n");
        }
        return;
    }

    char sym[64], sym2[64];
    serial_puts("[WDBG/throw#");
    serial_putdec((uint64_t)g_throw_hook_fired);
    serial_puts("] ret_va=");
    serial_puts(wdbg_symbolize(va, sym, sizeof sym));
    serial_puts(" esp="); serial_puthex(esp, 8);
    serial_puts(" ebp="); serial_puthex(ebp, 8);
    serial_puts("\n");

    /* Empirical post-disasm layout at INT2E inside CxxThrowException:
     *   stack_args[0] = exc-object pointer (FFileException*)
     *   stack_args[1] = throwInfo (0x1017D4B0)
     *   stack_args[2] = exc vtbl (0x10278FF4) — was the helper's local slot
     *   stack_args[3] = RET_helper — caller of the throw helper @0x1014BD10
     *   stack_args[4+] = caller's locals/args
     *
     * Surface stack_args[3] explicitly — that's the gold.
     */
    serial_puts("[WDBG/throw#");
    serial_putdec((uint64_t)g_throw_hook_fired);
    serial_puts("] throw-caller=");
    serial_puts(wdbg_symbolize(stack_args[3], sym2, sizeof sym2));
    serial_puts(" exc-obj="); serial_puthex(stack_args[0], 8);
    serial_puts(" exc-vtbl="); serial_puthex(stack_args[2], 8);
    serial_puts("\n");

    /* Full args window for forensics. */
    serial_puts("[WDBG/throw#");
    serial_putdec((uint64_t)g_throw_hook_fired);
    serial_puts("] args: ");
    for (int i = 0; i < 8; i++) {
        if (!va_readable(esp + i * 4, 4)) break;
        serial_puts("[+"); serial_putdec((uint64_t)(i * 4));
        serial_puts("]="); serial_puthex(stack_args[i], 8);
        serial_puts(" ");
    }
    serial_puts("\n");

    /* Engine state at throw time. */
    extern uint64_t g_int2e_user_rcx, g_int2e_user_rdx;
    extern uint64_t g_int2e_user_rsi, g_int2e_user_rdi, g_int2e_user_rbx;
    serial_puts("[WDBG/throw#");
    serial_putdec((uint64_t)g_throw_hook_fired);
    serial_puts("] regs: ECX="); serial_puthex((uint32_t)g_int2e_user_rcx, 8);
    serial_puts(" EDX="); serial_puthex((uint32_t)g_int2e_user_rdx, 8);
    serial_puts(" EBX="); serial_puthex((uint32_t)g_int2e_user_rbx, 8);
    serial_puts(" ESI="); serial_puthex((uint32_t)g_int2e_user_rsi, 8);
    serial_puts(" EDI="); serial_puthex((uint32_t)g_int2e_user_rdi, 8);
    serial_puts("\n");

    /* Dump the FFileException object @ stack_args[0]. Layout in UE1
     * typically has:
     *   +0  vtbl
     *   +4..  inherited FException::Msg ANSICHAR[1024] OR a pointer
     *   +N  filename / errno / context
     * Probe both ANSI in-place and a possible pointer at +4. */
    uint32_t exc = stack_args[0];
    if (va_readable(exc, 16)) {
        serial_puts("[WDBG/throw#");
        serial_putdec((uint64_t)g_throw_hook_fired);
        serial_puts("] exc-obj+0..+12: ");
        for (int i = 0; i < 4; i++) {
            uint32_t w = *(volatile uint32_t *)(uintptr_t)(exc + i * 4);
            serial_puts("[+"); serial_putdec((uint64_t)(i * 4));
            serial_puts("]="); serial_puthex(w, 8); serial_puts(" ");
        }
        serial_puts("\n");
        /* Try reading exc+4 as inline ANSI string (FException::Msg) */
        serial_puts("[WDBG/throw#");
        serial_putdec((uint64_t)g_throw_hook_fired);
        serial_puts("] exc-msg-inline=");
        wdbg_print_ansi(exc + 4);
        serial_puts("\n");
    }

    wdbg_stack_walk(ebp, 6, "throw-callers");
}

void wdbg_init(void)
{
    /* Pre-register UT99 module ranges (empirically observed). These
     * can be overwritten later by pe.c's actual load addresses. */
    wdbg_register_module("Core.dll",   0x10100000, 0x00200000);
    wdbg_register_module("Engine.dll", 0x10300000, 0x00200000);
    wdbg_register_module("UT.exe",     0x10900000, 0x00200000);

    /* Hook on the GetPackageLinker enclosing region. The throw at
     * 0x1014BD10 is NORETURN, so we observe ret_addrs that fall
     * inside its caller's basic block — typically the function body
     * around the call site. Cast a wide net first; tighten after
     * empirical data. */
    wdbg_addr_hook(0x1014BB00, 0x1014BF00,
                   throw_caller_dump,
                   "GetPackageLinker-region");

    serial_puts("[WDBG] init: 3 modules, 1 hook registered\n");
}
