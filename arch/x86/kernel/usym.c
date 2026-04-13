/*
 * arch/x86/kernel/usym.c — User-space ELF symbolizer
 *
 * Resolves a runtime instruction pointer (RIP) to its symbol name +
 * offset in the user binary that owns it. The crash dump in idt.c
 * calls into here to print "in <function>+0x<offset>" lines for
 * every faulted user-mode RIP and every backtrace return address.
 *
 * The symbol/string tables themselves live as kmalloc'd copies on
 * `process_t->user_symtab`/`user_strtab`, captured during
 * `elf_load_segments()` from the ELF file's `.symtab` (or
 * `.dynsym` if `.symtab` is absent on a stripped binary). Linear
 * scan over function symbols — costs O(n) per crash, which is fine
 * given that crashes are rare and Q2 has ~3K, GTA5 ~9K symbols.
 */

#include "../include/types.h"

/* Match the layout used by the ELF loader (kmod.c uses an identical
 * struct; we duplicate the few fields we need rather than pulling in
 * the whole header). */
typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

#define ELF64_ST_TYPE(i)  ((i) & 0x0F)
#define STT_FUNC          2

/* The full process_t lives in process.c without a struct tag, so we
 * use opaque void* here and call accessors that cast inside process.c.
 * Callers (idt.c) pass current_proc (process_t*) — implicit conversion
 * to void* is fine in C. */
extern void     *user_symtab_get(void *p);
extern uint64_t  user_symtab_size_get(void *p);
extern char     *user_strtab_get(void *p);
extern uint64_t  user_strtab_size_get(void *p);
extern uint64_t  user_load_bias_get(void *p);

bool user_symbolize(void *p, uint64_t addr,
                    const char **name_out, uint64_t *offset_out)
{
    if (!p) return false;

    void *symtab_raw = user_symtab_get(p);
    char *strtab     = user_strtab_get(p);
    if (!symtab_raw || !strtab) return false;

    uint64_t symtab_size = user_symtab_size_get(p);
    uint64_t strtab_size = user_strtab_size_get(p);
    uint64_t load_bias   = user_load_bias_get(p);

    /* Convert runtime RIP back to the binary-relative address that
     * the symbol table records use. For ET_EXEC binaries load_bias
     * is 0; for static-PIE / ET_DYN it's `(load_base - vaddr_min)`. */
    uint64_t rel = addr - load_bias;

    elf64_sym_t *symtab = (elf64_sym_t *)symtab_raw;
    uint64_t n_syms = symtab_size / sizeof(elf64_sym_t);

    for (uint64_t i = 0; i < n_syms; i++) {
        elf64_sym_t *s = &symtab[i];
        if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (s->st_size == 0) continue;
        if (rel < s->st_value) continue;
        if (rel >= s->st_value + s->st_size) continue;
        if (s->st_name >= strtab_size) continue;

        *name_out = strtab + s->st_name;
        *offset_out = rel - s->st_value;
        return true;
    }
    return false;
}
