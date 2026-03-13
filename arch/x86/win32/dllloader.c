/*
 * OsitoK Windows Compatibility Layer — DLL Loader Implementation
 *
 * Loads PE DLLs into memory, resolves their exports, manages
 * module tracking, and handles dependency chains.
 */

#include "dllloader.h"
#include "compat32.h"
extern TEB32 g_teb32;

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

static void dump_seh_chain(const char *label)
{
    serial_puts("[SEH-TRACK] ");
    serial_puts(label);
    serial_puts(": TEB32.ExceptionList=0x");
    serial_puthex(g_teb32.ExceptionList, 8);
    serial_puts("\n");
}

/* ── Helpers ───────────────────────────────────────────────── */

static int dl_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void dl_strcpy_lower(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        dst[i] = c;
    }
    dst[i] = 0;
}

static int dl_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Strip path from DLL name: "C:\\foo\\bar.dll" → "bar.dll" */
static const char *strip_path(const char *name)
{
    const char *last = name;
    for (const char *p = name; *p; p++) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

/* ── Module table ──────────────────────────────────────────── */

static LOADED_MODULE modules[MAX_LOADED_MODULES];
static int module_count = 0;

/* ── Shim registry (built-in DLL shims) ────────────────────── */

#define MAX_SHIMS 16

typedef struct {
    char            name[64];
    shim_resolver_fn resolver;
} SHIM_ENTRY;

static SHIM_ENTRY shims[MAX_SHIMS];
static int shim_count = 0;

void dll_register_shim(const char *dll_name, shim_resolver_fn resolver)
{
    if (shim_count >= MAX_SHIMS) return;
    dl_strcpy_lower(shims[shim_count].name, strip_path(dll_name), 64);
    shims[shim_count].resolver = resolver;
    shim_count++;
}

/* ── Find a shim by DLL name ───────────────────────────────── */

static shim_resolver_fn find_shim(const char *dll_name)
{
    char lower[64];
    dl_strcpy_lower(lower, strip_path(dll_name), 64);

    /* Strip .dll extension for matching */
    char lower_noext[64];
    dl_strcpy_lower(lower_noext, lower, 64);
    int len = 0;
    while (lower_noext[len]) len++;
    if (len > 4 && lower_noext[len-4] == '.' &&
        lower_noext[len-3] == 'd' && lower_noext[len-2] == 'l' &&
        lower_noext[len-1] == 'l') {
        lower_noext[len-4] = 0;
    }

    for (int i = 0; i < shim_count; i++) {
        if (dl_stricmp(lower, shims[i].name) == 0)
            return shims[i].resolver;

        /* Also try without .dll extension */
        char shim_noext[64];
        dl_strcpy_lower(shim_noext, shims[i].name, 64);
        int slen = 0;
        while (shim_noext[slen]) slen++;
        if (slen > 4 && shim_noext[slen-4] == '.' &&
            shim_noext[slen-3] == 'd' && shim_noext[slen-2] == 'l' &&
            shim_noext[slen-1] == 'l') {
            shim_noext[slen-4] = 0;
        }

        if (dl_stricmp(lower_noext, shim_noext) == 0)
            return shims[i].resolver;
    }

    return NULL;
}

/* ── Initialize DLL loader ─────────────────────────────────── */

void dll_loader_init(void)
{
    module_count = 0;
    shim_count = 0;

    /* Built-in shims are registered by winexec.c after calling this */
}

/* ── Find loaded module ────────────────────────────────────── */

LOADED_MODULE *dll_find_module(const char *dll_name)
{
    char lower[64];
    dl_strcpy_lower(lower, strip_path(dll_name), 64);

    for (int i = 0; i < module_count; i++) {
        if (dl_stricmp(lower, modules[i].name) == 0)
            return &modules[i];
    }

    /* Also try without .dll extension */
    char lower_noext[64];
    dl_strcpy_lower(lower_noext, lower, 64);
    int len = 0;
    while (lower_noext[len]) len++;
    if (len > 4 && lower_noext[len-4] == '.' &&
        lower_noext[len-3] == 'd' && lower_noext[len-2] == 'l' &&
        lower_noext[len-1] == 'l') {
        lower_noext[len-4] = 0;
    }

    for (int i = 0; i < module_count; i++) {
        char mod_noext[64];
        dl_strcpy_lower(mod_noext, modules[i].name, 64);
        int mlen = 0;
        while (mod_noext[mlen]) mlen++;
        if (mlen > 4 && mod_noext[mlen-4] == '.' &&
            mod_noext[mlen-3] == 'd' && mod_noext[mlen-2] == 'l' &&
            mod_noext[mlen-1] == 'l') {
            mod_noext[mlen-4] = 0;
        }

        if (dl_stricmp(lower_noext, mod_noext) == 0)
            return &modules[i];
    }

    return NULL;
}

/* ── Resolve export from PE export directory ───────────────── */

PVOID dll_resolve_export(LOADED_MODULE *mod, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal)
{
    if (!mod || !mod->image.ImageBase) return NULL;

    BYTE *base = (BYTE *)mod->image.ImageBase;

    /* Get NT headers from loaded image */
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    /* Access data directory — works for both PE32 and PE32+ */
    IMAGE_DATA_DIRECTORY *exp_dir;
    BYTE *nt_base = base + dos->e_lfanew;
    USHORT magic = *(USHORT *)(nt_base + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER));
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        PIMAGE_NT_HEADERS32 nt32 = (PIMAGE_NT_HEADERS32)nt_base;
        if (nt32->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return NULL;
        exp_dir = &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        PIMAGE_NT_HEADERS64 nt64 = (PIMAGE_NT_HEADERS64)nt_base;
        if (nt64->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return NULL;
        exp_dir = &nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }

    if (exp_dir->VirtualAddress == 0 || exp_dir->Size == 0)
        return NULL;

    PIMAGE_EXPORT_DIRECTORY exports =
        (PIMAGE_EXPORT_DIRECTORY)(base + exp_dir->VirtualAddress);

    ULONG *func_addrs  = (ULONG *)(base + exports->AddressOfFunctions);
    ULONG *name_ptrs   = (ULONG *)(base + exports->AddressOfNames);
    USHORT *name_ords  = (USHORT *)(base + exports->AddressOfNameOrdinals);

    if (by_ordinal) {
        ULONG index = ordinal - exports->Base;
        if (index < exports->NumberOfFunctions && func_addrs[index])
            return (PVOID)(base + func_addrs[index]);
        return NULL;
    }

    /* Search by name */
    if (!func_name) return NULL;

    for (ULONG i = 0; i < exports->NumberOfNames; i++) {
        const char *exp_name = (const char *)(base + name_ptrs[i]);
        if (dl_strcmp(func_name, exp_name) == 0) {
            USHORT ord_index = name_ords[i];
            if (ord_index < exports->NumberOfFunctions) {
                ULONG func_rva = func_addrs[ord_index];
                if (func_rva == 0) return NULL;

                /* Check for forwarder (RVA within export directory) */
                if (func_rva >= exp_dir->VirtualAddress &&
                    func_rva < exp_dir->VirtualAddress + exp_dir->Size) {
                    /* Forwarder string like "NTDLL.RtlInitUnicodeString" */
                    const char *fwd = (const char *)(base + func_rva);
                    /* Parse DLL.Function */
                    char fwd_dll[64], fwd_func[128];
                    int j = 0;
                    while (fwd[j] && fwd[j] != '.' && j < 63) {
                        fwd_dll[j] = fwd[j];
                        j++;
                    }
                    fwd_dll[j] = 0;
                    if (fwd[j] == '.') j++;
                    int k = 0;
                    while (fwd[j] && k < 127) {
                        fwd_func[k++] = fwd[j++];
                    }
                    fwd_func[k] = 0;

                    /* Recursively resolve the forwarder */
                    return dll_resolve_import(fwd_dll, fwd_func, 0, FALSE);
                }

                return (PVOID)(base + func_rva);
            }
        }
    }

    return NULL;
}

/* ── Load a PE DLL ─────────────────────────────────────────── */

PVOID dll_load(const char *dll_name, const BYTE *file_data, SIZE_T file_size)
{
    if (module_count >= MAX_LOADED_MODULES) {
        serial_puts("[DLL] max modules reached\n");
        return NULL;
    }

    /* Check if already loaded */
    LOADED_MODULE *existing = dll_find_module(dll_name);
    if (existing) {
        existing->ref_count++;
        return existing->image.ImageBase;
    }

    serial_puts("[DLL] loading: ");
    serial_puts(dll_name);
    serial_puts("\n");

    /*
     * Reserve the module slot BEFORE pe_load().
     * pe_load() can recursively trigger dll_load() for dependencies.
     * Without this, the recursive call uses the same modules[module_count]
     * slot, corrupting the parent's module entry.
     */
    LOADED_MODULE *mod = &modules[module_count];
    dl_strcpy_lower(mod->name, strip_path(dll_name), 64);
    mod->ref_count   = 1;
    mod->initialized = FALSE;
    mod->dll_main    = NULL;
    module_count++;  /* Reserve slot — recursive loads use next slot */

    /* Load the PE (may trigger recursive dll_load for dependencies) */
    NTSTATUS status = pe_load(file_data, file_size, &mod->image);

    if (!NT_SUCCESS(status)) {
        serial_puts("[DLL] pe_load failed: ");
        serial_puthex((uint64_t)status, 8);
        serial_puts("\n");
        module_count--;  /* Release reserved slot */
        mod->name[0] = 0;
        return NULL;
    }

    /* Fill remaining module info (name already set above) */
    mod->dll_main    = (mod->image.IsDLL && mod->image.EntryPointRVA != 0)
                       ? mod->image.EntryPoint : NULL;

    serial_puts("[DLL] loaded at ");
    serial_puthex((uint64_t)(ULONG_PTR)mod->image.ImageBase, 16);
    serial_puts("\n");

    /* PE32 DLLs: patch IAT to use compat32 thunks for shim DLL imports.
     * Without this, imports from KERNEL32/MSVCRT etc. have truncated
     * 64-bit addresses and the DLL jumps to garbage when calling them. */
    if (mod->image.Is32Bit) {
        NTSTATUS compat_st = compat32_patch_iat(&mod->image);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[DLL] WARNING: compat32 IAT patch failed for ");
            serial_puts(dll_name);
            serial_puts("\n");
        }
    }

    /* Call DllMain(DLL_PROCESS_ATTACH) if it has one.
     * Skip DllMain for DLLs that have a registered shim — the shim already
     * provides all CRT/API functions and the real DllMain may crash trying
     * to initialise Windows-internal data structures (e.g. bundled MSVCRT.dll
     * tries to init __pioinfo tables that don't exist in OsitoK). */
    if (mod->dll_main && mod->image.IsDLL && !find_shim(mod->name)) {
        if (mod->image.Is32Bit) {
            /* PE32 DLLs: call DllMain via compat32 callback mechanism.
             * Switches to 32-bit compat mode, calls DllMain(hInstance,
             * DLL_PROCESS_ATTACH, NULL), returns to 64-bit via INT 0x2E. */
            serial_puts("[DLL] PE32 DLL — calling DllMain via compat32\n");
            dump_seh_chain("before DllMain");
            uint32_t entry32 = (uint32_t)(uint64_t)mod->dll_main;
            uint32_t args[3] = {
                (uint32_t)(uint64_t)mod->image.ImageBase,
                1,  /* DLL_PROCESS_ATTACH */
                0   /* lpReserved = NULL */
            };
            uint32_t ok = compat32_callback_args(entry32, 3, args);
            dump_seh_chain("after DllMain");
            serial_puts("[DLL] DllMain returned ");
            serial_puthex((uint64_t)ok, 8);
            serial_puts("\n");
            mod->initialized = TRUE;
        } else {
            serial_puts("[DLL] calling DllMain(ATTACH)\n");

            typedef BOOL (WINAPI *dll_main_fn)(PVOID hinstDLL, DWORD fdwReason, PVOID lpReserved);
            dll_main_fn entry = (dll_main_fn)mod->dll_main;
            BOOL ok = entry(mod->image.ImageBase, DLL_PROCESS_ATTACH, NULL);

            if (!ok) {
                serial_puts("[DLL] DllMain returned FALSE\n");
                pe_unload(&mod->image);
                module_count--;
                return NULL;
            }

            mod->initialized = TRUE;
        }
    }

    return mod->image.ImageBase;
}

