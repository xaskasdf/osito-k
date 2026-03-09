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
#define PT_INTERP   3
#define PT_PHDR     6

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

/* ── ELF state ───────────────────────────────────────────────── */

#define ELF_MAX_SEGMENTS 16
#define USER_STACK_SIZE  (64 * 1024)  /* 64 KB stack */

typedef struct {
    uint64_t entry;
    uint64_t stack_top;
    void    *stack_base;     /* For freeing later */
    void    *segments[ELF_MAX_SEGMENTS];
    int      segment_count;
    uint64_t segment_pages[ELF_MAX_SEGMENTS];
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

    serial_puts("[ELF] Entry: 0x");
    serial_puthex(hdr->e_entry, 16);
    serial_puts(", ");
    serial_putdec(hdr->e_phnum);
    serial_puts(" program headers\n");

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
    uint64_t string_area = sp - 256;  /* Reserve 256 bytes for strings */
    uint64_t str_ptr = string_area;

    uint64_t argv_ptrs[16];
    int effective_argc = (argc > 16) ? 16 : argc;

    for (int i = 0; i < effective_argc; i++) {
        uint64_t len = strlen(argv[i]) + 1;
        memcpy((void *)str_ptr, argv[i], len);
        argv_ptrs[i] = str_ptr;
        str_ptr += len;
    }

    /* Build stack frame below string area */
    sp = string_area;
    sp &= ~0xFULL;  /* Align to 16 bytes */

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

    /* Sanity check */
    if (file_size < sizeof(elf64_hdr_t) || file_size > 64 * 1024 * 1024) {
        serial_puts("[ELF] Invalid file size\n");
        return -1;
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
