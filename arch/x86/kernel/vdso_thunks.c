/*
 * OsitoK x86-64 — VDSO Code Thunks
 *
 * Small machine-code stubs mapped into user address space at
 * VDSO_CODE_VA (page after the VDSO data page). When the ELF
 * dynamic linker resolves clock_gettime or gettimeofday, it can
 * point the GOT entry here instead of the libc wrapper, avoiding
 * the SYSCALL trap entirely.
 *
 * The thunks read from the VDSO data page (0x7FFFE000) using
 * the seqlock protocol.
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_pages(uint64_t count);
extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);

#define VDSO_DATA_VA  0x7FFFE000ULL
#define VDSO_CODE_VA  0x7FFFF000ULL  /* page after data */

#define PTE_PRESENT_V (1ULL << 0)
#define PTE_USER_V    (1ULL << 2)

static void    *vdso_code_phys;
static uint8_t *vdso_code_page;  /* kernel writable view */

/* ── Thunk machine code ──────────────────────────────────────
 *
 * clock_gettime_thunk:
 *   Reads monotonic_ns from VDSO data page with seqlock retry.
 *   Writes result to user's struct timespec at [rsi].
 *   Returns 0 in rax.
 *
 *   ABI: rdi = clk_id (ignored), rsi = timespec *tp
 *
 *   Layout of vdso_data_t at 0x7FFFE000:
 *     +0x00: seq (uint32_t, volatile)
 *     +0x08: monotonic_ns (uint64_t)
 *
 *   struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 * ─────────────────────────────────────────────────────────── */

static const uint8_t clock_gettime_code[] = {
    /* retry: */
    0x8B, 0x04, 0x25, 0x00, 0xE0, 0xFF, 0x7F,  /* mov eax, [0x7FFFE000] ; seq */
    0xA8, 0x01,                                   /* test al, 1 */
    0x75, 0xF5,                                   /* jnz retry */
    0x89, 0xC1,                                   /* mov ecx, eax ; save seq */
    0x0F, 0xAE, 0xE8,                             /* lfence */
    0x48, 0x8B, 0x14, 0x25, 0x08, 0xE0, 0xFF, 0x7F, /* mov rdx, [0x7FFFE008] ; monotonic_ns */
    0x0F, 0xAE, 0xE8,                             /* lfence */
    0x8B, 0x04, 0x25, 0x00, 0xE0, 0xFF, 0x7F,     /* mov eax, [0x7FFFE000] ; re-read seq */
    0x39, 0xC8,                                    /* cmp eax, ecx */
    0x75, 0xDD,                                    /* jne retry */
    /* rdx = monotonic_ns. Split into sec + nsec. */
    0x48, 0xB8, 0x00, 0xCA, 0x9A, 0x3B, 0x00, 0x00, 0x00, 0x00, /* mov rax, 1000000000 */
    0x48, 0x89, 0xD0,                              /* mov rax, rdx (save ns) */
    /* Simple: store ns/1e9 as sec, ns%1e9 as nsec */
    /* Actually, let's just store raw ns and let user interpret */
    /* For compatibility: tv_sec = ns / 1000000000, tv_nsec = ns % 1000000000 */
    0x48, 0xB9, 0x00, 0xCA, 0x9A, 0x3B, 0x00, 0x00, 0x00, 0x00, /* mov rcx, 1000000000 */
    0x31, 0xD2,                                    /* xor edx, edx */
    0x48, 0xF7, 0xF1,                              /* div rcx ; rax=sec, rdx=nsec */
    0x48, 0x89, 0x06,                              /* mov [rsi], rax    ; tv_sec */
    0x48, 0x89, 0x56, 0x08,                        /* mov [rsi+8], rdx  ; tv_nsec */
    0x31, 0xC0,                                    /* xor eax, eax ; return 0 */
    0xC3,                                          /* ret */
};

#define THUNK_CLOCK_GETTIME_OFF  0
#define THUNK_CLOCK_GETTIME_SIZE sizeof(clock_gettime_code)

/* ── Initialize VDSO code page ─────────────────────────────── */

void vdso_thunks_init(void)
{
    vdso_code_phys = mem_alloc_pages(1);
    if (!vdso_code_phys) {
        serial_puts("[VDSO] Thunk page alloc failed\n");
        return;
    }
    vdso_code_page = (uint8_t *)PHYS_TO_VIRT(vdso_code_phys);
    memset(vdso_code_page, 0xCC, 4096);  /* INT3 fill for safety */

    /* Copy thunk code */
    memcpy(vdso_code_page + THUNK_CLOCK_GETTIME_OFF,
           clock_gettime_code, THUNK_CLOCK_GETTIME_SIZE);

    serial_puts("[VDSO] Thunk page ready: clock_gettime at 0x");
    serial_puthex(VDSO_CODE_VA + THUNK_CLOCK_GETTIME_OFF, 12);
    serial_puts("\n");
}

/* Map VDSO code page into a process (called from proc_exec) */
int vdso_thunks_map_process(uint64_t cr3)
{
    if (!vdso_code_phys) return -1;
    /* Map as present + user + read (execute comes from NX=0 default) */
    return paging_map_page_in_cr3(cr3, VDSO_CODE_VA,
                                  (uint64_t)vdso_code_phys,
                                  PTE_PRESENT_V | PTE_USER_V);
}

/* Return the user-space address of the clock_gettime thunk.
 * The ELF dynamic linker can use this to replace GOT entries. */
uint64_t vdso_get_clock_gettime(void)
{
    return vdso_code_phys ? (VDSO_CODE_VA + THUNK_CLOCK_GETTIME_OFF) : 0;
}
