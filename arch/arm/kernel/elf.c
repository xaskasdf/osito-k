/*
 * elf.c -- ELF64 loader for AArch64
 *
 * Programs run identity-mapped in the kernel's address space, wherever the
 * page allocator puts them. All PT_LOAD segments go into one contiguous
 * allocation at their original distances, since code reaches its data
 * PC-relative. A static PIE (ET_DYN, as arch/arm/apps builds them) then
 * gets its R_AARCH64_RELATIVE relocations applied, the way start.S
 * relocates the kernel, so pointers stored in its data work too. An
 * ET_EXEC only runs if it is position-independent code with no stored
 * pointers (test/hello.S).
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "elf.h"

/* ── ELF64 definitions ──────────────────────────────────── */

#define EI_NIDENT   16
#define ELFMAG      "\177ELF"
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_AARCH64  183
#define ET_EXEC     2
#define ET_DYN      3
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define R_AARCH64_NONE      0
#define R_AARCH64_RELATIVE  1027

#define STACK_PAGES 64                  /* 256 KB */

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

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

/* Code just copied into [p, p + len) runs next: with the caches on, push it
 * out of the D-cache to the point of unification and drop stale I-cache
 * lines (the pages may have held an older kernel's code). */
static void code_written(void *p, uint64_t len)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xF);    /* DminLine, in bytes */
    for (uint64_t a = (uint64_t)p & ~(line - 1); a < (uint64_t)p + len; a += line)
        __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
}

static const Elf64_Phdr *phdr_at(const uint8_t *file, const Elf64_Ehdr *eh, int i)
{
    return (const Elf64_Phdr *)(file + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
}

/* A static PIE's RELATIVE relocations, found through PT_DYNAMIC (already
 * copied into the image). `bias` = where it landed - where it was linked. */
static int relocate(uint8_t *image, uint64_t lo, uint64_t span, uint64_t dyn_vaddr,
                    uint64_t bias)
{
    uint64_t rela = 0, relasz = 0, relaent = sizeof(Elf64_Rela);
    for (const Elf64_Dyn *d = (const Elf64_Dyn *)(image + (dyn_vaddr - lo));
         (uint8_t *)d < image + span && d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) rela = d->d_val;
        else if (d->d_tag == DT_RELASZ) relasz = d->d_val;
        else if (d->d_tag == DT_RELAENT) relaent = d->d_val;
    }
    if (!relasz)
        return 0;
    if (rela < lo || rela - lo + relasz > span || relaent != sizeof(Elf64_Rela))
        return -1;
    for (uint64_t off = 0; off < relasz; off += relaent) {
        const Elf64_Rela *r = (const Elf64_Rela *)(image + (rela - lo) + off);
        uint32_t type = (uint32_t)r->r_info;
        if (type == R_AARCH64_NONE)
            continue;
        if (type != R_AARCH64_RELATIVE || r->r_offset < lo || r->r_offset - lo + 8 > span) {
            serial_puts("[ELF ] unsupported relocation, type ");
            serial_putdec(type);
            serial_puts("\n");
            return -1;
        }
        *(uint64_t *)(image + (r->r_offset - lo)) = (uint64_t)(r->r_addend + (int64_t)bias);
    }
    return 0;
}

/* ── ELF loader ──────────────────────────────────────────── */

int elf_load(const void *data, uint64_t size, elf_info_t *info)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    const uint8_t *file = (const uint8_t *)data;

    if (size < sizeof(Elf64_Ehdr) ||
        eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        serial_puts("[ELF ] Bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_machine != EM_AARCH64) {
        serial_puts("[ELF ] Not an AArch64 ELF64\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        serial_puts("[ELF ] Neither ET_EXEC nor ET_DYN\n");
        return -1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size ||
        eh->e_phentsize < sizeof(Elf64_Phdr)) {
        serial_puts("[ELF ] Bad program headers\n");
        return -1;
    }

    /* The span of every PT_LOAD, page-aligned */
    uint64_t lo = ~0ULL, hi = 0, dyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(file, eh, i);
        if (ph->p_type == PT_DYNAMIC)
            dyn = ph->p_vaddr;
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
            serial_puts("[ELF ] Segment outside the file\n");
            return -1;
        }
        if (ph->p_vaddr < lo) lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi) hi = ph->p_vaddr + ph->p_memsz;
    }
    lo &= ~4095ULL;
    if (hi <= lo || hi - lo > (256ULL << 20)) {
        serial_puts("[ELF ] No loadable segments, or too big\n");
        return -1;
    }
    uint64_t span = hi - lo;
    uint64_t pages = (span + 4095) / 4096;
    uint8_t *image = mem_alloc_pages(pages);
    if (!image) {
        serial_puts("[ELF ] Out of memory\n");
        return -1;
    }
    memset(image, 0, pages * 4096);         /* BSS included */
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(file, eh, i);
        if (ph->p_type == PT_LOAD && ph->p_filesz)
            memcpy(image + (ph->p_vaddr - lo), file + ph->p_offset, ph->p_filesz);
    }
    uint64_t bias = (uint64_t)(uintptr_t)image - lo;
    if (eh->e_type == ET_DYN && dyn && relocate(image, lo, span, dyn, bias)) {
        mem_free_pages(image, pages);
        return -1;
    }
    code_written(image, span);

    uint8_t *stack = mem_alloc_pages(STACK_PAGES);
    if (!stack) {
        serial_puts("[ELF ] Out of memory for the stack\n");
        mem_free_pages(image, pages);
        return -1;
    }
    info->entry = eh->e_entry + bias;
    info->image = (uint64_t)(uintptr_t)image;
    info->image_pages = pages;
    info->stack = (uint64_t)(uintptr_t)stack;
    info->stack_pages = STACK_PAGES;
    info->stack_top = (info->stack + STACK_PAGES * 4096) & ~15ULL;
    return 0;
}

void elf_unload(elf_info_t *info)
{
    if (info->image)
        mem_free_pages((void *)(uintptr_t)info->image, info->image_pages);
    if (info->stack)
        mem_free_pages((void *)(uintptr_t)info->stack, info->stack_pages);
    info->image = info->stack = 0;
}
