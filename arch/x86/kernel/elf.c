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

/* OsitoFS */
extern void *osfs2_find(const char *name);  /* returns osfs2_file_t* */
extern int osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);

/* Process — register memory for cleanup on exit */
extern void proc_add_region(void *base, uint64_t pages);

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
    bool fixed_load = (hdr->e_type == ET_EXEC);

    if (fixed_load) {
        /* Reserve the exact pages from the page allocator.
         * If pages are already allocated (fork+execve of same binary),
         * proceed anyway — we'll overwrite in place. */
        if (mem_reserve_range(vaddr_min, total_pages) < 0) {
            serial_puts("[ELF] Fixed-load at 0x");
            serial_puthex(vaddr_min, 16);
            serial_puts(" already mapped — reusing (fork+execve)\n");

            /* Save PF_W segments before memset destroys the parent's
             * runtime data. Restored in proc_wait4 after child exits. */
            fork_save_count = 0;
            for (int i = 0; i < hdr->e_phnum && fork_save_count < MAX_FORK_SAVES; i++) {
                uint64_t phoff2 = hdr->e_phoff + (uint64_t)i * hdr->e_phentsize;
                if (phoff2 + sizeof(elf64_phdr_t) > data_size) break;
                const elf64_phdr_t *ph2 = (const elf64_phdr_t *)(data + phoff2);
                if (ph2->p_type != PT_LOAD || ph2->p_memsz == 0) continue;
                if (!(ph2->p_flags & PF_W)) continue;

                void *save = kmalloc(ph2->p_memsz);
                if (save) {
                    memcpy(save, (void *)ph2->p_vaddr, ph2->p_memsz);
                    fork_saves[fork_save_count].buf  = save;
                    fork_saves[fork_save_count].addr = ph2->p_vaddr;
                    fork_saves[fork_save_count].size = ph2->p_memsz;
                    fork_save_count++;
                    serial_puts("[ELF] Saved parent RW segment at 0x");
                    serial_puthex(ph2->p_vaddr, 16);
                    serial_puts(" (");
                    serial_putdec(ph2->p_memsz);
                    serial_puts(" bytes)\n");
                }
            }
        }
        base = (void *)vaddr_min;
        serial_puts("[ELF] Fixed load at vaddr 0x");
        serial_puthex(vaddr_min, 16);
        serial_puts(", ");
        serial_putdec(total_pages * 4);
        serial_puts(" KB\n");
    } else {
        /* PIE/shared: allocate dynamic memory */
        base = mem_alloc_aligned(total_pages * 4096, 4096);
        if (!base) {
            serial_puts("[ELF] Failed to allocate ");
            serial_putdec(total_pages);
            serial_puts(" pages\n");
            return -1;
        }
        serial_puts("[ELF] Load base: 0x");
        serial_puthex((uint64_t)base, 16);
        serial_puts(", ");
        serial_putdec(total_pages * 4);
        serial_puts(" KB\n");
    }

    /* Zero entire region (BSS segments need zeros) */
    memset(base, 0, total_pages * 4096);

    loaded->segments[0] = base;
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
                serial_puts("[ELF] Segment data out of bounds\n");
                return -1;
            }
            memcpy((uint8_t *)base + offset_in_block,
                   data + ph->p_offset, ph->p_filesz);
        }
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

    serial_puts("[ELF] Entry: 0x");
    serial_puthex(loaded->entry, 16);
    serial_puts("\n");

    return 0;
}

/* ── Set up user stack ───────────────────────────────────────── */

static uint64_t elf_setup_stack(elf_loaded_t *loaded,
                                int argc, const char **argv)
{
    /* Allocate stack */
    uint64_t stack_pages = USER_STACK_SIZE / 4096;
    loaded->stack_base = mem_alloc_aligned(USER_STACK_SIZE, 4096);
    if (!loaded->stack_base) return 0;

    memset(loaded->stack_base, 0, USER_STACK_SIZE);

    /* Stack grows downward. Top = base + size. */
    uint64_t sp = (uint64_t)loaded->stack_base + USER_STACK_SIZE;

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

    /* Calculate string space needed */
    int effective_argc = (argc > ELF_MAX_ARGS) ? ELF_MAX_ARGS : argc;
    uint64_t str_need = 0;
    for (int i = 0; i < effective_argc; i++)
        str_need += strlen(argv[i]) + 1;
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

    /* Push auxv (reverse order so AT_NULL is last/highest) */
    for (int i = auxv_count - 1; i >= 0; i--) {
        sp -= 8; *(uint64_t *)sp = auxv[i].val;
        sp -= 8; *(uint64_t *)sp = auxv[i].type;
    }

    /* Push envp terminator (NULL) */
    sp -= 8;
    *(uint64_t *)sp = 0;

    /* Push argv terminator (NULL) */
    sp -= 8;
    *(uint64_t *)sp = 0;

    /* Push argv pointers (reverse order) */
    for (int i = effective_argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = argv_ptrs[i];
    }

    /* Push argc */
    sp -= 8;
    *(uint64_t *)sp = (uint64_t)effective_argc;

    loaded->stack_top = sp;
    (void)stack_pages;

    return sp;
}

/* ── Free loaded ELF resources ───────────────────────────────── */

