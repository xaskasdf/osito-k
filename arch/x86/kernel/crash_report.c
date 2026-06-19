/*
 * OsitoK x86-64 — Structured Crash Report Engine
 *
 * Captures full process state on exception and saves to OsitoFS.
 * Called from idt.c before proc_exception_kill().
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void *proc_current(void);
extern bool  user_symbolize(void *p, uint64_t addr,
                            const char **name, uint64_t *off);
extern uint64_t idt_get_ticks(void);

/* Forward decls for OsitoFS write (may not be available early boot) */
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_delete(const char *name);
extern void  boot_diag_flush(const char *reason);
extern int   boot_diag_slot(void);
extern uint32_t boot_diag_next_crash_index(void);
extern void  boot_diag_format_crash_name(char *out, int slot, uint32_t idx,
                                         const char *ext);

/* ── Crash report structure ─────────────────────────────────── */

#define CRASH_MAGIC  0x4F534B43  /* "OSKC" */
#define CRASH_MAX_FRAMES 32

static const char *crash_vector_name(uint32_t vector)
{
    switch (vector) {
    case 0:  return "divide error";
    case 1:  return "debug";
    case 2:  return "nmi";
    case 3:  return "breakpoint";
    case 4:  return "overflow";
    case 5:  return "bounds";
    case 6:  return "invalid opcode";
    case 7:  return "device not available";
    case 8:  return "double fault";
    case 10: return "invalid tss";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection";
    case 14: return "page fault";
    case 16: return "x87 floating point";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "simd floating point";
    case 20: return "virtualization";
    case 21: return "control protection";
    default: return "unknown";
    }
}

static char *cr_append_str(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *cr_append_dec(char *p, char *end, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        if (p < end) *p++ = '0';
        return p;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0 && p < end) *p++ = tmp[n];
    return p;
}

static char *cr_append_hex(char *p, char *end, uint64_t v, int digits)
{
    static const char h[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0 && p < end; i--)
        *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

typedef struct {
    uint64_t addr;
    char     symbol[64];
    uint64_t offset;
} crash_frame_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* Process info */
    char     name[64];
    uint32_t pid;
    uint64_t uptime_ticks;

    /* Exception info */
    uint32_t vector;
    uint64_t error_code;
    uint64_t fault_addr;      /* CR2 for page faults */

    /* Registers */
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags, cs, ss;

    /* Symbolized backtrace */
    uint32_t frame_count;
    crash_frame_t frames[CRASH_MAX_FRAMES];

    /* Code bytes around RIP */
    uint8_t  code_before[32];
    uint8_t  code_after[32];
    uint64_t code_base;       /* RIP - 32 */
    bool     code_valid;

    /* Stack memory dump — 256 quadwords starting at RSP. Lets us recover
     * the failed-CALL return address (always at *RSP) and shallow callers
     * even when RBP is invalid or the frame walker can't find them. */
    uint64_t stack_base;      /* RSP at crash time */
    uint64_t stack_words[256];
    bool     stack_valid;
} crash_report_t;

/* ── Static report buffer (avoid allocation in crash context) ── */

static crash_report_t crash_buf;
static char crash_text[8192];

