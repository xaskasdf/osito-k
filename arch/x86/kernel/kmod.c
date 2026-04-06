/*
 * OsitoK x86-64 — Kernel Module Loader
 *
 * Loads ELF64 relocatable objects (.ko) as kernel extensions.
 * Resolves symbols against kernel symbol table, applies relocations,
 * and calls module init function.
 *
 * Usage: insmod <module.ko> from shell
 *        Module exports init_module() which is called after load.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* ── ELF64 Structures ───────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;         /* ET_REL=1 for relocatable */
    uint16_t e_machine;      /* EM_X86_64=62 */
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;        /* Section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
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

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

#define ELF_MAGIC       0x464C457F  /* 0x7F ELF */
#define ET_REL          1
#define EM_X86_64       62
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_PROGBITS    1
#define SHT_NOBITS      8

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i)  ((i) & 0xFFFFFFFF)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0x0F)

#define R_X86_64_64       1
#define R_X86_64_PC32     2
#define R_X86_64_32       10
#define R_X86_64_32S      11

/* ── Kernel Symbol Table ─────────────────────────────────────── */

#define MAX_KSYMS 256

typedef struct {
    const char *name;
    uint64_t    addr;
} ksym_entry_t;

static ksym_entry_t ksym_table[MAX_KSYMS];
static int ksym_count;

/* Register a kernel symbol (called during boot for key exports) */
void kmod_register_symbol(const char *name, uint64_t addr)
{
    if (ksym_count >= MAX_KSYMS) return;
    ksym_table[ksym_count].name = name;
    ksym_table[ksym_count].addr = addr;
    ksym_count++;
}