void elf_free(elf_loaded_t *loaded)
{
    for (int i = 0; i < loaded->segment_count; i++) {
        if (loaded->segments[i] && loaded->segment_pages[i] > 0)
            mem_free_pages(loaded->segments[i], loaded->segment_pages[i]);
    }
    if (loaded->stack_base)
        mem_free_pages(loaded->stack_base, USER_STACK_SIZE / 4096);
}

/* ── Execute ELF entry point ─────────────────────────────────── */

static void elf_jump(uint64_t entry, uint64_t sp)
{
    serial_puts("[ELF] Jumping to entry 0x");
    serial_puthex(entry, 16);
    serial_puts(" with SP=0x");
    serial_puthex(sp, 16);
    serial_puts("\n");

    /*
     * Set up stack pointer and jump to ELF entry.
     * The entry point expects:
     *   RSP → argc, argv[0], ..., argv[n-1], NULL, envp[0], ..., NULL
     * We call it like a regular function but with RSP set to our stack.
     */
    __asm__ volatile (
        "mov %0, %%rsp\n"
        "xor %%rbp, %%rbp\n"  /* Clear frame pointer */
        "jmp *%1\n"
        : : "r"(sp), "r"(entry)
        : "memory"
    );

    __builtin_unreachable();
}

/* ── Public API: Load and execute ELF from OsitoFS ───────────── */

int elf_exec(const char *filename, int argc, const char **argv)
{
    serial_puts("[ELF] Loading '");
    serial_puts(filename);
    serial_puts("'...\n");

    fb_puts_color("\n Exec: ", 0x0000FF00);
    fb_puts(filename);
    fb_puts("\n");

    /* Find file in OsitoFS */
    /* We need the file size — osfs2_file_t has it at offset 64 */
    typedef struct {
        char     name[64];
        uint64_t size;
        /* ... more fields */
    } osfs2_file_min_t;

    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[ELF] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        fb_puts(" File not found\n");
        return -1;
    }

    osfs2_file_min_t *finfo = (osfs2_file_min_t *)file;
    uint64_t file_size = finfo->size;

    serial_puts("[ELF] File size: ");
    serial_putdec(file_size);
    serial_puts(" bytes\n");

    /* Check against available memory */
    if (file_size < sizeof(elf64_hdr_t)) {
        serial_puts("[ELF] File too small\n");
        return -1;
    }
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
        /* Need ~2x file size (read buffer + load segments) */
        if (!sys_caps_check_alloc(file_size * 2, filename)) {
            return -1;
        }
    }

    /* Read entire file into memory */
    uint8_t *data = (uint8_t *)kmalloc(file_size);
    if (!data) {
        serial_puts("[ELF] Failed to allocate read buffer\n");
        return -1;
    }

    if (osfs2_read(file, 0, data, file_size) < 0) {
        serial_puts("[ELF] Failed to read file\n");
        kfree(data);
        return -1;
    }

    /* Validate ELF header */
    if (elf_validate((const elf64_hdr_t *)data) < 0) {
        kfree(data);
        return -1;
    }

    /* Load segments */
    elf_loaded_t loaded;
    memset(&loaded, 0, sizeof(loaded));

    if (elf_load_segments(data, file_size, &loaded) < 0) {
        kfree(data);
        return -1;
    }

    /* ── Patch NT_GNU_ABI_TAG notes ────────────────────────────────
     * glibc binaries have a .note.ABI-tag that specifies the minimum
     * Linux kernel version. Since OsitoK is not Linux, we patch the
     * required version to 0.0.0 so the check always passes.
     * This is done in-memory after loading, not on disk. */
    {
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

    /* ── Dynamic linking ─────────────────────────────────────────────
     * If the binary has a PT_DYNAMIC segment, parse it to resolve
     * shared library dependencies and apply relocations. This bridges
     * elf.c → dynlink.c for dynamically linked executables. */
    {
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

            /* Call INIT_ARRAY constructors */
            if (dt_init_array && dt_init_arraysz > 0) {
                uint64_t init_count = dt_init_arraysz / 8;
                typedef void (*init_fn_t)(void);
                init_fn_t *fns = (init_fn_t *)(loaded.load_bias + dt_init_array);
                serial_puts("[ELF] Calling ");
                serial_putdec(init_count);
                serial_puts(" INIT_ARRAY constructors\n");
                for (uint64_t ci = 0; ci < init_count; ci++) {
                    if (fns[ci] && (uint64_t)fns[ci] != (uint64_t)-1)
                        fns[ci]();
                }
            }
        }
    }

    /* Set up stack */
    const char *default_argv[] = { filename };
    if (!argv) { argv = default_argv; argc = 1; }

    uint64_t sp = elf_setup_stack(&loaded, argc, argv);
    if (sp == 0) {
        serial_puts("[ELF] Failed to set up stack\n");
        kfree(data);
        elf_free(&loaded);
        return -1;
    }

    /* Free the read buffer (segments are already copied) */
    kfree(data);

    /* Register ELF memory with the process for cleanup on exit.
     * After elf_jump, the loaded struct is on the abandoned stack,
     * so proc_free needs its own copy of the regions. */
    for (int i = 0; i < loaded.segment_count; i++) {
        if (loaded.segments[i] && loaded.segment_pages[i] > 0)
            proc_add_region(loaded.segments[i], loaded.segment_pages[i]);
    }
    if (loaded.stack_base)
        proc_add_region(loaded.stack_base, USER_STACK_SIZE / 4096);

    /* Jump to entry — does not return */
    elf_jump(loaded.entry, sp);

    /* Unreachable */
    return 0;
}