static uint32_t crash_report_text(char *out, uint32_t cap, const crash_report_t *r)
{
    char *p = out;
    char *end = out + cap - 1;

    p = cr_append_str(p, end, "OsitoK crash report v");
    p = cr_append_dec(p, end, r->version);
    p = cr_append_str(p, end, "\nprocess: ");
    p = cr_append_str(p, end, r->name[0] ? r->name : "(unknown)");
    p = cr_append_str(p, end, "\npid: ");
    p = cr_append_dec(p, end, r->pid);
    p = cr_append_str(p, end, "\nuptime_ticks: ");
    p = cr_append_dec(p, end, r->uptime_ticks);
    p = cr_append_str(p, end, "\nvector: ");
    p = cr_append_dec(p, end, r->vector);
    p = cr_append_str(p, end, " (");
    p = cr_append_str(p, end, crash_vector_name(r->vector));
    p = cr_append_str(p, end, ")\nerror_code: 0x");
    p = cr_append_hex(p, end, r->error_code, 16);
    p = cr_append_str(p, end, "\nfault_addr: 0x");
    p = cr_append_hex(p, end, r->fault_addr, 16);
    p = cr_append_str(p, end, "\n\nregisters:\n");

    p = cr_append_str(p, end, "  rip=0x"); p = cr_append_hex(p, end, r->rip, 16);
    p = cr_append_str(p, end, " rsp=0x"); p = cr_append_hex(p, end, r->rsp, 16);
    p = cr_append_str(p, end, " rbp=0x"); p = cr_append_hex(p, end, r->rbp, 16);
    p = cr_append_str(p, end, "\n  rax=0x"); p = cr_append_hex(p, end, r->rax, 16);
    p = cr_append_str(p, end, " rbx=0x"); p = cr_append_hex(p, end, r->rbx, 16);
    p = cr_append_str(p, end, " rcx=0x"); p = cr_append_hex(p, end, r->rcx, 16);
    p = cr_append_str(p, end, " rdx=0x"); p = cr_append_hex(p, end, r->rdx, 16);
    p = cr_append_str(p, end, "\n  rsi=0x"); p = cr_append_hex(p, end, r->rsi, 16);
    p = cr_append_str(p, end, " rdi=0x"); p = cr_append_hex(p, end, r->rdi, 16);
    p = cr_append_str(p, end, " rflags=0x"); p = cr_append_hex(p, end, r->rflags, 16);
    p = cr_append_str(p, end, "\n  r8 =0x"); p = cr_append_hex(p, end, r->r8, 16);
    p = cr_append_str(p, end, " r9 =0x"); p = cr_append_hex(p, end, r->r9, 16);
    p = cr_append_str(p, end, " r10=0x"); p = cr_append_hex(p, end, r->r10, 16);
    p = cr_append_str(p, end, " r11=0x"); p = cr_append_hex(p, end, r->r11, 16);
    p = cr_append_str(p, end, "\n  r12=0x"); p = cr_append_hex(p, end, r->r12, 16);
    p = cr_append_str(p, end, " r13=0x"); p = cr_append_hex(p, end, r->r13, 16);
    p = cr_append_str(p, end, " r14=0x"); p = cr_append_hex(p, end, r->r14, 16);
    p = cr_append_str(p, end, " r15=0x"); p = cr_append_hex(p, end, r->r15, 16);
    p = cr_append_str(p, end, "\n  cs=0x"); p = cr_append_hex(p, end, r->cs, 4);
    p = cr_append_str(p, end, " ss=0x"); p = cr_append_hex(p, end, r->ss, 4);

    p = cr_append_str(p, end, "\n\nbacktrace:\n");
    for (uint32_t i = 0; i < r->frame_count && i < CRASH_MAX_FRAMES; i++) {
        p = cr_append_str(p, end, "  #");
        p = cr_append_dec(p, end, i);
        p = cr_append_str(p, end, " 0x");
        p = cr_append_hex(p, end, r->frames[i].addr, 16);
        if (r->frames[i].symbol[0]) {
            p = cr_append_str(p, end, " ");
            p = cr_append_str(p, end, r->frames[i].symbol);
            p = cr_append_str(p, end, "+0x");
            p = cr_append_hex(p, end, r->frames[i].offset, 4);
        }
        p = cr_append_str(p, end, "\n");
    }

    if (r->code_valid) {
        p = cr_append_str(p, end, "\ncode bytes:\n  base=0x");
        p = cr_append_hex(p, end, r->code_base, 16);
        p = cr_append_str(p, end, "\n  before:");
        for (int i = 0; i < 32; i++) {
            p = cr_append_str(p, end, " ");
            p = cr_append_hex(p, end, r->code_before[i], 2);
        }
        p = cr_append_str(p, end, "\n  at_rip:");
        for (int i = 0; i < 32; i++) {
            p = cr_append_str(p, end, " ");
            p = cr_append_hex(p, end, r->code_after[i], 2);
        }
        p = cr_append_str(p, end, "\n");
    }

    if (r->stack_valid) {
        p = cr_append_str(p, end, "\nstack top:\n  base=0x");
        p = cr_append_hex(p, end, r->stack_base, 16);
        p = cr_append_str(p, end, "\n");
        for (int i = 0; i < 16; i++) {
            p = cr_append_str(p, end, "  [");
            p = cr_append_dec(p, end, (uint64_t)i);
            p = cr_append_str(p, end, "] 0x");
            p = cr_append_hex(p, end, r->stack_words[i], 16);
            p = cr_append_str(p, end, "\n");
        }
    }

    *p = '\0';
    return (uint32_t)(p - out);
}

/* ── Build and save crash report ────────────────────────────── */

/* interrupt_frame_t layout from idt.c (stub-pushed GPRs + CPU IRET frame):
 * [0]=R15 [1]=R14 [2]=R13 [3]=R12 [4]=R11 [5]=R10 [6]=R9 [7]=R8
 * [8]=RBP [9]=RDI [10]=RSI [11]=RDX [12]=RCX [13]=RBX [14]=RAX
 * [15]=vec [16]=error_code [17]=RIP [18]=CS [19]=RFLAGS [20]=RSP [21]=SS */

