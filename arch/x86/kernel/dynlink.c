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
 *   - Cross-module symbol resolution for shared library dependencies
 *   - ELF SysV hash for O(1) symbol lookup
 *   - DT_INIT/DT_FINI/DT_INIT_ARRAY constructor/destructor calls
 */

#include "../include/types.h"
#include "../include/dynlink.h"
#include "../include/paging.h"

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

/* Memory/string functions for kernel symbol export */
extern void *kcalloc(uint64_t count, uint64_t size);
extern void *krealloc(void *ptr, uint64_t new_size);
extern void *memmove(void *dst, const void *src, uint64_t n);
extern int   strncmp(const char *a, const char *b, uint64_t n);
extern char *strcpy(char *dst, const char *src);

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* ── ELF64 types (local to dl_open) ────────────────────────── */

#define DL_EI_NIDENT   16
#define DL_ELFCLASS64  2
#define DL_ELFDATA2LSB 1
#define DL_EM_X86_64   62
#define DL_ET_DYN      3

#define DL_PT_NULL     0
#define DL_PT_LOAD     1
#define DL_PT_DYNAMIC  2
#define DL_PT_TLS      7

#define DL_MAX_FILE_SIZE  (64ULL * 1024 * 1024)
#define DL_MAX_IMAGE_SIZE (256ULL * 1024 * 1024)
#define DL_MAX_PHNUM      128

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

static bool dl_range_valid(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool dl_image_range_valid(uint64_t address, uint64_t size,
                                 uint64_t vmin, uint64_t vmax)
{
    return address >= vmin && address <= vmax && size <= vmax - address;
}

static bool dl_is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool dl_strtab_name_valid(const char *strtab, uint64_t strtab_sz,
                                 uint32_t offset)
{
    if (!strtab || offset >= strtab_sz) return false;
    for (uint64_t i = offset; i < strtab_sz; i++) {
        if (strtab[i] == '\0') return true;
    }
    return false;
}

static bool dl_module_range_valid(const dl_module_t *m, uint64_t address,
                                  uint64_t size)
{
    if (!m || !m->base || m->pages == 0) return true;
    uint64_t start = (uint64_t)m->base;
    uint64_t bytes = m->pages * 4096;
    return address >= start && address <= start + bytes &&
           size <= start + bytes - address;
}

/* ── Module table ───────────────────────────────────────────── */

static dl_module_t modules[DL_MAX_MODULES];

/* Fault recovery for INIT_ARRAY — checked by exception handler in idt.c */
uint64_t *dl_fault_jmpbuf = NULL;
uint64_t *compat32_crash_jmpbuf = NULL;
extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int val);

/* ── Stub functions for kernel symbol fallback ─────────────── */

static void stub_exit(int code)
{
    serial_puts("[DL] exit(");
    serial_putdec((uint64_t)code);
    serial_puts(")\n");
    for (;;) __asm__ volatile("hlt");
}

static int64_t stub_write(int fd, const void *buf, uint64_t n)
{
    (void)fd;
    char tmp[2] = {0, 0};
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < n; i++) {
        tmp[0] = p[i];
        serial_puts(tmp);
    }
    return (int64_t)n;
}

static int64_t stub_read(int fd, void *buf, uint64_t n)
{
    (void)fd; (void)buf; (void)n;
    return 0;
}

static int stub_noop(void) { return 0; }

/* Safe landing pad for unresolved PLT entries — returns 0 instead of NULL-CALL */
static uint64_t stub_unresolved(void) { return 0; }

/* ── Direct kernel syscall stubs ─────────────────────────────
 * Chrome runs in ring 0 — PLT entries can call kernel functions
 * directly instead of going through the SYSCALL instruction.
 * This also handles the case where libc's wrappers fail. */
extern int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5,
                                 uint64_t a6);

static int64_t kern_open(const char *path, int flags, int mode)
{
    return syscall_dispatch(2/*SYS_OPEN*/, (uint64_t)path,
                            (uint64_t)flags, (uint64_t)mode, 0, 0, 0);
}

static int64_t kern_close(int fd)
{
    return syscall_dispatch(3/*SYS_CLOSE*/, (uint64_t)fd, 0, 0, 0, 0, 0);
}

static int64_t kern_read(int fd, void *buf, uint64_t n)
{
    return syscall_dispatch(0/*SYS_READ*/, (uint64_t)fd,
                            (uint64_t)buf, n, 0, 0, 0);
}

static int64_t kern_write(int fd, const void *buf, uint64_t n)
{
    return syscall_dispatch(1/*SYS_WRITE*/, (uint64_t)fd,
                            (uint64_t)buf, n, 0, 0, 0);
}

static void *kern_mmap(void *addr, uint64_t len, int prot,
                        int flags, int fd, int64_t off)
{
    int64_t r = syscall_dispatch(9/*SYS_MMAP*/, (uint64_t)addr, len,
                                  (uint64_t)prot, (uint64_t)flags,
                                  (uint64_t)fd, (uint64_t)off);
    return (void *)r;
}

static int kern_mprotect(void *addr, uint64_t len, int prot)
{
    return (int)syscall_dispatch(10/*SYS_MPROTECT*/, (uint64_t)addr,
                                 len, (uint64_t)prot, 0, 0, 0);
}

static int kern_munmap(void *addr, uint64_t len)
{
    return (int)syscall_dispatch(11/*SYS_MUNMAP*/, (uint64_t)addr,
                                 len, 0, 0, 0, 0);
}

/* __tls_get_addr: reads DTV from %fs:8, returns dtv[module] + offset */
static void *kern_tls_get_addr(void *ti_ptr)
{
    uint64_t *ti = (uint64_t *)ti_ptr;
    uint64_t module = ti[0];
    uint64_t offset = ti[1];
    uint64_t dtv_ptr;
    __asm__ volatile("movq %%fs:8, %0" : "=r"(dtv_ptr));
    if (!dtv_ptr || module == 0) return (void *)0;
    uint64_t *dtv = (uint64_t *)dtv_ptr;
    return (void *)(dtv[module] + offset);
}

