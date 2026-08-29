/*
 * OsitoK Windows Compatibility Layer — Co-located ABI descriptors
 *
 * NT never computes an import's arg count: its loader writes the raw function
 * VA and the callee's compiled `ret N` cleans the stack (ldrsnap.c). We only
 * need an arg count because our shim is 64-bit and the 32-bit thunk must emit
 * the `ret N`. NT's own cross-ABI thunk compiler (.thk files) derives that N
 * from each API's *typed signature*, declared right next to the function — it
 * never guesses. This header is that model: every shim export carries its argc
 * + calling convention, derived from its prototype, co-located with the func
 * pointer. The old name-keyed `guess_num_args` table with its silent default of
 * 4 is replaced; a lookup miss is a loud [ABI-MISS], never a wrong guess.
 *
 * See docs/win32-layer-correctness.md and memory feedback-win32-fix-layer-not-binary.
 */
#ifndef WIN32_ABI_H
#define WIN32_ABI_H

#include "nttypes.h"
#include "compat32.h"   /* CC_STDCALL / CC_CDECL */

/* Calling conventions beyond the two in compat32.h. */
#ifndef CC_THISCALL
#define CC_THISCALL 2   /* `this` in ECX; N-1 args on the stack */
#endif
#ifndef CC_FASTCALL
#define CC_FASTCALL 3   /* first two DWORD args in ECX/EDX */
#endif

/* The low bits retain the calling convention. ABI flags below preserve the
 * descriptor layout shared by the existing shim tables. */
#define WIN32_EXPORT_DATA_FLAG 0x80U
#define WIN32_EXPORT_ABI_MASK  0x7FU
#define WIN32_EXPORT_CC_MASK   CC_CONVENTION_MASK

/*
 * Co-located export descriptor. `argc` = number of 32-bit stack DWORD slots
 * the caller pushes (a 64-bit-by-value param — __int64/double — counts as 2).
 * `cc` = CC_STDCALL/CC_CDECL/CC_THISCALL. The resolver returns `func`; the
 * thunk generator reads `argc`/`cc` from the SAME row, so they cannot drift
 * from the prototype.
 */
typedef struct {
    const char *name;
    void       *func;
    uint8_t     argc;
    uint8_t     cc;
} WIN32_EXPORT;

/* Convenience initializers so migrated tables read like the inventory. */
#define WX_STD(name, fn, n)   { name, (void *)(fn), (uint8_t)(n), CC_STDCALL }
#define WX_CDL(name, fn, n)   { name, (void *)(fn), (uint8_t)(n), CC_CDECL }
#define WX_THIS(name, fn, n)  { name, (void *)(fn), (uint8_t)(n), CC_THISCALL }
#define WX_DATA(name, ptr)     { name, (void *)(ptr), 0, \
                                 (uint8_t)(CC_CDECL | WIN32_EXPORT_DATA_FLAG) }

/* Register a shim DLL's co-located export table for ABI lookup. Call once per
 * DLL alongside dll_register_shim(). `count` = number of rows. */
void win32_abi_register(const char *dll_name, const WIN32_EXPORT *table, int count);

/*
 * Resolve {argc, cc} for an import. Order:
 *   1. exact co-located table row for dll_name (case-insensitive name match)
 *   2. any registered table (covers api-ms-win-crt-* style redirections)
 *   3. MSVC C++ demangling for `?…@@…` names
 * Returns 1 and fills *out_argc/*out_cc on success, 0 on miss (caller logs).
 */
int win32_abi_lookup(const char *dll_name, const char *func_name,
                     uint8_t *out_argc, uint8_t *out_cc);

/* Resolve an ordinal import's ABI from the function selected by its DLL
 * resolver. Also returns the canonical export name for diagnostics. */
int win32_abi_lookup_target(const char *dll_name, const void *target,
                            const char **out_name, uint8_t *out_argc,
                            uint8_t *out_cc);

/* True when a registered resolver target is exported storage rather than an
 * entry point.  Such targets must be written directly to an IAT/GPA result. */
int win32_abi_target_is_data(const char *dll_name, const void *target);

/* Register and resolve a fixed-signature PE32 bridge for a native variadic
 * shim. The compat dispatcher passes the bridge a pointer to the first
 * variadic DWORD; native PE64 callers continue to use the original target. */
void win32_abi_register_compat32_bridge(const void *native_target,
                                        const void *compat32_target);
const void *win32_abi_compat32_bridge(const void *native_target);

/*
 * Decode argc (32-bit stack DWORDs) + calling convention from an MSVC-mangled
 * function name (`?name@@YA…@Z`, member `?m@C@@QAE…`). Conservative: returns 0
 * if it meets a by-value user-defined type or an unhandled token, so the caller
 * falls back to [ABI-MISS] rather than a wrong count. Returns 1 on success.
 */
int msvc_demangle_abi(const char *mangled, uint8_t *out_argc, uint8_t *out_cc);

#endif /* WIN32_ABI_H */
