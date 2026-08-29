/*
 * OsitoK x86-64 — ELF64 Loader
 *
 * X-OS5: Load ELF64 binaries from OsitoFS into memory and execute them.
 * Handles PT_LOAD segments, sets up user stack with argc/argv/envp,
 * jumps to entry point.
 *
 * For now: ring-0 execution (no user/kernel separation yet).
 * With X-OS6 (processes), this will create a new address space.
 */

#include "../include/types.h"
#include "../include/dynlink.h"
#include "../include/paging.h"
#include "../fs/vfs.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern int   mem_reserve_range(uint64_t phys, uint64_t count);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);

/* VFS */

/* Process — register memory for cleanup on exit */
extern void proc_add_region(void *base, uint64_t pages);
extern void proc_add_region_virt(void *base, uint64_t pages, uint64_t virt_base);
extern void *proc_current(void);
extern void  user_symtab_set(void *pp,
                             void *symtab, uint64_t symtab_size,
                             char *strtab, uint64_t strtab_size,
                             uint64_t load_bias);

/* ── ELF64 structures ───────────────────────────────────────── */

#define EI_NIDENT   16

#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_X86_64   62
#define ET_EXEC     2
#define ET_DYN      3

#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_NOTE     4
#define PT_TLS      7
#define PT_INTERP   3
#define PT_PHDR     6

/* NT_GNU_ABI_TAG note: glibc checks this at startup to verify
 * minimum kernel version. We patch it to accept OsitoK. */
#define NT_GNU_ABI_TAG  1

#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_hdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_DYNSYM  11

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

_Static_assert(sizeof(elf64_hdr_t) == 64, "ELF64 header must be 64 bytes");
_Static_assert(sizeof(elf64_phdr_t) == 56, "ELF64 phdr must be 56 bytes");

/* ── Fork RW data save/restore ───────────────────────────────── */
/*
 * When a forked child execve's the same binary in an identity-mapped OS,
 * elf_load_segments zeroes+reloads the entire load range, destroying the
 * parent's runtime data (modified globals, BSS state). We save the PF_W
 * segments before overwriting and restore them after the child is reaped.
 */

#define MAX_FORK_SAVES 4

typedef struct {
    void    *buf;       /* kmalloc'd backup buffer */
    uint64_t addr;      /* virtual address to restore to */
    uint64_t size;      /* bytes saved */
} fork_rw_save_t;

static fork_rw_save_t fork_saves[MAX_FORK_SAVES];
static int fork_save_count = 0;

/* Called from proc_wait4 after reaping a child — restores parent's RW data */
void elf_fork_restore(void)
{
    for (int i = 0; i < fork_save_count; i++) {
        if (fork_saves[i].buf) {
            serial_puts("[ELF] Restoring parent RW data at 0x");
            serial_puthex(fork_saves[i].addr, 16);
            serial_puts(" (");
            serial_putdec(fork_saves[i].size);
            serial_puts(" bytes)\n");
            memcpy((void *)fork_saves[i].addr,
                   fork_saves[i].buf, fork_saves[i].size);
            kfree(fork_saves[i].buf);
            fork_saves[i].buf = NULL;
        }
    }
    fork_save_count = 0;
}

/* ── ELF state ───────────────────────────────────────────────── */

#define ELF_MAX_SEGMENTS 24
/* User stack: dynamic from sys_caps (1-8MB based on RAM) */
#include "../include/sys_caps.h"
#define USER_STACK_SIZE  (g_sys_caps.user_stack_size ? g_sys_caps.user_stack_size : (1024 * 1024))

typedef struct {
    uint64_t entry;
    uint64_t stack_top;
    void    *stack_base;     /* For freeing later */
    void    *segments[ELF_MAX_SEGMENTS];
    int      segment_count;
    uint64_t segment_pages[ELF_MAX_SEGMENTS];
    /* For auxv AT_PHDR/AT_PHENT/AT_PHNUM */
    uint64_t phdr_addr;
    uint16_t phdr_entsize;
    uint16_t phdr_count;
    /* For dynamic linking */
    uint64_t load_bias;      /* base - vaddr_min (0 for ET_EXEC) */
    uint64_t vaddr_min;      /* lowest vaddr across all LOAD segments */
    uint64_t vaddr_max;      /* highest vaddr + memsz (demand path only) */
    bool     virt_mapped;    /* true if ET_EXEC virt→phys remap was used */
    bool     demand_paged;   /* true if segments are demand-paged */
} elf_loaded_t;

/* ── Validate ELF header ─────────────────────────────────────── */

static int elf_validate(const elf64_hdr_t *hdr)
{
    if (hdr->e_ident[0] != ELFMAG0 || hdr->e_ident[1] != ELFMAG1 ||
        hdr->e_ident[2] != ELFMAG2 || hdr->e_ident[3] != ELFMAG3) {
        serial_puts("[ELF] Bad magic\n");
        return -1;
    }

    if (hdr->e_ident[4] != ELFCLASS64) {
        serial_puts("[ELF] Not 64-bit\n");
        return -1;
    }

    if (hdr->e_ident[5] != ELFDATA2LSB) {
        serial_puts("[ELF] Not little-endian\n");
        return -1;
    }

    if (hdr->e_machine != EM_X86_64) {
        serial_puts("[ELF] Not x86-64\n");
        return -1;
    }

    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        serial_puts("[ELF] Not executable (type=");
        serial_putdec(hdr->e_type);
        serial_puts(")\n");
        return -1;
    }

    return 0;
}

/* ── Load ELF from buffer ────────────────────────────────────── */

