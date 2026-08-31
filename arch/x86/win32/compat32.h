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
#define COMPAT32_MAX_THUNKS  2048

/* Calling conventions for thunk generation */
#define CC_STDCALL  0   /* callee cleans stack: ret N  (KERNEL32, USER32, ...) */
#define CC_CDECL    1   /* caller cleans stack: ret    (MSVCRT) */
#define CC_CONVENTION_MASK 0x0F

/* The fixed arguments are followed by a PE32 vararg area. The dispatcher
 * appends a uint32_t pointer to that area when calling the registered bridge. */
#define CC_VARIADIC 0x40

/* Thunk entry: maps a 32-bit callable address to a 64-bit shim */
typedef struct {
    uint32_t thunk_addr;    /* 32-bit address of the thunk stub */
    uint64_t target_addr;   /* 64-bit address of the real shim function */
    uint8_t  num_args;      /* number of DWORD stack arguments (for dispatch) */
    uint8_t  callconv;      /* CC_* convention plus optional ABI flags */
    const char *name;       /* function name (for debug) */
} compat32_thunk_t;

/*
 * Initialize the 32-bit thunk system.
 * Allocates executable memory below 4GB for thunk stubs.
 */
void compat32_init(void);
BOOL compat32_is_initialized(void);
BOOL compat32_runtime_range_conflicts(ULONGLONG base, ULONGLONG size);

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
uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                 uint8_t num_args, uint8_t callconv);

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
TEB32 *compat32_current_teb(void);

/*
 * Enter 32-bit compatibility mode and jump to the PE32 entry point.
 * Only used on bare metal (OsitoK). On test harness, calls directly.
 *
 * entry:     32-bit entry point address
 * stack_top: 32-bit stack pointer
 */
void compat32_enter(uint32_t entry, uint32_t stack_top);

/*
 * Call a 32-bit void function from 64-bit code.
 * Switches to compat mode, calls the function, returns when it finishes.
 * Used by _initterm to call CRT initializers / C++ constructors.
 */
void compat32_callback(uint32_t func_addr);

/*
 * Call a 32-bit function with arguments, returning EAX.
 * Switches to compat mode, pushes args (right-to-left), calls func,
 * captures EAX return value, returns to 64-bit code.
 */
uint32_t compat32_callback_args(uint32_t func_addr, int nargs,
                                const uint32_t *args);
uint32_t compat32_callback_args_with_ebp(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t frame_ebp);
uint32_t compat32_callback_args_on_stack(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t stack_top);
uint32_t compat32_thread_entry_on_stack(uint32_t func_addr, int nargs,
                                        const uint32_t *args,
                                        uint32_t stack_top);

/* Return the active PE32 API caller's ESP for callbacks on this scheduler
 * thread. Zero means there is no validated user-stack context. */
uint32_t compat32_current_user_stack_top(void);

/*
 * Look up a thunk entry by its 32-bit stub address.
 * Returns the thunk index, or -1 if not found.
 */
int32_t compat32_find_thunk(uint32_t addr);

/*
 * Get the name of a thunk by index.
 */
const char *compat32_get_name(uint32_t thunk_idx);

/*
 * Dispatch an exception through the 32-bit SEH chain.
 * Walks TEB.ExceptionList reading 32-bit structs, calls handlers
 * via thunk table lookup or compat32_callback.
 * Returns: 1 if handled, 0 if unhandled.
 */
int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord);

/* Register state captured by the x86 exception entry path.  The dispatcher
 * expands this into the Win32 CONTEXT32 ABI exposed to PE32 handlers, then
 * copies any handler changes back before IRET resumes the application. */
typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
    uint32_t seg_cs, seg_ss, seg_ds, seg_es, seg_fs, seg_gs;
} compat32_cpu_context_t;

int compat32_seh_dispatch_cpu(PEXCEPTION_RECORD ExceptionRecord,
                              compat32_cpu_context_t *Context);
int compat32_seh_dispatch_active(void);
int compat32_range_readable(uint32_t address, uint32_t size);

/*
 * Global flag: set to 1 when running a PE32 (i386) executable.
 * Shim functions that write to output structs MUST check this flag
 * and use 32-bit struct layouts when set.
 *
 * The problem: 64-bit shim structs have 8-byte pointers/handles,
 * but 32-bit PE code allocates buffers with 4-byte pointer slots.
 * Writing a 64-bit struct to a 32-bit buffer overflows, corrupting
 * the stack (most commonly: SEH ExceptionList on the 32-bit stack).
 */

/*
 * Create a stub UObject with a valid vtable (all entries return 0).
 * Returns 32-bit address of the object, or 0 on failure.
 */
uint32_t create_stub_uobject(const char *name);

#endif /* COMPAT32_H */
