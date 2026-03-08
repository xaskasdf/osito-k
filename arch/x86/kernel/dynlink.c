/*
 * OsitoK x86-64 — Dynamic Linker
 *
 * X-DYN: Runtime loading of ET_DYN ELF shared objects.
 * Provides dl_open/dl_sym/dl_close for kernel-level dynamic linking.
 *
 * Supports:
 *   - PT_LOAD segment loading with contiguous allocation
 *   - PT_DYNAMIC parsing (DT_SYMTAB, DT_STRTAB, DT_HASH, DT_RELA, DT_JMPREL)
 *   - Relocations: R_X86_64_RELATIVE, R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
 *   - Kernel symbol resolution via export table
 *   - ELF SysV hash for O(1) symbol lookup
 *   - DT_INIT/DT_FINI constructor/destructor calls
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
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);

/* ── ELF64 types (local to avoid conflict with elf.c) ───────── */

#define DL_EI_NIDENT   16
#define DL_ELFCLASS64  2
#define DL_ELFDATA2LSB 1
#define DL_EM_X86_64   62
#define DL_ET_DYN      3

#define DL_PT_NULL     0
#define DL_PT_LOAD     1
#define DL_PT_DYNAMIC  2

/* Dynamic tags */
#define DT_NULL     0
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_PLTRELSZ 2
#define DT_GNU_HASH 0x6ffffef5

/* Relocation types */
#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define ELF64_R_SYM(i)    ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)   ((uint32_t)((i) & 0xffffffffULL))
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)

#define STB_GLOBAL  1
#define STB_WEAK    2
#define STT_FUNC    2
#define STT_OBJECT  1
#define SHN_UNDEF   0

typedef struct {
    uint8_t  e_ident[DL_EI_NIDENT];
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
} dl_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} dl_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} dl_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} dl_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} dl_rela_t;

/* ── Module table ───────────────────────────────────────────── */

#define MAX_MODULES     8
#define MAX_MOD_NAME    64

typedef struct {
    bool        loaded;
    char        name[MAX_MOD_NAME];
    void       *base;
    uint64_t    load_bias;     /* base - vaddr_min */
    uint64_t    size;
    uint64_t    pages;

    /* Dynamic symbol table */
    dl_sym_t   *symtab;
    char       *strtab;
    uint64_t    strtab_sz;
    uint32_t    sym_count;

    /* ELF SysV hash */
    uint32_t   *hashtab;
    uint32_t    nbucket;
    uint32_t    nchain;

    /* Init/fini */
    void      (*init_fn)(void);
    void      (*fini_fn)(void);

    uint32_t    refcount;
} dl_module_t;

static dl_module_t modules[MAX_MODULES];

/* ── Kernel symbol export table ─────────────────────────────── */

typedef struct {
    const char *name;
    uint64_t    addr;
} ksym_entry_t;

static const ksym_entry_t ksym_table[] = {
    { "serial_puts",    (uint64_t)serial_puts    },
    { "serial_putdec",  (uint64_t)serial_putdec  },
    { "serial_puthex",  (uint64_t)serial_puthex  },
    { "fb_puts",        (uint64_t)fb_puts        },
    { "fb_puts_color",  (uint64_t)fb_puts_color  },
    { "fb_putdec",      (uint64_t)fb_putdec      },
    { "kmalloc",        (uint64_t)kmalloc        },
    { "kfree",          (uint64_t)kfree          },
    { NULL, 0 }
};

static uint64_t ksym_resolve(const char *name)
{
    for (int i = 0; ksym_table[i].name; i++) {
        if (strcmp(ksym_table[i].name, name) == 0)
            return ksym_table[i].addr;
    }
    return 0;
}

/* ── ELF SysV hash ──────────────────────────────────────────── */

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    const uint8_t *p = (const uint8_t *)name;
    while (*p) {
        h = (h << 4) + *p++;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* ── GNU hash: compute symbol count ─────────────────────────── */

static uint32_t gnu_hash_nsyms(const uint32_t *gnu_hash)
{
    uint32_t nbuckets   = gnu_hash[0];
    uint32_t symoffset  = gnu_hash[1];
    uint32_t bloom_size = gnu_hash[2];

    /* Skip header(4 uint32) + bloom(bloom_size * 8 bytes) */
    const uint64_t *bloom = (const uint64_t *)(gnu_hash + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chains  = buckets + nbuckets;

    /* Find highest bucket value */
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (buckets[i] > last)
            last = buckets[i];
    }
    if (last < symoffset)
        return symoffset;  /* All buckets empty */

    /* Follow chain from last to find the end */
    const uint32_t *ch = chains + (last - symoffset);
    while (!(*ch & 1)) {
        last++;
        ch++;
    }
    return last + 1;
}

/* ── Allocate module slot ───────────────────────────────────── */

static dl_module_t *mod_alloc(void)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!modules[i].loaded)
            return &modules[i];
    }
    return NULL;
}