static int elf_load_segments(const uint8_t *data, uint64_t data_size,
                             elf_loaded_t *loaded)
{
    const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;

    loaded->entry = hdr->e_entry;
    loaded->segment_count = 0;
    loaded->phdr_entsize = hdr->e_phentsize;
    loaded->phdr_count = hdr->e_phnum;

    serial_puts("[ELF] Entry: 0x");
    serial_puthex(hdr->e_entry, 16);
    serial_puts(", ");
    serial_putdec(hdr->e_phnum);
    serial_puts(" program headers\n");

    /* Check for PT_INTERP (dynamic linker) */
    const char *interp_path = NULL;
    for (int i = 0; i < hdr->e_phnum; i++) {
        uint64_t phoff = hdr->e_phoff + (uint64_t)i * hdr->e_phentsize;
        if (phoff + sizeof(elf64_phdr_t) > data_size) break;
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + phoff);
        if (ph->p_type == PT_INTERP && ph->p_filesz > 0 && ph->p_filesz < 256) {
            interp_path = (const char *)(data + ph->p_offset);
            serial_puts("[ELF] Interpreter: ");
            serial_puts(interp_path);
            serial_puts("\n");
        }
    }

    /* First pass: find min/max vaddr across all LOAD segments.
     * We allocate one contiguous block so RIP-relative addressing
     * between segments (e.g. .text → .rodata) works correctly. */
    uint64_t vaddr_min = UINT64_MAX;
    uint64_t vaddr_max = 0;
    uint64_t first_filesz = 0;  /* filesz of first LOAD segment (code+data) */
    int load_count = 0;

    for (int i = 0; i < hdr->e_phnum; i++) {
        uint64_t phoff = hdr->e_phoff + (uint64_t)i * hdr->e_phentsize;
        if (phoff + sizeof(elf64_phdr_t) > data_size) break;

        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + phoff);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        serial_puts("[ELF]   LOAD: vaddr=0x");
        serial_puthex(ph->p_vaddr, 16);
        serial_puts(" filesz=");
        serial_putdec(ph->p_filesz);
        serial_puts(" memsz=");
        serial_putdec(ph->p_memsz);
        serial_puts(" flags=");
        if (ph->p_flags & PF_R) serial_puts("R");
        if (ph->p_flags & PF_W) serial_puts("W");
        if (ph->p_flags & PF_X) serial_puts("X");
        serial_puts("\n");

        if (ph->p_vaddr < vaddr_min)
            vaddr_min = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > vaddr_max)
            vaddr_max = ph->p_vaddr + ph->p_memsz;
        if (load_count == 0)
            first_filesz = ph->p_filesz;
        load_count++;
    }

    if (load_count == 0) {
        serial_puts("[ELF] No LOAD segments found\n");
        return -1;
    }

    uint64_t total_size = vaddr_max - vaddr_min;
    uint64_t total_pages = (total_size + 4095) / 4096;

    /*
     * For ET_EXEC (non-PIE): load at the exact vaddr requested.
     * The code has absolute addresses that must match the load address.
     * For ET_DYN (PIE): allocate dynamic memory and adjust entry.
     */
    void *base;
    void *phys_base;      /* physical base used for mem_free_pages */
    bool fixed_load = (hdr->e_type == ET_EXEC);

    if (fixed_load) {
        /* ET_EXEC: binary has absolute virtual addresses that must be
         * honoured.  Instead of identity-mapping (virtual == physical),
         * allocate free physical pages anywhere in usable RAM and map
         * them to Q2's requested virtual addresses via paging_map_page.
         * This avoids conflicts with UEFI-reserved physical pages that
         * happen to overlap the binary's fixed load range. */
        phys_base = mem_alloc_aligned(total_pages * 4096, 4096);
        if (!phys_base) {
            serial_puts("[ELF] Failed to allocate ");
            serial_putdec(total_pages);
            serial_puts(" pages for ET_EXEC\n");
            return -1;
        }
        for (uint64_t i = 0; i < total_pages; i++) {
            paging_map_page(vaddr_min + i * 4096,
                            (uint64_t)phys_base + i * 4096,
                            0x3 /* PRESENT | WRITABLE */);
        }
        base = (void *)vaddr_min;   /* access via virtual addresses */
        loaded->virt_mapped = true;
        serial_puts("[ELF] ET_EXEC virt 0x");
        serial_puthex(vaddr_min, 16);
        serial_puts(" -> phys 0x");
        serial_puthex((uint64_t)phys_base, 16);
        serial_puts(", ");
        serial_putdec(total_pages * 4);
        serial_puts(" KB\n");
        fork_save_count = 0;
    } else {
        /* PIE/shared: allocate dynamic memory */
        phys_base = mem_alloc_aligned(total_pages * 4096, 4096);
        if (!phys_base) {
            serial_puts("[ELF] Failed to allocate ");
            serial_putdec(total_pages);
            serial_puts(" pages\n");
            return -1;
        }
        base = phys_base;
        loaded->virt_mapped = false;
        serial_puts("[ELF] Load base: 0x");
        serial_puthex((uint64_t)base, 16);
        serial_puts(", ");
        serial_putdec(total_pages * 4);
        serial_puts(" KB\n");
    }

    /* Zero + load segments atomically (no preemption window).
     * ELF data is already in RAM; these are pure memory ops.
     * cli prevents the compositor (or any thread) from running
     * between the memset and the memcpy and writing stale data
     * into the new process's BSS before it has a chance to use it. */
    __asm__ volatile ("cli" ::: "memory");

    memset(base, 0, total_pages * 4096);

    loaded->segments[0] = phys_base;   /* physical base for mem_free_pages */
    loaded->segment_pages[0] = total_pages;
    loaded->segment_count = 1;

    /* Second pass: copy segment data at correct offsets within the block */
    for (int i = 0; i < hdr->e_phnum; i++) {
        uint64_t phoff = hdr->e_phoff + (uint64_t)i * hdr->e_phentsize;
        if (phoff + sizeof(elf64_phdr_t) > data_size) break;

        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + phoff);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t offset_in_block = ph->p_vaddr - vaddr_min;

        if (ph->p_filesz > 0) {
            if (ph->p_offset + ph->p_filesz > data_size) {
                __asm__ volatile ("sti" ::: "memory");
                serial_puts("[ELF] Segment data out of bounds\n");
                return -1;
            }
            memcpy((uint8_t *)base + offset_in_block,
                   data + ph->p_offset, ph->p_filesz);
        }
    }

    __asm__ volatile ("sti" ::: "memory");

    /* Write-protect .text pages AFTER segment data has been copied.
     * This catches any stray writes to code during rendering etc. */
    if (fixed_load && first_filesz > 0) {
        /* .text write-protection disabled: Q2 soft renderer has out-of-bounds
         * writes (R_PolygonScanRightEdge) that hit .text addresses. With RO
         * pages this causes unrecoverable triple fault. Without protection,
         * the write silently corrupts a few bytes of code but doesn't crash.
         * TODO: fix the actual OOB writes in r_poly.c instead. */
#if 0
        uint64_t code_rodata_size = (first_filesz * 3) / 4;
        uint64_t text_pages = code_rodata_size / 4096;
        if (text_pages > total_pages) text_pages = total_pages;
        for (uint64_t i = 0; i < text_pages; i++) {
            paging_map_page((uint64_t)phys_base + i * 4096,
                            (uint64_t)phys_base + i * 4096,
                            0x1);
            paging_map_page(vaddr_min + i * 4096,
                            (uint64_t)phys_base + i * 4096,
                            0x1);
        }
#endif
    }

    if (fixed_load) {
        /* ET_EXEC: use original entry point (absolute addresses) */
        loaded->entry = hdr->e_entry;
    } else {
        /* ET_DYN/PIE: adjust entry point */
        loaded->entry = (uint64_t)base + (hdr->e_entry - vaddr_min);
    }

    /* Store for dynamic linking */
    loaded->vaddr_min = vaddr_min;
    loaded->load_bias = fixed_load ? 0 : ((uint64_t)base - vaddr_min);

    /* Program headers address for auxv AT_PHDR */
    loaded->phdr_addr = (uint64_t)base + hdr->e_phoff;
    if (hdr->e_type == ET_EXEC)
        loaded->phdr_addr = vaddr_min + hdr->e_phoff;

    /* Install the eager-loaded ELF pages into the *current process's*
     * PML4. Both the ET_EXEC and ET_DYN paths above wrote to phys
     * (relying on the kernel identity map for code execution). After
     * Phase C, user PML4s have an empty PDPT[0], so the binary needs
     * to be explicitly mapped at its target VA in the per-process
     * PML4. For ET_EXEC we use vaddr_min..vaddr_min+size; for ET_DYN
     * we use base (which is phys_base treated as VA — the binary is
     * position-independent so any VA works as long as PTEs match). */
    {
        extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                          uint64_t phys, uint64_t flags);
        extern uint64_t proc_current_cr3(void);
        extern uint64_t proc_exec_target_cr3(void);
        /* Anchor to the exec_target's CR3, NOT proc_current_cr3(): the load
         * runs interrupts-enabled, so a timer context-switch can move
         * current_proc to the launchpad parent or a kernel thread. Mapping
         * into the wrong PML4 leaves the child that actually elf_jumps to
         * the entry with an unmapped entry page → instruction-fetch #PF.
         * Falls back to proc_current_cr3 for non-exec loads. */
        uint64_t cr3 = proc_exec_target_cr3();
        if (!cr3) cr3 = proc_current_cr3();
        if (cr3) {
            uint64_t va = fixed_load ? vaddr_min : (uint64_t)base;
            uint64_t pa = (uint64_t)phys_base;
            uint64_t flags = 0x3; /* PRESENT | WRITABLE */
            for (uint64_t i = 0; i < total_pages; i++)
                paging_map_page_in_cr3(cr3, va + i * 4096,
                                       pa + i * 4096, flags);
        }
    }

    serial_puts("[ELF] Entry: 0x");
    serial_puthex(loaded->entry, 16);
    serial_puts("\n");

    return 0;
}

/* ── Set up user stack ───────────────────────────────────────── */

