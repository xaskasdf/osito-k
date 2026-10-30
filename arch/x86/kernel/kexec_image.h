#ifndef OSITO_KEXEC_IMAGE_H
#define OSITO_KEXEC_IMAGE_H
#include "../include/types.h"
#include "../include/boot_info.h"

#define KEXEC_MAX_SEGMENTS 16
#define KEXEC_DIRECT_BASE 0xffff800000000000ULL
#define KEXEC_PHYS_LIMIT 0x100000000ULL

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} kexec_elf_header_t;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} kexec_elf_program_t;
typedef struct {
    uint64_t offset, physical, filesz, memsz;
} kexec_load_t;
typedef struct {
    uint64_t entry, low, high, staging_bytes;
    unsigned count;
    kexec_load_t load[KEXEC_MAX_SEGMENTS];
} kexec_image_t;

static inline bool kexec_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    if (!an || !bn) return false;
    if (an > UINT64_MAX - a || bn > UINT64_MAX - b) return true;
    return a < b + bn && b < a + an;
}

static inline const char *kexec_parse_image(const void *image, uint64_t size,
                                           kexec_image_t *plan)
{
    kexec_elf_header_t h;
    memset(plan, 0, sizeof(*plan));
    if (!image || size < sizeof(h)) return "short ELF header";
    memcpy(&h, image, sizeof(h));
    if (memcmp(h.ident, "\177ELF", 4) || h.ident[4] != 2 || h.ident[5] != 1 ||
        h.ident[6] != 1 || h.version != 1 || h.type != 2 || h.machine != 62 ||
        h.ehsize != sizeof(h)) return "expected ELF64 little-endian x86_64 ET_EXEC";
    if (!h.phnum || h.phnum > 128 || h.phentsize != sizeof(kexec_elf_program_t) ||
        h.phoff > size || h.phnum > (size - h.phoff) / sizeof(kexec_elf_program_t))
        return "invalid program-header table";
    plan->entry = h.entry;
    plan->low = UINT64_MAX;
    bool executable_entry = false;
    for (unsigned i = 0; i < h.phnum; i++) {
        kexec_elf_program_t p;
        memcpy(&p, (const uint8_t *)image + h.phoff + i * sizeof(p), sizeof(p));
        if (p.type != 1) continue;
        if (p.offset > size || p.filesz > size - p.offset || p.filesz > p.memsz)
            return "segment outside file";
        if (!p.memsz) continue;
        if (plan->count == KEXEC_MAX_SEGMENTS) return "too many load segments";
        if (p.paddr < 0x100000 || p.paddr >= KEXEC_PHYS_LIMIT ||
            p.memsz > KEXEC_PHYS_LIMIT - p.paddr || p.vaddr != KEXEC_DIRECT_BASE + p.paddr)
            return "invalid physical/direct-map destination";
        if (p.align > 1 && ((p.align & (p.align - 1)) ||
            ((p.vaddr ^ p.offset) & (p.align - 1)))) return "invalid segment alignment";
        for (unsigned j = 0; j < plan->count; j++)
            if (kexec_overlap(p.paddr, p.memsz, plan->load[j].physical, plan->load[j].memsz))
                return "overlapping load segments";
        if ((p.flags & 1) && h.entry >= p.vaddr && h.entry - p.vaddr < p.filesz)
            executable_entry = true;
        plan->load[plan->count++] = (kexec_load_t){p.offset, p.paddr, p.filesz, p.memsz};
        if (p.paddr < plan->low) plan->low = p.paddr;
        if (p.paddr + p.memsz > plan->high) plan->high = p.paddr + p.memsz;
        uint64_t bytes = (p.filesz + 4095) & ~4095ULL;
        if (bytes > UINT64_MAX - plan->staging_bytes) return "staging size overflow";
        plan->staging_bytes += bytes;
    }
    if (!plan->count || !executable_entry) return "entry is not in file-backed executable code";
    return NULL;
}

/* UEFI descriptors have this prefix, independently of their advertised stride. */
typedef struct {
    uint32_t type, pad;
    uint64_t physical, virtual_address, pages, attributes;
} kexec_memory_descriptor_t;

static inline bool kexec_destination_is_ram(const boot_info_t *boot,
                                            uint64_t base, uint64_t size)
{
    if (!boot->mmap_addr || boot->mmap_desc_size < sizeof(kexec_memory_descriptor_t) ||
        !boot->mmap_size || boot->mmap_size > 65536 ||
        boot->mmap_size > UINT64_MAX - boot->mmap_addr ||
        boot->mmap_size % boot->mmap_desc_size ||
        !size || base >= KEXEC_PHYS_LIMIT || size > KEXEC_PHYS_LIMIT - base) return false;
    uint64_t end = base + size;
    while (base < end) {
        uint64_t next = base;
        for (uint64_t off = 0; off < boot->mmap_size; off += boot->mmap_desc_size) {
            kexec_memory_descriptor_t d;
            memcpy(&d, (const uint8_t *)(uintptr_t)boot->mmap_addr + off, sizeof(d));
            if ((d.physical & 4095) || d.pages > (UINT64_MAX - d.physical) / 4096) return false;
            if (!d.pages) continue;
            if (d.type != 1 && d.type != 2 && d.type != 3 && d.type != 4 && d.type != 7) {
                if (kexec_overlap(d.physical, d.pages * 4096, base, end - base)) return false;
                continue;
            }
            uint64_t limit = d.physical + d.pages * 4096;
            if (d.physical <= base && limit > next) next = limit;
        }
        if (next == base) return false;
        base = next;
    }
    return true;
}
#endif
