/*
 * elf.c -- ELF64 loader for AArch64
 *
 * Loads static ET_EXEC binaries. Identity-mapped (no separate address space).
 * Validates EM_AARCH64, maps PT_LOAD segments, sets up user stack.
 */

#include "../include/hal.h"
#include "../include/types.h"

/* ── ELF64 definitions ──────────────────────────────────── */

#define EI_NIDENT   16
#define ELFMAG      "\177ELF"
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_AARCH64  183
#define ET_EXEC     2
#define PT_LOAD     1

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
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* ── Load result ─────────────────────────────────────────── */

typedef struct {
    uint64_t entry;
    uint64_t stack_top;     /* user stack top (grows down) */
    uint64_t brk_base;     /* end of loaded segments (for brk) */
} elf_info_t;

/* ── ELF loader ──────────────────────────────────────────── */

int elf_load(const void *data, uint64_t size, elf_info_t *info)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* Validate ELF magic */
    if (size < sizeof(Elf64_Ehdr) ||
        ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        serial_puts("[ELF ] Bad magic\n");
        return -1;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        serial_puts("[ELF ] Not 64-bit\n");
        return -1;
    }

    if (ehdr->e_machine != EM_AARCH64) {
        serial_puts("[ELF ] Not AArch64 (machine=");
        serial_putdec(ehdr->e_machine);
        serial_puts(")\n");
        return -1;
    }

    if (ehdr->e_type != ET_EXEC) {
        serial_puts("[ELF ] Not ET_EXEC\n");
        return -1;
    }

    serial_puts("[ELF ] Entry: ");
    serial_puthex(ehdr->e_entry, 16);
    serial_puts(", ");
    serial_putdec(ehdr->e_phnum);
    serial_puts(" segments\n");

    /* Load PT_LOAD segments */
    uint64_t max_vaddr = 0;
    const uint8_t *base = (const uint8_t *)data;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff +
                                                       i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;

        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;

        serial_puts("[ELF ] LOAD: vaddr=");
        serial_puthex(vaddr, 8);
        serial_puts(" filesz=");
        serial_putdec(filesz);
        serial_puts(" memsz=");
        serial_putdec(memsz);
        serial_puts("\n");

        /* Allocate pages and copy data.
         * Since we run identity-mapped, we allocate physical pages
         * and the vaddr MUST equal the physical address.
         * For simplicity: allocate at any address, memcpy data there,
         * and the binary must be position-independent or linked at
         * the address we allocate. We'll use the allocated address. */

        uint64_t pages = (memsz + 4095) / 4096;
        void *seg = mem_alloc_pages(pages);
        if (!seg) {
            serial_puts("[ELF ] alloc failed\n");
            return -1;
        }

        /* Copy file data */
        if (filesz > 0 && phdr->p_offset + filesz <= size)
            memcpy(seg, base + phdr->p_offset, filesz);

        /* Zero BSS (memsz > filesz) — already zeroed by alloc */

        /* Track highest address for brk */
        uint64_t end = (uint64_t)seg + memsz;
        if (end > max_vaddr) max_vaddr = end;

        /* If vaddr doesn't match our allocation, fixup entry point */
        if (i == 0 && vaddr != (uint64_t)seg) {
            /* Rebase entry: entry = entry - vaddr + seg */
            info->entry = ehdr->e_entry - vaddr + (uint64_t)seg;
        }
    }

    if (info->entry == 0)
        info->entry = ehdr->e_entry;

    /* Allocate user stack (64KB) */
    void *stack = mem_alloc_pages(16);  /* 64KB */
    if (!stack) {
        serial_puts("[ELF ] stack alloc failed\n");
        return -1;
    }
    info->stack_top = (uint64_t)stack + 16 * 4096;
    info->brk_base = (max_vaddr + 4095) & ~4095ULL;

    /* Set up argc/argv/envp on stack (minimal: argc=0, no argv) */
    uint64_t sp = info->stack_top;
    sp -= 8; *(uint64_t *)sp = 0;   /* envp terminator */
    sp -= 8; *(uint64_t *)sp = 0;   /* argv terminator */
    sp -= 8; *(uint64_t *)sp = 0;   /* argc = 0 */
    sp &= ~15ULL;                    /* 16-byte align */
    info->stack_top = sp;

    serial_puts("[ELF ] Loaded: entry=");
    serial_puthex(info->entry, 16);
    serial_puts(" sp=");
    serial_puthex(info->stack_top, 16);
    serial_puts("\n");

    return 0;
}