/* Keep initial argc/argv/envp/auxv construction scalar and deterministic. */
#if defined(__clang__)
__attribute__((noinline, optnone))
#else
__attribute__((noinline, optimize("O0")))
#endif
static uint64_t elf_setup_stack(elf_loaded_t *loaded,
                                int argc, const char **argv)
{
    /* Allocate stack via the upper-half mirror — the user RSP needs
     * to be reachable from the user process's CR3 (only PML4[256]
     * is shared once the lower-half identity map disappears). */
    uint64_t stack_pages = USER_STACK_SIZE / 4096;
    void *stack_phys = mem_alloc_aligned(USER_STACK_SIZE, 4096);
    if (!stack_phys) return 0;
    loaded->stack_base = PHYS_TO_VIRT(stack_phys);
    serial_puts("[ELF] stack_base=0x");
    serial_puthex((uint64_t)loaded->stack_base, 16);
    serial_puts(" size=0x");
    serial_puthex(USER_STACK_SIZE, 8);
    serial_puts("\n");

    memset(loaded->stack_base, 0, USER_STACK_SIZE);

    /* Stack grows downward. Top = base + size.
     *
     * ASLR-lite: randomize stack top by 0..4080 bytes (256-slot granularity,
     * 16-byte aligned to preserve System V stack alignment). Not a security
     * feature (full trust model) — the benefit is cache-set diversification
     * across processes and surfacing layout-dependent bugs in user code that
     * would otherwise only trigger on alternate runs.
     * Source: hw_random64() uses RDRAND when available, TSC mix otherwise. */
    extern uint64_t hw_random64(void);
    uint64_t sp_jitter = (hw_random64() & 0xFF) * 16;   /* 0..4080, 16-aligned */
    uint64_t sp = (uint64_t)loaded->stack_base + USER_STACK_SIZE - sp_jitter;

    /* Push strings first (below stack top), then pointers.
     * Layout (growing downward):
     *   [strings area]
     *   NULL (envp terminator)
     *   NULL (argv terminator)
     *   argv[n-1] pointer
     *   ...
     *   argv[0] pointer
     *   argc
     *   ← SP points here
     */

    /* Copy argument strings to stack */
    #define ELF_MAX_ARGS 256

    /* Default environment */
    static const char *default_env[] = {
        "HOME=/", "PATH=/", "USER=root", "TERM=xterm-256color",
        "LANG=C", "PWD=/", "SHELL=/bin/sh"
    };
    #define DEFAULT_ENV_COUNT 7

    /* Calculate string space needed */
    int effective_argc = (argc > ELF_MAX_ARGS) ? ELF_MAX_ARGS : argc;
    uint64_t str_need = 0;
    for (int i = 0; i < effective_argc; i++)
        str_need += strlen(argv[i]) + 1;
    for (int i = 0; i < DEFAULT_ENV_COUNT; i++)
        str_need += strlen(default_env[i]) + 1;
    str_need += 16;   /* AT_RANDOM lives in the same string area */
    if (str_need < 256) str_need = 256;
    str_need = (str_need + 15) & ~15ULL;

    uint64_t string_area = sp - str_need;
    uint64_t str_ptr = string_area;

    uint64_t argv_ptrs[ELF_MAX_ARGS];

    for (int i = 0; i < effective_argc; i++) {
        uint64_t len = strlen(argv[i]) + 1;
        memcpy((void *)str_ptr, argv[i], len);
        argv_ptrs[i] = str_ptr;
        str_ptr += len;
    }

    /* Place AT_RANDOM 16 bytes in the string area (glibc needs this
     * for stack canary + PTR_MANGLE/PTR_DEMANGLE pointer guard) */
    uint64_t at_random_addr = str_ptr;
    {
        uint8_t *rnd = (uint8_t *)str_ptr;
        /* Simple PRNG seed from RDTSC — good enough for non-crypto use */
        uint64_t tsc;
        __asm__ volatile ("rdtsc" : "=A"(tsc));
        for (int i = 0; i < 16; i++)
            rnd[i] = (uint8_t)((tsc >> (i & 7)) ^ (tsc >> ((i + 3) & 7)) ^ i);
        str_ptr += 16;
    }

    /* Copy env strings */
    uint64_t env_ptrs[DEFAULT_ENV_COUNT];
    for (int i = 0; i < DEFAULT_ENV_COUNT; i++) {
        uint64_t len = strlen(default_env[i]) + 1;
        memcpy((void *)str_ptr, default_env[i], len);
        env_ptrs[i] = str_ptr;
        str_ptr += len;
    }

    /* Build stack frame below string area */
    sp = string_area;
    sp &= ~0xFULL;  /* Align to 16 bytes */

    /* ── Auxiliary vector (auxv) ──
     * Must come AFTER envp NULL terminator, BEFORE alignment.
     * We build it top-down and then copy. */
    #define AT_NULL     0
    #define AT_PAGESZ   6
    #define AT_RANDOM   25
    #define AT_ENTRY    9
    #define AT_PHDR     3
    #define AT_PHENT    4
    #define AT_PHNUM    5
    #define AT_UID      11
    #define AT_EUID     12
    #define AT_GID      13
    #define AT_EGID     14
    #define AT_SECURE   23
    #define AT_HWCAP    16
    #define AT_CLKTCK   17

    struct { uint64_t type; uint64_t val; } auxv[] = {
        { AT_PHDR,    loaded->phdr_addr },
        { AT_PHENT,   loaded->phdr_entsize },
        { AT_PHNUM,   loaded->phdr_count },
        { AT_PAGESZ,  4096 },
        { AT_RANDOM,  at_random_addr },
        { AT_ENTRY,   loaded->entry },
        { AT_UID,     0 }, { AT_EUID, 0 },
        { AT_GID,     0 }, { AT_EGID, 0 },
        { AT_SECURE,  0 },
        { AT_HWCAP,   0 },
        { AT_CLKTCK,  100 },
        { AT_NULL,    0 },
    };
    int auxv_count = sizeof(auxv) / sizeof(auxv[0]);

    /* Build the final stack frame in FORWARD order (low → high addresses).
     *
     * Linux ABI layout at process entry, growing upward from RSP:
     *   argc
     *   argv[0..argc-1]   pointers
     *   NULL              argv terminator
     *   envp[0..n-1]      pointers
     *   NULL              envp terminator
     *   auxv[0..n-1]      type+val pairs
     *   AT_NULL/0         auxv terminator (already in auxv[])
     *
     * This used to be a series of backward push loops that GCC at -O2
     * vectorized into SSE stores with loop bounds that wrapped rax past 0,
     * faulting on a write to address -8. Forward construction with a
     * single advancing pointer cannot be reordered into a backward loop. */

    /* Compute total bytes for the frame */
    uint64_t frame_bytes =
        8                                    /* argc */
        + (uint64_t)(effective_argc + 1) * 8 /* argv + NULL */
        + (uint64_t)(DEFAULT_ENV_COUNT + 1) * 8 /* envp + NULL */
        + (uint64_t)auxv_count * 16;         /* auxv pairs */

    /* Allocate, then 16-byte align the bottom of the frame (where SP will
     * point on entry — System V x86-64 ABI requires 16-byte alignment so
     * that the first call instruction sees a 16-byte-aligned stack). */
    sp -= frame_bytes;
    sp &= ~0xFULL;

    uint64_t *out = (uint64_t *)sp;

    /* argc */
    *out++ = (uint64_t)effective_argc;

    /* argv pointers + NULL */
    for (int i = 0; i < effective_argc; i++)
        *out++ = argv_ptrs[i];
    *out++ = 0;

    /* envp pointers + NULL */
    for (int i = 0; i < DEFAULT_ENV_COUNT; i++)
        *out++ = env_ptrs[i];
    *out++ = 0;

    /* auxv pairs (already includes AT_NULL terminator at the end) */
    for (int i = 0; i < auxv_count; i++) {
        *out++ = auxv[i].type;
        *out++ = auxv[i].val;
    }

    #undef PUSH_U64

    loaded->stack_top = sp;
    (void)stack_pages;

    return sp;
}

/* ── Free loaded ELF resources ───────────────────────────────── */