/* ── Kernel symbol export table ─────────────────────────────── */

typedef struct {
    const char *name;
    uint64_t    addr;
} ksym_entry_t;

static const ksym_entry_t ksym_table[] = {
    /* Kernel I/O */
    { "serial_puts",    (uint64_t)serial_puts    },
    { "serial_putdec",  (uint64_t)serial_putdec  },
    { "serial_puthex",  (uint64_t)serial_puthex  },
    { "fb_puts",        (uint64_t)fb_puts        },
    { "fb_puts_color",  (uint64_t)fb_puts_color  },
    { "fb_putdec",      (uint64_t)fb_putdec      },

    /* Memory allocation */
    { "kmalloc",        (uint64_t)kmalloc        },
    { "kfree",          (uint64_t)kfree          },
    { "malloc",         (uint64_t)kmalloc        },
    { "free",           (uint64_t)kfree          },
    { "calloc",         (uint64_t)kcalloc        },
    { "realloc",        (uint64_t)krealloc       },

    /* Memory ops */
    { "memcpy",         (uint64_t)memcpy         },
    { "memset",         (uint64_t)memset         },
    { "memmove",        (uint64_t)memmove        },

    /* String */
    { "strlen",         (uint64_t)strlen         },
    { "strcmp",          (uint64_t)strcmp          },
    { "strncmp",        (uint64_t)strncmp        },
    { "strcpy",         (uint64_t)strcpy         },

    /* Process */
    { "exit",           (uint64_t)stub_exit      },
    { "_exit",          (uint64_t)stub_exit      },
    { "abort",          (uint64_t)stub_exit      },

    /* POSIX I/O — direct kernel calls (bypass SYSCALL instruction) */
    { "open",           (uint64_t)kern_open      },
    { "open64",         (uint64_t)kern_open      },
    { "openat",         (uint64_t)stub_unresolved}, /* TODO */
    { "close",          (uint64_t)kern_close     },
    { "read",           (uint64_t)kern_read      },
    { "write",          (uint64_t)kern_write     },
    { "mmap",           (uint64_t)kern_mmap      },
    { "mmap64",         (uint64_t)kern_mmap      },
    { "mprotect",       (uint64_t)kern_mprotect  },
    { "munmap",         (uint64_t)kern_munmap    },
    { "syscall_dispatch", (uint64_t)syscall_dispatch },

    /* TLS */
    { "__tls_get_addr",        (uint64_t)kern_tls_get_addr },

    /* Threading (no-ops for single-threaded) */
    { "pthread_mutex_lock",    (uint64_t)stub_noop },
    { "pthread_mutex_unlock",  (uint64_t)stub_noop },
    { "pthread_mutex_init",    (uint64_t)stub_noop },
    { "pthread_mutex_destroy", (uint64_t)stub_noop },

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

uint32_t dl_gnu_hash_nsyms(const uint32_t *gnu_hash)
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

static uint32_t dl_gnu_hash_nsyms_bounded(const uint32_t *gnu_hash,
                                           uint64_t available)
{
    if (available < 4 * sizeof(uint32_t)) return 0;

    uint32_t nbuckets = gnu_hash[0];
    uint32_t symoffset = gnu_hash[1];
    uint32_t bloom_size = gnu_hash[2];
    if (nbuckets == 0 || bloom_size == 0) return 0;
    uint64_t bloom_bytes = (uint64_t)bloom_size * sizeof(uint64_t);
    uint64_t bucket_bytes = (uint64_t)nbuckets * sizeof(uint32_t);
    uint64_t prefix = 4 * sizeof(uint32_t);
    if (!dl_range_valid(prefix, bloom_bytes, available)) return 0;
    prefix += bloom_bytes;
    if (!dl_range_valid(prefix, bucket_bytes, available)) return 0;

    const uint32_t *buckets = (const uint32_t *)((const uint8_t *)gnu_hash + prefix);
    prefix += bucket_bytes;
    uint64_t chain_count = (available - prefix) / sizeof(uint32_t);
    const uint32_t *chains = (const uint32_t *)((const uint8_t *)gnu_hash + prefix);

    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (buckets[i] > last) last = buckets[i];
    }
    if (last < symoffset) return symoffset;

    uint64_t chain_index = (uint64_t)last - symoffset;
    while (chain_index < chain_count) {
        if (chains[chain_index] & 1) return last + 1;
        chain_index++;
        if (last == UINT32_MAX) return 0;
        last++;
    }
    return 0;
}

/* ── Allocate module slot ───────────────────────────────────── */

static dl_module_t *mod_alloc(void)
{
    for (int i = 0; i < DL_MAX_MODULES; i++) {
        if (!modules[i].loaded)
            return &modules[i];
    }
    return NULL;
}

/* ── Find module by name ────────────────────────────────────── */

static dl_module_t *mod_find(const char *name, uint32_t owner_id)
{
    for (int i = 0; i < DL_MAX_MODULES; i++) {
        if (modules[i].loaded && modules[i].owner_id == owner_id &&
            strcmp(modules[i].name, name) == 0)
            return &modules[i];
    }
    return NULL;
}

/* ── Resolve symbol (with cross-module lookup) ──────────────── */

