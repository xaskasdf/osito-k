/*
 * OsitoK x86-64 — Dynamic Linker Interface
 *
 * Shared types and declarations for elf.c <-> dynlink.c integration.
 * Allows the ELF loader to use dynlink's relocation engine for
 * dynamically linked executables.
 */

#ifndef DYNLINK_H
#define DYNLINK_H

#include "types.h"

/* ── Dynamic section tags ──────────────────────────────────── */

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_PLTREL       20
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_GNU_HASH     0x6ffffef5

/* ── Relocation types ──────────────────────────────────────── */

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_TPOFF32    18
#define R_X86_64_IRELATIVE  37

#define ELF64_R_SYM(i)     ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)    ((uint32_t)((i) & 0xffffffffULL))
#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xf)

#define STB_GLOBAL   1
#define STB_WEAK     2
#define STT_FUNC     2
#define STT_OBJECT   1
#define SHN_UNDEF    0

/* ── Shared ELF types ──────────────────────────────────────── */

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

/* ── Module structure ──────────────────────────────────────── */

#define DL_MAX_MODULES  32
#define DL_MAX_MOD_NAME 64

typedef struct {
    bool        loaded;
    char        name[DL_MAX_MOD_NAME];
    void       *base;
    uint64_t    load_bias;
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

    /* Deferred linking (for phased loading) */
    dl_rela_t  *defer_rela;
    uint64_t    defer_rela_count;
    dl_rela_t  *defer_jmprel;
    uint64_t    defer_jmprel_count;
    uint64_t    defer_init_array;   /* load_bias-adjusted address */
    uint64_t    defer_init_arraysz;
    bool        linked;             /* relocations applied */
    bool        inited;             /* constructors called */

    uint32_t    refcount;
} dl_module_t;

/* ── API ───────────────────────────────────────────────────── */

void  dl_init(void);
void *dl_open(const char *filename);
void *dl_sym(void *handle, const char *name);
int   dl_close(void *handle);
void *dl_find(const char *name);
void  dl_list_modules(void);

/* Phased loading — load all libs first, then link, then init */
#define DL_DEFER_LINK  1   /* Don't apply relocations or call init */
void *dl_open_flags(const char *filename, int flags);
void  dl_link_all(void);   /* Apply relocs + call init for all unlinked modules */

/* Relocation engine — used by elf.c for main binary */
int      dl_apply_rela(dl_module_t *m, const dl_rela_t *rela, uint64_t count);
uint32_t dl_gnu_hash_nsyms(const uint32_t *gnu_hash);

#endif /* DYNLINK_H */