void elf_free(elf_loaded_t *loaded)
{
    /* If this was a demand-paged process, release the slot. The actual
     * VMA cleanup is done by syscall_reset_process walking PTEs. */

    /* For ET_EXEC with virtual→physical remapping: unmap virtual pages first
     * so the virtual address range is free for the next launch, then free
     * the physical pages (segments[0] holds the physical base). */
    if (loaded->virt_mapped && loaded->vaddr_min && loaded->segment_pages[0] > 0) {
        for (uint64_t i = 0; i < loaded->segment_pages[0]; i++)
            paging_unmap_page(loaded->vaddr_min + i * 4096);
    }
    for (int i = 0; i < loaded->segment_count; i++) {
        if (loaded->segments[i] && loaded->segment_pages[i] > 0)
            mem_free_pages(loaded->segments[i], loaded->segment_pages[i]);
    }
    if (loaded->stack_base)
        mem_free_pages((void *)VIRT_TO_PHYS(loaded->stack_base),
                       USER_STACK_SIZE / 4096);
}

/* ── Execute ELF entry point ─────────────────────────────────── */

static void elf_jump(uint64_t entry, uint64_t sp)
{
    /* Flush stale shell keystrokes before new process starts. */
    extern void kbd_flush(void);
    extern void input_flush(void);
    kbd_flush();
    input_flush();

    serial_puts("[ELF] Jumping to entry 0x");
    serial_puthex(entry, 16);
    serial_puts(" with SP=0x");
    serial_puthex(sp, 16);
    serial_puts("\n");

    /* Switch CR3 to the new process and jump in one asm block. After
     * the CR3 switch the lower-half boot kernel stack is no longer
     * mapped, so we must NOT touch the C stack until the asm has
     * loaded the new RSP. A timer IRQ firing after scheduler ownership
     * changes, or between the CR3 switch and RSP load, could save or use
     * the old boot stack under the child's CR3. Disable interrupts across
     * the whole transition and re-enable them just before the
     * jmp into user code (sti has a 1-instruction delay, so the jmp
     * runs first and the user binary starts with IF=1). The transition does:
     *   cli                     ;; before proc_launch_prepare
     *   mov new_cr3, %cr3   ;; switch address space
     *   mov sp, %rsp        ;; switch to user stack (upper-half)
     *   xor %rbp, %rbp
     *   sti
     *   jmp *entry          ;; into user code (IF becomes 1 here)
     * The instruction fetches between mov %cr3 and jmp succeed because
     * kernel text lives at the upper-half mirror (PML4[256], shared
     * across all CR3s). */
    /* Re-anchor current_proc to the exec target and use ITS cr3: a timer
     * context-switch during the ELF load can leave current_proc (hence
     * proc_current_cr3) pointing at a kernel thread, which would launch
     * the binary un-isolated under kernel_cr3 (load-VA / identity-map
     * collision → self-corruption). See proc_launch_prepare. */
    extern uint64_t proc_launch_prepare(void);
    __asm__ volatile ("cli" ::: "memory");
    uint64_t pcr3 = proc_launch_prepare();

    /* Reset the FPU/SSE state to the x86-64 ABI default before entering a
     * fresh image: x87 FCW=0x037F + MXCSR=0x1F80 (ALL FP exceptions MASKED).
     * execve reuses the caller's process slot, so the new binary would
     * otherwise inherit its live MXCSR — and gcc leaves divide-by-zero
     * UNMASKED, so cc1's first `divsd` by zero (it relies on IEEE infinity,
     * not a trap) raised #XM. fninit + ldmxcsr here gives every launched
     * binary the clean state the ABI promises. No FPU use after this until
     * the jmp into user code. */
    {
        uint32_t _mxcsr = 0x00001F80;
        __asm__ volatile ("fninit; ldmxcsr %0" :: "m"(_mxcsr) : "memory");
    }

    __asm__ volatile (
        "test %2, %2\n"
        "jz   1f\n"
        "mov  %2, %%cr3\n"
        "1:\n"
        "mov %0, %%rsp\n"
        "xor %%rbp, %%rbp\n"
        "sti\n"
        "jmp *%1\n"
        : : "r"(sp), "r"(entry), "r"(pcr3)
        : "memory"
    );

    __builtin_unreachable();
}

/* ── Demand-paged segment setup ────────────────────────────────
 * For static binaries (no PT_DYNAMIC), register VMAs that point
 * to the file on disk instead of reading the whole file into RAM.
 * The page fault handler reads pages on first access.
 *
 * Limitation: fork+execve of the same binary is not supported on the
 * demand-paged path. The eager path uses fork_saves[] (memcpy of PF_W
 * segments) to preserve parent state across the child's segment reload,
 * but with demand paging the child's per-page faults rewrite parent PTEs
 * one at a time, leaking parent's pages and breaking the parent's view
 * of its own data after the child is reaped. Static binaries that
 * fork+exec themselves should use the eager path (link with PT_DYNAMIC). */

#define VMA_FILE_ELF 1

extern int vma_register_file(uint64_t base, uint64_t pages, uint32_t prot,
                             uint8_t type, vfs_node_t *node,
                             uint64_t file_offset, uint64_t file_size);

