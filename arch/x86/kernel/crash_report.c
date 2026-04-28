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

/* ── Crash report structure ─────────────────────────────────── */

#define CRASH_MAGIC  0x4F534B43  /* "OSKC" */
#define CRASH_MAX_FRAMES 32

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
} crash_report_t;

/* ── Static report buffer (avoid allocation in crash context) ── */

static crash_report_t crash_buf;
static uint32_t crash_count;

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
    r->version = 1;
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

    if ((r->cs & 0xFFFF) == 0x28) {
        for (int d = 0; d < CRASH_MAX_FRAMES - 1; d++) {
            if (rbp == 0 || (rbp & 7) || rbp + 16 < rbp) break;
            if (rbp < rsp - 256 || rbp > rsp + win) break;

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

    serial_puts("  +--------------------------------------------------+\n\n");

    /* Save to OsitoFS as crash_<pid>.bin */
    crash_count++;
    char fname[32] = "crash_000.bin";
    fname[6] = '0' + (crash_count / 100) % 10;
    fname[7] = '0' + (crash_count / 10) % 10;
    fname[8] = '0' + crash_count % 10;

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
}