static uint64_t resolve_symbol(dl_module_t *m, uint32_t sym_idx)
{
    if (sym_idx == 0 || sym_idx >= m->sym_count ||
        !m->symtab || !m->strtab)
        return 0;

    dl_sym_t *sym = &m->symtab[sym_idx];
    if (!dl_strtab_name_valid(m->strtab, m->strtab_sz, sym->st_name))
        return 0;
    const char *name = m->strtab + sym->st_name;

    /* Defined in this module */
    if (sym->st_shndx != SHN_UNDEF)
        return m->load_bias + sym->st_value;

    /* Kernel exports FIRST — critical for ring 0 execution.
     * Functions like mmap/open/write must go through kernel stubs
     * (which call syscall_dispatch directly) instead of libc's
     * wrappers (which use the SYSCALL instruction, broken in ring 0). */
    uint64_t addr = ksym_resolve(name);
    if (addr) return addr;

    /* Then search other loaded modules */
    for (int i = 0; i < DL_MAX_MODULES; i++) {
        if (!modules[i].loaded) continue;
        if (&modules[i] == m) continue;
        if (m->owner_id != 0) {
            if (modules[i].owner_id != 0 &&
                modules[i].owner_id != m->owner_id)
                continue;
        } else if (modules[i].owner_id != 0) {
            continue;
        }
        void *val = dl_sym(&modules[i], name);
        if (val) return (uint64_t)val;
    }

    /* Not found anywhere */
    if (!addr) {
        static int unresolved_log = 0;
        if (unresolved_log < 20) {
            serial_puts("[DL] Unresolved: ");
            serial_puts(name);
            serial_puts("\n");
        } else if (unresolved_log == 20) {
            serial_puts("[DL] (suppressing further unresolved)\n");
        }
        unresolved_log++;
    }
    return addr;
}

/* ── Apply relocations ──────────────────────────────────────── */

int dl_apply_rela(dl_module_t *m, const dl_rela_t *rela, uint64_t count)
{
    uint32_t resolved = 0, failed = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint32_t type    = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        uint64_t *target = (uint64_t *)(m->load_bias + rela[i].r_offset);

        uint64_t target_size = (type == R_X86_64_TPOFF32) ? 4 : 8;
        if (type != R_X86_64_NONE &&
            !dl_module_range_valid(m, (uint64_t)target, target_size)) {
            serial_puts("[DL] Relocation target outside module\n");
            failed++;
            continue;
        }

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
            if (!val && sym_idx != 0) {
                *target = (uint64_t)stub_unresolved;
                failed++;
                break;
            }
            *target = val;
            resolved++;
            break;
        }

        case R_X86_64_IRELATIVE: {
            /* IFUNC: call resolver to get optimal implementation */
            typedef uint64_t (*ifunc_resolver_t)(void);
            ifunc_resolver_t resolver = (ifunc_resolver_t)(m->load_bias + rela[i].r_addend);
            *target = resolver();
            resolved++;
            break;
        }

        case R_X86_64_DTPMOD64: {
            /* TLS module ID */
            uint64_t modid = m->tls_modid ? m->tls_modid : 1;
            if (sym_idx > 0 && sym_idx < m->sym_count &&
                m->symtab[sym_idx].st_shndx == SHN_UNDEF) {
                /* Symbol from another module — search */
                const char *sname = m->strtab + m->symtab[sym_idx].st_name;
                for (int j = 0; j < DL_MAX_MODULES; j++) {
                    if (!modules[j].loaded || !modules[j].tls_modid) continue;
                    if (m->owner_id != 0) {
                        if (modules[j].owner_id != 0 &&
                            modules[j].owner_id != m->owner_id)
                            continue;
                    } else if (modules[j].owner_id != 0) {
                        continue;
                    }
                    if (dl_sym(&modules[j], sname)) {
                        modid = modules[j].tls_modid;
                        break;
                    }
                }
            }
            *target = modid;
            resolved++;
            break;
        }

        case R_X86_64_DTPOFF64: {
            /* Offset within module's TLS block */
            uint64_t off = 0;
            if (sym_idx > 0 && sym_idx < m->sym_count)
                off = m->symtab[sym_idx].st_value;
            *target = off;
            resolved++;
            break;
        }

        case R_X86_64_TPOFF32: {
            /* Signed 32-bit offset from TP */
            int64_t off = m->tls_offset;
            if (sym_idx > 0 && sym_idx < m->sym_count)
                off += (int64_t)m->symtab[sym_idx].st_value;
            *(int32_t *)target = (int32_t)off;
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

/* ── Apply RELR (compact RELATIVE relocations) ─────────────── */

uint64_t dl_apply_relr(uint64_t load_bias, const uint64_t *relr, uint64_t count)
{
    uint64_t where = 0;
    uint64_t applied = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t entry = relr[i];
        if ((entry & 1) == 0) {
            /* Base address: apply one RELATIVE, advance */
            uint64_t *p = (uint64_t *)(load_bias + entry);
            *p += load_bias;
            applied++;
            where = (uint64_t)(p + 1);
        } else {
            /* Bitmap: each set bit = one RELATIVE relocation */
            uint64_t bitmap = entry >> 1;
            for (int bit = 0; bitmap; bit++, bitmap >>= 1) {
                if (bitmap & 1) {
                    uint64_t *p = (uint64_t *)(where + (uint64_t)bit * 8);
                    *p += load_bias;
                    applied++;
                }
            }
            where += 63 * 8;
        }
    }

    return applied;
}

/* ── dl_open: load shared object ────────────────────────────── */

static bool dl_rela_targets_valid(const dl_rela_t *rela, uint64_t count,
                                  uint64_t vmin, uint64_t vmax)
{
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint64_t target_size;

        switch (type) {
        case R_X86_64_NONE:
            continue;
        case R_X86_64_TPOFF32:
            target_size = 4;
            break;
        case R_X86_64_RELATIVE:
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_IRELATIVE:
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
            target_size = 8;
            break;
        default:
            continue;
        }

        if (!dl_image_range_valid(rela[i].r_offset, target_size, vmin, vmax))
            return false;
        if (type == R_X86_64_IRELATIVE &&
            (rela[i].r_addend < 0 ||
             !dl_image_range_valid((uint64_t)rela[i].r_addend, 1, vmin, vmax)))
            return false;
    }
    return true;
}