static int elf_setup_demand_segments(const elf64_hdr_t *hdr,
                                     const uint8_t *phdr_buf,
                                     vfs_node_t *file_node,
                                     elf_loaded_t *loaded)
{
    /* Pass 1: find vaddr range across all PT_LOAD segments */
    uint64_t vaddr_min = ~0ULL, vaddr_max = 0;
    int load_count = 0;

    for (int i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(phdr_buf + (uint64_t)i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        if (seg_start < vaddr_min) vaddr_min = seg_start;
        if (seg_end > vaddr_max)   vaddr_max = seg_end;
        load_count++;
    }

    if (load_count == 0) {
        serial_puts("[ELF] No LOAD segments\n");
        return -1;
    }

    loaded->vaddr_min = vaddr_min;
    loaded->vaddr_max = vaddr_max;
    loaded->demand_paged = true;
    loaded->load_bias = 0;          /* ET_EXEC only — PIE goes through eager */
    loaded->virt_mapped = true;
    loaded->segment_count = 0;

    /* Pass 2: register a VMA per PT_LOAD segment */
    for (int i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(phdr_buf + (uint64_t)i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t seg_vaddr = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t seg_pages = (seg_end - seg_vaddr) / 4096;

        /* File offset aligned to page boundary; in-page bias preserved */
        uint64_t in_page = ph->p_vaddr & 0xFFF;
        uint64_t file_off = ph->p_offset - in_page;
        uint64_t file_sz  = ph->p_filesz + in_page;

        /* Convert ELF flags to PROT_* */
        uint32_t prot = 0;
        if (ph->p_flags & PF_R) prot |= 0x1; /* PROT_READ */
        if (ph->p_flags & PF_W) prot |= 0x2; /* PROT_WRITE */
        if (ph->p_flags & PF_X) prot |= 0x4; /* PROT_EXEC */

        if (vma_register_file(seg_vaddr, seg_pages, prot,
                              VMA_FILE_ELF, file_node,
                              file_off, file_sz) < 0) {
            serial_puts("[ELF] VMA table full\n");
            return -1;
        }

        serial_puts("[ELF] Demand VMA: 0x");
        serial_puthex(seg_vaddr, 16);
        serial_puts(" (");
        serial_putdec(seg_pages * 4);
        serial_puts(" KB) flags=0x");
        serial_puthex(prot, 1);
        serial_puts("\n");
    }

    loaded->entry = hdr->e_entry;
    loaded->phdr_addr = vaddr_min + hdr->e_phoff;
    loaded->phdr_entsize = hdr->e_phentsize;
    loaded->phdr_count = hdr->e_phnum;

    serial_puts("[ELF] Demand-paged: entry=0x");
    serial_puthex(loaded->entry, 16);
    serial_puts(", range 0x");
    serial_puthex(vaddr_min, 16);
    serial_puts("..0x");
    serial_puthex(vaddr_max, 16);
    serial_puts("\n");

    return 0;
}

/* ── Symbol-table capture for crash-dump symbolizer ──────────────
 *
 * Walks the section headers of an already-loaded ELF image in `data`
 * and copies `.symtab`+`.strtab` (or `.dynsym`+`.dynstr` as a fallback
 * for stripped PIEs) into kmalloc'd kernel buffers, then hands them
 * off to `user_symtab_set()` on the current process. Safe to call on
 * any ELF — if the section tables are missing or malformed, it just
 * leaves the process's symtab fields NULL and the symbolizer will
 * return false at crash time.
 *
 * Call site: eager-load path only, right before `kfree(data)`.
 * Demand-paged ELFs skip this (no `data` buffer), which means stripped
 * and demand-paged binaries are symbol-less — acceptable for now. */
static void elf_capture_symtab(const uint8_t *data, uint64_t data_size,
                               uint64_t load_bias)
{
    if (!data || data_size < sizeof(elf64_hdr_t)) return;

    const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;
    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) return;
    if (hdr->e_shentsize < sizeof(elf64_shdr_t)) return;

    uint64_t shtab_bytes = (uint64_t)hdr->e_shnum * hdr->e_shentsize;
    if (hdr->e_shoff + shtab_bytes > data_size) return;

    const elf64_shdr_t *shdrs = (const elf64_shdr_t *)(data + hdr->e_shoff);

    /* Section-name string table: e_shstrndx indexes into shdrs[] and
     * the referenced section is itself a string table. */
    const char *shstrtab = NULL;
    uint64_t    shstrtab_size = 0;
    if (hdr->e_shstrndx < hdr->e_shnum) {
        const elf64_shdr_t *sh = &shdrs[hdr->e_shstrndx];
        if (sh->sh_type == SHT_STRTAB &&
            sh->sh_offset + sh->sh_size <= data_size) {
            shstrtab      = (const char *)(data + sh->sh_offset);
            shstrtab_size = sh->sh_size;
        }
    }
    if (!shstrtab) return;

    /* Locate .symtab/.strtab first; fall back to .dynsym/.dynstr
     * for stripped PIEs where the static symbol table was removed. */
    const elf64_shdr_t *sym_sh = NULL;
    const elf64_shdr_t *str_sh = NULL;
    const elf64_shdr_t *dynsym_sh = NULL;
    const elf64_shdr_t *dynstr_sh = NULL;

    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = &shdrs[i];
        if (sh->sh_name >= shstrtab_size) continue;
        const char *name = shstrtab + sh->sh_name;

        if (sh->sh_type == SHT_SYMTAB) {
            /* .symtab: its sh_link points at the matching .strtab */
            sym_sh = sh;
            if (sh->sh_link < hdr->e_shnum) {
                const elf64_shdr_t *linked = &shdrs[sh->sh_link];
                if (linked->sh_type == SHT_STRTAB)
                    str_sh = linked;
            }
        } else if (sh->sh_type == SHT_DYNSYM) {
            dynsym_sh = sh;
            if (sh->sh_link < hdr->e_shnum) {
                const elf64_shdr_t *linked = &shdrs[sh->sh_link];
                if (linked->sh_type == SHT_STRTAB)
                    dynstr_sh = linked;
            }
        }
        (void)name;
    }

    const elf64_shdr_t *chosen_sym = sym_sh    ? sym_sh    : dynsym_sh;
    const elf64_shdr_t *chosen_str = sym_sh    ? str_sh    : dynstr_sh;
    if (!chosen_sym || !chosen_str) return;
    if (chosen_sym->sh_size == 0 || chosen_str->sh_size == 0) return;
    if (chosen_sym->sh_offset + chosen_sym->sh_size > data_size) return;
    if (chosen_str->sh_offset + chosen_str->sh_size > data_size) return;

    void *sym_copy = kmalloc(chosen_sym->sh_size);
    if (!sym_copy) return;
    char *str_copy = (char *)kmalloc(chosen_str->sh_size);
    if (!str_copy) { kfree(sym_copy); return; }

    memcpy(sym_copy, data + chosen_sym->sh_offset, chosen_sym->sh_size);
    memcpy(str_copy, data + chosen_str->sh_offset, chosen_str->sh_size);

    user_symtab_set(proc_current(),
                    sym_copy, chosen_sym->sh_size,
                    str_copy, chosen_str->sh_size,
                    load_bias);

    serial_puts("[ELF] symtab captured: ");
    serial_putdec(chosen_sym->sh_size / sizeof(uint64_t) / 3);
    serial_puts(" entries, strtab ");
    serial_putdec(chosen_str->sh_size);
    serial_puts(" B\n");
}

/* ── Symbol-table capture (demand-paged variant, reads via vfs) ──
 *
 * Same contract as `elf_capture_symtab()` but reads the section
 * header table and the selected symtab/strtab sections from the file
 * on disk via vfs_read(). Used by the demand-paged load path where
 * the whole ELF is never pulled into memory.
 */
static void elf_capture_symtab_vfs(vfs_node_t *node, uint64_t file_size,
                                   const elf64_hdr_t *hdr, uint64_t load_bias)
{
    if (!node || !hdr) return;
    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) return;
    if (hdr->e_shentsize < sizeof(elf64_shdr_t)) return;

    uint64_t shtab_bytes = (uint64_t)hdr->e_shnum * hdr->e_shentsize;
    if (hdr->e_shoff + shtab_bytes > file_size) return;

    elf64_shdr_t *shdrs = (elf64_shdr_t *)kmalloc(shtab_bytes);
    if (!shdrs) return;
    if (vfs_read(node, hdr->e_shoff, shdrs, shtab_bytes) < 0) {
        kfree(shdrs);
        return;
    }

    /* Read the section-name string table. */
    char    *shstrtab      = NULL;
    uint64_t shstrtab_size = 0;
    if (hdr->e_shstrndx < hdr->e_shnum) {
        elf64_shdr_t *sh = &shdrs[hdr->e_shstrndx];
        if (sh->sh_type == SHT_STRTAB && sh->sh_size > 0 &&
            sh->sh_offset + sh->sh_size <= file_size) {
            shstrtab = (char *)kmalloc(sh->sh_size);
            if (shstrtab && vfs_read(node, sh->sh_offset, shstrtab,
                                     sh->sh_size) >= 0) {
                shstrtab_size = sh->sh_size;
            } else if (shstrtab) {
                kfree(shstrtab);
                shstrtab = NULL;
            }
        }
    }
    if (!shstrtab) { kfree(shdrs); return; }

    /* Locate .symtab/.strtab (preferred) or .dynsym/.dynstr fallback. */
    elf64_shdr_t *sym_sh    = NULL;
    elf64_shdr_t *str_sh    = NULL;
    elf64_shdr_t *dynsym_sh = NULL;
    elf64_shdr_t *dynstr_sh = NULL;

    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        elf64_shdr_t *sh = &shdrs[i];
        if (sh->sh_name >= shstrtab_size) continue;

        if (sh->sh_type == SHT_SYMTAB) {
            sym_sh = sh;
            if (sh->sh_link < hdr->e_shnum) {
                elf64_shdr_t *linked = &shdrs[sh->sh_link];
                if (linked->sh_type == SHT_STRTAB) str_sh = linked;
            }
        } else if (sh->sh_type == SHT_DYNSYM) {
            dynsym_sh = sh;
            if (sh->sh_link < hdr->e_shnum) {
                elf64_shdr_t *linked = &shdrs[sh->sh_link];
                if (linked->sh_type == SHT_STRTAB) dynstr_sh = linked;
            }
        }
    }

    elf64_shdr_t *chosen_sym = sym_sh ? sym_sh : dynsym_sh;
    elf64_shdr_t *chosen_str = sym_sh ? str_sh : dynstr_sh;
    if (!chosen_sym || !chosen_str ||
        chosen_sym->sh_size == 0 || chosen_str->sh_size == 0 ||
        chosen_sym->sh_offset + chosen_sym->sh_size > file_size ||
        chosen_str->sh_offset + chosen_str->sh_size > file_size) {
        kfree(shdrs);
        kfree(shstrtab);
        return;
    }

    void *sym_copy = kmalloc(chosen_sym->sh_size);
    char *str_copy = sym_copy ? (char *)kmalloc(chosen_str->sh_size) : NULL;
    if (!sym_copy || !str_copy) {
        if (sym_copy) kfree(sym_copy);
        kfree(shdrs);
        kfree(shstrtab);
        return;
    }

    if (vfs_read(node, chosen_sym->sh_offset, sym_copy,
                 chosen_sym->sh_size) < 0 ||
        vfs_read(node, chosen_str->sh_offset, str_copy,
                 chosen_str->sh_size) < 0) {
        kfree(sym_copy);
        kfree(str_copy);
        kfree(shdrs);
        kfree(shstrtab);
        return;
    }

    uint64_t sym_bytes = chosen_sym->sh_size;
    uint64_t str_bytes = chosen_str->sh_size;

    user_symtab_set(proc_current(),
                    sym_copy, sym_bytes,
                    str_copy, str_bytes,
                    load_bias);

    kfree(shdrs);
    kfree(shstrtab);

    serial_puts("[ELF] symtab captured (vfs): ");
    serial_putdec(sym_bytes / sizeof(uint64_t) / 3);
    serial_puts(" entries, strtab ");
    serial_putdec(str_bytes);
    serial_puts(" B\n");
}