/* ── Find module by name ────────────────────────────────────── */

static dl_module_t *mod_find(const char *name)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        if (modules[i].loaded && strcmp(modules[i].name, name) == 0)
            return &modules[i];
    }
    return NULL;
}

/* ── Resolve symbol for relocation ──────────────────────────── */

static uint64_t resolve_symbol(dl_module_t *m, uint32_t sym_idx)
{
    if (sym_idx == 0 || sym_idx >= m->sym_count)
        return 0;

    dl_sym_t *sym = &m->symtab[sym_idx];
    const char *name = m->strtab + sym->st_name;

    /* Defined in module */
    if (sym->st_shndx != SHN_UNDEF)
        return m->load_bias + sym->st_value;

    /* Look up in kernel export table */
    uint64_t addr = ksym_resolve(name);
    if (!addr) {
        serial_puts("[DL] Unresolved: ");
        serial_puts(name);
        serial_puts("\n");
    }
    return addr;
}

/* ── Apply relocations ──────────────────────────────────────── */

static int apply_rela(dl_module_t *m, const dl_rela_t *rela, uint64_t count)
{
    uint32_t resolved = 0, failed = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint32_t type    = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        uint64_t *target = (uint64_t *)(m->load_bias + rela[i].r_offset);

        switch (type) {
        case R_X86_64_RELATIVE:
            *target = m->load_bias + rela[i].r_addend;
            resolved++;
            break;

        case R_X86_64_64: {
            uint64_t val = resolve_symbol(m, sym_idx);
            if (!val && sym_idx != 0) { failed++; break; }
            *target = val + rela[i].r_addend;
            resolved++;
            break;
        }

        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            uint64_t val = resolve_symbol(m, sym_idx);
            if (!val && sym_idx != 0) { failed++; break; }
            *target = val;
            resolved++;
            break;
        }

        case R_X86_64_NONE:
            break;

        default:
            serial_puts("[DL] Unknown reloc type ");
            serial_putdec(type);
            serial_puts("\n");
            failed++;
            break;
        }
    }

    serial_puts("[DL] Relocations: ");
    serial_putdec(resolved);
    serial_puts(" applied");
    if (failed) {
        serial_puts(", ");
        serial_putdec(failed);
        serial_puts(" failed");
    }
    serial_puts("\n");

    return (failed > 0) ? -1 : 0;
}

/* ── dl_open: load shared object ────────────────────────────── */