void crash_report_save(uint64_t *frame, uint32_t vector, uint64_t fault_addr,
                       const char *proc_name, uint32_t pid)
{
    crash_report_t *r = &crash_buf;
    memset(r, 0, sizeof(*r));

    r->magic   = CRASH_MAGIC;
    r->version = 2;
    r->vector  = vector;
    r->error_code = frame[16];
    r->fault_addr = fault_addr;
    r->uptime_ticks = idt_get_ticks();
    r->pid = pid;

    /* Copy name */
    if (proc_name) {
        int i = 0;
        while (proc_name[i] && i < 63) { r->name[i] = proc_name[i]; i++; }
        r->name[i] = '\0';
    }

    /* Copy registers */
    r->r15 = frame[0];  r->r14 = frame[1];  r->r13 = frame[2];  r->r12 = frame[3];
    r->r11 = frame[4];  r->r10 = frame[5];  r->r9  = frame[6];  r->r8  = frame[7];
    r->rbp = frame[8];  r->rdi = frame[9];  r->rsi = frame[10]; r->rdx = frame[11];
    r->rcx = frame[12]; r->rbx = frame[13]; r->rax = frame[14];
    r->rip = frame[17]; r->cs  = frame[18]; r->rflags = frame[19];
    r->rsp = frame[20]; r->ss  = frame[21];

    /* Capture code bytes around RIP (native ELF user mode only) */
    if ((r->cs & 0xFFFF) == 0x28 && r->rip > 32 && r->rip < 0x800000000000ULL) {
        r->code_base = r->rip - 32;
        r->code_valid = true;
        /* Safe read: these pages should be mapped since the process just
         * executed from them. If they fault, the read simply stays zero. */
        uint8_t *p = (uint8_t *)r->code_base;
        for (int i = 0; i < 32; i++) r->code_before[i] = p[i];
        p = (uint8_t *)r->rip;
        for (int i = 0; i < 32; i++) r->code_after[i] = p[i];
    }

    /* Capture stack memory at RSP. *RSP is the return address from a failed
     * CALL — invaluable when the bug is a NULL function pointer. */
    if (r->rsp != 0 && (r->rsp & 7) == 0) {
        r->stack_base = r->rsp;
        r->stack_valid = true;
        uint64_t *sp = (uint64_t *)r->rsp;
        /* Do NOT read past the current page: a small pthread stack (worker /
         * render thread) places an unmapped guard page right above the top,
         * so an unconditional 2 KB read faults the crash handler itself and
         * cascades to a halt — losing the report for the very crash we care
         * about. Stop at the page boundary; zero-fill the rest. */
        uint64_t page_end = (r->rsp + 0x1000ULL) & ~0xFFFULL;
        int max_words = (int)((page_end - r->rsp) / 8);
        if (max_words > 256) max_words = 256;
        for (int i = 0; i < max_words; i++) r->stack_words[i] = sp[i];
        for (int i = max_words; i < 256; i++) r->stack_words[i] = 0;
    }

    /* Walk backtrace (same algorithm as idt.c but captures into struct) */
    void *proc = proc_current();
    uint64_t rbp = r->rbp;
    uint64_t rsp = r->rsp;
    uint64_t win = 8ULL * 1024 * 1024;

    /* Frame #0 is the crash RIP itself */
    r->frames[0].addr = r->rip;
    const char *nm = 0; uint64_t off = 0;
    if (user_symbolize(proc, r->rip, &nm, &off) && nm) {
        int j = 0;
        while (nm[j] && j < 63) { r->frames[0].symbol[j] = nm[j]; j++; }
        r->frames[0].offset = off;
    }
    r->frame_count = 1;

    /* Confine rbp-chain reads to rsp's own page. A small pthread stack (worker/
     * render thread) has an unmapped guard page adjacent to the live frame; a
     * loose [rsp-256, rsp+8MB] window let the walker deref into it and fault the
     * crash handler again (crash_report_save+0x300). Staying within the page we
     * already know is mapped (rsp's) yields a shallow-but-safe backtrace. */
    (void)win;
    uint64_t rsp_page_end = (rsp | 0xFFFULL) + 1;
    if ((r->cs & 0xFFFF) == 0x28) {
        for (int d = 0; d < CRASH_MAX_FRAMES - 1; d++) {
            if (rbp == 0 || (rbp & 7) || rbp + 16 < rbp) break;
            if (rbp < rsp || rbp + 16 > rsp_page_end) break;

            uint64_t ret = ((uint64_t *)rbp)[1];
            uint64_t prev = ((uint64_t *)rbp)[0];

            crash_frame_t *f = &r->frames[r->frame_count];
            f->addr = ret;
            nm = 0; off = 0;
            if (user_symbolize(proc, ret, &nm, &off) && nm) {
                int j = 0;
                while (nm[j] && j < 63) { f->symbol[j] = nm[j]; j++; }
                f->offset = off;
            }
            r->frame_count++;

            if (prev <= rbp) break;
            rbp = prev;
        }
    }

    /* Print formatted crash box to serial */
    serial_puts("\n");
    serial_puts("  +----- CRASH REPORT --------------------------------+\n");
    serial_puts("  | Process: ");
    serial_puts(r->name[0] ? r->name : "(unknown)");
    serial_puts(" (PID ");
    serial_putdec(r->pid);
    serial_puts(")\n");
    serial_puts("  | Uptime:  ");
    serial_putdec(r->uptime_ticks / 100);
    serial_puts(".");
    serial_putdec(r->uptime_ticks % 100);
    serial_puts("s\n");
    serial_puts("  |\n");

    serial_puts("  | Backtrace:\n");
    for (uint32_t i = 0; i < r->frame_count; i++) {
        serial_puts("  |   #");
        serial_putdec(i);
        serial_puts("  0x");
        serial_puthex(r->frames[i].addr, 12);
        if (r->frames[i].symbol[0]) {
            serial_puts("  ");
            serial_puts(r->frames[i].symbol);
            serial_puts("+0x");
            serial_puthex(r->frames[i].offset, 4);
        }
        serial_puts("\n");
    }

    if (r->code_valid) {
        serial_puts("  |\n  | Code @ RIP:\n  |   ");
        for (int i = 0; i < 16; i++) {
            serial_puthex(r->code_after[i], 2);
            serial_puts(" ");
        }
        serial_puts("\n");
    }

    if (r->stack_valid) {
        serial_puts("  |\n  | Stack @ RSP (top 8 qwords, [0]=return-addr from failed CALL):\n");
        for (int i = 0; i < 8; i++) {
            serial_puts("  |   [");
            serial_putdec(i);
            serial_puts("] 0x");
            serial_puthex(r->stack_words[i], 16);
            serial_puts("\n");
        }
    }

    serial_puts("  +--------------------------------------------------+\n\n");

    boot_diag_flush("crash");

    /* Save to OsitoFS under the current boot slot. */
    int slot = boot_diag_slot();
    uint32_t crash_idx = boot_diag_next_crash_index();
    char fname[32];
    char tname[32];
    boot_diag_format_crash_name(fname, slot, crash_idx, "bin");
    boot_diag_format_crash_name(tname, slot, crash_idx, "txt");

    osfs2_delete(fname);
    void *file = osfs2_create(fname, sizeof(crash_report_t));
    if (file) {
        extern uint64_t osfs2_file_byte_offset(void *file);
        uint64_t off = osfs2_file_byte_offset(file);
        serial_puts("[CRASH] osfs2_create OK, file_byte_offset=0x");
        serial_puthex(off, 16);
        serial_puts("\n");

        /* Verify we have non-zero bytes BEFORE write. */
        serial_puts("[CRASH] crash_buf magic=0x");
        serial_puthex(r->magic, 8);
        serial_puts(" rip=0x");
        serial_puthex(r->rip, 16);
        serial_puts("\n");

        int wrc = osfs2_write(file, 0, r, sizeof(crash_report_t));
        serial_puts("[CRASH] osfs2_write returned ");
        serial_putdec(wrc < 0 ? (uint64_t)-wrc : (uint64_t)wrc);
        serial_puts(wrc == 0 ? " (ok)\n" : " (FAIL)\n");

        if (wrc == 0) {
            serial_puts("[CRASH] Report saved: ");
            serial_puts(fname);
            serial_puts("\n");
        }
    } else {
        serial_puts("[CRASH] osfs2_create FAILED\n");
    }

    uint32_t txt_len = crash_report_text(crash_text, sizeof(crash_text), r);
    osfs2_delete(tname);
    file = osfs2_create(tname, txt_len);
    if (file && osfs2_write(file, 0, crash_text, txt_len) == 0) {
        serial_puts("[CRASH] Summary saved: ");
        serial_puts(tname);
        serial_puts("\n");
    } else {
        serial_puts("[CRASH] summary save FAILED\n");
    }

    boot_diag_flush("crash-saved");
}