/* ── Public API: Load and execute ELF from OsitoFS ───────────── */

/* elf_path_exists — true if `path` resolves to a file in the VFS. proc_execve
 * calls this to validate the target BEFORE tearing down the caller's image: a
 * missing target must fail with -ENOENT while the old image is still intact
 * (otherwise a vfork/posix_spawn child is left running a half-destroyed image).
 * Uses the same vfs_find primitive elf_exec uses for the real load. */
int elf_path_exists(const char *path)
{
    vfs_node_t node;
    char norm[256];
    const char *lookup = path;
    extern bool path_normalize_flat(const char *path, char *out, int out_sz);
    if (path_normalize_flat(path, norm, sizeof(norm)) && norm[0])
        lookup = norm;
    return vfs_find(lookup, VFS_MODE_POSIX, &node) ? 1 : 0;
}

int elf_exec(const char *filename, int argc, const char **argv)
{
    serial_puts("[ELF] Loading '");
    serial_puts(filename);
    serial_puts("'...\n");

    fb_puts_color("\n Exec: ", 0x0000FF00);
    fb_puts(filename);
    fb_puts("\n");

    vfs_node_t node;
    if (!vfs_find(filename, VFS_MODE_POSIX, &node)) {
        serial_puts("[ELF] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        fb_puts(" File not found\n");
        return -1;
    }

    uint64_t file_size = node.size;

    serial_puts("[ELF] File size: ");
    serial_putdec(file_size);
    serial_puts(" bytes\n");

    if (file_size < sizeof(elf64_hdr_t)) {
        serial_puts("[ELF] File too small\n");
        return -1;
    }

    /* Read only the ELF header (64 bytes) — enough to determine layout */
    elf64_hdr_t hdr_buf;
    if (vfs_read(&node, 0, &hdr_buf, sizeof(hdr_buf)) < 0) {
        serial_puts("[ELF] Failed to read header\n");
        return -1;
    }
    if (elf_validate(&hdr_buf) < 0) return -1;

    /* Read program headers (typically <1KB) */
    uint64_t phdr_size = (uint64_t)hdr_buf.e_phnum * hdr_buf.e_phentsize;
    uint8_t *phdr_buf = (uint8_t *)kmalloc(phdr_size);
    if (!phdr_buf) return -1;
    if (vfs_read(&node, hdr_buf.e_phoff, phdr_buf, phdr_size) < 0) {
        kfree(phdr_buf);
        return -1;
    }

    /* Decide path: ET_EXEC without PT_DYNAMIC → demand-paged
     * ET_DYN or anything with PT_DYNAMIC → eager (needs dynamic linker)
     *
     * With X-PGTBL (per-process CR3) + VMA owner filtering, a fork'd
     * child running execve can safely use the demand-paged path: its
     * fault-installed PTEs go into its own CR3 (not parent's), and the
     * child's own VMAs (tagged by owner) are cleaned up on exit without
     * touching the parent's VMAs. The old `syscall_in_fork_exec()` gate
     * is no longer needed. */
    bool has_dynamic = false;
    for (int i = 0; i < hdr_buf.e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(phdr_buf + (uint64_t)i * hdr_buf.e_phentsize);
        if (ph->p_type == PT_DYNAMIC) { has_dynamic = true; break; }
    }
    bool use_demand = (hdr_buf.e_type == ET_EXEC) && !has_dynamic;

    elf_loaded_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    uint8_t *data = NULL;

    if (use_demand) {
        serial_puts("[ELF] Path: demand-paged (static binary, ");
        serial_putdec(file_size / (1024 * 1024));
        serial_puts(" MB)\n");
        if (elf_setup_demand_segments(&hdr_buf, phdr_buf, &node, &loaded) < 0) {
            kfree(phdr_buf);
            return -1;
        }
        kfree(phdr_buf);

        /* Capture symbol tables via VFS for the crash-dump symbolizer.
         * No in-memory data buffer in this path — read sections on
         * demand. load_bias stays 0 for ET_EXEC demand-paged binaries. */
        elf_capture_symtab_vfs(&node, file_size, &hdr_buf, loaded.load_bias);
    } else {
        serial_puts("[ELF] Path: eager (");
        serial_puts(has_dynamic ? "dynamic" : "PIE");
        serial_puts(")\n");
        kfree(phdr_buf);

        {
            extern int sys_caps_check_alloc(uint64_t bytes, const char *what);
            extern uint64_t mem_get_free(void);
            uint64_t avail = mem_get_free();
            serial_puts("[ELF] ");
            serial_puts(filename);
            serial_puts(": ");
            serial_putdec(file_size / (1024 * 1024));
            serial_puts(" MB, available: ");
            serial_putdec(avail / (1024 * 1024));
            serial_puts(" MB\n");
            if (!sys_caps_check_alloc(file_size * 2, filename))
                return -1;
        }

        data = (uint8_t *)kmalloc(file_size);
        if (!data) {
            serial_puts("[ELF] Failed to allocate read buffer\n");
            return -1;
        }
        if (vfs_read(&node, 0, data, file_size) < 0) {
            serial_puts("[ELF] Failed to read file\n");
            kfree(data);
            return -1;
        }

        if (elf_load_segments(data, file_size, &loaded) < 0) {
            kfree(data);
            return -1;
        }
    }

    /* ── Patch NT_GNU_ABI_TAG notes ────────────────────────────────
     * glibc binaries have a .note.ABI-tag that specifies the minimum
     * Linux kernel version. Since OsitoK is not Linux, we patch the
     * required version to 0.0.0 so the check always passes.
     * Eager path only — demand-paged binaries have no in-memory data buffer. */
    if (data) {
        elf64_hdr_t *hdr = (elf64_hdr_t *)data;
        for (int i = 0; i < hdr->e_phnum; i++) {
            elf64_phdr_t *ph = (elf64_phdr_t *)(data + hdr->e_phoff + i * hdr->e_phentsize);
            if (ph->p_type != PT_NOTE) continue;

            /* Walk notes in the loaded segment (already in memory at p_vaddr) */
            uint8_t *note = (uint8_t *)(ph->p_vaddr);
            uint8_t *end  = note + ph->p_filesz;

            while (note + 12 <= end) {
                uint32_t namesz = *(uint32_t *)(note + 0);
                uint32_t descsz = *(uint32_t *)(note + 4);
                uint32_t type   = *(uint32_t *)(note + 8);
                uint8_t *name   = note + 12;
                uint8_t *desc   = name + ((namesz + 3) & ~3);

                if (type == NT_GNU_ABI_TAG && namesz == 4 &&
                    name[0]=='G' && name[1]=='N' && name[2]=='U' && name[3]=='\0' &&
                    descsz >= 16) {
                    uint32_t *abi = (uint32_t *)desc;
                    serial_puts("[ELF] Patching ABI tag: OS=");
                    serial_putdec(abi[0]);
                    serial_puts(" min=");
                    serial_putdec(abi[1]); serial_puts(".");
                    serial_putdec(abi[2]); serial_puts(".");
                    serial_putdec(abi[3]);
                    serial_puts(" -> OsitoK 1.0.0\n");
                    abi[0] = 0;  /* OS: keep as Linux (0) for glibc compat */
                    abi[1] = 1;  /* major: 1 (matches our uname) */
                    abi[2] = 0;  /* minor: 0 */
                    abi[3] = 0;  /* patch: 0 */
                }
                note = desc + ((descsz + 3) & ~3);
            }
        }
    }

    /* ── Dynamic linking (eager path only) ───────────────────────────
     * If the binary has a PT_DYNAMIC segment, parse it to resolve
     * shared library dependencies and apply relocations. */
    if (data) {
        const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;
        dl_dyn_t *dyn_table = NULL;
        uint64_t dyn_count = 0;

        for (int i = 0; i < hdr->e_phnum; i++) {
            uint64_t phoff = hdr->e_phoff + (uint64_t)i * hdr->e_phentsize;
            if (phoff + sizeof(elf64_phdr_t) > file_size) break;
            const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + phoff);
            if (ph->p_type == PT_DYNAMIC) {
                dyn_table = (dl_dyn_t *)(loaded.load_bias + ph->p_vaddr);
                dyn_count = ph->p_memsz / sizeof(dl_dyn_t);
                break;
            }
        }

        if (dyn_table) {
            serial_puts("[ELF] PT_DYNAMIC: ");
            serial_putdec(dyn_count);
            serial_puts(" entries\n");

            /* Parse dynamic table */
            uint64_t dt_symtab = 0, dt_strtab = 0, dt_strsz = 0;
            uint64_t dt_hash = 0, dt_gnu_hash_val = 0;
            uint64_t dt_rela = 0, dt_relasz = 0;
            uint64_t dt_jmprel = 0, dt_pltrelsz = 0;
            uint64_t dt_init_array = 0, dt_init_arraysz = 0;

            #define MAX_DT_NEEDED 32
            uint64_t needed_offsets[MAX_DT_NEEDED];
            int needed_count = 0;

            for (uint64_t di = 0; di < dyn_count; di++) {
                if (dyn_table[di].d_tag == DT_NULL) break;
                switch (dyn_table[di].d_tag) {
                case DT_SYMTAB:      dt_symtab       = dyn_table[di].d_val; break;
                case DT_STRTAB:      dt_strtab       = dyn_table[di].d_val; break;
                case DT_STRSZ:       dt_strsz        = dyn_table[di].d_val; break;
                case DT_HASH:        dt_hash         = dyn_table[di].d_val; break;
                case DT_GNU_HASH:    dt_gnu_hash_val = dyn_table[di].d_val; break;
                case DT_RELA:        dt_rela         = dyn_table[di].d_val; break;
                case DT_RELASZ:      dt_relasz       = dyn_table[di].d_val; break;
                case DT_JMPREL:      dt_jmprel       = dyn_table[di].d_val; break;
                case DT_PLTRELSZ:    dt_pltrelsz     = dyn_table[di].d_val; break;
                case DT_INIT_ARRAY:  dt_init_array   = dyn_table[di].d_val; break;
                case DT_INIT_ARRAYSZ:dt_init_arraysz = dyn_table[di].d_val; break;
                case DT_NEEDED:
                    if (needed_count < MAX_DT_NEEDED)
                        needed_offsets[needed_count++] = dyn_table[di].d_val;
                    break;
                }
            }

            /* Load DT_NEEDED shared libraries */
            if (needed_count > 0 && dt_strtab) {
                const char *strtab = (const char *)(loaded.load_bias + dt_strtab);
                serial_puts("[ELF] ");
                serial_putdec(needed_count);
                serial_puts(" DT_NEEDED libraries\n");

                for (int ni = 0; ni < needed_count; ni++) {
                    const char *libname = strtab + needed_offsets[ni];

                    /* Strip path prefix (/lib/x86_64-linux-gnu/libc.so.6 → libc.so.6) */
                    const char *basename = libname;
                    for (const char *p = libname; *p; p++) {
                        if (*p == '/') basename = p + 1;
                    }

                    serial_puts("[ELF] Loading: ");
                    serial_puts(basename);

                    void *handle = dl_open_flags(basename, DL_DEFER_LINK);
                    if (handle) {
                        serial_puts(" OK\n");
                    } else {
                        serial_puts(" not found (kernel stubs)\n");
                    }
                }

                /* ── TLS setup (before linking, so DTPMOD/TPOFF work) ──── */
                {
                    extern void proc_set_fs_base(uint64_t addr);
                    #define TCBHEAD_SIZE 0x80

                    /* Scan main binary for PT_TLS */
                    uint64_t main_tls_filesz = 0, main_tls_memsz = 0;
                    uint64_t main_tls_align = 1, main_tls_initimg = 0;
                    for (int ti = 0; ti < hdr->e_phnum; ti++) {
                        uint64_t toff = hdr->e_phoff + (uint64_t)ti * hdr->e_phentsize;
                        if (toff + sizeof(elf64_phdr_t) > file_size) break;
                        const elf64_phdr_t *tph = (const elf64_phdr_t *)(data + toff);
                        if (tph->p_type == PT_TLS && tph->p_memsz > 0) {
                            main_tls_filesz = tph->p_filesz;
                            main_tls_memsz  = tph->p_memsz;
                            main_tls_align  = tph->p_align ? tph->p_align : 1;
                            main_tls_initimg = loaded.load_bias + tph->p_vaddr;
                        }
                    }

                    /* Compute total static TLS size (Variant II) */
                    uint64_t tls_total = 0;
                    uint64_t next_modid = 1;
                    int64_t main_tls_off = 0;
                    uint64_t main_tls_modid = 0;

                    if (main_tls_memsz > 0) {
                        uint64_t aligned = (main_tls_memsz + main_tls_align - 1)
                                           & ~(main_tls_align - 1);
                        tls_total += aligned;
                        main_tls_modid = next_modid++;
                        main_tls_off = -(int64_t)tls_total;
                    }

                    for (int mi = 0; mi < DL_MAX_MODULES; mi++) {
                        dl_module_t *mod = dl_get_module(mi);
                        if (!mod || mod->tls_memsz == 0) continue;
                        uint64_t al = mod->tls_align ? mod->tls_align : 1;
                        uint64_t aligned = (mod->tls_memsz + al - 1) & ~(al - 1);
                        tls_total += aligned;
                        mod->tls_modid = next_modid++;
                        mod->tls_offset = -(int64_t)tls_total;
                    }

                    /* Allocate: [TLS data] [tcbhead_t] [DTV] */
                    uint64_t dtv_slots = next_modid + 1;
                    uint64_t alloc_sz = tls_total + TCBHEAD_SIZE +
                                        dtv_slots * sizeof(uint64_t);
                    alloc_sz = (alloc_sz + 4095) & ~4095ULL;

                    uint8_t *tls_area = (uint8_t *)mem_alloc_aligned(alloc_sz, 4096);
                    if (tls_area) {
                        memset(tls_area, 0, alloc_sz);
                        uint64_t tp = (uint64_t)(tls_area + tls_total);
                        uint64_t *tcb = (uint64_t *)tp;
                        uint64_t *dtv = (uint64_t *)(tp + TCBHEAD_SIZE);

                        /* Copy .tdata init images */
                        if (main_tls_memsz > 0 && main_tls_initimg) {
                            memcpy((void *)(tp + main_tls_off),
                                   (void *)main_tls_initimg, main_tls_filesz);
                            dtv[main_tls_modid] = tp + main_tls_off;
                        }
                        for (int mi = 0; mi < DL_MAX_MODULES; mi++) {
                            dl_module_t *mod = dl_get_module(mi);
                            if (!mod || mod->tls_memsz == 0) continue;
                            if (mod->tls_filesz > 0 && mod->tls_initimage) {
                                memcpy((void *)(tp + mod->tls_offset),
                                       (void *)mod->tls_initimage,
                                       mod->tls_filesz);
                            }
                            dtv[mod->tls_modid] = tp + mod->tls_offset;
                        }

                        /* Fill tcbhead_t */
                        tcb[0] = tp;                /* tcb → self */
                        tcb[1] = (uint64_t)dtv;     /* DTV pointer */
                        tcb[2] = tp;                /* self (pthread) */
                        tcb[3] = 0;                 /* multiple_threads=0 */
                        tcb[4] = 0;                 /* sysinfo */

                        uint64_t tsc_lo, tsc_hi;
                        __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
                        uint64_t seed = (tsc_hi << 32) | tsc_lo;
                        tcb[5] = seed ^ 0xDEADBEEFCAFEBABEULL; /* stack_guard */
                        tcb[6] = seed ^ 0x1234567890ABCDEFULL;  /* pointer_guard */

                        /* Set MSR_FS_BASE (0xC0000100) */
                        __asm__ volatile("wrmsr"
                            : : "c"(0xC0000100U),
                                "a"((uint32_t)(tp & 0xFFFFFFFF)),
                                "d"((uint32_t)(tp >> 32)));
                        proc_set_fs_base(tp);
                        proc_add_region(tls_area, alloc_sz / 4096);

                        serial_puts("[TLS] TP=0x");
                        serial_puthex(tp, 16);
                        serial_puts(" size=");
                        serial_putdec(tls_total);
                        serial_puts(" mods=");
                        serial_putdec(next_modid - 1);
                        serial_puts(" DTV=0x");
                        serial_puthex((uint64_t)dtv, 16);
                        serial_puts("\n");
                    }
                }

                /* All libs loaded + TLS ready — now link (relocs + init) */
                dl_link_all();
            }

            /* Build temporary module for the main binary and apply relocations */
            uint64_t total_rela = 0;
            if ((dt_rela && dt_relasz > 0) || (dt_jmprel && dt_pltrelsz > 0)) {
                dl_module_t main_mod;
                memset(&main_mod, 0, sizeof(main_mod));
                main_mod.loaded = true;
                main_mod.load_bias = loaded.load_bias;

                if (dt_symtab)
                    main_mod.symtab = (dl_sym_t *)(loaded.load_bias + dt_symtab);
                if (dt_strtab) {
                    main_mod.strtab = (char *)(loaded.load_bias + dt_strtab);
                    main_mod.strtab_sz = dt_strsz;
                }

                /* Determine symbol count */
                if (dt_hash) {
                    uint32_t *ht = (uint32_t *)(loaded.load_bias + dt_hash);
                    main_mod.hashtab = ht;
                    main_mod.nbucket = ht[0];
                    main_mod.nchain = ht[1];
                    main_mod.sym_count = ht[1];
                } else if (dt_gnu_hash_val) {
                    uint32_t *gh = (uint32_t *)(loaded.load_bias + dt_gnu_hash_val);
                    main_mod.sym_count = dl_gnu_hash_nsyms(gh);
                } else if (dt_strtab > dt_symtab && dt_symtab != 0) {
                    main_mod.sym_count = (uint32_t)((dt_strtab - dt_symtab) / sizeof(dl_sym_t));
                } else {
                    main_mod.sym_count = 256;
                }

                serial_puts("[ELF] Symbols: ");
                serial_putdec(main_mod.sym_count);
                serial_puts("\n");

                /* Apply .rela.dyn */
                if (dt_rela && dt_relasz > 0) {
                    dl_rela_t *rela = (dl_rela_t *)(loaded.load_bias + dt_rela);
                    uint64_t count = dt_relasz / sizeof(dl_rela_t);
                    serial_puts("[ELF] .rela.dyn: ");
                    serial_putdec(count);
                    serial_puts(" entries\n");
                    dl_apply_rela(&main_mod, rela, count);
                    total_rela += count;
                }

                /* Apply .rela.plt */
                if (dt_jmprel && dt_pltrelsz > 0) {
                    dl_rela_t *jmprel = (dl_rela_t *)(loaded.load_bias + dt_jmprel);
                    uint64_t count = dt_pltrelsz / sizeof(dl_rela_t);
                    serial_puts("[ELF] .rela.plt: ");
                    serial_putdec(count);
                    serial_puts(" entries\n");
                    dl_apply_rela(&main_mod, jmprel, count);
                    total_rela += count;
                }

                serial_puts("[ELF] Total relocations: ");
                serial_putdec(total_rela);
                serial_puts("\n");
            }

            /* Skip main binary INIT_ARRAY — __libc_csu_init will call it
             * via __libc_start_main. Calling it here would double-init
             * (e.g. PartitionAlloc pool reservation would run twice). */
            if (dt_init_array && dt_init_arraysz > 0) {
                serial_puts("[ELF] Main binary has ");
                serial_putdec(dt_init_arraysz / 8);
                serial_puts(" INIT_ARRAY constructors (deferred to __libc_csu_init)\n");
            }
        }
    }

    /* Set up stack */
    const char *default_argv[] = { filename };
    if (!argv) { argv = default_argv; argc = 1; }

    uint64_t sp = elf_setup_stack(&loaded, argc, argv);
    if (sp == 0) {
        serial_puts("[ELF] Failed to set up stack\n");
        if (data) kfree(data);
        elf_free(&loaded);
        return -1;
    }

    /* Capture symbol tables for crash-dump symbolization before the
     * ELF buffer is released. Only the eager path has `data` here;
     * demand-paged binaries run without symbols. */
    if (data)
        elf_capture_symtab(data, file_size, loaded.load_bias);

    /* Free the read buffer (eager path only — demand-paged has no buffer) */
    if (data) kfree(data);

    /* Register ELF memory with the process for cleanup on exit.
     * Demand-paged segments are tracked via the global VMA table and
     * freed by syscall_reset_process; only register eager segments.
     *
     * For ET_EXEC (virt_mapped=true) we stash the vaddr_min on the FIRST
     * region so proc_execve can unmap the stale PTE range before freeing
     * the physical backing. Without this, VAs the old ELF installed
     * outlive the free and leak stale bytes into the next exec. */
    if (!loaded.demand_paged) {
        for (int i = 0; i < loaded.segment_count; i++) {
            if (loaded.segments[i] && loaded.segment_pages[i] > 0) {
                if (i == 0 && loaded.virt_mapped) {
                    proc_add_region_virt(loaded.segments[i],
                                         loaded.segment_pages[i],
                                         loaded.vaddr_min);
                } else {
                    proc_add_region(loaded.segments[i],
                                    loaded.segment_pages[i]);
                }
            }
        }
    }
    if (loaded.stack_base)
        proc_add_region((void *)VIRT_TO_PHYS(loaded.stack_base),
                        USER_STACK_SIZE / 4096);

    /* Analyze .text for speculation hints (basic blocks, loops, syscalls).
     * Results stored in process_t for use by spec_prefetch and future TLS. */
    {
        extern int spec_analyze_text(uint64_t base, uint64_t size, void *out);
        extern void *kmalloc(uint64_t size);
        /* Use first PT_LOAD segment as .text approximation */
        if (loaded.segment_count > 0 && loaded.entry) {
            void *analysis = kmalloc(sizeof(uint64_t) * 16);  /* spec_analysis_t */
            if (analysis) {
                /* Analyze 64KB around entry point as a reasonable .text estimate */
                uint64_t text_base = loaded.entry & ~0xFFFULL;
                uint64_t text_size = 65536;
                int blocks = spec_analyze_text(text_base, text_size, analysis);
                if (blocks > 0) {
                    extern void proc_set_spec_info(void *info);
                    proc_set_spec_info(analysis);
                } else {
                    kfree(analysis);
                }
            }
        }
    }

    /* Jump to entry — does not return */
    elf_jump(loaded.entry, sp);

    /* Unreachable */
    return 0;
}
