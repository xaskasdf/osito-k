/*
 * OsitoK Windows Compatibility Layer — PE Execution Engine
 *
 * This is the main entry point for running Windows PE executables.
 * Orchestrates: PE load → import resolution → PEB/TEB setup → jump to entry.
 *
 * Called from the OsitoK shell or process subsystem:
 *   winexec_run(file_data, file_size, argc, argv)
 */

#include "pe.h"
#include "ntsyscall.h"
#include "ntdll_shim.h"
#include "kernel32_shim.h"
#include "msvcrt_shim.h"
#include "dllloader.h"
#include "advapi32_shim.h"
#include "user32_shim.h"
#include "gdi32_shim.h"
#include "ddraw_shim.h"
#include "dsound_shim.h"
#include "wsock32_shim.h"
#include "shell32_shim.h"
#include "winmm_shim.h"
#include "ole32_shim.h"
#include "comctl32_shim.h"
#include "comdlg32_shim.h"
#include "compat32.h"
#include "handle.h"

/* ── External kernel interfaces ─────────────────────────────── */

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  proc_exit(int32_t code);

/* ── PE allocator callbacks (used by pe.c) ──────────────────── */

extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)

PVOID pe_alloc(PVOID preferred, SIZE_T size)
{
    uint64_t pages = (size + 0xFFF) / 4096;
#ifdef TEST_HARNESS
    #include <sys/mman.h>
    if (preferred) {
        void *p = mmap(preferred, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED)
            return p;
    }
    return mem_alloc_pages(pages);
#else
    void *phys = mem_alloc_pages(pages);
    if (!phys) return NULL;

    if (preferred) {
        /* Map physical pages at the PE's preferred ImageBase */
        uint64_t va = (uint64_t)preferred;
        uint64_t pa = (uint64_t)phys;
        for (uint64_t i = 0; i < pages; i++)
            paging_map_page(va + i * 4096, pa + i * 4096,
                            PTE_PRESENT | PTE_WRITABLE);
        return preferred;
    }
    /* No preference — return identity-mapped phys addr */
    return phys;
#endif
}

void pe_free(PVOID addr, SIZE_T size)
{
    uint64_t pages = (size + 0xFFF) / 4096;
    /* If this was a mapped PE, unmap the VA pages */
    for (uint64_t i = 0; i < pages; i++)
        paging_unmap_page((uint64_t)addr + i * 4096);
    /* Note: physical pages leak here — pe_free is only called on error
     * or process exit where the entire address space is torn down. */
}

void pe_log(const char *msg)
{
    serial_puts(msg);
    serial_puts("\n");
}

void pe_log_hex(const char *prefix, ULONGLONG val)
{
    serial_puts(prefix);
    serial_puthex(val, 16);
    serial_puts("\n");
}

/* ── Import resolver (used by pe.c) ─────────────────────────── */
/*
 * Delegates to the DLL loader which checks built-in shims first,
 * then loaded PE modules, then falls back to searching all shims.
 */
PVOID pe_resolve_import(const char *dll_name, const char *func_name,
                        USHORT ordinal, BOOL by_ordinal)
{
    return dll_resolve_import(dll_name, func_name, ordinal, by_ordinal);
}

/* ── PEB/TEB setup ──────────────────────────────────────────── */

static PEB  g_peb;
TEB  g_teb;   /* non-static: accessed by ntdll_shim.c for SEH dispatch */

/*
 * 32-bit TEB/PEB for PE32 compat mode.
 * PE32 (i386) code accesses TEB via FS segment with 4-byte pointer offsets.
 * The 64-bit TEB has 8-byte pointers, so offsets are all wrong for 32-bit code.
 * Example: 32-bit TEB.Self is at +0x18, but 64-bit TEB.Self is at +0x30.
 */
static PEB32  g_peb32;
TEB32  g_teb32;  /* non-static: accessed by ntdll_shim.c for SEH/LastError */

