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

/* CEF keeps several isolated helper processes alive at once.  Each process
 * needs its own PE mappings and HMODULE facades, so this is a system-wide
 * capacity rather than a per-process DLL limit. */
#define MAX_LOADED_MODULES 1024
#define DLL_SYNTHETIC_SHIM_IMAGE_SIZE 0x10000UL

typedef struct _LOADED_MODULE {
    volatile int    state;          /* 0=free, 1=loading, 2=ready */
    ULONG           owner_pid;      /* Win32 process owning this image */
    int             next_owner_index; /* process-local loaded-module list */
    char            name[64];       /* DLL name (lowercase, no path) */
    char            path[260];      /* canonical OsitoFS path, if known */
    PE_IMAGE_INFO   image;          /* PE image info */
    BOOL            synthetic_shim; /* mapped PE facade for a built-in shim */
    BOOL            published_exports; /* complete named function EAT */
    PVOID           dll_main;       /* DllMain entry point (if DLL) */
    BOOL            initialized;    /* DllMain(ATTACH) called? */
    BOOL            thread_notifications_disabled;
    uint64_t        pending_attach_order; /* mapped graph awaiting TLS/DllMain */
    ULONG           init_order;     /* completed process-attach order */
    int             ref_count;      /* LoadLibrary reference count */
    BOOL            pinned;         /* retained until process teardown */
} LOADED_MODULE;

/* Stable, pointer-free copy used by Toolhelp/PSAPI-style enumerators. The
 * loader owns no storage referenced by this record after the snapshot call
 * returns, so unloading a DLL cannot invalidate an in-progress enumeration. */
typedef struct _DLL_MODULE_SNAPSHOT_ENTRY {
    PVOID image_base;
    ULONG image_size;
    BOOL  synthetic_shim;
    char  name[64];
    char  path[260];
} DLL_MODULE_SNAPSHOT_ENTRY;

/* ── API ───────────────────────────────────────────────────── */

/*
 * Initialize the DLL loader subsystem.
 * Registers built-in shim modules (ntdll, kernel32, msvcrt).
 */
void dll_loader_init(void);

/* The process loader lock backs PEB.LoaderLock and serializes module-list
 * mutations, recursive dependency loads, and DLL lifecycle callbacks. */
void dll_loader_lock_enter(void);
BOOL dll_loader_lock_try_enter(void);
BOOL dll_loader_lock_leave(void);
BOOL dll_loader_lock_owned_by_current_thread(void);

/* Return 32 or 64 from the mapped main executable, or 0 before publication. */
int dll_current_process_bitness(void);

/* Map and pin the process's NTDLL after the main image is published, before
 * import binding or CreateProcess returns. Does not execute guest code. */
NTSTATUS dll_prepare_process(void);

/* Initialize PE32+ static TLS and run process-attach TLS callbacks. */
NTSTATUS win64_attach_tls(PE_IMAGE_INFO *info);

/*
 * Load a PE DLL from file data. Resolves imports, calls DllMain.
 * Returns module handle (base address) or NULL on failure.
 */
PVOID dll_load(const char *dll_name, const BYTE *file_data, SIZE_T file_size);

/* Search OsitoFS using the process DLL search order, read, and load a PE.
 * `append_dll` supports explicit LoadLibrary("name") calls; import-table
 * dependencies already carry their file extension. */
PVOID dll_load_from_fs(const char *dll_name, BOOL append_dll);

/* Resolve a version-resource filename with the process loader policy, without
 * loading it or changing module references. Output is a physical OsitoFS path:
 * subsequent file reads must not apply WOW64 redirection a second time. */
BOOL dll_resolve_data_path(const char *name, char fs_path[260]);

/* LoadLibraryEx variant. Search flags are inherited by import-table
 * dependencies while the image is being mapped. */
PVOID dll_load_from_fs_ex(const char *dll_name, BOOL append_dll,
                          DWORD search_flags);

/* Process-local DLL search policy used by the Kernel32 directory APIs. Paths
 * are normalized OsitoFS-relative names; returned cookies are opaque to PE
 * callers and remain valid until removed or the process exits. */
BOOL  dll_set_default_search_flags(DWORD flags);
BOOL  dll_set_search_directory(const char *normalized_path,
                               BOOL disable_current_directory);
PVOID dll_add_search_directory(const char *normalized_path);
BOOL  dll_remove_search_directory(PVOID cookie);

/* Inspect or evict the shared read-only PE file cache. Loaded images keep
 * private relocations/IAT/TLS; only immutable source bytes are shared. */
void dll_file_cache_dump(void);
int  dll_file_cache_flush_unused(void);

/*
 * Look up a loaded module by name (case-insensitive).
 * Returns the LOADED_MODULE entry or NULL.
 */
LOADED_MODULE *dll_find_module(const char *dll_name);