static bool dl_relr_targets_valid(const uint64_t *relr, uint64_t count,
                                  uint64_t vmin, uint64_t vmax)
{
    uint64_t where = 0;
    bool have_base = false;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t entry = relr[i];
        if ((entry & 1) == 0) {
            if (!dl_image_range_valid(entry, 8, vmin, vmax)) return false;
            if (entry > UINT64_MAX - 8) return false;
            where = entry + 8;
            have_base = true;
            continue;
        }

        if (!have_base) return false;
        uint64_t bitmap = entry >> 1;
        for (uint64_t bit = 0; bitmap; bit++, bitmap >>= 1) {
            if ((bitmap & 1) == 0) continue;
            uint64_t delta = bit * 8;
            if (where > UINT64_MAX - delta ||
                !dl_image_range_valid(where + delta, 8, vmin, vmax))
                return false;
        }
        if (where > UINT64_MAX - 63 * 8) return false;
        where += 63 * 8;
    }
    return true;
}

static void *dl_open_flags_owner(const char *filename, int flags,
                                 uint32_t owner_id);

void *dl_open(const char *filename)
{
    return dl_open_flags_owner(filename, 0, 0);
}

void *dl_open_flags(const char *filename, int flags)
{
    return dl_open_flags_owner(filename, flags, 0);
}

void *dl_open_private(const char *filename, uint32_t owner_id)
{
    if (owner_id == 0) return NULL;
    return dl_open_flags_owner(filename, 0, owner_id);
}