/* ── Unload a DLL ──────────────────────────────────────────── */

void dll_unload(LOADED_MODULE *mod)
{
    if (!mod) return;

    mod->ref_count--;
    if (mod->ref_count > 0) return;

    /* Call DllMain(DLL_PROCESS_DETACH) — skip for PE32 DLLs */
    if (mod->dll_main && mod->initialized && !mod->image.Is32Bit) {
        typedef BOOL (WINAPI *dll_main_fn)(PVOID, DWORD, PVOID);
        dll_main_fn entry = (dll_main_fn)mod->dll_main;
        entry(mod->image.ImageBase, DLL_PROCESS_DETACH, NULL);
    }

    pe_unload(&mod->image);

    /* Remove from module list (swap with last) */
    int idx = (int)(mod - modules);
    if (idx < module_count - 1) {
        modules[idx] = modules[module_count - 1];
    }
    module_count--;
}

/* ── Auto-load DLLs from filesystem ────────────────────────── */

extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Recursion guard to prevent infinite loops */
static int load_depth = 0;
#define MAX_LOAD_DEPTH 8

/*
 * Try to load a DLL from the filesystem by name.
 * Called when an import references a DLL that isn't shimmed or loaded.
 * Returns the loaded module or NULL.
 */
static LOADED_MODULE *dll_try_load_from_fs(const char *dll_name)
{
    if (load_depth >= MAX_LOAD_DEPTH) {
        serial_puts("[DLL] max dependency depth reached: ");
        serial_puts(dll_name);
        serial_puts("\n");
        return NULL;
    }

    /* Strip path from DLL name */
    const char *basename = dll_name;
    for (const char *p = dll_name; *p; p++) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    /* Try to find the file */
    void *fsfile = osfs2_find(basename);
    if (!fsfile) {
        /* Try original name */
        if (basename != dll_name)
            fsfile = osfs2_find(dll_name);
    }

    if (!fsfile) return NULL;

    uint64_t fsize = osfs2_file_size(fsfile);
    if (fsize == 0) return NULL;

    serial_puts("[DLL] auto-loading dependency: ");
    serial_puts(basename);
    serial_puts("\n");

    uint64_t pages = (fsize + 0xFFF) / 4096;
    uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
    if (!buf) return NULL;

    osfs2_read(fsfile, 0, buf, fsize);

    load_depth++;
    PVOID base = dll_load(dll_name, (const BYTE *)buf, (SIZE_T)fsize);
    load_depth--;

    mem_free_pages(buf, pages);

    if (base)
        return dll_find_module(dll_name);

    return NULL;
}