static void setup_environment(PVOID image_base, int is32bit)
{
    /* Minimal PEB (64-bit) */
    g_peb.BeingDebugged    = 0;
    g_peb.ImageBaseAddress = image_base;
    g_peb.Ldr              = NULL;  /* no module list yet */
    g_peb.ProcessParameters = NULL; /* no command line yet */
    g_peb.ProcessHeap      = (PVOID)(ULONG_PTR)0xBEEF0001; /* our heap sentinel */

    /* Minimal TEB (64-bit) */
    g_teb.Self                       = &g_teb;
    g_teb.ProcessEnvironmentBlock    = &g_peb;
    g_teb.ClientId.UniqueProcess     = (HANDLE)(ULONG_PTR)1;
    g_teb.ClientId.UniqueThread      = (HANDLE)(ULONG_PTR)1;
    g_teb.LastErrorValue             = 0;
    g_teb.ExceptionList              = (PVOID)(ULONG_PTR)-1; /* empty SEH chain */

    if (is32bit) {
        /*
         * TEB32/PEB32 already pre-initialized before pe_load().
         * Just update the image base address (now known) and log.
         */
        g_peb32.ImageBaseAddress = (uint32_t)(ULONG_PTR)image_base;

        serial_puts("[WINEXEC] TEB32 at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)&g_teb32, 16);
        serial_puts(" PEB32 at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)&g_peb32, 16);
        serial_puts("\n");
    }

    /*
     * Windows stores TEB pointer in GS:0x30 (x86-64).
     * Set GS base to point to our TEB structure.
     * On OsitoK, we use MSR_GS_BASE (0xC0000101).
     */
#ifndef TEST_HARNESS
    uint64_t teb_addr = (uint64_t)&g_teb;
    __asm__ volatile (
        "mov $0xC0000101, %%ecx\n"   /* MSR_GS_BASE */
        "mov %0, %%rax\n"
        "mov %0, %%rdx\n"
        "shr $32, %%rdx\n"
        "wrmsr\n"
        :
        : "r"(teb_addr)
        : "rax", "rcx", "rdx"
    );
#else
    /* In test harness mode on Linux, use arch_prctl to set GS base.
     * This is needed for PE code that accesses TEB via gs:0 (e.g., SEH). */
    {
        #include <asm/prctl.h>
        extern int arch_prctl(int code, unsigned long addr);
        arch_prctl(ARCH_SET_GS, (unsigned long)&g_teb);
    }
#endif
}

/* ── Main execution entry ───────────────────────────────────── */

/*
 * Load and execute a Windows PE executable.
 *
 * file_data: raw PE file bytes (read from OsitoFS or network)
 * file_size: size of file_data in bytes
 *
 * Returns: exit code from the PE, or -1 on load failure
 */
int winexec_run(const uint8_t *file_data, uint64_t file_size)
{
    serial_puts("\n=== OsitoK Windows Compatibility Layer ===\n");

    /* Initialize subsystems */
    NT_SERVICE_TABLE ssdt;
    nt_syscall_init(&ssdt);
    ntdll_shim_init();
    kernel32_shim_init();
    msvcrt_shim_init();

    /* Initialize DLL loader and register built-in shims */
    dll_loader_init();
    dll_register_shim("ntdll.dll",    ntdll_resolve);
    dll_register_shim("kernel32.dll", kernel32_resolve);
    dll_register_shim("msvcrt.dll",   msvcrt_resolve);
    dll_register_shim("advapi32.dll", advapi32_resolve);
    dll_register_shim("user32.dll",   user32_resolve);
    dll_register_shim("gdi32.dll",    gdi32_resolve);
    dll_register_shim("ddraw.dll",    ddraw_resolve);
    dll_register_shim("dsound.dll",   dsound_resolve);
    dll_register_shim("wsock32.dll",  wsock32_resolve);
    dll_register_shim("ws2_32.dll",   wsock32_resolve);
    dll_register_shim("shell32.dll",  shell32_resolve);
    dll_register_shim("winmm.dll",   winmm_resolve);
    dll_register_shim("ole32.dll",   ole32_resolve);
    dll_register_shim("comctl32.dll", comctl32_resolve);
    dll_register_shim("comdlg32.dll", comdlg32_resolve);

    /*
     * Initialize compat32 thunk pool BEFORE pe_load(), because pe_load()
     * recursively loads DLLs (via dll_resolve_import → dll_load) and those
     * DLLs need the thunk pool to exist for IAT patching.
     */
    compat32_init();

    /*
     * Pre-initialize TEB32/PEB32 and set FS base BEFORE pe_load().
     *
     * pe_load() recursively loads DLLs and calls DllMain via compat32
     * callbacks. DllMain's CRT startup code registers SEH handlers:
     *   push dword ptr fs:[0]   ; save current ExceptionList
     *   mov  fs:[0], esp        ; install new handler
     *
     * If FS_BASE isn't pointing to g_teb32 yet, fs:[0] reads garbage
     * from whatever address FS_BASE points to (0 or stale kernel TLS).
     * That garbage gets saved as the "previous" SEH chain link.
     * Later, SEH unwind restores it: mov fs:[0], eax → ExceptionList
     * becomes the garbage value (e.g. 0x6), corrupting the SEH chain.
     *
     * Fix: set up TEB32 with ExceptionList=0xFFFFFFFF and point
     * FS_BASE to it before any 32-bit code executes.
     */
    {
        uint8_t *p;

        p = (uint8_t *)&g_peb32;
        for (int i = 0; i < (int)sizeof(g_peb32); i++) p[i] = 0;
        g_peb32.BeingDebugged = 0;
        g_peb32.ProcessHeap   = 0xBEEF0001;

        p = (uint8_t *)&g_teb32;
        for (int i = 0; i < (int)sizeof(g_teb32); i++) p[i] = 0;
        g_teb32.Self                    = (uint32_t)(ULONG_PTR)&g_teb32;
        g_teb32.ProcessEnvironmentBlock = (uint32_t)(ULONG_PTR)&g_peb32;
        g_teb32.ClientId_UniqueProcess  = 1;
        g_teb32.ClientId_UniqueThread   = 1;
        g_teb32.LastErrorValue          = 0;
        g_teb32.ExceptionList           = 0xFFFFFFFF; /* empty SEH chain */

        compat32_setup_teb(&g_teb32);

        serial_puts("[WINEXEC] TEB32 pre-initialized, FS base set\n");
    }

    serial_puts("[WINEXEC] loading PE...\n");

    /* Load PE */
    PE_IMAGE_INFO info;
    NTSTATUS status = pe_load(file_data, file_size, &info);

    if (!NT_SUCCESS(status)) {
        serial_puts("[WINEXEC] PE load failed: ");
        serial_puthex((uint64_t)status, 8);
        serial_puts("\n");
        return -1;
    }

    serial_puts("[WINEXEC] PE loaded successfully\n");
    serial_puts("[WINEXEC] subsystem: ");
    serial_puthex(info.Subsystem, 4);
    serial_puts(info.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI
                ? " (console)\n" : " (other)\n");

    /* Set up PEB/TEB */
    setup_environment(info.ImageBase, info.Is32Bit);
    serial_puts("[WINEXEC] PEB/TEB initialized, GS base set\n");

    /* For PE32 (i386): patch EXE IAT and set up FS:TEB */
    if (info.Is32Bit) {
        extern int g_compat32_mode;
        g_compat32_mode = 1;
        serial_puts("[WINEXEC] PE32 (i386) detected — patching EXE IAT\n");

        /* Patch IAT: replace truncated 64-bit ptrs with 32-bit thunk addrs */
        NTSTATUS compat_st = compat32_patch_iat(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] compat32 IAT patch failed\n");
            pe_unload(&info);
            return -1;
        }

        /* Set FS base for 32-bit TEB access (Windows i386 uses FS:0) */
        compat32_setup_teb(&g_teb32);

        serial_puts("[WINEXEC] TEB32.ExceptionList after setup = 0x");
        serial_puthex(g_teb32.ExceptionList, 8);
        serial_puts("\n");
    }

    /* Allocate user stack */
    uint64_t stack_size = info.StackCommit;
    if (stack_size < 65536) stack_size = 65536;  /* minimum 64KB */
    uint64_t stack_pages = (stack_size + 0xFFF) / 4096;
    uint8_t *stack_base = (uint8_t *)mem_alloc_pages(stack_pages);

    if (!stack_base) {
        serial_puts("[WINEXEC] failed to allocate stack\n");
        pe_unload(&info);
        return -1;
    }

    /* Stack grows down — entry RSP/ESP points near top */
    uint8_t *stack_top = stack_base + (stack_pages * 4096) - 64;
    /* Align to 16-byte boundary */
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFULL);

    if (info.Is32Bit) {
        serial_puts("[WINEXEC] TEB32.ExceptionList before EXE entry = 0x");
        serial_puthex(g_teb32.ExceptionList, 8);
        serial_puts("\n");

        /* Arm hardware watchpoint on TEB32.ExceptionList to catch
         * the exact instruction that writes corrupt values (like 0x6) */
        extern void idt_watch_write4(void *addr);
        idt_watch_write4(&g_teb32.ExceptionList);
    }

    serial_puts("[WINEXEC] jumping to entry point at ");
    serial_puthex((uint64_t)info.EntryPoint, 16);
    if (info.Is32Bit) serial_puts(" (32-bit compat mode)");
    serial_puts("\n");

    /*
     * Windows CUI entry point signature:
     *   For EXE: mainCRTStartup(void) — CRT calls main()
     *   For native: NtProcessStartup(PPEB)
     *   For DLL: DllMain(HINSTANCE, DWORD, LPVOID)
     *
     * We call it as a void(void) function. The CRT startup
     * will call GetCommandLineA/W and eventually main().
     * Without a CRT, native PE apps receive PEB in RCX.
     */

    if (info.Is32Bit) {
        /* PE32 (i386): enter 32-bit compatibility mode */
        uint32_t entry32 = (uint32_t)(ULONG_PTR)info.EntryPoint;
        uint32_t sp32    = (uint32_t)(ULONG_PTR)stack_top;
        compat32_enter(entry32, sp32);
    } else {
        /* PE32+ (x86-64): direct 64-bit execution */
        typedef void (*pe_entry_fn)(void);
        typedef void (*pe_native_entry_fn)(PPEB);

        if (info.Subsystem == IMAGE_SUBSYSTEM_NATIVE) {
            pe_native_entry_fn entry = (pe_native_entry_fn)info.EntryPoint;
#ifndef TEST_HARNESS
            __asm__ volatile (
                "mov %0, %%rsp\n"
                "mov %1, %%rcx\n"
                "call *%2\n"
                :
                : "r"(stack_top), "r"(&g_peb), "r"(entry)
                : "memory"
            );
#else
            entry(&g_peb);
#endif
        } else {
            pe_entry_fn entry = (pe_entry_fn)info.EntryPoint;
#ifndef TEST_HARNESS
            __asm__ volatile (
                "mov %0, %%rsp\n"
                "call *%1\n"
                :
                : "r"(stack_top), "r"(entry)
                : "memory"
            );
#else
            (void)stack_top;
            entry();
#endif
        }
    }

    /* If entry point returns (unusual — most call ExitProcess) */
    serial_puts("[WINEXEC] PE entry returned\n");

    /* Cleanup */
    mem_free_pages(stack_base, stack_pages);
    pe_unload(&info);

    return 0;
}