static void *dl_open_flags_owner(const char *filename, int flags,
                                 uint32_t owner_id)
{
    const char *error = NULL;
    uint8_t *data = NULL;
    void *base = NULL;
    void *phys_base = NULL;
    uint64_t total_pages = 0;

    serial_puts("[DL] Opening '");
    serial_puts(filename);
    serial_puts("'");
    if (owner_id) {
        serial_puts(" owner=");
        serial_putdec(owner_id);
    }
    serial_puts("\n");

    /* Check if already loaded */
    dl_module_t *existing = mod_find(filename, owner_id);
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
    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[DL] File not found\n");
        return NULL;
    }

    uint64_t file_size = osfs2_file_size(file);

    if (file_size < sizeof(dl_ehdr_t) || file_size > DL_MAX_FILE_SIZE) {
        serial_puts("[DL] Invalid file size: ");
        serial_putdec(file_size);
        serial_puts(" bytes\n");
        return NULL;
    }

    data = (uint8_t *)kmalloc(file_size);
    if (!data) {
        serial_puts("[DL] Out of memory for read\n");
        return NULL;
    }

    if (osfs2_read(file, 0, data, file_size) != (int)file_size) {
        error = "Read failed";
        goto fail;
    }

    /* Validate ELF */
    dl_ehdr_t *ehdr = (dl_ehdr_t *)data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != DL_ELFCLASS64 || ehdr->e_ident[5] != DL_ELFDATA2LSB ||
        ehdr->e_ident[6] != 1 || ehdr->e_version != 1 ||
        ehdr->e_machine != DL_EM_X86_64 || ehdr->e_type != DL_ET_DYN ||
        ehdr->e_ehsize != sizeof(dl_ehdr_t) ||
        ehdr->e_phentsize != sizeof(dl_phdr_t) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > DL_MAX_PHNUM) {
        error = "Not a valid x86-64 shared object";
        goto fail;
    }

    uint64_t phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(dl_phdr_t);
    if (!dl_range_valid(ehdr->e_phoff, phdr_bytes, file_size)) {
        error = "Program headers outside file";
        goto fail;
    }
    dl_phdr_t *phdrs = (dl_phdr_t *)(data + ehdr->e_phoff);

    /* ── Pass 1: find LOAD range ─────────────────────────────── */

    uint64_t vmin = UINT64_MAX, vmax = 0;
    uint64_t load_align = 4096;
    int load_count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        dl_phdr_t *ph = &phdrs[i];

        if (ph->p_filesz > 0 &&
            !dl_range_valid(ph->p_offset, ph->p_filesz, file_size)) {
            error = "Segment data outside file";
            goto fail;
        }
        if (ph->p_align != 0 &&
            (!dl_is_power_of_two(ph->p_align) ||
             ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1)) != 0)) {
            error = "Invalid segment alignment";
            goto fail;
        }

        if (ph->p_type != DL_PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_filesz > ph->p_memsz ||
            ph->p_vaddr > UINT64_MAX - ph->p_memsz) {
            error = "Invalid LOAD segment size";
            goto fail;
        }

        uint64_t seg_start = ph->p_vaddr & ~4095ULL;
        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end > UINT64_MAX - 4095) {
            error = "LOAD segment address overflow";
            goto fail;
        }
        seg_end = (seg_end + 4095) & ~4095ULL;

        if (seg_start < vmin) vmin = seg_start;
        if (seg_end > vmax) vmax = seg_end;
        if (ph->p_align > load_align) load_align = ph->p_align;
        load_count++;
    }

    if (load_count == 0) {
        error = "No LOAD segments";
        goto fail;
    }

    uint64_t total_size = vmax - vmin;
    if (total_size == 0 || total_size > DL_MAX_IMAGE_SIZE ||
        load_align > DL_MAX_IMAGE_SIZE) {
        error = "Invalid loaded image size";
        goto fail;
    }
    total_pages = total_size / 4096;

    /* Keep runtime modules in the shared upper-half direct map.  A physical
     * identity address is also a valid Win32 user VA, so loading there lets a
     * process mapping (notably HotSpot's heap) hide or overwrite the module. */
    phys_base = mem_alloc_aligned(total_size, load_align);
    if (!phys_base) {
        serial_puts("[DL] Failed to allocate ");
        serial_putdec(total_pages);
        serial_puts(" pages\n");
        error = "Module image allocation failed";
        goto fail;
    }
    base = PHYS_TO_VIRT(phys_base);

    memset(base, 0, total_size);
    uint64_t load_bias = (uint64_t)base - vmin;

    serial_puts("[DL] Base: 0x");
    serial_puthex((uint64_t)base, 16);
    serial_puts(", size: ");
    serial_putdec(total_pages * 4);
    serial_puts(" KB\n");

    /* ── Pass 2: copy LOAD segments ──────────────────────────── */

    for (int i = 0; i < ehdr->e_phnum; i++) {
        dl_phdr_t *ph = &phdrs[i];

        if (ph->p_type != DL_PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t dest_off = ph->p_vaddr - vmin;
        if (ph->p_filesz > 0)
            memcpy((uint8_t *)base + dest_off, data + ph->p_offset, ph->p_filesz);
    }

    /* ── Pass 3: find PT_DYNAMIC and PT_TLS ──────────────────── */

    dl_dyn_t *dynamic = NULL;
    uint64_t dyn_count = 0;
    uint64_t pt_tls_filesz = 0, pt_tls_memsz = 0, pt_tls_align = 1;
    uint64_t pt_tls_vaddr = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        dl_phdr_t *ph = &phdrs[i];

        if (ph->p_type == DL_PT_DYNAMIC) {
            if (dynamic || ph->p_filesz < sizeof(dl_dyn_t) ||
                ph->p_filesz > ph->p_memsz ||
                ph->p_filesz % sizeof(dl_dyn_t) != 0 ||
                !dl_image_range_valid(ph->p_vaddr, ph->p_filesz, vmin, vmax)) {
                error = "Invalid PT_DYNAMIC segment";
                goto fail;
            }
            dynamic = (dl_dyn_t *)((uint8_t *)base + (ph->p_vaddr - vmin));
            dyn_count = ph->p_filesz / sizeof(dl_dyn_t);
        }
        if (ph->p_type == DL_PT_TLS && ph->p_memsz > 0) {
            if (pt_tls_memsz != 0 || ph->p_filesz > ph->p_memsz ||
                !dl_image_range_valid(ph->p_vaddr, ph->p_memsz, vmin, vmax) ||
                (ph->p_align != 0 && !dl_is_power_of_two(ph->p_align))) {
                error = "Invalid PT_TLS segment";
                goto fail;
            }
            pt_tls_filesz = ph->p_filesz;
            pt_tls_memsz  = ph->p_memsz;
            pt_tls_align  = ph->p_align ? ph->p_align : 1;
            pt_tls_vaddr  = ph->p_vaddr;
        }
    }

    /* Free file data — segments already copied */
    kfree(data);
    data = NULL;

    if (!dynamic) {
        error = "No PT_DYNAMIC segment";
        goto fail;
    }

    /* ── Parse dynamic table ─────────────────────────────────── */

    uint64_t dt_symtab = 0, dt_strtab = 0, dt_strsz = 0;
    uint64_t dt_hash = 0, dt_gnu_hash_val = 0;
    uint64_t dt_rela = 0, dt_relasz = 0;
    uint64_t dt_jmprel = 0, dt_pltrelsz = 0;
    uint64_t dt_init = 0, dt_fini = 0;
    uint64_t dt_init_array = 0, dt_init_arraysz = 0;
    uint64_t dt_relr = 0, dt_relrsz = 0;
    uint64_t dt_syment = sizeof(dl_sym_t);
    uint64_t dt_relaent = sizeof(dl_rela_t);
    uint64_t dt_relrent = sizeof(uint64_t);
    uint64_t dt_pltrel = 0;
    bool dynamic_terminated = false;

    for (uint64_t i = 0; i < dyn_count; i++) {
        if (dynamic[i].d_tag == DT_NULL) {
            dynamic_terminated = true;
            break;
        }
        switch (dynamic[i].d_tag) {
        case DT_SYMTAB:      dt_symtab      = dynamic[i].d_val; break;
        case DT_STRTAB:      dt_strtab      = dynamic[i].d_val; break;
        case DT_STRSZ:       dt_strsz       = dynamic[i].d_val; break;
        case DT_HASH:        dt_hash        = dynamic[i].d_val; break;
        case DT_GNU_HASH:    dt_gnu_hash_val = dynamic[i].d_val; break;
        case DT_RELA:        dt_rela        = dynamic[i].d_val; break;
        case DT_RELASZ:      dt_relasz      = dynamic[i].d_val; break;
        case DT_JMPREL:      dt_jmprel      = dynamic[i].d_val; break;
        case DT_PLTRELSZ:    dt_pltrelsz    = dynamic[i].d_val; break;
        case DT_INIT:        dt_init        = dynamic[i].d_val; break;
        case DT_FINI:        dt_fini        = dynamic[i].d_val; break;
        case DT_INIT_ARRAY:  dt_init_array  = dynamic[i].d_val; break;
        case DT_INIT_ARRAYSZ:dt_init_arraysz = dynamic[i].d_val; break;
        case DT_RELR:        dt_relr        = dynamic[i].d_val; break;
        case DT_RELRSZ:      dt_relrsz      = dynamic[i].d_val; break;
        case DT_SYMENT:      dt_syment      = dynamic[i].d_val; break;
        case DT_RELAENT:     dt_relaent     = dynamic[i].d_val; break;
        case DT_RELRENT:     dt_relrent     = dynamic[i].d_val; break;
        case DT_PLTREL:      dt_pltrel      = dynamic[i].d_val; break;
        }
    }

    if (!dynamic_terminated) {
        error = "Unterminated dynamic table";
        goto fail;
    }
    if (dt_syment != sizeof(dl_sym_t) || dt_relaent != sizeof(dl_rela_t) ||
        dt_relrent != sizeof(uint64_t)) {
        error = "Unsupported dynamic entry size";
        goto fail;
    }
    if (!dt_symtab || !dt_strtab || dt_strsz == 0 ||
        !dl_image_range_valid(dt_symtab, sizeof(dl_sym_t), vmin, vmax) ||
        !dl_image_range_valid(dt_strtab, dt_strsz, vmin, vmax) ||
        *((char *)(load_bias + dt_strtab + dt_strsz - 1)) != '\0') {
        error = "Invalid dynamic symbol or string table";
        goto fail;
    }
    if ((dt_init && !dl_image_range_valid(dt_init, 1, vmin, vmax)) ||
        (dt_fini && !dl_image_range_valid(dt_fini, 1, vmin, vmax)) ||
        (dt_init_arraysz % sizeof(uint64_t)) != 0 ||
        (dt_init_arraysz > 0 &&
         (!dt_init_array ||
          !dl_image_range_valid(dt_init_array, dt_init_arraysz, vmin, vmax)))) {
        error = "Invalid initializer metadata";
        goto fail;
    }

    if ((dt_relasz % sizeof(dl_rela_t)) != 0 ||
        (dt_relasz > 0 &&
         (!dt_rela || !dl_image_range_valid(dt_rela, dt_relasz, vmin, vmax))) ||
        (dt_pltrelsz % sizeof(dl_rela_t)) != 0 ||
        (dt_pltrelsz > 0 &&
         (!dt_jmprel || dt_pltrel != DT_RELA ||
          !dl_image_range_valid(dt_jmprel, dt_pltrelsz, vmin, vmax))) ||
        (dt_relrsz % sizeof(uint64_t)) != 0 ||
        (dt_relrsz > 0 &&
         (!dt_relr || !dl_image_range_valid(dt_relr, dt_relrsz, vmin, vmax)))) {
        error = "Invalid relocation metadata";
        goto fail;
    }

    /* Fill module structure */
    uint32_t *validated_hashtab = NULL;
    uint32_t validated_nbucket = 0;
    uint32_t validated_nchain = 0;
    uint32_t validated_sym_count = 0;

    if (dt_hash) {
        if (!dl_image_range_valid(dt_hash, 2 * sizeof(uint32_t), vmin, vmax)) {
            error = "Invalid SysV hash table";
            goto fail;
        }
        validated_hashtab = (uint32_t *)(load_bias + dt_hash);
        validated_nbucket = validated_hashtab[0];
        validated_nchain = validated_hashtab[1];
        uint64_t hash_words = 2ULL + validated_nbucket + validated_nchain;
        if (validated_nbucket == 0 || validated_nchain == 0 ||
            !dl_image_range_valid(dt_hash, hash_words * sizeof(uint32_t),
                                  vmin, vmax)) {
            error = "Invalid SysV hash dimensions";
            goto fail;
        }
        validated_sym_count = validated_nchain;

        uint32_t *buckets = validated_hashtab + 2;
        uint32_t *chains = buckets + validated_nbucket;
        for (uint32_t i = 0; i < validated_nbucket; i++) {
            if (buckets[i] >= validated_nchain) {
                error = "Invalid SysV hash bucket";
                goto fail;
            }
        }
        for (uint32_t i = 0; i < validated_nchain; i++) {
            if (chains[i] >= validated_nchain) {
                error = "Invalid SysV hash chain";
                goto fail;
            }
        }
    } else if (dt_gnu_hash_val) {
        if (!dl_image_range_valid(dt_gnu_hash_val, 4 * sizeof(uint32_t),
                                  vmin, vmax)) {
            error = "Invalid GNU hash table";
            goto fail;
        }
        uint32_t *gh = (uint32_t *)(load_bias + dt_gnu_hash_val);
        validated_sym_count = dl_gnu_hash_nsyms_bounded(
            gh, vmax - dt_gnu_hash_val);
        if (validated_sym_count == 0) {
            error = "Invalid GNU hash chains";
            goto fail;
        }
    } else if (dt_strtab > dt_symtab) {
        uint64_t sym_bytes = dt_strtab - dt_symtab;
        if (sym_bytes % sizeof(dl_sym_t) != 0 ||
            sym_bytes / sizeof(dl_sym_t) > UINT32_MAX) {
            error = "Cannot determine dynamic symbol count";
            goto fail;
        }
        validated_sym_count = (uint32_t)(sym_bytes / sizeof(dl_sym_t));
    } else {
        error = "Shared object has no bounded symbol index";
        goto fail;
    }

    uint64_t symtab_bytes = (uint64_t)validated_sym_count * sizeof(dl_sym_t);
    if (validated_sym_count == 0 ||
        !dl_image_range_valid(dt_symtab, symtab_bytes, vmin, vmax)) {
        error = "Dynamic symbol table outside image";
        goto fail;
    }

    const dl_rela_t *validated_rela =
        dt_relasz ? (const dl_rela_t *)(load_bias + dt_rela) : NULL;
    const dl_rela_t *validated_jmprel =
        dt_pltrelsz ? (const dl_rela_t *)(load_bias + dt_jmprel) : NULL;
    const uint64_t *validated_relr =
        dt_relrsz ? (const uint64_t *)(load_bias + dt_relr) : NULL;
    if ((validated_rela &&
         !dl_rela_targets_valid(validated_rela,
                                dt_relasz / sizeof(dl_rela_t), vmin, vmax)) ||
        (validated_jmprel &&
         !dl_rela_targets_valid(validated_jmprel,
                                dt_pltrelsz / sizeof(dl_rela_t), vmin, vmax)) ||
        (validated_relr &&
         !dl_relr_targets_valid(validated_relr,
                                dt_relrsz / sizeof(uint64_t), vmin, vmax))) {
        error = "Relocation target outside image";
        goto fail;
    }

    memset(m, 0, sizeof(*m));
    m->loaded    = true;
    m->owner_id  = owner_id;
    m->base      = base;
    m->phys_base = phys_base;
    m->load_bias = load_bias;
    m->size      = total_size;
    m->pages     = total_pages;
    m->refcount  = 1;

    /* Copy name */
    int j = 0;
    while (filename[j] && j < DL_MAX_MOD_NAME - 1) {
        m->name[j] = filename[j];
        j++;
    }
    m->name[j] = '\0';

    /* Store TLS info */
    m->tls_filesz    = pt_tls_filesz;
    m->tls_memsz     = pt_tls_memsz;
    m->tls_align     = pt_tls_align;
    m->tls_initimage = pt_tls_memsz ? (load_bias + pt_tls_vaddr) : 0;

    /* Resolve dynamic pointers (vaddr → loaded address) */
    if (dt_symtab)   m->symtab    = (dl_sym_t *)(load_bias + dt_symtab);
    if (dt_strtab)  { m->strtab   = (char *)(load_bias + dt_strtab); m->strtab_sz = dt_strsz; }
    m->hashtab = validated_hashtab;
    if (dt_init)     m->init_fn   = (void (*)(void))(load_bias + dt_init);
    if (dt_fini)     m->fini_fn   = (void (*)(void))(load_bias + dt_fini);

    /* Determine symbol count */
    m->nbucket   = validated_nbucket;
    m->nchain    = validated_nchain;
    m->sym_count = validated_sym_count;

    serial_puts("[DL] Symbols: ");
    serial_putdec(m->sym_count);
    serial_puts(", strtab: ");
    serial_putdec(dt_strsz);
    serial_puts(" bytes\n");

    /* ── Store deferred linking info ─────────────────────────── */

    if (dt_rela && dt_relasz > 0) {
        m->defer_rela = (dl_rela_t *)(load_bias + dt_rela);
        m->defer_rela_count = dt_relasz / sizeof(dl_rela_t);
    }
    if (dt_jmprel && dt_pltrelsz > 0) {
        m->defer_jmprel = (dl_rela_t *)(load_bias + dt_jmprel);
        m->defer_jmprel_count = dt_pltrelsz / sizeof(dl_rela_t);
    }
    if (dt_relr && dt_relrsz > 0) {
        m->defer_relr = (uint64_t *)(load_bias + dt_relr);
        m->defer_relr_count = dt_relrsz / 8;
    }
    if (dt_init_array && dt_init_arraysz > 0) {
        m->defer_init_array = load_bias + dt_init_array;
        m->defer_init_arraysz = dt_init_arraysz;
    }

    /* ── Apply relocations + init (or defer) ─────────────────── */

    if (flags & DL_DEFER_LINK) {
        m->linked = false;
        m->inited = false;
        serial_puts("[DL] Deferred link for '");
        serial_puts(m->name);
        serial_puts("'\n");
    } else {
        /* Immediate mode: apply relocations and call init now */
        if (m->defer_relr_count > 0) {
            uint64_t n = dl_apply_relr(m->load_bias, m->defer_relr, m->defer_relr_count);
            serial_puts("[DL] .relr: ");
            serial_putdec(n);
            serial_puts(" RELATIVE\n");
        }
        if (m->defer_rela_count > 0) {
            serial_puts("[DL] .rela.dyn: ");
            serial_putdec(m->defer_rela_count);
            serial_puts(" entries\n");
            dl_apply_rela(m, m->defer_rela, m->defer_rela_count);
        }
        if (m->defer_jmprel_count > 0) {
            serial_puts("[DL] .rela.plt: ");
            serial_putdec(m->defer_jmprel_count);
            serial_puts(" entries\n");
            dl_apply_rela(m, m->defer_jmprel, m->defer_jmprel_count);
        }
        m->linked = true;

        if (m->init_fn) {
            serial_puts("[DL] Calling init\n");
            m->init_fn();
        }
        if (m->defer_init_arraysz > 0) {
            uint64_t init_count = m->defer_init_arraysz / 8;
            typedef void (*init_fn_t)(void);
            init_fn_t *fns = (init_fn_t *)m->defer_init_array;
            serial_puts("[DL] Calling ");
            serial_putdec(init_count);
            serial_puts(" INIT_ARRAY constructors\n");
            for (uint64_t ci = 0; ci < init_count; ci++) {
                if (fns[ci] && (uint64_t)fns[ci] != (uint64_t)-1)
                    fns[ci]();
            }
        }
        m->inited = true;
    }

    serial_puts("[DL] Module '");
    serial_puts(m->name);
    serial_puts("' loaded OK\n");

    return m;

