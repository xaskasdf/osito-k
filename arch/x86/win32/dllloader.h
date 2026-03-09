/*
 * OsitoK Windows Compatibility Layer — DLL Loader
 *
 * Manages loaded PE DLLs: tracks modules, resolves exports from
 * PE export directories, handles dependency chains, calls DllMain.
 */

#ifndef DLLLOADER_H
#define DLLLOADER_H

#include "pe.h"

/* ── DllMain constants ─────────────────────────────────────── */

#define DLL_PROCESS_DETACH  0
#define DLL_PROCESS_ATTACH  1
#define DLL_THREAD_ATTACH   2
#define DLL_THREAD_DETACH   3

/* ── Loaded module tracking ────────────────────────────────── */

#define MAX_LOADED_MODULES 64

typedef struct _LOADED_MODULE {
    char            name[64];       /* DLL name (lowercase, no path) */
    PE_IMAGE_INFO   image;          /* PE image info */
    PVOID           dll_main;       /* DllMain entry point (if DLL) */
    BOOL            initialized;    /* DllMain(ATTACH) called? */
    int             ref_count;      /* LoadLibrary reference count */
} LOADED_MODULE;

/* ── API ───────────────────────────────────────────────────── */

/*
 * Initialize the DLL loader subsystem.
 * Registers built-in shim modules (ntdll, kernel32, msvcrt).
 */
void dll_loader_init(void);

/*
 * Load a PE DLL from file data. Resolves imports, calls DllMain.
 * Returns module handle (base address) or NULL on failure.
 */
PVOID dll_load(const char *dll_name, const BYTE *file_data, SIZE_T file_size);

/*
 * Look up a loaded module by name (case-insensitive).
 * Returns the LOADED_MODULE entry or NULL.
 */
LOADED_MODULE *dll_find_module(const char *dll_name);

/*
 * Resolve an export from a loaded module's PE export directory.
 * Walks the IMAGE_EXPORT_DIRECTORY in the loaded image.
 */
PVOID dll_resolve_export(LOADED_MODULE *mod, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal);

/*
 * Unload a DLL (calls DllMain(DETACH), frees image).
 */
void dll_unload(LOADED_MODULE *mod);

/*
 * Register a built-in shim as a "virtual" module.
 * The resolver function handles export lookups.
 */
typedef PVOID (*shim_resolver_fn)(const char *func_name, USHORT ordinal, BOOL by_ordinal);

void dll_register_shim(const char *dll_name, shim_resolver_fn resolver);

/*
 * Master resolve: try loaded module exports, then built-in shims.
 * Called from pe_resolve_import in winexec.c.
 */
PVOID dll_resolve_import(const char *dll_name, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal);

#endif /* DLLLOADER_H */