/* Lookup kernel symbol by name */
static uint64_t kmod_find_symbol(const char *name)
{
    for (int i = 0; i < ksym_count; i++) {
        const char *a = ksym_table[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return ksym_table[i].addr;
    }
    return 0;
}

/* ── Module State ────────────────────────────────────────────── */

#define MAX_MODULES 16

typedef struct {
    char     name[64];
    void    *base;           /* Allocated memory for all sections */
    uint64_t size;
    void   (*cleanup)(void); /* Module cleanup function (optional) */
    bool     loaded;
} kmod_t;

static kmod_t modules[MAX_MODULES];

/* ── Load Module ─────────────────────────────────────────────── */

int kmod_load(const char *name, const uint8_t *data, uint64_t data_len)
{
    /* Validate ELF header */
    if (data_len < sizeof(elf64_ehdr_t)) return -1;
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC ||
        ehdr->e_type != ET_REL ||
        ehdr->e_machine != EM_X86_64) {
        serial_puts("[KMOD] Invalid ELF (not x86_64 relocatable)\n");
        return -1;
    }

    /* Find free module slot */
    int slot = -1;
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!modules[i].loaded) { slot = i; break; }
    }
    if (slot < 0) {
        serial_puts("[KMOD] Too many modules loaded\n");
        return -1;
    }

    /* Calculate total memory needed for all PROGBITS + NOBITS sections */
    elf64_shdr_t *shdrs = (elf64_shdr_t *)(data + ehdr->e_shoff);
    uint64_t total_size = 0;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_PROGBITS || shdrs[i].sh_type == SHT_NOBITS) {
            uint64_t align = shdrs[i].sh_addralign;
            if (align > 1) total_size = (total_size + align - 1) & ~(align - 1);
            total_size += shdrs[i].sh_size;
        }
    }

    /* Allocate module memory */
    uint8_t *base = (uint8_t *)kmalloc(total_size);
    if (!base) {
        serial_puts("[KMOD] Failed to allocate ");
        serial_putdec(total_size);
        serial_puts(" bytes\n");
        return -1;
    }
    memset(base, 0, total_size);

    /* Load sections and track their runtime addresses */
    uint64_t *sec_addrs = (uint64_t *)kmalloc(ehdr->e_shnum * 8);
    if (!sec_addrs) { kfree(base); return -1; }
    memset(sec_addrs, 0, ehdr->e_shnum * 8);

    uint64_t offset = 0;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_PROGBITS) {
            uint64_t align = shdrs[i].sh_addralign;
            if (align > 1) offset = (offset + align - 1) & ~(align - 1);
            memcpy(base + offset, data + shdrs[i].sh_offset, shdrs[i].sh_size);
            sec_addrs[i] = (uint64_t)(base + offset);
            offset += shdrs[i].sh_size;
        } else if (shdrs[i].sh_type == SHT_NOBITS) {
            uint64_t align = shdrs[i].sh_addralign;
            if (align > 1) offset = (offset + align - 1) & ~(align - 1);
            sec_addrs[i] = (uint64_t)(base + offset);
            offset += shdrs[i].sh_size;  /* BSS — already zeroed */
        }
    }

    /* Find symbol table and string table */
    elf64_sym_t *symtab = NULL;
    uint32_t sym_count = 0;
    const char *strtab = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab = (elf64_sym_t *)(data + shdrs[i].sh_offset);
            sym_count = (uint32_t)(shdrs[i].sh_size / sizeof(elf64_sym_t));
            if (shdrs[i].sh_link < ehdr->e_shnum)
                strtab = (const char *)(data + shdrs[shdrs[i].sh_link].sh_offset);
        }
    }

    /* Resolve symbols: update values for defined symbols, look up externals */
    if (symtab) {
        for (uint32_t s = 0; s < sym_count; s++) {
            if (symtab[s].st_shndx > 0 && symtab[s].st_shndx < ehdr->e_shnum) {
                /* Defined symbol: adjust value relative to loaded section */
                symtab[s].st_value += sec_addrs[symtab[s].st_shndx];
            } else if (symtab[s].st_shndx == 0 && ELF64_ST_BIND(symtab[s].st_info) == 1) {
                /* Undefined global: look up in kernel symbol table */
                const char *sym_name = strtab ? strtab + symtab[s].st_name : "";
                uint64_t addr = kmod_find_symbol(sym_name);
                if (addr == 0) {
                    serial_puts("[KMOD] Unresolved symbol: ");
                    serial_puts(sym_name);
                    serial_puts("\n");
                    kfree(sec_addrs);
                    kfree(base);
                    return -1;
                }
                symtab[s].st_value = addr;
            }
        }
    }

    /* Apply relocations */
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_RELA) continue;
        if (shdrs[i].sh_info >= ehdr->e_shnum) continue;

        uint64_t target_base = sec_addrs[shdrs[i].sh_info];
        if (target_base == 0) continue;

        elf64_rela_t *relas = (elf64_rela_t *)(data + shdrs[i].sh_offset);
        uint32_t nrelas = (uint32_t)(shdrs[i].sh_size / sizeof(elf64_rela_t));

        for (uint32_t r = 0; r < nrelas; r++) {
            uint32_t sym_idx = (uint32_t)ELF64_R_SYM(relas[r].r_info);
            uint32_t rtype = (uint32_t)ELF64_R_TYPE(relas[r].r_info);
            uint64_t sym_val = symtab ? symtab[sym_idx].st_value : 0;
            uint8_t *patch = (uint8_t *)(target_base + relas[r].r_offset);

            switch (rtype) {
            case R_X86_64_64:
                *(uint64_t *)patch = sym_val + relas[r].r_addend;
                break;
            case R_X86_64_PC32:
            case R_X86_64_32S:
                *(int32_t *)patch = (int32_t)(sym_val + relas[r].r_addend -
                                              (uint64_t)patch);
                break;
            case R_X86_64_32:
                *(uint32_t *)patch = (uint32_t)(sym_val + relas[r].r_addend);
                break;
            }
        }
    }

    kfree(sec_addrs);

    /* Find and call init_module() */
    void (*init_fn)(void) = NULL;
    if (symtab && strtab) {
        for (uint32_t s = 0; s < sym_count; s++) {
            const char *sn = strtab + symtab[s].st_name;
            if (sn[0] == 'i' && sn[1] == 'n' && sn[2] == 'i' && sn[3] == 't' &&
                sn[4] == '_' && sn[5] == 'm' && sn[6] == 'o' && sn[7] == 'd' &&
                sn[8] == 'u' && sn[9] == 'l' && sn[10] == 'e' && sn[11] == '\0') {
                init_fn = (void (*)(void))symtab[s].st_value;
                break;
            }
        }
    }

    /* Register module */
    kmod_t *mod = &modules[slot];
    int nl = 0;
    while (name[nl] && nl < 63) { mod->name[nl] = name[nl]; nl++; }
    mod->name[nl] = '\0';
    mod->base = base;
    mod->size = total_size;
    mod->loaded = true;

    serial_puts("[KMOD] Loaded \"");
    serial_puts(mod->name);
    serial_puts("\" at 0x");
    serial_puthex((uint64_t)base, 16);
    serial_puts(" (");
    serial_putdec(total_size);
    serial_puts(" bytes)\n");

    if (init_fn) {
        serial_puts("[KMOD] Calling init_module()...\n");
        init_fn();
    }

    return 0;
}

/* List loaded modules */
void kmod_list(void)
{
    serial_puts("[KMOD] Loaded modules:\n");
    int count = 0;
    for (int i = 0; i < MAX_MODULES; i++) {
        if (modules[i].loaded) {
            serial_puts("  ");
            serial_puts(modules[i].name);
            serial_puts("  (");
            serial_putdec(modules[i].size);
            serial_puts(" bytes)\n");
            count++;
        }
    }
    if (count == 0) serial_puts("  (none)\n");
}

/* Unload module */
int kmod_unload(const char *name)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!modules[i].loaded) continue;
        const char *a = modules[i].name, *b = name;
        bool match = true;
        while (*a && *b) { if (*a++ != *b++) { match = false; break; } }
        if (match && *a == *b) {
            if (modules[i].cleanup) modules[i].cleanup();
            kfree(modules[i].base);
            modules[i].loaded = false;
            serial_puts("[KMOD] Unloaded \"");
            serial_puts(name);
            serial_puts("\"\n");
            return 0;
        }
    }
    return -1;
}