fail:
    if (data) kfree(data);
    if (phys_base) mem_free_pages(phys_base, total_pages);
    memset(m, 0, sizeof(*m));
    serial_puts("[DL] ");
    serial_puts(error ? error : "Load failed");
    serial_puts("\n");
    return NULL;
}

/* ── dl_link_all: apply relocations + init for deferred modules ── */

void dl_link_all(void)
{
    /* Phase 1: apply relocations to all unlinked modules */
    for (int i = 0; i < DL_MAX_MODULES; i++) {
        dl_module_t *m = &modules[i];
        if (!m->loaded || m->linked) continue;

        serial_puts("[DL] Linking '");
        serial_puts(m->name);
        serial_puts("'\n");

        /* RELR first (compact RELATIVE-only) */
        if (m->defer_relr_count > 0) {
            uint64_t n = dl_apply_relr(m->load_bias, m->defer_relr, m->defer_relr_count);
            serial_puts("[DL] .relr: ");
            serial_putdec(n);
            serial_puts(" RELATIVE\n");
        }
        if (m->defer_rela_count > 0) {
            serial_puts("[DL] .rela.dyn: ");
            serial_putdec(m->defer_rela_count);
            serial_puts(" entries\n");
            dl_apply_rela(m, m->defer_rela, m->defer_rela_count);
        }
        if (m->defer_jmprel_count > 0) {
            serial_puts("[DL] .rela.plt: ");
            serial_putdec(m->defer_jmprel_count);
            serial_puts(" entries\n");
            dl_apply_rela(m, m->defer_jmprel, m->defer_jmprel_count);
        }
        m->linked = true;
    }

    /* Phase 2: call constructors (reverse order = base libs first)
     * Uses fault recovery: if a constructor crashes (e.g. NULL call),
     * the exception handler longjmps back and we skip it. */
    uint64_t init_jmpbuf[8];
    for (int i = DL_MAX_MODULES - 1; i >= 0; i--) {
        dl_module_t *m = &modules[i];
        if (!m->loaded || m->inited) continue;

        if (m->init_fn) {
            serial_puts("[DL] init: ");
            serial_puts(m->name);
            serial_puts("\n");
            dl_fault_jmpbuf = init_jmpbuf;
            if (kern_setjmp(init_jmpbuf) == 0) {
                m->init_fn();
            }
            dl_fault_jmpbuf = NULL;
        }
        if (m->defer_init_arraysz > 0) {
            uint64_t count = m->defer_init_arraysz / 8;
            typedef void (*init_fn_t)(void);
            init_fn_t *fns = (init_fn_t *)m->defer_init_array;
            serial_puts("[DL] INIT_ARRAY: ");
            serial_puts(m->name);
            serial_puts(" (");
            serial_putdec(count);
            serial_puts(")\n");
            for (uint64_t ci = 0; ci < count; ci++) {
                if (fns[ci] && (uint64_t)fns[ci] != (uint64_t)-1) {
                    dl_fault_jmpbuf = init_jmpbuf;
                    if (kern_setjmp(init_jmpbuf) == 0) {
                        fns[ci]();
                    }
                    dl_fault_jmpbuf = NULL;
                }
            }
        }
        m->inited = true;
    }

    serial_puts("[DL] All modules linked\n");
}

