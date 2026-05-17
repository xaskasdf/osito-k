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

/* EXE ImageBase — used by GetModuleHandleA(NULL) */
uint32_t g_exe_image_base;
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
extern void  serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  proc_exit(int32_t code);

/* ── PE allocator callbacks (used by pe.c) ──────────────────── */

extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)

/* ── PE VA range tracker (detect ImageBase collisions) ───────── */

#define PE_VA_MAX 32
static struct {
    uint64_t base;
    uint64_t size;
} pe_va_ranges[PE_VA_MAX];
static int pe_va_count = 0;

static int pe_va_conflict(uint64_t base, uint64_t size)
{
    uint64_t end = base + size;
    for (int i = 0; i < pe_va_count; i++) {
        uint64_t rend = pe_va_ranges[i].base + pe_va_ranges[i].size;
        if (base < rend && end > pe_va_ranges[i].base)
            return 1;
    }
    return 0;
}

static void pe_va_record(uint64_t base, uint64_t size)
{
    if (pe_va_count < PE_VA_MAX) {
        pe_va_ranges[pe_va_count].base = base;
        pe_va_ranges[pe_va_count].size = size;
        pe_va_count++;
    }
}

PVOID pe_alloc(PVOID preferred, SIZE_T size)
{
    uint64_t pages = (size + 0xFFF) / 4096;
#ifdef TEST_HARNESS
    #include <sys/mman.h>
    if (preferred) {
        void *p = mmap(preferred, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            pe_va_record((uint64_t)p, size);
            return p;
        }
    }
    void *r = mem_alloc_pages(pages);
    if (r) pe_va_record((uint64_t)r, size);
    return r;
#else
    /* Check for VA range conflict before mapping at preferred address */
    if (preferred && pe_va_conflict((uint64_t)preferred, size)) {
        serial_puts("[pe_alloc] CONFLICT: VA 0x");
        serial_puthex((uint64_t)preferred, 8);
        serial_puts(" already occupied, relocating\n");
        preferred = NULL;  /* force relocation */
    }

    void *phys = mem_alloc_pages(pages);
    if (!phys) return NULL;

    if (preferred) {
        /* Map physical pages at the PE's preferred ImageBase.
         * Map in BOTH kernel and Win32 page tables so the PE is
         * visible from both contexts (kernel IAT patching uses
         * kernel CR3, PE code runs under Win32 CR3). */
        extern int paging_win32_map_page(uint64_t, uint64_t, uint64_t);
        uint64_t va = (uint64_t)preferred;
        uint64_t pa = (uint64_t)phys;
        int fail = 0;
        for (uint64_t i = 0; i < pages; i++) {
            int r = paging_map_page(va + i * 4096, pa + i * 4096,
                                    PTE_PRESENT | PTE_WRITABLE);
            if (r != 0) fail++;
            /* Also map in Win32 page table if it exists */
            paging_win32_map_page(va + i * 4096, pa + i * 4096,
                                  PTE_PRESENT | PTE_WRITABLE);
        }
        if (fail) {
            serial_puts("[pe_alloc] WARN: ");
            serial_puthex(fail, 4);
            serial_puts("/");
            serial_puthex(pages, 4);
            serial_puts(" page maps failed for VA 0x");
            serial_puthex(va, 16);
            serial_puts("\n");
            /* Fall back to identity-mapped phys */
            pe_va_record((uint64_t)phys, size);
            return phys;
        }
        serial_puts("[pe_alloc] mapped ");
        serial_puthex(pages, 4);
        serial_puts(" pages VA 0x");
        serial_puthex(va, 8);
        serial_puts(" -> PA 0x");
        serial_puthex(pa, 8);
        serial_puts("\n");
        pe_va_record(va, size);
        /* Return the VA (preferred address). The page table maps VA→PA
         * and a full TLB flush after the 2MB→4KB split ensures the
         * CPU uses the new 4KB PTEs. All pe_memcpy/pe_memset/IAT
         * patching goes through the VA → correct PA. This also means
         * delta = 0 (no relocations needed) since the PE is loaded
         * at its preferred address. */
        return (void *)va;
    }
    /* No preference — return identity-mapped phys addr */
    pe_va_record((uint64_t)phys, size);
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

/* ── Pre-load all DLLs from filesystem ──────────────────────── */
/*
 * Load all .dll files from OsitoFS before the EXE entry point runs.
 * This ensures all native UE1 classes (IMPLEMENT_CLASS) are registered
 * during _initterm, so ProcessRegistrants() finds them all.
 * Without this, dynamic LoadLibrary fails due to FName corruption
 * (VirtualAlloc identity-map recycling bug).
 */
extern void     *osfs2_find(const char *name);
extern int       osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t  osfs2_file_size(void *file);
extern uint32_t  osfs2_file_count(void);
extern void     *osfs2_file_by_index(uint32_t idx);
extern const char *osfs2_file_name(void *file);

static int str_ends_with_dll(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    if (len < 4) return 0;
    char c0 = s[len-4], c1 = s[len-3], c2 = s[len-2], c3 = s[len-1];
    if (c0 >= 'A' && c0 <= 'Z') c0 += 32;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    if (c3 >= 'A' && c3 <= 'Z') c3 += 32;
    return c0 == '.' && c1 == 'd' && c2 == 'l' && c3 == 'l';
}

static void winexec_preload_dlls(void)
{
    uint32_t count = osfs2_file_count();
    int loaded = 0;

    serial_puts("[WINEXEC] pre-loading DLLs from filesystem...\n");

    for (uint32_t i = 0; i < count; i++) {
        void *f = osfs2_file_by_index(i);
        if (!f) continue;
        const char *name = osfs2_file_name(f);
        if (!name || !str_ends_with_dll(name)) continue;

        /* Skip if already loaded (import DLLs or shim DLLs) */
        if (dll_find_module(name)) continue;

        uint64_t fsize = osfs2_file_size(f);
        if (fsize == 0) continue;

        serial_puts("[WINEXEC] preload: ");
        serial_puts(name);
        serial_puts(" (");
        serial_putdec(fsize / 1024);
        serial_puts(" KB)...\n");

        uint64_t pages = (fsize + 0xFFF) / 4096;
        uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
        if (!buf) continue;

        osfs2_read(f, 0, buf, fsize);
        dll_load(name, (const BYTE *)buf, (SIZE_T)fsize);
        serial_puts("[WINEXEC] preload done: ");
        serial_puts(name);
        serial_puts("\n");
        loaded++;
        /* Intentionally leak temp buffer — freeing pages allows
         * mem_alloc_pages to recycle them for VirtualAlloc, which
         * zero-fills, potentially corrupting live PE32 heap data
         * (FName::Names entries, UClass objects, etc.) */
        loaded++;
    }

    serial_puts("[WINEXEC] preloaded ");
    serial_puthex(loaded, 2);
    serial_puts(" DLLs\n");
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

    /* Create Win32 per-process page table (fixes VirtualAlloc aliasing) */
    {
        extern uint64_t paging_create_win32_cr3(void);
        uint64_t w32cr3 = paging_create_win32_cr3();
        /* Store in current process so scheduler restores it on context switch */
        if (w32cr3) {
            extern void *proc_current(void);
            typedef struct { uint32_t pid; uint32_t ppid; uint32_t state;
                             char name[64]; int32_t exit_code; /* ... */ } proc_hdr_t;
            /* cr3 is at a known offset in process_t — use the accessor pattern */
            extern void proc_set_cr3(uint64_t cr3);
            /* TEMPORARILY DISABLED: proc_set_cr3 causes PE32 to run under
             * Win32 CR3 where many identity-mapped buffers aren't visible.
             * Until the shared PDPT fix is verified, keep PE32 under kernel CR3. */
            /* proc_set_cr3(w32cr3); */
            (void)w32cr3;
        }
    }

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

    /* Create base SEH handler thunk (needs thunk pool from compat32_init) */
    {
        extern void crt_install_base_seh_thunk(void);
        crt_install_base_seh_thunk();
    }

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

    g_exe_image_base = (uint32_t)(ULONG_PTR)info.ImageBase;
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

    /* Pre-load all DLLs from filesystem (registers native classes) */
    winexec_preload_dlls();

    /* ── Diagnostic: inspect UE1 GAutoRegister linked list ─────── */
    {
        LOADED_MODULE *core = dll_find_module("Core.dll");
        if (!core) { serial_puts("[DIAG] Core.dll not found!\n"); }
        else {
            PVOID ar_ptr = dll_resolve_export(core,
                "?GAutoRegister@UObject@@0PAV1@A", 0, FALSE);
            PVOID uobj_psc = dll_resolve_export(core,
                "?PrivateStaticClass@UObject@@0VUClass@@A", 0, FALSE);
            /* GetSuperClass: read first few bytes to find SuperField offset */
            PVOID gsc_fn = dll_resolve_export(core,
                "?GetSuperClass@UClass@@QBEPAV1@XZ", 0, FALSE);

            serial_puts("\n[DIAG] === GAutoRegister Inspection ===\n");
            serial_puts("[DIAG] GAutoRegister @");
            serial_puthex((uint64_t)(ULONG_PTR)ar_ptr, 8);
            serial_puts("\n[DIAG] UObject::PrivateStaticClass @");
            serial_puthex((uint64_t)(ULONG_PTR)uobj_psc, 8);
            serial_puts("\n");

            /* Disassemble GetSuperClass to find SuperField offset.
             * Expected: mov eax,[ecx+XX]; ret  →  8B 41 XX C3 */
            int super_offset = -1;
            if (gsc_fn) {
                uint8_t *code = (uint8_t *)gsc_fn;
                serial_puts("[DIAG] GetSuperClass bytes:");
                for (int b = 0; b < 8; b++) {
                    serial_puts(" ");
                    serial_puthex(code[b], 2);
                }
                serial_puts("\n");
                if (code[0] == 0x8B && code[1] == 0x41) {
                    super_offset = (int)(int8_t)code[2];
                } else if (code[0] == 0x8B && code[1] == 0x81) {
                    super_offset = *(int32_t *)(code + 2);
                }
                if (super_offset >= 0) {
                    serial_puts("[DIAG] SuperField offset = +0x");
                    serial_puthex(super_offset, 2);
                    serial_puts("\n");
                }
            }

            if (ar_ptr) {
                uint32_t head = *(uint32_t *)ar_ptr;
                serial_puts("[DIAG] GAutoRegister head = 0x");
                serial_puthex(head, 8);
                serial_puts("\n");

                if (head == 0) {
                    serial_puts("[DIAG] ** NULL — NO classes! **\n");
                } else {
                    /* Discover next offset */
                    uint32_t first_vt = *(uint32_t *)(ULONG_PTR)head;
                    int next_off = -1;
                    for (int off = 4; off <= 32; off += 4) {
                        uint32_t v = *(uint32_t *)((ULONG_PTR)head + off);
                        if (v >= 0x100000 && v < 0x12000000
                            && v != first_vt && v != head) {
                            uint32_t pv = *(uint32_t *)(ULONG_PTR)v;
                            if (pv == first_vt) { next_off = off; break; }
                        }
                    }
                    serial_puts("[DIAG] next_off=+0x");
                    serial_puthex(next_off >= 0 ? next_off : 0xFF, 2);
                    serial_puts("\n");

                    /* Count entries and find UObject's class */
                    uint32_t cur = head;
                    int total = 0;
                    int uobj_idx = -1;
                    uint32_t uobj_addr = uobj_psc
                        ? (uint32_t)(ULONG_PTR)uobj_psc : 0;

                    while (cur && total < 5000) {
                        if (cur == uobj_addr) uobj_idx = total;
                        total++;
                        if (next_off < 0) break;
                        uint32_t n = *(uint32_t *)((ULONG_PTR)cur + next_off);
                        if (n == 0 || n < 0x1000) break;
                        /* Validate: same vtable? */
                        uint32_t nv = *(uint32_t *)(ULONG_PTR)n;
                        if (nv != first_vt) break;
                        cur = n;
                    }

                    serial_puts("[DIAG] total=");
                    serial_putdec(total);
                    serial_puts(" UObject_idx=");
                    if (uobj_idx >= 0) serial_putdec(uobj_idx);
                    else serial_puts("NOT_FOUND");
                    serial_puts("\n");

                    /* Dump UObject's PrivateStaticClass fields */
                    if (uobj_psc) {
                        uint8_t *p = (uint8_t *)uobj_psc;
                        serial_puts("[DIAG] UObject UClass dump:\n");
                        for (int j = 0; j < 64; j += 4) {
                            uint32_t v = *(uint32_t *)(p + j);
                            serial_puts("[DIAG]   +0x");
                            serial_puthex(j, 2);
                            serial_puts(": 0x");
                            serial_puthex(v, 8);
                            if (j == 0) serial_puts(" (vtable)");
                            if (super_offset >= 0 && j == super_offset)
                                serial_puts(" (SuperField)");
                            if (j == 0x1C) serial_puts(" (flags?)");
                            serial_puts("\n");
                        }
                    }

                    /* Dump 3 entries: first, middle, last */
                    int show_idx[] = {0, total/2, total-1};
                    for (int si = 0; si < 3; si++) {
                        int target = show_idx[si];
                        cur = head;
                        for (int k = 0; k < target && next_off >= 0; k++) {
                            uint32_t n = *(uint32_t *)((ULONG_PTR)cur + next_off);
                            if (n == 0 || n < 0x1000) break;
                            cur = n;
                        }
                        serial_puts("[DIAG] entry[");
                        serial_putdec(target);
                        serial_puts("] @0x");
                        serial_puthex(cur, 8);
                        serial_puts(":\n");
                        uint8_t *p = (uint8_t *)(ULONG_PTR)cur;
                        for (int j = 0; j < 48; j += 4) {
                            uint32_t v = *(uint32_t *)(p + j);
                            serial_puts("[DIAG]   +0x");
                            serial_puthex(j, 2);
                            serial_puts(": 0x");
                            serial_puthex(v, 8);
                            if (super_offset >= 0 && j == super_offset)
                                serial_puts(" <SuperField>");
                            serial_puts("\n");
                        }
                    }
                }
            }
            serial_puts("[DIAG] === End ===\n\n");
        }
    }

    /*
     * Pre-allocate GObjRegistrants TArray buffer to prevent realloc data loss.
     *
     * Root cause: FMallocWindows::Realloc during TArray growth fails to
     * preserve existing entries (first 140 entries zeroed after growth from
     * capacity 140→225). The native memcpy (MSVC intrinsic) loses data,
     * possibly due to physical page aliasing between identity-mapped kernel
     * VA and VirtualAlloc-mapped PE VA.
     *
     * Fix: pre-allocate a large buffer for GObjRegistrants before the PE
     * entry point runs. With enough capacity, the TArray never needs to grow.
     */
    {
        LOADED_MODULE *core = dll_find_module("Core.dll");
        if (core) {
            PVOID gobjreg_ptr = dll_resolve_export(core,
                "?GObjRegistrants@UObject@@0V?$TArray@PAVUObject@@@@A", 0, FALSE);
            if (gobjreg_ptr) {
                uint32_t *tarray = (uint32_t *)gobjreg_ptr;
                /* Use a page from the PE image range (already identity-mapped
                 * and accessible from 32-bit compat mode). Allocate 1 page =
                 * 4096 bytes = room for 1024 UObject* entries (4 bytes each). */
                void *buf = mem_alloc_pages(1);
                if (buf) {
                    uint64_t pa = (uint64_t)buf;
                    /* Zero via identity-mapped VA (PA == VA for kernel) */
                    uint8_t *p = (uint8_t *)pa;
                    for (int i = 0; i < 4096; i++) p[i] = 0;

                    /* The buffer is at PA which is identity-mapped as VA=PA.
                     * 32-bit PE code can access it since PA < 4GB. */
                    tarray[0] = (uint32_t)pa;   /* Data pointer */
                    tarray[1] = 0;              /* Num = 0 */
                    tarray[2] = 1024;           /* Max = 1024 entries */

                    serial_puts("[WINEXEC] Pre-allocated GObjRegistrants: Data=0x");
                    serial_puthex(pa, 8);
                    serial_puts(" Max=1024 @TArray=0x");
                    serial_puthex((uint64_t)(ULONG_PTR)gobjreg_ptr, 8);
                    serial_puts("\n");
                }
            }
        }
    }

    /* Allocate user stack — MUST be zeroed.
     * On Windows, stack pages come from VirtualAlloc (MEM_COMMIT) which
     * always zeroes pages. PE32 code relies on this: local variables of
     * class types (FString, TArray) have constructors that expect zeroed
     * memory for their fields (Data=NULL, ArrayNum=0, ArrayMax=0).
     * Without zeroing, destructors read garbage → REP MOVSD 4GB hang. */
    uint64_t stack_size = info.StackReserve;
    if (stack_size < 65536) stack_size = 65536;  /* minimum 64KB */
    if (stack_size > 8ULL * 1024 * 1024) stack_size = 8ULL * 1024 * 1024;  /* cap 8MB */
    uint64_t stack_pages = (stack_size + 0xFFF) / 4096;
    /* Add 1 extra page at the bottom as a guard page */
    uint8_t *stack_base = (uint8_t *)mem_alloc_pages(stack_pages + 1);
    if (stack_base)
        memset(stack_base, 0, (stack_pages + 1) * 4096);

    if (!stack_base) {
        serial_puts("[WINEXEC] failed to allocate stack\n");
        pe_unload(&info);
        return -1;
    }

    /* Guard page: unmap the bottom page so stack overflow faults cleanly
     * instead of silently corrupting adjacent memory. __chkstk probing
     * will hit this guard page and trigger a page fault. */
    extern int paging_set_flags(uint64_t virt, uint64_t flags);
    paging_set_flags((uint64_t)stack_base, 0);  /* remove all flags → not present */

    serial_puts("[WINEXEC] Stack: ");
    serial_putdec(stack_size / 1024);
    serial_puts("KB at 0x");
    extern void serial_puthex(uint64_t val, int digits);
    serial_puthex((uint64_t)stack_base, 8);
    serial_puts(" (guard page at bottom)\n");

    /* Usable stack starts after the guard page */
    uint8_t *stack_usable = stack_base + 4096;
    /* Stack grows down — entry RSP/ESP points near top */
    uint8_t *stack_top = stack_usable + (stack_pages * 4096) - 64;
    /* Align to 16-byte boundary */
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFULL);

    if (info.Is32Bit) {
        serial_puts("[WINEXEC] TEB32.ExceptionList before EXE entry = 0x");
        serial_puthex(g_teb32.ExceptionList, 8);
        serial_puts("\n");

        /* Hardware watchpoint on TEB32 removed — was causing #GP
         * when #DB fires from compat mode during DLL init */
    }

    serial_puts("[WINEXEC] jumping to entry point at ");
    serial_puthex((uint64_t)info.EntryPoint, 16);
    if (info.Is32Bit) serial_puts(" (32-bit compat mode)");
    serial_puts("\n");

    /* Dump UGameEngine class hierarchy BEFORE EXE entry.
     * ConstructObject fails because SuperField may be NULL at this point. */
    {
        volatile uint32_t *ge_cls = (volatile uint32_t *)(uintptr_t)0x105928A0;
        volatile uint32_t *ue_iat = (volatile uint32_t *)(uintptr_t)0x10958D74;
        serial_puts("[DIAG] Before EXE: UGameEngine::SC SuperField=0x");
        serial_puthex(ge_cls[0x28/4], 8);
        serial_puts(" UEngine::SC(IAT)=0x");
        serial_puthex(*ue_iat, 8);
        serial_puts("\n");

        /* Patch INT3 at EXE+0xBC72 (after ConstructObject) and at
         * EXE+0xBC81 (load GEngine into ECX before Init call). */
        uint8_t *bp1 = (uint8_t *)((uintptr_t)info.ImageBase + 0xBC72);
        uint8_t *bp2 = (uint8_t *)((uintptr_t)info.ImageBase + 0xBC81);
        serial_puts("[DIAG] INT3 at 0x");
        serial_puthex((uint64_t)(uintptr_t)bp1, 8);
        serial_puts(" + 0x");
        serial_puthex((uint64_t)(uintptr_t)bp2, 8);
        serial_puts("\n");
        bp1[0] = 0xCC;
        bp2[0] = 0xCC;
    }

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

        /* Install permanent base SEH frame on PE stack.
         * This sits at the bottom of the chain and survives all
         * stack corruption from inner frames (WinDrv.dll bug). */
        {
            extern uint32_t crt_get_base_seh_thunk(void);
            uint32_t handler = crt_get_base_seh_thunk();
            if (handler) {
                uint32_t *sp = (uint32_t *)(uintptr_t)sp32;
                sp -= 3;
                sp[0] = g_teb32.ExceptionList; /* Next = current head */
                sp[1] = handler;               /* Handler = catch-all */
                sp[2] = 0;                     /* Scope/state = 0 */
                g_teb32.ExceptionList = (uint32_t)(uintptr_t)sp;
                extern uint32_t g_base_seh_frame_addr;
                g_base_seh_frame_addr = (uint32_t)(uintptr_t)sp;
                sp32 = (uint32_t)(uintptr_t)sp;
                serial_puts("[WINEXEC] Base SEH frame at 0x");
                serial_puthex((uint32_t)(uintptr_t)sp, 8);
                serial_puts(" handler=0x");
                serial_puthex(handler, 8);
                serial_puts("\n");
            }
        }

        /* Clear GErrorHist and GIsCriticalError before WinMain.
         * DLL _initterm callbacks trigger null-pointer faults (handled by our
         * null-page write handler), which cause the engine's error handler to
         * set GErrorHist="General protection fault!". If GErrorHist is set when
         * the engine tries to Browse() a map, it skips rendering → error exit.
         * Clear both so the engine starts fresh. */
        {
            /* GErrorHist: Core.dll + RVA 0xE3474 (TCHAR[1024], wide string) */
            volatile uint16_t *gerr = (volatile uint16_t *)(uintptr_t)0x101E3474;
            /* GIsCriticalError: Core.dll + RVA 0xE568C (INT, flag) */
            volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
            if (*gerr != 0 || *gcrit != 0) {
                *gerr = 0;   /* Clear error string */
                *gcrit = 0;  /* Clear critical error flag */
                serial_puts("[WINEXEC] Cleared GErrorHist + GIsCriticalError\n");
            }
        }

        /* Set up exec_jmpbuf so proc_exit can longjmp back here.
         * compat32_enter never returns — proc_exit's kern_longjmp is the
         * only way back. Without this, exec_jmpbuf is uninitialized and
         * proc_exit longjmps to garbage → NULL-CALL crash. */
        {
            extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
            extern uint64_t exec_jmpbuf[];
            extern int32_t  last_exit_code;
            if (kern_setjmp(exec_jmpbuf) != 0) {
                /* proc_exit returned here — PE process has exited.
                 *
                 * Two cleanup steps before we let the scheduler see
                 * this kernel context again:
                 *
                 * (1) Restore 64-bit kernel data segments. compat32_enter
                 *     set DS/ES/SS to GDT_SEL_DATA32 (0x48) for the PE
                 *     lifetime. proc_exit longjmped back, restoring
                 *     RIP/RSP but NOT segment selectors. If we don't
                 *     fix them, the next scheduler tick observes PID 1
                 *     with SS=0x48 and bails with "[SCHED] CORRUPT PID 1",
                 *     leaving the shell unschedulable.
                 *
                 * (2) Re-enable the APIC LVT timer. compat32_enter
                 *     masked it for the entire PE lifetime; without
                 *     re-enabling, the preemptive scheduler can't tick
                 *     and `desktop` (which sched_spawns a compositor
                 *     thread) never runs because PID 1 never yields. */
                __asm__ volatile (
                    "mov $0x30, %%ax\n"
                    "mov %%ax, %%ds\n"
                    "mov %%ax, %%es\n"
                    "mov %%ax, %%ss\n"
                    ::: "ax"
                );

                /* Reap any orphan win32 threads spawned by the PE via
                 * CreateThread. Why this is necessary: if we leave
                 * them schedulable, the scheduler will dispatch them
                 * and they'll re-enter compat32_callback_args, which
                 * re-masks the APIC LVT timer (the entire-PE-lifetime
                 * mask from compat32_enter). The callback path
                 * deliberately doesn't unmask on the way out
                 * (compat32.c:1294), so any subsequent kernel
                 * `hlt`-wait — including compositor's
                 * display_wait_vblank — would deadlock forever.
                 *
                 * proc_kill_pid runs the full proc_free cleanup so
                 * the slot is reusable for the next PE invocation,
                 * not left as a permanent ZOMBIE. */
                {
                    typedef struct {
                        int      kernel_pid;
                        uint32_t tid;
                        uint32_t func_addr;
                    } win32_orphan_info_t;
                    extern int proc_kill_pid(int pid);
                    extern int win32_collect_orphan_threads(
                        win32_orphan_info_t *out, int max);

                    win32_orphan_info_t orphans[16];
                    int n = win32_collect_orphan_threads(orphans, 16);
                    for (int i = 0; i < n; i++) {
                        int rc = proc_kill_pid(orphans[i].kernel_pid);
                        serial_puts(rc == 0
                            ? "[winexec] reaped orphan PE thread tid="
                            : "[winexec] FAILED to reap orphan PE thread tid=");
                        serial_putdec((uint64_t)orphans[i].tid);
                        serial_puts(" pid=");
                        serial_putdec((uint64_t)orphans[i].kernel_pid);
                        serial_puts(" entry=0x");
                        serial_puthex(orphans[i].func_addr, 8);
                        serial_puts("\n");
                    }
                }

                {
                    extern volatile uint32_t *idt_get_apic_base(void);
                    volatile uint32_t *apic = idt_get_apic_base();
                    if (apic) apic[0x320/4] &= ~0x10000u;  /* LVT_TIMER &= ~MASKED */
                }
                __asm__ volatile ("sti");
                serial_puts("[WINEXEC] PE process exited, code=");
                serial_putdec((uint32_t)last_exit_code);
                serial_puts(" (SS restored, APIC re-enabled)\n");
                return last_exit_code;
            }
        }

        /* Arm hardware breakpoint at Core.dll+0x22DBB — the instruction
         * just AFTER an inline appSprintf wrapper around _vsnwprintf
         * (the wrapper lives at Core.dll+0x22DA0..0x22DBB and the
         * earlier wdbg stack-scan from the 'Failed to load 0' cascade
         * always shows 0x22DBB as the post-call retaddr).
         *
         * At HWBP fire time, the 4 dwords just pushed for vsnwprintf
         * are still on the stack:
         *   [esp+0]  = buffer (now contains the formatted wide string)
         *   [esp+4]  = count (= 1024)
         *   [esp+8]  = format
         *   [esp+12] = va_list pointer
         *
         * Rendering the wstring at [esp+0] tells us exactly what
         * string Core.dll just produced. When the cascade is
         * formatting the package name "0" / "" / " .GameEngine" via
         * this wrapper, the next 8 fires after Engine.u will show
         * the offending string content. */
        if ((uint32_t)(ULONG_PTR)info.ImageBase == 0x10900000) {
            extern int hwbp_set(int slot, uint64_t addr, int cond, int len,
                                const char *name);
            if (hwbp_set(0, 0x10122dbbULL, /*HWBP_EXECUTE*/0, /*HWBP_LEN_1*/0,
                          "Core-appSprintf-post@22DBB") == 0) {
                serial_puts("[winexec] HWBP slot 0 armed at Core.dll+0x22DBB\n");
            } else {
                serial_puts("[winexec] HWBP slot 0 arm FAILED\n");
            }
            /* Slot 1: WRITE-watch the heap slot where the engine has
             * been writing the wide-string "0" right before the
             * PackageNotFound throw cascade. Every previous run has
             * placed the string at 0x4013F9FC (compat32 heap is
             * deterministic in our layout), so when the writer
             * touches that slot we'll see RIP land in the function
             * that constructs the bad package name. */
            if (hwbp_set(1, 0x4013F9FCULL, /*HWBP_WRITE*/1, /*HWBP_LEN_4*/3,
                          "engine-pkgname-buf") == 0) {
                serial_puts("[winexec] HWBP slot 1 armed at 0x4013F9FC (WRITE)\n");
            }
            /* Slot 2: EXECUTE on StaticFindObject body (Core.dll+0x101570B0).
             * When PE32 calls UObject::StaticFindObject, we dump args:
             *   [esp+4]  = Class*  (UClass to find — UPackage, UClass, etc.)
             *   [esp+8]  = Outer*  (parent UObject, or NULL)
             *   [esp+0xC] = Name   (WCHAR* of object name)
             *   [esp+0x10] = ExactClass (BOOL)
             * Knowing which classes are LOOKED UP and FAIL tells us
             * which UObjects are missing from GObj at the time of the
             * cascade — pinpointing where the package load left gaps. */
            if (hwbp_set(2, 0x101570B0ULL, /*HWBP_EXECUTE*/0, /*HWBP_LEN_1*/0,
                          "StaticFindObject") == 0) {
                serial_puts("[winexec] HWBP slot 2 armed at StaticFindObject\n");
            }
            /* Slot 3: EXECUTE on StaticLoadClass body (Core.dll+0x1015A670).
             * Pairs with slot 2 — together they show the engine's full
             * class-lookup pattern. */
            if (hwbp_set(3, 0x1015A670ULL, /*HWBP_EXECUTE*/0, /*HWBP_LEN_1*/0,
                          "StaticLoadClass") == 0) {
                serial_puts("[winexec] HWBP slot 3 armed at StaticLoadClass\n");
            }
        }

        compat32_enter(entry32, sp32);
        /* never reached — control returns via proc_exit → longjmp above */
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

    /* Restore kernel CR3 */
    {
        extern uint64_t paging_get_kernel_cr3(void);
        __asm__ volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_cr3()) : "memory");
    }

    /* Cleanup */
    mem_free_pages(stack_base, stack_pages);
    pe_unload(&info);

    return 0;
}