/* ── Master import resolver ────────────────────────────────── */

PVOID dll_resolve_import(const char *dll_name, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal)
{
    /* 1. Try built-in shims first (ntdll, kernel32, msvcrt, etc.) */
    shim_resolver_fn shim = find_shim(dll_name);
    if (shim) {
        PVOID fn = shim(func_name, ordinal, by_ordinal);
        if (fn) return fn;
    }

    /* 2. Try loaded PE modules */
    LOADED_MODULE *mod = dll_find_module(dll_name);
    if (mod) {
        PVOID fn = dll_resolve_export(mod, func_name, ordinal, by_ordinal);
        if (fn) return fn;
    }

    /* 3. Fallback: search all shims (for api-ms-win-crt-* redirections) */
    if (func_name) {
        for (int i = 0; i < shim_count; i++) {
            PVOID fn = shims[i].resolver(func_name, ordinal, by_ordinal);
            if (fn) return fn;
        }
    }

    /* 4. Search all loaded modules */
    if (func_name) {
        for (int i = 0; i < module_count; i++) {
            PVOID fn = dll_resolve_export(&modules[i], func_name, ordinal, by_ordinal);
            if (fn) return fn;
        }
    }

    /* 5. Auto-load DLL from filesystem (recursive dependency resolution)
     *    Skip if we have a registered shim — trust the shim, don't load
     *    conflicting PE DLLs (e.g. real MSVCRT.dll from game directory) */
    if (dll_name && !find_shim(dll_name)) {
        LOADED_MODULE *auto_mod = dll_try_load_from_fs(dll_name);
        if (auto_mod) {
            PVOID fn = dll_resolve_export(auto_mod, func_name, ordinal, by_ordinal);
            if (fn) return fn;
        }
    }

    serial_puts("[DLL] unresolved: ");
    if (dll_name) serial_puts(dll_name);
    serial_puts(" -> ");
    if (func_name) serial_puts(func_name);
    serial_puts("\n");

    return NULL;
}