void *dl_open(const char *filename)
{
    serial_puts("[DL] Opening '");
    serial_puts(filename);
    serial_puts("'\n");

    /* Check if already loaded */
    dl_module_t *existing = mod_find(filename);
    if (existing) {
        existing->refcount++;
        serial_puts("[DL] Already loaded, refcount=");
        serial_putdec(existing->refcount);
        serial_puts("\n");
        return existing;
    }

    /* Allocate module slot */
    dl_module_t *m = mod_alloc();
    if (!m) {
        serial_puts("[DL] Module table full\n");
        return NULL;
    }

    /* Read file from OsitoFS */
    typedef struct { char name[64]; uint64_t size; } finfo_t;
    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[DL] File not found\n");
        return NULL;
    }

    finfo_t *fi = (finfo_t *)file;
    uint64_t file_size = fi->size;

    if (file_size < sizeof(dl_ehdr_t) || file_size > 16 * 1024 * 1024) {
        serial_puts("[DL] Invalid file size\n");
        return NULL;
    }

    uint8_t *data = (uint8_t *)kmalloc(file_size);
    if (!data) {
        serial_puts("[DL] Out of memory for read\n");
        return NULL;
    }

    if (osfs2_read(file, 0, data, file_size) < 0) {
        kfree(data);
        serial_puts("[DL] Read failed\n");
        return NULL;
    }

    /* Validate ELF */
    dl_ehdr_t *ehdr = (dl_ehdr_t *)data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != DL_ELFCLASS64 || ehdr->e_ident[5] != DL_ELFDATA2LSB ||
        ehdr->e_machine != DL_EM_X86_64 || ehdr->e_type != DL_ET_DYN) {
        serial_puts("[DL] Not a valid x86-64 shared object\n");
        kfree(data);
        return NULL;
    }

    /* ── Pass 1: find LOAD range ─────────────────────────────── */

    uint64_t vmin = UINT64_MAX, vmax = 0;
    int load_count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint64_t off = ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize;
        if (off + sizeof(dl_phdr_t) > file_size) break;
        dl_phdr_t *ph = (dl_phdr_t *)(data + off);

        if (ph->p_type != DL_PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_vaddr < vmin) vmin = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > vmax)
            vmax = ph->p_vaddr + ph->p_memsz;
        load_count++;
    }

    if (load_count == 0) {
        serial_puts("[DL] No LOAD segments\n");
        kfree(data);
        return NULL;
    }

    uint64_t total_size = vmax - vmin;
    uint64_t total_pages = (total_size + 4095) / 4096;

    /* Allocate memory for module */
    void *base = mem_alloc_aligned(total_pages * 4096, 4096);
    if (!base) {
        serial_puts("[DL] Failed to allocate ");
        serial_putdec(total_pages);
        serial_puts(" pages\n");
        kfree(data);
        return NULL;
    }

    memset(base, 0, total_pages * 4096);
    uint64_t load_bias = (uint64_t)base - vmin;

    serial_puts("[DL] Base: 0x");
    serial_puthex((uint64_t)base, 16);
    serial_puts(", size: ");
    serial_putdec(total_pages * 4);
    serial_puts(" KB\n");

    /* ── Pass 2: copy LOAD segments ──────────────────────────── */

    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint64_t off = ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize;
        if (off + sizeof(dl_phdr_t) > file_size) break;
        dl_phdr_t *ph = (dl_phdr_t *)(data + off);

        if (ph->p_type != DL_PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t dest_off = ph->p_vaddr - vmin;
        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= file_size)
            memcpy((uint8_t *)base + dest_off, data + ph->p_offset, ph->p_filesz);
    }

    /* ── Pass 3: find PT_DYNAMIC ─────────────────────────────── */

    dl_dyn_t *dynamic = NULL;
    uint64_t dyn_count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint64_t off = ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize;
        if (off + sizeof(dl_phdr_t) > file_size) break;
        dl_phdr_t *ph = (dl_phdr_t *)(data + off);

        if (ph->p_type == DL_PT_DYNAMIC) {
            dynamic = (dl_dyn_t *)((uint8_t *)base + (ph->p_vaddr - vmin));
            dyn_count = ph->p_memsz / sizeof(dl_dyn_t);
            break;
        }
    }

    /* Free file data — segments already copied */
    kfree(data);

    if (!dynamic) {
        serial_puts("[DL] No PT_DYNAMIC segment\n");
        mem_free_pages(base, total_pages);
        return NULL;
    }

    /* ── Parse dynamic table ─────────────────────────────────── */

    uint64_t dt_symtab = 0, dt_strtab = 0, dt_strsz = 0;
    uint64_t dt_hash = 0, dt_gnu_hash = 0;
    uint64_t dt_rela = 0, dt_relasz = 0;
    uint64_t dt_jmprel = 0, dt_pltrelsz = 0;
    uint64_t dt_init = 0, dt_fini = 0;

    for (uint64_t i = 0; i < dyn_count; i++) {
        if (dynamic[i].d_tag == DT_NULL) break;
        switch (dynamic[i].d_tag) {
        case DT_SYMTAB:    dt_symtab    = dynamic[i].d_val; break;
        case DT_STRTAB:    dt_strtab    = dynamic[i].d_val; break;
        case DT_STRSZ:     dt_strsz     = dynamic[i].d_val; break;
        case DT_HASH:      dt_hash      = dynamic[i].d_val; break;
        case DT_GNU_HASH:  dt_gnu_hash  = dynamic[i].d_val; break;
        case DT_RELA:      dt_rela      = dynamic[i].d_val; break;
        case DT_RELASZ:    dt_relasz    = dynamic[i].d_val; break;
        case DT_JMPREL:    dt_jmprel    = dynamic[i].d_val; break;
        case DT_PLTRELSZ:  dt_pltrelsz  = dynamic[i].d_val; break;
        case DT_INIT:      dt_init      = dynamic[i].d_val; break;
        case DT_FINI:      dt_fini      = dynamic[i].d_val; break;
        }
    }

    /* Fill module structure */
    memset(m, 0, sizeof(*m));
    m->loaded    = true;
    m->base      = base;
    m->load_bias = load_bias;
    m->size      = total_size;
    m->pages     = total_pages;
    m->refcount  = 1;

    /* Copy name */
    int j = 0;
    while (filename[j] && j < MAX_MOD_NAME - 1) {
        m->name[j] = filename[j];
        j++;
    }
    m->name[j] = '\0';

    /* Resolve dynamic pointers (vaddr → loaded address) */
    if (dt_symtab)   m->symtab    = (dl_sym_t *)(load_bias + dt_symtab);
    if (dt_strtab)  { m->strtab   = (char *)(load_bias + dt_strtab); m->strtab_sz = dt_strsz; }
    if (dt_hash)     m->hashtab   = (uint32_t *)(load_bias + dt_hash);
    if (dt_init)     m->init_fn   = (void (*)(void))(load_bias + dt_init);
    if (dt_fini)     m->fini_fn   = (void (*)(void))(load_bias + dt_fini);

    /* Determine symbol count */
    if (m->hashtab) {
        m->nbucket   = m->hashtab[0];
        m->nchain    = m->hashtab[1];
        m->sym_count = m->nchain;
    } else if (dt_gnu_hash) {
        uint32_t *gh = (uint32_t *)(load_bias + dt_gnu_hash);
        m->sym_count = gnu_hash_nsyms(gh);
    } else {
        /* Heuristic: estimate from strtab proximity */
        if (dt_strtab > dt_symtab && dt_symtab != 0)
            m->sym_count = (uint32_t)((dt_strtab - dt_symtab) / sizeof(dl_sym_t));
        else
            m->sym_count = 64;  /* Fallback guess */
    }

    serial_puts("[DL] Symbols: ");
    serial_putdec(m->sym_count);
    serial_puts(", strtab: ");
    serial_putdec(dt_strsz);
    serial_puts(" bytes\n");

    /* ── Apply relocations ───────────────────────────────────── */

    int rela_ok = 0;

    /* .rela.dyn */
    if (dt_rela && dt_relasz > 0) {
        dl_rela_t *rela = (dl_rela_t *)(load_bias + dt_rela);
        uint64_t count = dt_relasz / sizeof(dl_rela_t);
        serial_puts("[DL] .rela.dyn: ");
        serial_putdec(count);
        serial_puts(" entries\n");
        if (apply_rela(m, rela, count) < 0)
            rela_ok = -1;
    }

    /* .rela.plt (DT_JMPREL) */
    if (dt_jmprel && dt_pltrelsz > 0) {
        dl_rela_t *rela = (dl_rela_t *)(load_bias + dt_jmprel);
        uint64_t count = dt_pltrelsz / sizeof(dl_rela_t);
        serial_puts("[DL] .rela.plt: ");
        serial_putdec(count);
        serial_puts(" entries\n");
        if (apply_rela(m, rela, count) < 0)
            rela_ok = -1;
    }

    if (rela_ok < 0) {
        serial_puts("[DL] Warning: some relocations failed\n");
        /* Continue anyway — module may still be partially usable */
    }

    /* Call init function */
    if (m->init_fn) {
        serial_puts("[DL] Calling init\n");
        m->init_fn();
    }

    serial_puts("[DL] Module '");
    serial_puts(m->name);
    serial_puts("' loaded OK\n");

    return m;
}