/* Look up a loaded module by its LoadLibrary handle (image base). */
LOADED_MODULE *dll_find_module_by_base(PVOID image_base);

/* Look up a loaded module containing an address in its mapped image. */
LOADED_MODULE *dll_find_module_by_address(PVOID address);

/* Copy the modules visible to one Win32 process in load order. The return
 * value is the required entry count. No entries are written unless capacity
 * is large enough for the complete immutable snapshot. */
DWORD dll_snapshot_modules(ULONG owner_pid,
                           DLL_MODULE_SNAPSHOT_ENTRY *entries,
                           DWORD capacity);

/* Return a loaded module handle, optionally taking a LoadLibrary reference. */
PVOID dll_get_module_handle(const char *name, BOOL add_reference);

/* GetModuleHandleEx helpers. Lookup and reference/pin changes are atomic with
 * respect to FreeLibrary in the owning process. */
PVOID dll_get_module_handle_ex(const char *name, BOOL add_reference,
                               BOOL pin);
PVOID dll_get_module_handle_by_address(PVOID address, BOOL add_reference,
                                       BOOL pin);

/* Materialize a process-local PE image for a registered built-in shim. */
PVOID dll_get_shim_module_handle(const char *name, BOOL add_reference);
PVOID dll_get_shim_module_handle_ex(const char *name, BOOL add_reference,
                                    BOOL pin);

/* Emit module/base/RVA details for an address while debugging PE faults. */
void dll_debug_log_address(PVOID address);

/*
 * Resolve an export from a loaded module's PE export directory.
 * Walks the IMAGE_EXPORT_DIRECTORY in the loaded image.
 */
PVOID dll_resolve_export(LOADED_MODULE *mod, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal);
void dll_export_lookup_dump(void);
int  dll_export_lookup_selftest(void);

/* Release a LoadLibrary reference. Pinned modules remain until teardown. */
BOOL dll_release_module(PVOID image_base);

/* Drop every private DLL image owned by a terminating process. */
void dll_release_process(ULONG owner_pid);
/* Explicit loader shutdown: normal guest callbacks, separate from forced
 * image teardown. Caller is in the owning process with its TEB still valid. */
void dll_shutdown_process(void);
BOOL dll_shutdown_in_progress(void);
BOOL dll_within_loader_callout(void);
void win32_tls_process_detach_image(PVOID image_base,BOOL compat32);
void win32_tls_notify_image(PVOID image_base, BOOL compat32, DWORD reason);

/* Notify initialized same-bitness DLLs owned by the current process. */
void dll_notify_thread(DWORD reason);

/* Stop DLL_THREAD_ATTACH/DETACH callbacks for a loaded module. */
BOOL dll_disable_thread_notifications(PVOID image_base);

/*
 * Register a built-in shim as a "virtual" module.
 * The resolver function handles export lookups.
 */
typedef PVOID (*shim_resolver_fn)(const char *func_name, USHORT ordinal, BOOL by_ordinal);
typedef BOOL (*shim_lifecycle_fn)(const char *dll_name, PVOID module,
                                  DWORD reason, PVOID reserved);

void dll_register_shim(const char *dll_name, shim_resolver_fn resolver);
void dll_register_shim_ex(const char *dll_name, shim_resolver_fn resolver,
                          shim_lifecycle_fn lifecycle);

/* Opt in only providers with complete static function descriptors and no
 * pre-existing public ordinals/data exports. Ordinal = descriptor index + 1.
 * The same provider also supplies win32_abi_register_provider(). */
struct _WIN32_EXPORT;
void dll_register_shim_named_exports(const char *dll_name,
                                      const struct _WIN32_EXPORT *table,
                                      int count);

/* Return TRUE when a DLL name resolves through the built-in shim registry. */
BOOL dll_is_shim(const char *dll_name);

/* Resolve only against the named shim, without cross-module fallback. */
PVOID dll_resolve_shim_export(const char *dll_name, const char *func_name,
                              USHORT ordinal, BOOL by_ordinal);

/* Return a process-local PE32+ jump thunk for a built-in shim function.
 * PE code must never observe the kernel's high-half implementation address. */
PVOID dll_get_shim_export_thunk(const char *dll_name, PVOID target);

/*
 * Master resolve: try loaded module exports, then built-in shims.
 * Called from pe_resolve_import in winexec.c.
 */
PVOID dll_resolve_import(const char *dll_name, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal);

/* Preserve a failed dependency load across recursive imports/forwarders.
 * Missing DLLs/exports remain subject to the PE compatibility policy; other
 * failures must stop binding before guest callbacks or the entry point. */
PVOID dll_resolve_import_status(const char *dll_name, const char *func_name,
                                USHORT ordinal, BOOL by_ordinal,
                                NTSTATUS *status);

#endif /* DLLLOADER_H */
