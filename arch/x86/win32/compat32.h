/*
 * OsitoK Windows Compatibility Layer — 32-bit Compatibility Mode
 *
 * Provides the thunk layer for PE32 (i386) executables running on
 * x86-64 Long Mode via Compatibility Mode.
 *
 * Architecture:
 *   PE32 code runs in 32-bit compat mode (L=0, D=1 GDT segment).
 *   IAT entries point to 32-bit thunk stubs in low memory (<4GB).
 *   Each thunk: marshals stack args → registers, switches to 64-bit
 *   mode via far call, invokes the 64-bit shim, returns to 32-bit.
 *
 * On the test harness (Linux x86-64), PE32 code runs natively as
 * 64-bit (addresses are <4GB via mmap), so thunks just do the
 * cdecl→ms_abi register marshaling without mode switching.
 */

#ifndef COMPAT32_H
#define COMPAT32_H

#include "nttypes.h"
#include "pe.h"

/* GDT selectors for 32-bit code/data (Ring 0 for OsitoK)
 * Indices 8 and 9 in the kernel GDT, added by win32_init().
 * (0x28/0x30 are taken by SYSCALL's 64-bit CS/SS) */
#define GDT_SEL_CODE32  0x40    /* 32-bit code segment (GDT index 8) */
#define GDT_SEL_DATA32  0x48    /* 32-bit data segment (GDT index 9) */
#define GDT_SEL_CODE64  0x38    /* 64-bit code segment (IDT gate CS) */

/* Maximum number of thunked functions */
#define COMPAT32_MAX_THUNKS  512

/* Thunk entry: maps a 32-bit callable address to a 64-bit shim */
typedef struct {
    uint32_t thunk_addr;    /* 32-bit address of the thunk stub */
    uint64_t target_addr;   /* 64-bit address of the real shim function */
    uint8_t  num_args;      /* number of DWORD stack arguments (for cleanup) */
    const char *name;       /* function name (for debug) */
} compat32_thunk_t;

/*
 * Initialize the 32-bit thunk system.
 * Allocates executable memory below 4GB for thunk stubs.
 */
void compat32_init(void);

/*
 * Create a thunk for a 64-bit shim function.
 * Returns a 32-bit address that PE32 code can call via IAT.
 *
 * target:   64-bit address of the real shim (ms_abi)
 * name:     function name (for debug logging)
 * num_args: number of 32-bit DWORD stack arguments
 *
 * Returns: 32-bit thunk address, or 0 on failure.
 */
uint32_t compat32_make_thunk(uint64_t target, const char *name, uint8_t num_args);

/*
 * Patch PE32 IAT entries to use thunks instead of raw 64-bit addresses.
 * Called after pe_load() for PE32 images.
 */
NTSTATUS compat32_patch_iat(PE_IMAGE_INFO *info);

/*
 * Set up the 32-bit TEB with FS base.
 * Windows i386 uses FS:0 to access the TEB.
 */
void compat32_setup_teb(void *teb_addr);

/*
 * Enter 32-bit compatibility mode and jump to the PE32 entry point.
 * Only used on bare metal (OsitoK). On test harness, calls directly.
 *
 * entry:     32-bit entry point address
 * stack_top: 32-bit stack pointer
 */
void compat32_enter(uint32_t entry, uint32_t stack_top);

#endif /* COMPAT32_H */