/* ── dl_get_module: access module table from elf.c ──────────── */

dl_module_t *dl_get_module(int index)
{
    if (index < 0 || index >= DL_MAX_MODULES) return NULL;
    return modules[index].loaded ? &modules[index] : NULL;
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
        uint32_t steps = 0;
        while (idx != 0 && idx < m->sym_count && steps++ < m->sym_count) {
            dl_sym_t *sym = &m->symtab[idx];
            if (dl_strtab_name_valid(m->strtab, m->strtab_sz, sym->st_name) &&
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
        if (!dl_strtab_name_valid(m->strtab, m->strtab_sz, sym->st_name)) continue;
        if (strcmp(m->strtab + sym->st_name, name) == 0)
            return (void *)(m->load_bias + sym->st_value);
    }

    return NULL;
}

/* ── dl_close: unload module ────────────────────────────────── */

static int dl_release(void *handle, bool call_fini)
{
    dl_module_t *m = (dl_module_t *)handle;
    if (!m || !m->loaded)
        return -1;

    if (m->refcount == 0) return -1;
    m->refcount--;
    if (m->refcount > 0) {
        serial_puts("[DL] Refcount=");
        serial_putdec(m->refcount);
        serial_puts(", keeping loaded\n");
        return 0;
    }

    /* Call fini function */
    if (call_fini && m->fini_fn) {
        serial_puts("[DL] Calling fini\n");
        m->fini_fn();
    }

    serial_puts("[DL] Unloading '");
    serial_puts(m->name);
    serial_puts("'\n");

    mem_free_pages(m->phys_base, m->pages);
    memset(m, 0, sizeof(*m));

    return 0;
}

int dl_close(void *handle)
{
    return dl_release(handle, true);
}

int dl_discard(void *handle)
{
    return dl_release(handle, false);
}

/* ── dl_list: list loaded modules ───────────────────────────── */

void dl_list_modules(void)
{
    int count = 0;
    for (int i = 0; i < DL_MAX_MODULES; i++) {
        if (!modules[i].loaded) continue;
        count++;
    }

    if (count == 0) {
        serial_puts("[DL] No modules loaded\n");
        fb_puts(" No modules loaded\n");
        return;
    }

    for (int i = 0; i < DL_MAX_MODULES; i++) {
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
        if (m->owner_id) {
            serial_puts(" owner=");
            serial_putdec(m->owner_id);
        }
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
    return mod_find(name, 0);
}

/* ── Initialize dynamic linker ──────────────────────────────── */

void dl_init(void)
{
    memset(modules, 0, sizeof(modules));
    serial_puts("[DL] Dynamic linker ready (");
    serial_putdec(DL_MAX_MODULES);
    serial_puts(" slots)\n");
}

/* ── DT_INIT / DT_FINI / DT_INIT_ARRAY callbacks ─────────────── */

/* Call module constructors (DT_INIT + DT_INIT_ARRAY) */
void dl_call_constructors(dl_module_t *m)
{
    if (!m || m->inited) return;

    /* DT_INIT */
    if (m->init_fn) {
        serial_puts("[DL] Calling DT_INIT for ");
        serial_puts(m->name);
        serial_puts("\n");
        m->init_fn();
    }

    /* DT_INIT_ARRAY */
    if (m->defer_init_array && m->defer_init_arraysz) {
        uint64_t count = m->defer_init_arraysz / 8;
        void (**fns)(void) = (void (**)(void))m->defer_init_array;
        for (uint64_t i = 0; i < count; i++) {
            if (fns[i] && (uint64_t)fns[i] != 0xFFFFFFFFFFFFFFFFULL)
                fns[i]();
        }
    }

    m->inited = true;
}

/* Call module destructors (DT_FINI) */
void dl_call_destructors(dl_module_t *m)
{
    if (!m) return;
    if (m->fini_fn)
        m->fini_fn();
}