/* ── dl_sym: look up symbol by name ─────────────────────────── */

void *dl_sym(void *handle, const char *name)
{
    dl_module_t *m = (dl_module_t *)handle;
    if (!m || !m->loaded || !m->symtab || !m->strtab)
        return NULL;

    /* Try hash lookup first */
    if (m->hashtab && m->nbucket > 0) {
        uint32_t h = elf_hash(name);
        uint32_t *bucket = m->hashtab + 2;
        uint32_t *chain  = bucket + m->nbucket;

        uint32_t idx = bucket[h % m->nbucket];
        while (idx != 0 && idx < m->sym_count) {
            dl_sym_t *sym = &m->symtab[idx];
            if (sym->st_name < m->strtab_sz &&
                strcmp(m->strtab + sym->st_name, name) == 0) {
                if (sym->st_shndx != SHN_UNDEF)
                    return (void *)(m->load_bias + sym->st_value);
            }
            idx = chain[idx];
        }
        return NULL;
    }

    /* Linear scan fallback */
    for (uint32_t i = 1; i < m->sym_count; i++) {
        dl_sym_t *sym = &m->symtab[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        if (sym->st_name >= m->strtab_sz) continue;
        if (strcmp(m->strtab + sym->st_name, name) == 0)
            return (void *)(m->load_bias + sym->st_value);
    }

    return NULL;
}

/* ── dl_close: unload module ────────────────────────────────── */

int dl_close(void *handle)
{
    dl_module_t *m = (dl_module_t *)handle;
    if (!m || !m->loaded)
        return -1;

    m->refcount--;
    if (m->refcount > 0) {
        serial_puts("[DL] Refcount=");
        serial_putdec(m->refcount);
        serial_puts(", keeping loaded\n");
        return 0;
    }

    /* Call fini function */
    if (m->fini_fn) {
        serial_puts("[DL] Calling fini\n");
        m->fini_fn();
    }

    serial_puts("[DL] Unloading '");
    serial_puts(m->name);
    serial_puts("'\n");

    mem_free_pages(m->base, m->pages);
    m->loaded = false;
    m->base = NULL;

    return 0;
}

/* ── dl_list: list loaded modules ───────────────────────────── */

void dl_list_modules(void)
{
    int count = 0;
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!modules[i].loaded) continue;
        count++;
    }

    if (count == 0) {
        serial_puts("[DL] No modules loaded\n");
        fb_puts(" No modules loaded\n");
        return;
    }

    for (int i = 0; i < MAX_MODULES; i++) {
        dl_module_t *m = &modules[i];
        if (!m->loaded) continue;

        fb_puts_color(" [", 0x0000AAFF);
        fb_puts(m->name);
        fb_puts_color("]", 0x0000AAFF);
        fb_puts(" base=0x");
        serial_puts("  ");
        serial_puts(m->name);
        serial_puts(" base=0x");

        serial_puthex((uint64_t)m->base, 16);
        fb_putdec((uint64_t)m->base);

        serial_puts(" size=");
        serial_putdec(m->size);
        serial_puts(" syms=");
        serial_putdec(m->sym_count);
        serial_puts(" ref=");
        serial_putdec(m->refcount);
        serial_puts("\n");

        fb_puts(" ");
        fb_putdec(m->size);
        fb_puts("B ");
        fb_putdec(m->sym_count);
        fb_puts(" syms\n");

        /* List exported symbols */
        if (m->symtab && m->strtab) {
            int shown = 0;
            for (uint32_t s = 1; s < m->sym_count && shown < 16; s++) {
                dl_sym_t *sym = &m->symtab[s];
                if (sym->st_shndx == SHN_UNDEF) continue;
                if (sym->st_name == 0 || sym->st_name >= m->strtab_sz) continue;
                uint8_t bind = ELF64_ST_BIND(sym->st_info);
                if (bind != STB_GLOBAL && bind != STB_WEAK) continue;

                const char *name = m->strtab + sym->st_name;
                uint8_t type = ELF64_ST_TYPE(sym->st_info);

                serial_puts("    ");
                serial_puts(type == STT_FUNC ? "F " : type == STT_OBJECT ? "O " : "? ");
                serial_puts(name);
                serial_puts(" = 0x");
                serial_puthex(m->load_bias + sym->st_value, 16);
                serial_puts("\n");

                fb_puts("    ");
                fb_puts_color(type == STT_FUNC ? "F " : type == STT_OBJECT ? "O " : "? ", 0x0000FF00);
                fb_puts(name);
                fb_puts("\n");
                shown++;
            }
        }
    }
}

/* ── dl_find_by_name: find loaded module by filename ────────── */

void *dl_find(const char *name)
{
    return mod_find(name);
}

/* ── Initialize dynamic linker ──────────────────────────────── */

void dl_init(void)
{
    memset(modules, 0, sizeof(modules));
    serial_puts("[DL] Dynamic linker ready (");
    serial_putdec(MAX_MODULES);
    serial_puts(" slots)\n");
}
