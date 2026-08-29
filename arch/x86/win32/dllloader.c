/*
 * OsitoK Windows Compatibility Layer — DLL Loader Implementation
 *
 * Loads PE DLLs into memory, resolves their exports, manages
 * module tracking, and handles dependency chains.
 */

#include "dllloader.h"
#include "compat32.h"
#include "win32_abi.h"
#include "../include/paging.h"
extern void serial_puts(const char *s);
extern void serial_putchar(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void WINAPI SetLastError(DWORD error);
extern ULONG_PTR win32_current_image_base(void);

static void dump_seh_chain(const char *label)
{
    serial_puts("[SEH-TRACK] ");
    serial_puts(label);
    serial_puts(": TEB32.ExceptionList=0x");
    serial_puthex(compat32_current_teb()->ExceptionList, 8);
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

static void dl_strcpy(char *dst, const char *src, int max)
{
    int i = 0;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int dl_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static BOOL dl_has_prefix(const char *value, const char *prefix)
{
    while (*prefix) {
        if (*value++ != *prefix++) return FALSE;
    }
    return TRUE;
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

#define LIBCEF_PROBE_RVA         0x0355CA10UL
#define LIBCEF_PROBE_FILE_OFFSET 0x0355C010UL
#define LIBCEF_PROBE_SIZE        16UL

static BOOL dl_is_libcef(const char *name)
{
    return name && dl_stricmp(strip_path(name), "libcef.dll") == 0;
}

static void dl_probe_libcef_bytes(const char *stage, const BYTE *bytes,
                                  SIZE_T size, SIZE_T offset)
{
    if (!bytes || offset > size || LIBCEF_PROBE_SIZE > size - offset) {
        serial_puts("[LIBCEF-PROBE] ");
        serial_puts(stage);
        serial_puts(" unavailable\n");
        return;
    }

    serial_puts("[LIBCEF-PROBE] ");
    serial_puts(stage);
    serial_puts(" bytes=");
    for (SIZE_T i = 0; i < LIBCEF_PROBE_SIZE; i++) {
        serial_puthex(bytes[offset + i], 2);
        if (i + 1 < LIBCEF_PROBE_SIZE) serial_putchar(' ');
    }
    serial_puts("\n");
}

/* Return 32 or 64 for a valid x86 PE header, and 0 for malformed/unsupported
 * input. This preflight runs before mapping so a cross-ABI DLL cannot reach
 * relocation, IAT, or TLS setup. */
static int dll_pe_bitness(const BYTE *data, SIZE_T size)
{
    if (!data || size < sizeof(IMAGE_DOS_HEADER)) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 0;

    SIZE_T nt_offset = (SIZE_T)(ULONG)dos->e_lfanew;
    SIZE_T required = sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER) +
                      sizeof(USHORT);
    if (nt_offset > size || required > size - nt_offset) return 0;

    const BYTE *nt = data + nt_offset;
    if (*(const ULONG *)nt != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_FILE_HEADER *file =
        (const IMAGE_FILE_HEADER *)(nt + sizeof(ULONG));
    USHORT magic = *(const USHORT *)(nt + sizeof(ULONG) +
                                     sizeof(IMAGE_FILE_HEADER));
    if (file->Machine == IMAGE_FILE_MACHINE_I386 &&
        magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 32;
    if (file->Machine == IMAGE_FILE_MACHINE_AMD64 &&
        magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 64;
    return 0;
}

int dll_current_process_bitness(void)
{
    ULONG_PTR image_base = win32_current_image_base();
    if (!image_base) return 0;

    /* PE headers are fully copied into the first mapped page. Constrain the
     * parser to that page so a corrupt e_lfanew cannot escape the image. */
    return dll_pe_bitness((const BYTE *)image_base, 4096);
}

/* ── Module table ──────────────────────────────────────────── */

#define DLL_PTE_PRESENT   (1ULL << 0)
#define DLL_PTE_WRITABLE  (1ULL << 1)
#define DLL_PTE_GLOBAL    (1ULL << 8)
#define DLL_PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_set_flags(uint64_t virt, uint64_t flags);
extern uint64_t *paging_get_pte(uint64_t virt);

static BOOL dl_section_is_text(const IMAGE_SECTION_HEADER *section)
{
    static const char name[] = ".text";
    for (int i = 0; i < 5; i++)
        if (section->Name[i] != (BYTE)name[i]) return FALSE;
    return section->Name[5] == 0;
}

/* Catch the first write behind the observed tier0_s64.dll corruption. The
 * loader has already applied relocations, imports, and CFG fixups here, so
 * .text must remain read-only. Protect the PE VA and physical mirror because
 * kernel shims can access either alias. */
static void tier0_arm_text_write_trap(const char *dll_name,
                                      PPE_IMAGE_INFO image)
{
    BYTE *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_FILE_HEADER *file_header;
    IMAGE_SECTION_HEADER *sections;
    uint64_t nt_offset;
    uint64_t section_offset;

    if (!image || image->Is32Bit || !image->ImageBase ||
        (ULONGLONG)(ULONG_PTR)image->ImageBase != image->PreferredBase ||
        dl_stricmp(strip_path(dll_name), "tier0_s64.dll") != 0)
        return;

    base = (BYTE *)image->ImageBase;
    if (image->SizeOfImage < sizeof(IMAGE_DOS_HEADER)) return;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return;

    nt_offset = (uint64_t)(ULONG)dos->e_lfanew;
    if (nt_offset > image->SizeOfImage - sizeof(ULONG) -
                    sizeof(IMAGE_FILE_HEADER) ||
        *(ULONG *)(base + nt_offset) != IMAGE_NT_SIGNATURE)
        return;

    file_header = (IMAGE_FILE_HEADER *)(base + nt_offset + sizeof(ULONG));
    section_offset = nt_offset + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER) +
                     file_header->SizeOfOptionalHeader;
    if (file_header->NumberOfSections > 96 ||
        section_offset > image->SizeOfImage ||
        (uint64_t)file_header->NumberOfSections *
            sizeof(IMAGE_SECTION_HEADER) > image->SizeOfImage - section_offset)
        return;
    sections = (IMAGE_SECTION_HEADER *)(base + section_offset);

    for (USHORT i = 0; i < file_header->NumberOfSections; i++) {
        IMAGE_SECTION_HEADER *section = &sections[i];
        uint64_t section_size;
        uint64_t page_start;
        uint64_t page_end;
        uint64_t protected_pages = 0;
        uint64_t failures = 0;

        if (!dl_section_is_text(section)) continue;
        section_size = section->Misc.VirtualSize;
        if (section->SizeOfRawData > section_size)
            section_size = section->SizeOfRawData;
        if (!section_size || section->VirtualAddress >= image->SizeOfImage ||
            section_size > image->SizeOfImage - section->VirtualAddress)
            return;

        page_start = ((uint64_t)(ULONG_PTR)base + section->VirtualAddress) &
                     ~0xFFFULL;
        page_end = ((uint64_t)(ULONG_PTR)base + section->VirtualAddress +
                    section_size + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t page = page_start; page < page_end; page += 4096) {
            uint64_t *pte = paging_get_pte(page);
            uint64_t phys;
            uint64_t flags;
            if (!pte || !(*pte & DLL_PTE_PRESENT)) {
                failures++;
                continue;
            }
            phys = *pte & DLL_PTE_ADDR_MASK;
            flags = (*pte & ~DLL_PTE_ADDR_MASK) & ~DLL_PTE_WRITABLE;
            if (paging_set_flags(page, flags) != 0)
                failures++;
            if (paging_map_page(KERNEL_VBASE + phys, phys,
                                DLL_PTE_PRESENT | DLL_PTE_GLOBAL) != 0)
                failures++;
            protected_pages++;
        }

        serial_puts("[TIER0-WRITE-TRAP] .text base=0x");
        serial_puthex(page_start, 16);
        serial_puts(" size=0x");
        serial_puthex(page_end - page_start, 16);
        serial_puts(" pages=");
        serial_putdec(protected_pages);
        serial_puts(" failures=");
        serial_putdec(failures);
        serial_puts("\n");
        return;
    }
}

static LOADED_MODULE modules[MAX_LOADED_MODULES];

#define SHIM64_THUNK_RVA         0x1000UL
#define SHIM64_THUNK_PATCH_SIZE  16UL
#define SHIM64_THUNK_SIZE        32UL
#define SHIM64_THUNK_MAX         1024
_Static_assert(SHIM64_THUNK_RVA +
                   SHIM64_THUNK_SIZE * SHIM64_THUNK_MAX <=
               DLL_SYNTHETIC_SHIM_IMAGE_SIZE,
               "Win64 shim thunks must fit in the synthetic image");
static USHORT shim64_thunk_count[MAX_LOADED_MODULES];
static ULONG_PTR shim64_thunk_targets[MAX_LOADED_MODULES][SHIM64_THUNK_MAX];
static ULONG next_init_order = 1;
static volatile uint64_t export_hint_hits;
static volatile uint64_t export_binary_hits;
static volatile uint64_t export_ordinal_hits;
static volatile uint64_t export_lookup_misses;
static volatile uint64_t export_name_comparisons;

/* Windows serializes image mapping and DllMain per process, not system-wide.
 * CEF starts several helper processes concurrently, so a single global lock
 * needlessly turned independent DLL loads into one long queue. The same
 * process state also owns its PEB-like loaded-module list and recursion depth. */
typedef struct {
    volatile uint64_t identity; /* owner PID + 1; bit 63 means reserving */
    volatile int held;          /* 0=free, 1=held, 2=stale-owner recovery */
    int kernel_pid;
    unsigned depth;
    volatile int module_head;
    unsigned load_depth;
    char load_directory[260];
    DWORD default_search_flags;
    int user_directory_head;
    BOOL current_directory_disabled;
    char search_directory[260];
} DLL_PROCESS_STATE;

#define DLL_USER_DIRECTORY_CAP 128
#define DLL_USER_DIRECTORY_RESERVED 0xFFFFFFFFU

typedef struct {
    volatile ULONG owner_key; /* Win32 PID + 1, or RESERVED while publishing */
    ULONG generation;
    int next;
    char path[260];
} DLL_USER_DIRECTORY;

#define DLL_PROCESS_STATE_RESERVED (1ULL << 63)
_Static_assert((MAX_LOADED_MODULES & (MAX_LOADED_MODULES - 1)) == 0,
               "loader process table must be a power of two");
static DLL_PROCESS_STATE loader_processes[MAX_LOADED_MODULES];
static DLL_PROCESS_STATE loader_overflow;
static DLL_USER_DIRECTORY loader_user_directories[DLL_USER_DIRECTORY_CAP];

extern DWORD win32_current_process_id(void);
extern int32_t proc_current_pid(void);
extern void *proc_find_ptr(uint16_t pid);
extern void sched_yield(void);

static ULONG dll_current_owner_pid(void)
{
    return win32_current_process_id();
}

static DLL_PROCESS_STATE *loader_process_state(ULONG owner_pid, BOOL create)
{
    uint64_t key = (uint64_t)owner_pid + 1;
    uint64_t reserving = key | DLL_PROCESS_STATE_RESERVED;
    uint32_t start = (owner_pid * 2654435761U) &
                     (MAX_LOADED_MODULES - 1);

restart:
    for (uint32_t probe = 0; probe < MAX_LOADED_MODULES; probe++) {
        DLL_PROCESS_STATE *state =
            &loader_processes[(start + probe) & (MAX_LOADED_MODULES - 1)];
        uint64_t identity =
            __atomic_load_n(&state->identity, __ATOMIC_ACQUIRE);
        if (identity == key) return state;
        if (identity == reserving) {
            sched_yield();
            goto restart;
        }
        if (!identity && create) {
            uint64_t expected = 0;
            if (!__atomic_compare_exchange_n(
                    &state->identity, &expected, reserving, FALSE,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            __atomic_store_n(&state->held, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&state->kernel_pid, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->depth, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&state->module_head, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->load_depth, 0, __ATOMIC_RELAXED);
            state->load_directory[0] = 0;
            state->default_search_flags = 0;
            state->user_directory_head = -1;
            state->current_directory_disabled = FALSE;
            state->search_directory[0] = 0;
            __atomic_store_n(&state->identity, key, __ATOMIC_RELEASE);
            return state;
        }
    }
    return NULL;
}

static DLL_PROCESS_STATE *loader_lock_state(ULONG owner_pid, BOOL create)
{
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, create);
    return state ? state : &loader_overflow;
}

static void loader_lock_reset(DLL_PROCESS_STATE *state)
{
    __atomic_store_n(&state->depth, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->kernel_pid, -1, __ATOMIC_RELAXED);
}

/* A Win32 task can be reaped after a fault or forced termination while it
 * owns a process loader lock. Claim state 2 before clearing a stale owner so
 * two waiters cannot both recover the same lock and race a new acquisition. */
static void loader_lock_recover_stale(DLL_PROCESS_STATE *state,
                                      ULONG owner_pid)
{
    int owner_kernel_pid =
        __atomic_load_n(&state->kernel_pid, __ATOMIC_ACQUIRE);
    if (owner_kernel_pid <= 0 || owner_kernel_pid > 0xFFFF ||
        proc_find_ptr((uint16_t)owner_kernel_pid))
        return;

    int expected = 1;
    if (!__atomic_compare_exchange_n(&state->held, &expected, 2, FALSE,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    if (__atomic_load_n(&state->kernel_pid, __ATOMIC_ACQUIRE) !=
            owner_kernel_pid ||
        proc_find_ptr((uint16_t)owner_kernel_pid)) {
        __atomic_store_n(&state->held, 1, __ATOMIC_RELEASE);
        return;
    }

    loader_lock_reset(state);
    __atomic_store_n(&state->held, 0, __ATOMIC_RELEASE);

    serial_puts("[DLL-LOCK] recovered stale owner kpid=");
    serial_putdec((uint64_t)(uint32_t)owner_kernel_pid);
    serial_puts(" pid=");
    serial_putdec(owner_pid);
    serial_puts("\n");
}

static void loader_lock_acquire_for(ULONG owner_pid)
{
    DLL_PROCESS_STATE *state = loader_lock_state(owner_pid, TRUE);
    int kernel_pid = proc_current_pid();
    if (__atomic_load_n(&state->held, __ATOMIC_ACQUIRE) == 1 &&
        __atomic_load_n(&state->kernel_pid, __ATOMIC_RELAXED) ==
            kernel_pid) {
        __atomic_add_fetch(&state->depth, 1, __ATOMIC_RELAXED);
        return;
    }

    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&state->held, &expected, 1,
                                        FALSE, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
        if (expected == 1)
            loader_lock_recover_stale(state, owner_pid);
        sched_yield();
    }

    __atomic_store_n(&state->kernel_pid, kernel_pid, __ATOMIC_RELAXED);
    __atomic_store_n(&state->depth, 1, __ATOMIC_RELEASE);
}

static void loader_lock_acquire(void)
{
    loader_lock_acquire_for(dll_current_owner_pid());
}

static void loader_lock_release_for(ULONG owner_pid)
{
    DLL_PROCESS_STATE *state = loader_lock_state(owner_pid, FALSE);
    unsigned depth = __atomic_load_n(&state->depth, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&state->held, __ATOMIC_ACQUIRE) != 1 || !depth ||
        __atomic_load_n(&state->kernel_pid, __ATOMIC_RELAXED) !=
            proc_current_pid())
        return;
    if (depth > 1) {
        __atomic_store_n(&state->depth, depth - 1, __ATOMIC_RELAXED);
        return;
    }

    loader_lock_reset(state);
    __atomic_store_n(&state->held, 0, __ATOMIC_RELEASE);
}

static void loader_lock_release(void)
{
    loader_lock_release_for(dll_current_owner_pid());
}

static void loader_lock_abandon_process(ULONG owner_pid)
{
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (!state || __atomic_load_n(&state->held, __ATOMIC_ACQUIRE) != 1)
        return;

    int expected = 1;
    if (!__atomic_compare_exchange_n(&state->held, &expected, 2, FALSE,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    loader_lock_reset(state);
    __atomic_store_n(&state->held, 0, __ATOMIC_RELEASE);
}

static void loader_process_forget(ULONG owner_pid)
{
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (!state || __atomic_load_n(&state->held, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&state->module_head, __ATOMIC_ACQUIRE) >= 0)
        return;
    __atomic_store_n(&state->identity, 0, __ATOMIC_RELEASE);
}

BOOL dll_set_default_search_flags(DWORD flags)
{
    loader_lock_acquire();
    DLL_PROCESS_STATE *state = loader_process_state(
        dll_current_owner_pid(), TRUE);
    if (state) state->default_search_flags = flags;
    loader_lock_release();
    return state != NULL;
}

BOOL dll_set_search_directory(const char *normalized_path,
                              BOOL disable_current_directory)
{
    loader_lock_acquire();
    DLL_PROCESS_STATE *state = loader_process_state(
        dll_current_owner_pid(), TRUE);
    if (!state) {
        loader_lock_release();
        return FALSE;
    }

    SIZE_T length = 0;
    if (normalized_path) {
        while (normalized_path[length] && length < 260) length++;
        if (length >= 260) {
            loader_lock_release();
            return FALSE;
        }
        for (SIZE_T i = 0; i <= length; i++)
            state->search_directory[i] = normalized_path[i];
    } else {
        state->search_directory[0] = 0;
    }
    state->current_directory_disabled = disable_current_directory;
    loader_lock_release();
    return TRUE;
}

PVOID dll_add_search_directory(const char *normalized_path)
{
    if (!normalized_path || !normalized_path[0]) return NULL;

    SIZE_T length = 0;
    while (normalized_path[length] && length < 260) length++;
    if (length >= 260) return NULL;

    loader_lock_acquire();
    ULONG owner_pid = dll_current_owner_pid();
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, TRUE);
    if (!state) {
        loader_lock_release();
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < DLL_USER_DIRECTORY_CAP; i++) {
        ULONG expected = 0;
        if (__atomic_compare_exchange_n(
                &loader_user_directories[i].owner_key, &expected,
                DLL_USER_DIRECTORY_RESERVED, FALSE, __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        loader_lock_release();
        return NULL;
    }

    DLL_USER_DIRECTORY *entry = &loader_user_directories[slot];
    ULONG generation = (entry->generation + 1U) & 0x00FFFFFFU;
    if (!generation) generation = 1;
    entry->generation = generation;
    entry->next = state->user_directory_head;
    for (SIZE_T i = 0; i <= length; i++) entry->path[i] = normalized_path[i];
    __atomic_store_n(&entry->owner_key, owner_pid + 1U, __ATOMIC_RELEASE);
    state->user_directory_head = slot;

    ULONG_PTR cookie = ((ULONG_PTR)generation << 8) | (ULONG_PTR)(slot + 1);
    loader_lock_release();
    return (PVOID)cookie;
}

BOOL dll_remove_search_directory(PVOID cookie_value)
{
    ULONG_PTR cookie = (ULONG_PTR)cookie_value;
    unsigned encoded_slot = (unsigned)(cookie & 0xFFU);
    ULONG generation = (ULONG)(cookie >> 8);
    if (!encoded_slot || encoded_slot > DLL_USER_DIRECTORY_CAP ||
        !generation)
        return FALSE;

    int slot = (int)encoded_slot - 1;
    loader_lock_acquire();
    ULONG owner_pid = dll_current_owner_pid();
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    DLL_USER_DIRECTORY *entry = &loader_user_directories[slot];
    if (!state ||
        __atomic_load_n(&entry->owner_key, __ATOMIC_ACQUIRE) != owner_pid + 1U ||
        entry->generation != generation) {
        loader_lock_release();
        return FALSE;
    }

    int *link = &state->user_directory_head;
    int visited = 0;
    while (*link >= 0 && *link < DLL_USER_DIRECTORY_CAP &&
           visited++ < DLL_USER_DIRECTORY_CAP) {
        if (*link == slot) {
            *link = entry->next;
            entry->next = -1;
            entry->path[0] = 0;
            __atomic_store_n(&entry->owner_key, 0, __ATOMIC_RELEASE);
            loader_lock_release();
            return TRUE;
        }
        link = &loader_user_directories[*link].next;
    }

    loader_lock_release();
    return FALSE;
}

static void dll_release_search_state_locked(DLL_PROCESS_STATE *state,
                                             ULONG owner_pid)
{
    if (!state) return;

    int slot = state->user_directory_head;
    state->user_directory_head = -1;
    for (int visited = 0;
         slot >= 0 && slot < DLL_USER_DIRECTORY_CAP &&
         visited < DLL_USER_DIRECTORY_CAP;
         visited++) {
        DLL_USER_DIRECTORY *entry = &loader_user_directories[slot];
        int next = entry->next;
        if (__atomic_load_n(&entry->owner_key, __ATOMIC_ACQUIRE) ==
            owner_pid + 1U) {
            entry->next = -1;
            entry->path[0] = 0;
            __atomic_store_n(&entry->owner_key, 0, __ATOMIC_RELEASE);
        }
        slot = next;
    }
    state->default_search_flags = 0;
    state->current_directory_disabled = FALSE;
    state->search_directory[0] = 0;
    state->load_directory[0] = 0;
}

static BOOL module_visible(const LOADED_MODULE *mod, ULONG owner_pid)
{
    return __atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) > 0 &&
           mod->owner_pid == owner_pid;
}

static BOOL module_matches_name(const LOADED_MODULE *mod,
                                const char *lower_name)
{
    if (dl_stricmp(lower_name, mod->name) == 0)
        return TRUE;

    char requested_noext[64];
    char module_noext[64];
    dl_strcpy_lower(requested_noext, lower_name, sizeof(requested_noext));
    dl_strcpy_lower(module_noext, mod->name, sizeof(module_noext));

    char *names[2] = { requested_noext, module_noext };
    for (int i = 0; i < 2; i++) {
        int len = 0;
        while (names[i][len]) len++;
        if (len > 4 && names[i][len - 4] == '.' &&
            names[i][len - 3] == 'd' && names[i][len - 2] == 'l' &&
            names[i][len - 1] == 'l')
            names[i][len - 4] = 0;
    }

    return dl_stricmp(requested_noext, module_noext) == 0;
}

static BOOL dll_caller_is_32bit(void)
{
    int process_bits = dll_current_process_bitness();
    return process_bits
        ? process_bits == 32
        : (g_compat32_mode ? TRUE : FALSE);
}

static BOOL module_matches_caller_abi(const LOADED_MODULE *mod)
{
    return mod->image.Is32Bit == dll_caller_is_32bit();
}

static LOADED_MODULE *module_reserve(const char *dll_name, ULONG owner_pid)
{
    DLL_PROCESS_STATE *process_state =
        loader_process_state(owner_pid, TRUE);
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        LOADED_MODULE *mod = &modules[i];
        int expected = 0;
        if (!__atomic_compare_exchange_n(&mod->state, &expected, -1, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        uint8_t *image = (uint8_t *)&mod->image;
        for (unsigned j = 0; j < sizeof(mod->image); j++) image[j] = 0;
        mod->owner_pid = owner_pid;
        mod->next_owner_index = process_state
            ? __atomic_load_n(&process_state->module_head, __ATOMIC_RELAXED)
            : -1;
        dl_strcpy_lower(mod->name, strip_path(dll_name), 64);
        dl_strcpy(mod->path, dll_name, sizeof(mod->path));
        mod->ref_count = 1;
        mod->synthetic_shim = FALSE;
        mod->initialized = FALSE;
        mod->thread_notifications_disabled = FALSE;
        mod->init_order = 0;
        mod->dll_main = NULL;
        shim64_thunk_count[i] = 0;
        if (process_state)
            __atomic_store_n(&process_state->module_head, i,
                             __ATOMIC_RELEASE);
        __atomic_store_n(&mod->state, 1, __ATOMIC_RELEASE);
        return mod;
    }

    int active = 0;
    int owned = 0;
    int synthetic = 0;
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (__atomic_load_n(&modules[i].state, __ATOMIC_ACQUIRE) <= 0)
            continue;
        active++;
        if (modules[i].owner_pid == owner_pid) owned++;
        if (modules[i].synthetic_shim) synthetic++;
    }
    serial_puts("[DLL-CAPACITY] full active=");
    serial_putdec((uint64_t)active);
    serial_puts(" owner=");
    serial_putdec((uint64_t)owner_pid);
    serial_puts(" owner_active=");
    serial_putdec((uint64_t)owned);
    serial_puts(" synthetic=");
    serial_putdec((uint64_t)synthetic);
    serial_puts(" requested=");
    serial_puts(dll_name ? dll_name : "<null>");
    serial_puts("\n");
    return NULL;
}

static void module_unlink_record(LOADED_MODULE *mod)
{
    ULONG owner_pid = mod->owner_pid;
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (!state) return;

    int target = (int)(mod - modules);
    int previous = -1;
    int current =
        __atomic_load_n(&state->module_head, __ATOMIC_ACQUIRE);
    for (int visited = 0;
         current >= 0 && current < MAX_LOADED_MODULES &&
         visited < MAX_LOADED_MODULES;
         visited++) {
        int next = modules[current].next_owner_index;
        if (current == target) {
            if (previous < 0)
                __atomic_store_n(&state->module_head, next,
                                 __ATOMIC_RELEASE);
            else
                modules[previous].next_owner_index = next;
            return;
        }
        previous = current;
        current = next;
    }
}

static void module_release_record(LOADED_MODULE *mod)
{
    if (!mod || __atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) <= 0)
        return;
    __atomic_store_n(&mod->state, -1, __ATOMIC_RELEASE);
    module_unlink_record(mod);
    mod->owner_pid = 0;
    mod->next_owner_index = -1;
    mod->name[0] = 0;
    mod->path[0] = 0;
    mod->dll_main = NULL;
    mod->synthetic_shim = FALSE;
    mod->initialized = FALSE;
    mod->thread_notifications_disabled = FALSE;
    mod->init_order = 0;
    mod->ref_count = 0;
    shim64_thunk_count[mod - modules] = 0;
    uint8_t *image = (uint8_t *)&mod->image;
    for (unsigned j = 0; j < sizeof(mod->image); j++) image[j] = 0;
    __atomic_store_n(&mod->state, 0, __ATOMIC_RELEASE);
}

static int ptr_is_canonical(const void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    return x < 0x0000800000000000ULL || x >= 0xFFFF800000000000ULL;
}

static const char *strip_path_bounded(const char *name, int max)
{
    const char *last = name;

    if (!name || !ptr_is_canonical(name))
        return NULL;

    for (int i = 0; i < max && name[i]; i++) {
        if (name[i] == '\\' || name[i] == '/')
            last = name + i + 1;
    }

    return last;
}

static int module_name_valid(const char *name)
{
    for (int i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0)
            return i > 0;
        if (c < 0x20 || c > 0x7e)
            return 0;
    }
    return 0;
}

static int sane_module_count(void)
{
    static int warned_bad_count;

    if (module_count < 0 || module_count > MAX_LOADED_MODULES) {
        if (!warned_bad_count) {
            warned_bad_count = 1;
            serial_puts("[DLL] corrupt module_count=");
            serial_puthex((uint64_t)(uint32_t)module_count, 8);
            serial_puts(" clamped\n");
        }
        if (module_count < 0)
            module_count = 0;
        else
            module_count = MAX_LOADED_MODULES;
    }

    return module_count;
}

/* ── Shim registry (built-in DLL shims) ────────────────────── */

#define MAX_SHIMS 64

typedef struct {
    char            name[64];
    shim_resolver_fn resolver;
} SHIM_ENTRY;

static SHIM_ENTRY shims[MAX_SHIMS];
static int shim_count = 0;

void dll_register_shim(const char *dll_name, shim_resolver_fn resolver)
{
    if (shim_count >= MAX_SHIMS) {
        serial_puts("[DLL] shim registry full: ");
        serial_puts(dll_name);
        serial_puts("\n");
        return;
    }
    dl_strcpy_lower(shims[shim_count].name, strip_path(dll_name), 64);
    shims[shim_count].resolver = resolver;
    shim_count++;
}

/* ── Find a shim by DLL name ───────────────────────────────── */

static const char *dll_shim_provider_name(const char *dll_name,
                                          char provider[64])
{
    if (!dll_name) {
        provider[0] = 0;
        return provider;
    }
    dl_strcpy_lower(provider, strip_path(dll_name), 64);
    if (dl_has_prefix(provider, "api-ms-win-core-"))
        dl_strcpy_lower(provider, "kernel32.dll", 64);
    else if (dl_has_prefix(provider, "api-ms-win-crt-") ||
             dl_has_prefix(provider, "ext-ms-win-crt-"))
        dl_strcpy_lower(provider, "ucrtbase.dll", 64);
    return provider;
}

shim_resolver_fn find_shim(const char *dll_name)
{
    char lower[64];
    dll_shim_provider_name(dll_name, lower);

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

BOOL dll_is_shim(const char *dll_name)
{
    return dll_name && find_shim(dll_name) != NULL;
}

PVOID dll_resolve_shim_export(const char *dll_name, const char *func_name,
                              USHORT ordinal, BOOL by_ordinal)
{
    shim_resolver_fn resolver = dll_name ? find_shim(dll_name) : NULL;
    return resolver ? resolver(func_name, ordinal, by_ordinal) : NULL;
}

void dll_loader_init(void)
{
    shim_count = 0;
    next_init_order = 1;
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        loader_processes[i].identity = 0;
        loader_processes[i].held = 0;
        loader_processes[i].kernel_pid = -1;
        loader_processes[i].depth = 0;
        loader_processes[i].module_head = -1;
        loader_processes[i].load_depth = 0;
        loader_processes[i].load_directory[0] = 0;
        loader_processes[i].default_search_flags = 0;
        loader_processes[i].user_directory_head = -1;
        loader_processes[i].current_directory_disabled = FALSE;
        loader_processes[i].search_directory[0] = 0;
    }
    loader_overflow.identity = 0;
    loader_overflow.held = 0;
    loader_overflow.kernel_pid = -1;
    loader_overflow.depth = 0;
    loader_overflow.module_head = -1;
    loader_overflow.load_depth = 0;
    loader_overflow.load_directory[0] = 0;
    loader_overflow.default_search_flags = 0;
    loader_overflow.user_directory_head = -1;
    loader_overflow.current_directory_disabled = FALSE;
    loader_overflow.search_directory[0] = 0;
    for (int i = 0; i < DLL_USER_DIRECTORY_CAP; i++) {
        loader_user_directories[i].owner_key = 0;
        loader_user_directories[i].generation = 0;
        loader_user_directories[i].next = -1;
        loader_user_directories[i].path[0] = 0;
    }
    /* Re-exec: scrub stale module records (base addrs / export tables of the
     * previous run) so nothing can resolve against a dead image. */
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        uint8_t *p = (uint8_t *)&modules[i];
        for (unsigned j = 0; j < sizeof(modules[i]); j++) p[j] = 0;
        shim64_thunk_count[i] = 0;
    }

    /* Built-in shims are registered by winexec.c after calling this */
}

/* ── Find loaded module ────────────────────────────────────── */

LOADED_MODULE *dll_find_module(const char *dll_name)
{
    ULONG owner_pid = dll_current_owner_pid();
    char lower[64];
    const char *base = strip_path_bounded(dll_name, 256);
    int count = sane_module_count();
    static int warned_bad_name;

    if (!base) {
        if (!warned_bad_name) {
            warned_bad_name = 1;
            serial_puts("[DLL] bad dll name ptr: 0x");
            serial_puthex((uint64_t)(uintptr_t)dll_name, 16);
            serial_puts("\n");
        }
        return NULL;
    }

    dl_strcpy_lower(lower, base, 64);

    for (int i = 0; i < count; i++) {
        if (!module_name_valid(modules[i].name))
            continue;
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

    for (int i = 0; i < count; i++) {
        if (!module_name_valid(modules[i].name))
            continue;
        char mod_noext[64];
        dl_strcpy_lower(mod_noext, modules[i].name, 64);
        int mlen = 0;
        while (mod_noext[mlen]) mlen++;
        if (mlen > 4 && mod_noext[mlen-4] == '.' &&
            mod_noext[mlen-3] == 'd' && mod_noext[mlen-2] == 'l' &&
            mod_noext[mlen-1] == 'l') {
            mod_noext[mlen-4] = 0;
        }
        return NULL;
    }

    /* Bootstrap/overflow fallback. Normal Win32 processes have a state and
     * only walk their short loaded-module list. */
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (module_visible(&modules[i], owner_pid) &&
            module_matches_name(&modules[i], lower) &&
            module_matches_caller_abi(&modules[i]))
            return &modules[i];
    }

    return NULL;
}

LOADED_MODULE *dll_find_module_by_base(PVOID image_base)
{
    ULONG owner_pid = dll_current_owner_pid();
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (state) {
        int current =
            __atomic_load_n(&state->module_head, __ATOMIC_ACQUIRE);
        for (int visited = 0;
             current >= 0 && current < MAX_LOADED_MODULES &&
             visited < MAX_LOADED_MODULES;
             visited++) {
            LOADED_MODULE *mod = &modules[current];
            int next = mod->next_owner_index;
            if (module_visible(mod, owner_pid) &&
                mod->image.ImageBase == image_base)
                return mod;
            current = next;
        }
        return NULL;
    }
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (module_visible(&modules[i], owner_pid) &&
            modules[i].image.ImageBase == image_base)
            return &modules[i];
    }
    return NULL;
}

LOADED_MODULE *dll_find_module_by_address(PVOID address)
{
    ULONG owner_pid = dll_current_owner_pid();
    ULONG_PTR value = (ULONG_PTR)address;
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (state) {
        int current =
            __atomic_load_n(&state->module_head, __ATOMIC_ACQUIRE);
        for (int visited = 0;
             current >= 0 && current < MAX_LOADED_MODULES &&
             visited < MAX_LOADED_MODULES;
             visited++) {
            LOADED_MODULE *mod = &modules[current];
            int next = mod->next_owner_index;
            ULONG_PTR base = (ULONG_PTR)mod->image.ImageBase;
            if (module_visible(mod, owner_pid) && value >= base &&
                value - base < mod->image.SizeOfImage)
                return mod;
            current = next;
        }
        return NULL;
    }
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (!module_visible(&modules[i], owner_pid)) continue;
        ULONG_PTR base = (ULONG_PTR)modules[i].image.ImageBase;
        if (value >= base && value - base < modules[i].image.SizeOfImage)
            return &modules[i];
    }
    return NULL;
}

void dll_debug_log_address(PVOID address)
{
    ULONG_PTR value = (ULONG_PTR)address;
    LOADED_MODULE *mod = dll_find_module_by_address(address);

    serial_puts("[PE64-ADDR] addr=0x");
    serial_puthex(value, 16);
    if (!mod) {
        serial_puts(" module=<none>\n");
        return;
    }

    ULONG_PTR base = (ULONG_PTR)mod->image.ImageBase;
    serial_puts(" module=");
    serial_puts(mod->name);
    serial_puts(" base=0x");
    serial_puthex(base, 16);
    serial_puts(" rva=0x");
    serial_puthex(value - base, 16);
    serial_puts(" size=0x");
    serial_puthex(mod->image.SizeOfImage, 8);
    serial_puts(" owner=");
    serial_putdec(mod->owner_pid);
    serial_puts("\n");
}

static BOOL dll_debug_read(ULONG_PTR address, void *dst, SIZE_T size)
{
    uint64_t cr3;
    BYTE *out = (BYTE *)dst;

    if (!address || !dst || address + size < address)
        return FALSE;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    while (size) {
        uint64_t phys = paging_translate_in_cr3(cr3, address);
        if (phys == UINT64_MAX)
            return FALSE;

        SIZE_T chunk = 0x1000 - (SIZE_T)(phys & 0xFFF);
        if (chunk > size)
            chunk = size;

        const BYTE *src = (const BYTE *)PHYS_TO_VIRT(phys);
        for (SIZE_T i = 0; i < chunk; i++)
            out[i] = src[i];

        address += chunk;
        out += chunk;
        size -= chunk;
    }
    return TRUE;
}

static void dll_debug_put_string(ULONG_PTR address)
{
    if (!address) {
        serial_puts("<null>");
        return;
    }

    for (SIZE_T i = 0; i < 256; i++) {
        char c;
        if (!dll_debug_read(address + i, &c, 1)) {
            serial_puts("<unmapped>");
            return;
        }
        if (!c)
            return;
        serial_putchar(c);
    }
    serial_puts("<truncated>");
}

void dll_debug_log_delay_failure(PVOID address, PVOID info_ptr)
{
    LOADED_MODULE *mod = dll_find_module_by_address(address);
    if (!mod || dl_stricmp(mod->name, "libcef.dll") != 0)
        return;

    ULONG_PTR rva = (ULONG_PTR)address - (ULONG_PTR)mod->image.ImageBase;
    if (rva < 0x59EF805 || rva > 0x59EF809)
        return;

    ULONG_PTR info_addr = (ULONG_PTR)info_ptr;
    BYTE info[0x48];
    if (!dll_debug_read(info_addr, info, sizeof(info))) {
        serial_puts("[PE64-DELAY] invalid info=0x");
        serial_puthex(info_addr, 16);
        serial_puts("\n");
        return;
    }

    ULONG_PTR dll_name = *(ULONG_PTR *)(info + 0x18);
    DWORD by_name = *(DWORD *)(info + 0x20);
    ULONG_PTR proc = *(ULONG_PTR *)(info + 0x28);
    ULONG_PTR iat_slot = *(ULONG_PTR *)(info + 0x10);
    ULONG_PTR module = *(ULONG_PTR *)(info + 0x30);
    ULONG_PTR resolved = *(ULONG_PTR *)(info + 0x38);
    DWORD error = *(DWORD *)(info + 0x40);
    ULONG_PTR iat_value = 0;
    if (iat_slot)
        dll_debug_read(iat_slot, &iat_value, sizeof(iat_value));

    serial_puts("[PE64-DELAY] dll='");
    dll_debug_put_string(dll_name);
    serial_puts("' ");
    if (by_name) {
        serial_puts("symbol='");
        dll_debug_put_string(proc);
        serial_puts("'");
    } else {
        serial_puts("ordinal=");
        serial_putdec(proc);
    }
    serial_puts(" iat=0x");
    serial_puthex(iat_slot, 16);
    serial_puts(" value=0x");
    serial_puthex(iat_value, 16);
    serial_puts(" hmod=0x");
    serial_puthex(module, 16);
    serial_puts(" pfn=0x");
    serial_puthex(resolved, 16);
    serial_puts(" error=");
    serial_putdec(error);
    serial_puts("\n");
}

static void dl_zero_bytes(void *address, SIZE_T size)
{
    BYTE *bytes = (BYTE *)address;
    for (SIZE_T i = 0; i < size; i++) bytes[i] = 0;
}

static void dl_init_synthetic_section(PIMAGE_SECTION_HEADER section)
{
    static const char name[] = ".text";
    for (int i = 0; i < 6; i++) section->Name[i] = (BYTE)name[i];
    section->Misc.VirtualSize = DLL_SYNTHETIC_SHIM_IMAGE_SIZE -
                                SHIM64_THUNK_RVA;
    section->VirtualAddress = SHIM64_THUNK_RVA;
    section->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                               IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
}

static void dl_build_synthetic_shim_image(PVOID image_base, BOOL is_32bit,
                                           PPE_IMAGE_INFO info)
{
    BYTE *base = (BYTE *)image_base;
    PIMAGE_DOS_HEADER dos;

    dl_zero_bytes(base, DLL_SYNTHETIC_SHIM_IMAGE_SIZE);
    dos = (PIMAGE_DOS_HEADER)base;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_cblp = 0x90;
    dos->e_cp = 3;
    dos->e_cparhdr = 4;
    dos->e_maxalloc = 0xFFFF;
    dos->e_sp = 0xB8;
    dos->e_lfarlc = 0x40;
    dos->e_lfanew = 0x80;

    if (is_32bit) {
        PIMAGE_NT_HEADERS32 nt =
            (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
                                         IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                         IMAGE_FILE_DLL;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        nt->OptionalHeader.MajorLinkerVersion = 1;
        nt->OptionalHeader.SizeOfCode = DLL_SYNTHETIC_SHIM_IMAGE_SIZE -
                                        SHIM64_THUNK_RVA;
        nt->OptionalHeader.BaseOfCode = SHIM64_THUNK_RVA;
        nt->OptionalHeader.ImageBase = (ULONG)(ULONG_PTR)image_base;
        nt->OptionalHeader.SectionAlignment = 0x1000;
        nt->OptionalHeader.FileAlignment = 0x200;
        nt->OptionalHeader.MajorOperatingSystemVersion = 6;
        nt->OptionalHeader.MajorSubsystemVersion = 6;
        nt->OptionalHeader.SizeOfImage = DLL_SYNTHETIC_SHIM_IMAGE_SIZE;
        nt->OptionalHeader.SizeOfHeaders = 0x200;
        nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        nt->OptionalHeader.DllCharacteristics =
            IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
            IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
        nt->OptionalHeader.SizeOfStackReserve = 0x100000;
        nt->OptionalHeader.SizeOfStackCommit = 0x1000;
        nt->OptionalHeader.SizeOfHeapReserve = 0x100000;
        nt->OptionalHeader.SizeOfHeapCommit = 0x1000;
        nt->OptionalHeader.NumberOfRvaAndSizes =
            IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        dl_init_synthetic_section(IMAGE_FIRST_SECTION(nt));
    } else {
        PIMAGE_NT_HEADERS64 nt =
            (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
                                         IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                         IMAGE_FILE_DLL;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.MajorLinkerVersion = 1;
        nt->OptionalHeader.SizeOfCode = DLL_SYNTHETIC_SHIM_IMAGE_SIZE -
                                        SHIM64_THUNK_RVA;
        nt->OptionalHeader.BaseOfCode = SHIM64_THUNK_RVA;
        nt->OptionalHeader.ImageBase = (ULONGLONG)(ULONG_PTR)image_base;
        nt->OptionalHeader.SectionAlignment = 0x1000;
        nt->OptionalHeader.FileAlignment = 0x200;
        nt->OptionalHeader.MajorOperatingSystemVersion = 6;
        nt->OptionalHeader.MajorSubsystemVersion = 6;
        nt->OptionalHeader.SizeOfImage = DLL_SYNTHETIC_SHIM_IMAGE_SIZE;
        nt->OptionalHeader.SizeOfHeaders = 0x200;
        nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        nt->OptionalHeader.DllCharacteristics =
            IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
            IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
        nt->OptionalHeader.SizeOfStackReserve = 0x100000;
        nt->OptionalHeader.SizeOfStackCommit = 0x1000;
        nt->OptionalHeader.SizeOfHeapReserve = 0x100000;
        nt->OptionalHeader.SizeOfHeapCommit = 0x1000;
        nt->OptionalHeader.NumberOfRvaAndSizes =
            IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        dl_init_synthetic_section(IMAGE_FIRST_SECTION(nt));
    }

    dl_zero_bytes(info, sizeof(*info));
    info->ImageBase = image_base;
    info->PreferredBase = (ULONGLONG)(ULONG_PTR)image_base;
    info->SizeOfImage = DLL_SYNTHETIC_SHIM_IMAGE_SIZE;
    info->Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    info->DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                               IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    info->StackReserve = 0x100000;
    info->StackCommit = 0x1000;
    info->IsDLL = TRUE;
    info->Is32Bit = is_32bit;
}

PVOID dll_get_module_handle(const char *name, BOOL add_reference)
{
    PVOID image_base = NULL;
    if (!name) return NULL;

    BOOL trace_shell =
        dl_stricmp(strip_path(name), "shell32.dll") == 0;
    if (trace_shell) {
        serial_puts("[DLL-SHELL-LOOKUP] get owner=");
        serial_putdec(dll_current_owner_pid());
        serial_puts(" image=0x");
        serial_puthex(win32_current_image_base(), 16);
        serial_puts(" bits=");
        serial_putdec((uint64_t)dll_current_process_bitness());
        serial_puts(" compat=");
        serial_putdec((uint64_t)(g_compat32_mode ? 1 : 0));
        serial_puts("\n");
    }

    loader_lock_acquire();
    LOADED_MODULE *mod = dll_find_module(name);
    if (mod) {
        if (add_reference)
            __atomic_add_fetch(&mod->ref_count, 1, __ATOMIC_RELAXED);
        image_base = mod->image.ImageBase;
    }
    loader_lock_release();
    if (trace_shell) {
        serial_puts("[DLL-SHELL-LOOKUP] result=0x");
        serial_puthex((ULONG_PTR)image_base, 16);
        if (mod) {
            serial_puts(" state=");
            serial_putdec((uint64_t)mod->state);
            serial_puts(" owner=");
            serial_putdec(mod->owner_pid);
            serial_puts(" bits=");
            serial_putdec(mod->image.Is32Bit ? 32 : 64);
            serial_puts(" refs=");
            serial_putdec((uint64_t)mod->ref_count);
        }
        serial_puts("\n");
    }
    return image_base;
}

PVOID dll_get_shim_module_handle(const char *name, BOOL add_reference)
{
    BOOL trace_shell = name &&
        dl_stricmp(strip_path(name), "shell32.dll") == 0;
    shim_resolver_fn shim = name ? find_shim(name) : NULL;
    if (trace_shell) {
        serial_puts("[DLL-SHELL-LOOKUP] shim resolver=0x");
        serial_puthex((ULONG_PTR)shim, 16);
        serial_puts("\n");
    }
    if (!name || !shim) return NULL;

    char provider[64];
    name = dll_shim_provider_name(name, provider);

    loader_lock_acquire();
    LOADED_MODULE *mod = dll_find_module(name);
    if (mod) {
        if (add_reference)
            __atomic_add_fetch(&mod->ref_count, 1, __ATOMIC_RELAXED);
        PVOID existing = mod->image.ImageBase;
        loader_lock_release();
        return existing;
    }

    ULONG owner_pid = dll_current_owner_pid();
    mod = module_reserve(name, owner_pid);
    if (!mod) {
        if (trace_shell)
            serial_puts("[DLL-SHELL-LOOKUP] reserve failed\n");
        loader_lock_release();
        return NULL;
    }

    /* Import resolution starts after the main image is mapped but before the
     * PE32 entry trampoline sets the task-local compatibility flag. Derive
     * both lookup and facade creation from the published image bitness so the
     * first import cannot create an unreachable PE64 facade for a PE32 app. */
    BOOL is_32bit = dll_caller_is_32bit();
    PVOID image_base = pe_alloc(NULL, DLL_SYNTHETIC_SHIM_IMAGE_SIZE,
                                is_32bit);
    if (!image_base) {
        if (trace_shell) {
            serial_puts("[DLL-SHELL-LOOKUP] pe_alloc failed bits=");
            serial_putdec(is_32bit ? 32 : 64);
            serial_puts(" owner=");
            serial_putdec(owner_pid);
            serial_puts("\n");
        }
        module_release_record(mod);
        loader_lock_release();
        return NULL;
    }

    dl_build_synthetic_shim_image(image_base, is_32bit, &mod->image);
    mod->synthetic_shim = TRUE;
    __atomic_store_n(&mod->state, 2, __ATOMIC_RELEASE);

#if !defined(OK_QUIET) || !OK_QUIET
    serial_puts("[DLL] mapped shim module ");
    serial_puts(mod->name);
    serial_puts(" at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)image_base, 16);
    serial_puts(" owner=");
    serial_putdec(owner_pid);
    serial_puts("\n");
#endif

    loader_lock_release();
    return image_base;
}

/* ── Resolve export from PE export directory ───────────────── */

static BYTE *dl_shim64_writable_alias(PVOID address)
{
    uint64_t va = (uint64_t)(ULONG_PTR)address;
    uint64_t *pte = paging_get_pte(va);
    if (!pte || !(*pte & DLL_PTE_PRESENT))
        return NULL;

    uint64_t phys = (*pte & DLL_PTE_ADDR_MASK) | (va & 0xFFFULL);
    return (BYTE *)(ULONG_PTR)(KERNEL_VBASE + phys);
}

PVOID dll_get_shim_export_thunk(const char *dll_name, PVOID target)
{
    if (!dll_name || !target || g_compat32_mode ||
        (ULONG_PTR)target < KERNEL_VBASE)
        return target;

    char provider[64];
    dll_name = dll_shim_provider_name(dll_name, provider);

    const char *export_name = NULL;
    uint8_t argc = 0;
    uint8_t callconv = 0;
    if (win32_abi_target_is_data(dll_name, target))
        return target;
    if (!win32_abi_lookup_target(dll_name, target, &export_name, &argc,
                                 &callconv))
        return target;
    (void)argc;
    (void)callconv;

    PVOID module = dll_get_shim_module_handle(dll_name, FALSE);
    if (!module)
        return target;

    loader_lock_acquire();
    LOADED_MODULE *mod = dll_find_module_by_base(module);
    if (!mod || !mod->synthetic_shim || mod->image.Is32Bit) {
        loader_lock_release();
        return target;
    }

    int module_index = (int)(mod - modules);
    USHORT count = shim64_thunk_count[module_index];
    for (USHORT i = 0; i < count; i++) {
        if (shim64_thunk_targets[module_index][i] == (ULONG_PTR)target) {
            PVOID thunk = (BYTE *)module + SHIM64_THUNK_RVA +
                          (SIZE_T)i * SHIM64_THUNK_SIZE;
            loader_lock_release();
            return thunk;
        }
    }

    if (count >= SHIM64_THUNK_MAX) {
        serial_puts("[SHIM64] thunk table full for ");
        serial_puts(mod->name);
        serial_puts("\n");
        loader_lock_release();
        return target;
    }

    PVOID thunk = (BYTE *)module + SHIM64_THUNK_RVA +
                  (SIZE_T)count * SHIM64_THUNK_SIZE;
    BYTE *code = dl_shim64_writable_alias(thunk);
    if (!code) {
        loader_lock_release();
        return target;
    }

    /* Leave a conventional patch window for Steam's overlay hooker. It may
     * replace the first instructions and resume through a trampoline, so the
     * import-boundary body must begin after the complete patch window. */
    for (SIZE_T i = 0; i < SHIM64_THUNK_PATCH_SIZE; i++) code[i] = 0x90;

    /* PE64 currently runs at CPL0. On Windows, user-mode POPF cannot change
     * IF, but at CPL0 a PE library can accidentally leave interrupts masked.
     * Re-establish the user-mode invariant at every kernel import boundary so
     * blocking shims can yield and timer preemption keeps making progress.
     * STI preserves the Win64 argument registers and caller return address. */
    SIZE_T body = SHIM64_THUNK_PATCH_SIZE;
    code[body++] = 0xFB; /* sti */
    code[body++] = 0x48; /* movabs rax, target */
    code[body++] = 0xB8;
    uint64_t target_value = (uint64_t)(ULONG_PTR)target;
    for (int i = 0; i < 8; i++) {
        code[body++] = (BYTE)target_value;
        target_value >>= 8;
    }
    code[body++] = 0xFF; /* jmp rax */
    code[body++] = 0xE0;
    while (body < SHIM64_THUNK_SIZE) code[body++] = 0xCC;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    shim64_thunk_targets[module_index][count] = (ULONG_PTR)target;
    shim64_thunk_count[module_index] = count + 1;

    static uint32_t thunk_logs;
    if (__atomic_fetch_add(&thunk_logs, 1, __ATOMIC_RELAXED) < 32) {
        serial_puts("[SHIM64] ");
        serial_puts(mod->name);
        serial_puts("!");
        serial_puts(export_name ? export_name : "<ordinal>");
        serial_puts(" thunk=0x");
        serial_puthex((uint64_t)(ULONG_PTR)thunk, 16);
        serial_puts(" target=0x");
        serial_puthex((uint64_t)(ULONG_PTR)target, 16);
        serial_puts("\n");
    }

    loader_lock_release();
    return thunk;
}

static LONG dll_export_name_index(BYTE *base,
                                  PIMAGE_EXPORT_DIRECTORY exports,
                                  const char *func_name, USHORT hint,
                                  BOOL account)
{
    ULONG *name_ptrs = (ULONG *)(base + exports->AddressOfNames);

    if (hint < exports->NumberOfNames) {
        const char *hint_name = (const char *)(base + name_ptrs[hint]);
        if (account)
            __atomic_add_fetch(&export_name_comparisons, 1,
                               __ATOMIC_RELAXED);
        if (dl_strcmp(func_name, hint_name) == 0) {
            if (account)
                __atomic_add_fetch(&export_hint_hits, 1,
                                   __ATOMIC_RELAXED);
            return (LONG)hint;
        }
    }

    ULONG low = 0;
    ULONG high = exports->NumberOfNames;
    while (low < high) {
        ULONG middle = low + (high - low) / 2;
        const char *export_name =
            (const char *)(base + name_ptrs[middle]);
        int comparison = dl_strcmp(func_name, export_name);
        if (account)
            __atomic_add_fetch(&export_name_comparisons, 1,
                               __ATOMIC_RELAXED);
        if (comparison == 0) {
            if (account)
                __atomic_add_fetch(&export_binary_hits, 1,
                                   __ATOMIC_RELAXED);
            return (LONG)middle;
        }
        if (comparison < 0)
            high = middle;
        else
            low = middle + 1;
    }

    if (account)
        __atomic_add_fetch(&export_lookup_misses, 1, __ATOMIC_RELAXED);
    return -1;
}

static BOOL dll_parse_forwarded_ordinal(const char *name, USHORT *ordinal)
{
    if (!name || name[0] != '#' || !name[1]) return FALSE;

    ULONG value = 0;
    for (int i = 1; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return FALSE;
        value = value * 10 + (ULONG)(name[i] - '0');
        if (value > 0xFFFF) return FALSE;
    }
    *ordinal = (USHORT)value;
    return TRUE;
}

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

    ULONG *func_addrs = (ULONG *)(base + exports->AddressOfFunctions);
    USHORT *name_ords = (USHORT *)(base + exports->AddressOfNameOrdinals);
    ULONG func_rva = 0;

    if (by_ordinal) {
        ULONG index = ordinal - exports->Base;
        if (index < exports->NumberOfFunctions && func_addrs[index]) {
            __atomic_add_fetch(&export_ordinal_hits, 1, __ATOMIC_RELAXED);
            func_rva = func_addrs[index];
        } else {
            __atomic_add_fetch(&export_lookup_misses, 1, __ATOMIC_RELAXED);
            return NULL;
        }
    } else {
        if (!func_name) return NULL;
        LONG name_index = dll_export_name_index(base, exports, func_name,
                                                ordinal, TRUE);
        if (name_index < 0) return NULL;

        USHORT ord_index = name_ords[name_index];
        if (ord_index >= exports->NumberOfFunctions) return NULL;
        func_rva = func_addrs[ord_index];
        if (func_rva == 0) return NULL;
    }

    /* A forwarder is either DLL.Function or DLL.#Ordinal. */
    if (func_rva >= exp_dir->VirtualAddress &&
        func_rva < exp_dir->VirtualAddress + exp_dir->Size) {
        const char *fwd = (const char *)(base + func_rva);
        char fwd_dll[64], fwd_func[128];
        int j = 0;
        while (fwd[j] && fwd[j] != '.' && j < 63) {
            fwd_dll[j] = fwd[j];
            j++;
        }
        fwd_dll[j] = 0;
        if (fwd[j] == '.') j++;
        int k = 0;
        while (fwd[j] && k < 127)
            fwd_func[k++] = fwd[j++];
        fwd_func[k] = 0;

        USHORT forwarded_ordinal = 0;
        if (dll_parse_forwarded_ordinal(fwd_func, &forwarded_ordinal))
            return dll_resolve_import(fwd_dll, NULL, forwarded_ordinal,
                                      TRUE);
        return dll_resolve_import(fwd_dll, fwd_func, 0, FALSE);
    }

    return (PVOID)(base + func_rva);
}

void dll_export_lookup_dump(void)
{
    serial_puts("[DLL-EXPORT] hint_hits=");
    serial_putdec(__atomic_load_n(&export_hint_hits, __ATOMIC_RELAXED));
    serial_puts(" binary_hits=");
    serial_putdec(__atomic_load_n(&export_binary_hits, __ATOMIC_RELAXED));
    serial_puts(" ordinal_hits=");
    serial_putdec(__atomic_load_n(&export_ordinal_hits, __ATOMIC_RELAXED));
    serial_puts(" misses=");
    serial_putdec(__atomic_load_n(&export_lookup_misses, __ATOMIC_RELAXED));
    serial_puts(" comparisons=");
    serial_putdec(__atomic_load_n(&export_name_comparisons,
                                  __ATOMIC_RELAXED));
    serial_puts("\n");
}

int dll_export_lookup_selftest(void)
{
    BYTE image[512] = {0};
    IMAGE_EXPORT_DIRECTORY exports = {0};
    ULONG *name_ptrs = (ULONG *)(image + 64);

    exports.AddressOfNames = 64;
    exports.NumberOfNames = 3;
    name_ptrs[0] = 128;
    name_ptrs[1] = 144;
    name_ptrs[2] = 160;
    const char alpha[] = "Alpha";
    const char bravo[] = "Bravo";
    const char charlie[] = "Charlie";
    for (unsigned i = 0; i < sizeof(alpha); i++)
        image[name_ptrs[0] + i] = (BYTE)alpha[i];
    for (unsigned i = 0; i < sizeof(bravo); i++)
        image[name_ptrs[1] + i] = (BYTE)bravo[i];
    for (unsigned i = 0; i < sizeof(charlie); i++)
        image[name_ptrs[2] + i] = (BYTE)charlie[i];

    int failures = 0;
    if (dll_export_name_index(image, &exports, "Bravo", 1, FALSE) != 1)
        failures++;
    if (dll_export_name_index(image, &exports, "Charlie", 0, FALSE) != 2)
        failures++;
    if (dll_export_name_index(image, &exports, "Alpha", 2, FALSE) != 0)
        failures++;
    if (dll_export_name_index(image, &exports, "Missing", 0, FALSE) != -1)
        failures++;
    USHORT forwarded = 0;
    if (!dll_parse_forwarded_ordinal("#27", &forwarded) || forwarded != 27)
        failures++;
    if (dll_parse_forwarded_ordinal("#70000", &forwarded) ||
        dll_parse_forwarded_ordinal("#2x", &forwarded))
        failures++;
    return failures;
}

/* ── IAT auto-recovery: resolve original function for corrupted IAT entry ── */

static PVOID dll_resolve_export_any(ULONG owner_pid, const char *func_name,
                                    USHORT ordinal, BOOL by_ordinal)
{
    DLL_PROCESS_STATE *state = loader_process_state(owner_pid, FALSE);
    if (state) {
        int current =
            __atomic_load_n(&state->module_head, __ATOMIC_ACQUIRE);
        for (int visited = 0;
             current >= 0 && current < MAX_LOADED_MODULES &&
             visited < MAX_LOADED_MODULES;
             visited++) {
            LOADED_MODULE *mod = &modules[current];
            int next = mod->next_owner_index;
            if (module_visible(mod, owner_pid)) {
                PVOID result = dll_resolve_export(
                    mod, func_name, ordinal, by_ordinal);
                if (result) return result;
            }
            current = next;
        }
        return NULL;
    }

    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (!module_visible(&modules[i], owner_pid)) continue;
        PVOID result = dll_resolve_export(
            &modules[i], func_name, ordinal, by_ordinal);
        if (result) return result;
    }
    return NULL;
}

uint32_t dll_resolve_iat_original(uint32_t iat_va)
{
    LOADED_MODULE *module = dll_find_module_by_address(
        (PVOID)(uintptr_t)iat_va);
    if (!module || !module->image.Is32Bit) return 0;
    BYTE *base = (BYTE *)module->image.ImageBase;
    uint32_t base32 = (uint32_t)(uintptr_t)base;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS32 nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= 1) return 0;

    uint32_t imp_rva = nt->OptionalHeader.DataDirectory[1].VirtualAddress;
    if (!imp_rva) return 0;

    typedef struct {
        uint32_t INT, ts, fwd, name, IAT;
    } ImpDesc;
    ImpDesc *imp = (ImpDesc *)(base + imp_rva);
    for (; imp->name; imp++) {
        uint32_t iat_start = base32 + imp->IAT;
        uint32_t int_rva = imp->INT ? imp->INT : imp->IAT;
        uint32_t *iat_arr = (uint32_t *)(uintptr_t)(base32 + imp->IAT);
        uint32_t *int_arr = (uint32_t *)(uintptr_t)(base32 + int_rva);
        const char *dll_name = (const char *)(base + imp->name);

        for (int j = 0; iat_arr[j]; j++) {
            if (iat_start + j * 4 != iat_va) continue;
            uint32_t int_entry = int_arr[j];
            if (int_entry & 0x80000000) {
                uint16_t ord = (uint16_t)(int_entry & 0xFFFF);
                PVOID fn = dll_resolve_import(dll_name, NULL, ord, TRUE);
                return fn ? (uint32_t)(uintptr_t)fn : 0;
            }
            typedef struct { uint16_t hint; char name[1]; } HintName;
            HintName *hn = (HintName *)(base + int_entry);
            PVOID fn = dll_resolve_import(dll_name, hn->name, 0, FALSE);
            return fn ? (uint32_t)(uintptr_t)fn : 0;
        }
    }
    return 0;
}

/* ── Dynamic IAT guard table ────────────────────────────────── */

#define MAX_IAT_GUARDS 16
static struct { uint32_t addr; uint32_t value; } iat_guards[MAX_IAT_GUARDS];
static int iat_guard_count = 0;

void iat_guard_add(uint32_t addr, uint32_t value)
{
    /* Check if already guarded */
    for (int i = 0; i < iat_guard_count; i++)
        if (iat_guards[i].addr == addr) return;
    if (iat_guard_count < MAX_IAT_GUARDS) {
        iat_guards[iat_guard_count].addr = addr;
        iat_guards[iat_guard_count].value = value;
        iat_guard_count++;
        serial_puts("[IAT-GUARD] Added 0x");
        serial_puthex(addr, 8);
        serial_puts(" = 0x");
        serial_puthex(value, 8);
        serial_puts("\n");
    }
}

void iat_guard_check(void)
{
    for (int i = 0; i < iat_guard_count; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)iat_guards[i].addr;
        if (*p != iat_guards[i].value)
            *p = iat_guards[i].value;
    }
}

/* ── Load a PE DLL ─────────────────────────────────────────── */

static PVOID dll_load_impl(const char *dll_name, const BYTE *file_data,
                           SIZE_T file_size)
{
    if (sane_module_count() >= MAX_LOADED_MODULES) {
        serial_puts("[DLL] max modules reached\n");
        return NULL;
    }

    /* Check if already loaded */
    LOADED_MODULE *existing = dll_find_module(dll_name);
    if (existing) {
        __atomic_add_fetch(&existing->ref_count, 1, __ATOMIC_RELAXED);
        return existing->image.ImageBase;
    }

    int image_bits = dll_pe_bitness(file_data, file_size);
    int process_bits = dll_current_process_bitness();
    if (!image_bits || (process_bits && image_bits != process_bits)) {
        serial_puts("[DLL] rejected ");
        serial_puts(dll_name ? dll_name : "<unnamed>");
        if (!image_bits) {
            serial_puts(": invalid PE image\n");
        } else {
            serial_puts(": PE");
            serial_putdec((uint64_t)image_bits);
            serial_puts(" image in PE");
            serial_putdec((uint64_t)process_bits);
            serial_puts(" process\n");
        }
        SetLastError(193); /* ERROR_BAD_EXE_FORMAT */
        return NULL;
    }

    serial_puts("[DLL] loading: ");
    serial_puts(dll_name);
    serial_puts("\n");

    if (dl_is_libcef(dll_name))
        dl_probe_libcef_bytes("source", file_data, file_size,
                              LIBCEF_PROBE_FILE_OFFSET);

    /* Reserve a stable slot before recursive dependency loading. */
    LOADED_MODULE *mod =
        module_reserve(dll_name, dll_current_owner_pid());
    if (!mod) {
        serial_puts("[DLL] max modules reached\n");
        return NULL;
    }

    /* Load the PE (may trigger recursive dll_load for dependencies) */
    NTSTATUS status = pe_load_named(file_data, file_size, &mod->image,
                                    dll_name);

    if (!NT_SUCCESS(status)) {
        serial_puts("[DLL] pe_load failed: ");
        serial_puthex((uint64_t)status, 8);
        serial_puts("\n");
        module_release_record(mod);
        return NULL;
    }

    if (dl_is_libcef(dll_name))
        dl_probe_libcef_bytes("mapped", mod->image.ImageBase,
                              mod->image.SizeOfImage, LIBCEF_PROBE_RVA);

    tier0_arm_text_write_trap(dll_name, &mod->image);

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
        serial_puts("[DLL] IAT patching ");
        serial_puts(dll_name);
        serial_puts("...\n");
        NTSTATUS compat_st = compat32_patch_iat(&mod->image);
        serial_puts("[DLL] IAT done\n");

        /* IAT protection: instead of page-level write protection (which
         * requires x86 instruction decoding or single-step to handle #PF),
         * we use the watchdog in compat32_dispatch to restore IAT entries
         * on every INT 0x2E call. See the IAT guard block in compat32.c. */

        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[DLL] WARNING: compat32 IAT patch failed for ");
            serial_puts(dll_name);
            serial_puts("\n");
        } else {
            NTSTATUS protect_st =
                pe_finalize_image_protections(&mod->image);
            if (!NT_SUCCESS(protect_st)) {
                serial_puts("[DLL] final protection setup failed for ");
                serial_puts(dll_name);
                serial_puts(": 0x");
                serial_puthex((uint32_t)protect_st, 8);
                serial_puts("\n");
                pe_unload(&mod->image);
                module_release_record(mod);
                return NULL;
            }

            NTSTATUS tls_st = compat32_attach_tls(&mod->image);
            if (!NT_SUCCESS(tls_st)) {
                serial_puts("[DLL] compat32 TLS setup failed for ");
                serial_puts(dll_name);
                serial_puts(": 0x");
                serial_puthex((uint32_t)tls_st, 8);
                serial_puts("\n");
                pe_unload(&mod->image);
                module_release_record(mod);
                return NULL;
            }
        }
    } else {
        NTSTATUS protect_st = pe_finalize_image_protections(&mod->image);
        if (!NT_SUCCESS(protect_st)) {
            serial_puts("[DLL] final protection setup failed for ");
            serial_puts(dll_name);
            serial_puts(": 0x");
            serial_puthex((uint32_t)protect_st, 8);
            serial_puts("\n");
            pe_unload(&mod->image);
            module_release_record(mod);
            return NULL;
        }

        NTSTATUS tls_st = win64_attach_tls(&mod->image);
        if (!NT_SUCCESS(tls_st)) {
            serial_puts("[DLL] PE64 TLS setup failed for ");
            serial_puts(dll_name);
            serial_puts(": 0x");
            serial_puthex((uint32_t)tls_st, 8);
            serial_puts("\n");
            pe_unload(&mod->image);
            module_release_record(mod);
            return NULL;
        }
    }

    if (dl_is_libcef(dll_name))
        dl_probe_libcef_bytes("finalized", mod->image.ImageBase,
                              mod->image.SizeOfImage, LIBCEF_PROBE_RVA);

    /* Core.dll-specific: pre-allocate GObjRegistrants TArray BEFORE
     * Core.dll's DllMain runs.  Core.dll's _initterm constructs every
     * UClass's static class object, each of which calls
     * GObjRegistrants.Add(this).  If the TArray is in its default zero
     * state at that moment, the first Add() triggers FArray::Realloc
     * with bogus Max → corrupted alloc → registrants get dropped or
     * land in garbage memory.  Pre-allocating a 1024-slot buffer with
     * sane {Data, Num=0, Max=1024} lets each Add() succeed without
     * triggering realloc.
     *
     * The late pre-alloc in winexec.c (after all preloads) was too
     * late — Core.dll's _initterm had already run with empty TArray.
     */
    if (mod->image.IsDLL && !find_shim(mod->name) &&
        ((mod->name[0]=='C' && mod->name[1]=='o' && mod->name[2]=='r' && mod->name[3]=='e') ||
         (mod->name[0]=='c' && mod->name[1]=='o' && mod->name[2]=='r' && mod->name[3]=='e'))) {
        PVOID gobjreg_ptr = dll_resolve_export(mod,
            "?GObjRegistrants@UObject@@0V?$TArray@PAVUObject@@@@A", 0, FALSE);
        if (gobjreg_ptr) {
            uint32_t *tarray = (uint32_t *)gobjreg_ptr;
            extern void *mem_alloc_pages(uint64_t count);
            void *buf = mem_alloc_pages(1);
            if (buf) {
                uint64_t pa = (uint64_t)buf;
                uint8_t *p = (uint8_t *)pa;
                for (int i = 0; i < 4096; i++) p[i] = 0;
                tarray[0] = (uint32_t)pa;
                tarray[1] = 0;
                tarray[2] = 1024;
                serial_puts("[DLL-EARLY] Pre-allocated GObjRegistrants BEFORE DllMain: Data=0x");
                serial_puthex(pa, 8);
                serial_puts(" Max=1024 @TArray=0x");
                serial_puthex((uint64_t)(ULONG_PTR)gobjreg_ptr, 8);
                serial_puts("\n");
            }
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
                extern void win64_tls_unregister_image(PVOID image_base);
                win64_tls_unregister_image(mod->image.ImageBase);
                pe_unload(&mod->image);
                module_release_record(mod);
                return NULL;
            }

            mod->initialized = TRUE;
        }
    }

    if (dl_is_libcef(dll_name))
        dl_probe_libcef_bytes("dllmain", mod->image.ImageBase,
                              mod->image.SizeOfImage, LIBCEF_PROBE_RVA);

    /* After Window.dll loads, initialize NULL global stubs.
     * GWindowManager (USubsystem*) and GLogWindow (WLog*) are DATA
     * exports that stay NULL because DllMain doesn't construct them.
     * Virtual calls through NULL crash with #PF at address 0. */
    if (dl_stricmp(mod->name, "window.dll") == 0) {
        static const char *globals[] = {
            "?GWindowManager@@3PAVUSubsystem@@A",
            "?GLogWindow@@3PAVWLog@@A",
        };
        for (int gi = 0; gi < 2; gi++) {
            ULONG owner_pid = dll_current_owner_pid();
            PVOID addr = dll_resolve_export_any(
                owner_pid, globals[gi], 0, FALSE);
            const char *short_name = (gi == 0) ? "GWindowManager" : "GLogWindow";
            if (addr && *(uint32_t *)(uintptr_t)addr == 0) {
                uint32_t stub = create_stub_uobject(short_name);
                if (stub) {
                    *(uint32_t *)(uintptr_t)addr = stub;
                    serial_puts("[WIN32] ");
                    serial_puts(short_name);
                    serial_puts(" -> 0x");
                    serial_puthex(stub, 8);
                    serial_puts("\n");
                }
            }
        }
    }

    if (mod->initialized)
        mod->init_order = __atomic_fetch_add(&next_init_order, 1,
                                             __ATOMIC_RELAXED);
    __atomic_store_n(&mod->state, 2, __ATOMIC_RELEASE);
    return mod->image.ImageBase;
}

PVOID dll_load(const char *dll_name, const BYTE *file_data, SIZE_T file_size)
{
    loader_lock_acquire();
    PVOID base = dll_load_impl(dll_name, file_data, file_size);
    loader_lock_release();
    return base;
}

BOOL dll_disable_thread_notifications(PVOID image_base)
{
    BOOL disabled = FALSE;

    loader_lock_acquire();
    LOADED_MODULE *mod = dll_find_module_by_base(image_base);
    if (mod && mod->image.IsDLL) {
        mod->thread_notifications_disabled = TRUE;
        disabled = TRUE;
#if !defined(OK_QUIET) || !OK_QUIET
        serial_puts("[DLL-THREAD] notifications disabled for ");
        serial_puts(mod->name);
        serial_puts("\n");
#endif
    }
    loader_lock_release();
    return disabled;
}

void dll_notify_thread(DWORD reason)
{
    if (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH)
        return;

    loader_lock_acquire();

    ULONG owner_pid = dll_current_owner_pid();
    BOOL is32bit = g_compat32_mode ? TRUE : FALSE;
    LOADED_MODULE *ready[MAX_LOADED_MODULES];
    int count = 0;

    DLL_PROCESS_STATE *process_state =
        loader_process_state(owner_pid, FALSE);
    int current = process_state
        ? __atomic_load_n(&process_state->module_head, __ATOMIC_ACQUIRE)
        : -1;
    for (int visited = 0;
         current >= 0 && current < MAX_LOADED_MODULES &&
         visited < MAX_LOADED_MODULES;
         visited++) {
        LOADED_MODULE *mod = &modules[current];
        current = mod->next_owner_index;
        if (__atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) != 2 ||
            mod->owner_pid != owner_pid || !mod->initialized ||
            !mod->dll_main || mod->thread_notifications_disabled ||
            mod->image.Is32Bit != is32bit)
            continue;

        int pos = count;
        while (pos > 0 &&
               ready[pos - 1]->init_order > mod->init_order) {
            ready[pos] = ready[pos - 1];
            pos--;
        }
        ready[pos] = mod;
        count++;
    }

    /* Only an overflow process lacks a local module list. */
    if (!process_state) {
        for (int i = 0; i < MAX_LOADED_MODULES; i++) {
            LOADED_MODULE *mod = &modules[i];
            if (__atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) != 2 ||
                mod->owner_pid != owner_pid || !mod->initialized ||
                !mod->dll_main || mod->thread_notifications_disabled ||
                mod->image.Is32Bit != is32bit)
                continue;
            int pos = count;
            while (pos > 0 &&
                   ready[pos - 1]->init_order > mod->init_order) {
                ready[pos] = ready[pos - 1];
                pos--;
            }
            ready[pos] = mod;
            count++;
        }
    }

#if !defined(OK_QUIET) || !OK_QUIET
    serial_puts("[DLL-THREAD] ");
    serial_puts(reason == DLL_THREAD_ATTACH ? "attach" : "detach");
    serial_puts(" pid=");
    serial_putdec(owner_pid);
    serial_puts(" arch=");
    serial_puts(is32bit ? "32" : "64");
    serial_puts(" count=");
    serial_putdec((uint64_t)count);
    serial_puts("\n");
#endif

    for (int n = 0; n < count; n++) {
        int index = reason == DLL_THREAD_ATTACH ? n : count - 1 - n;
        LOADED_MODULE *mod = ready[index];

        /* A notification may unload another module. */
        if (__atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) != 2 ||
            mod->owner_pid != owner_pid ||
            mod->thread_notifications_disabled)
            continue;

        if (mod->image.Is32Bit) {
            uint32_t args[3] = {
                (uint32_t)(ULONG_PTR)mod->image.ImageBase,
                reason,
                0
            };
            compat32_callback_args((uint32_t)(ULONG_PTR)mod->dll_main,
                                   3, args);
        } else {
            typedef BOOL (WINAPI *dll_main_fn)(PVOID, DWORD, PVOID);
            ((dll_main_fn)mod->dll_main)(mod->image.ImageBase, reason, NULL);
        }
    }

    loader_lock_release();
}

/* ── Unload a DLL ──────────────────────────────────────────── */

void dll_unload(LOADED_MODULE *mod)
{
    if (!mod || __atomic_load_n(&mod->state, __ATOMIC_ACQUIRE) <= 0)
        return;

    loader_lock_acquire();

    if (__atomic_sub_fetch(&mod->ref_count, 1, __ATOMIC_ACQ_REL) > 0) {
        loader_lock_release();
        return;
    }

    PVOID image_base = mod->image.ImageBase;
    SIZE_T image_size = mod->image.SizeOfImage;
    ULONG owner_pid = mod->owner_pid;
    BOOL synthetic_shim = mod->synthetic_shim;

    /* Call DllMain(DLL_PROCESS_DETACH) — skip for PE32 DLLs */
    if (mod->dll_main && mod->initialized && !mod->image.Is32Bit) {
        typedef BOOL (WINAPI *dll_main_fn)(PVOID, DWORD, PVOID);
        dll_main_fn entry = (dll_main_fn)mod->dll_main;
        entry(mod->image.ImageBase, DLL_PROCESS_DETACH, NULL);
    }

    if (!synthetic_shim && !mod->image.Is32Bit) {
        extern void win64_tls_unregister_image(PVOID image_base);
        win64_tls_unregister_image(image_base);
    }

    if (synthetic_shim)
        pe_free_for_owner(image_base, image_size, owner_pid);
    else
        pe_unload(&mod->image);
    module_release_record(mod);
    loader_lock_release();
}

void dll_release_process(ULONG owner_pid)
{
    loader_lock_abandon_process(owner_pid);
    loader_lock_acquire_for(owner_pid);

    int released = 0;
    DLL_PROCESS_STATE *process_state =
        loader_process_state(owner_pid, FALSE);
    while (process_state) {
        int index =
            __atomic_load_n(&process_state->module_head, __ATOMIC_ACQUIRE);
        if (index < 0 || index >= MAX_LOADED_MODULES) break;
        LOADED_MODULE *mod = &modules[index];
        if (!module_visible(mod, owner_pid)) {
            __atomic_store_n(&process_state->module_head,
                             mod->next_owner_index, __ATOMIC_RELEASE);
            continue;
        }

        PVOID image_base = mod->image.ImageBase;
        SIZE_T image_size = mod->image.SizeOfImage;
        BOOL synthetic_shim = mod->synthetic_shim;

        /* A crashed process must not re-enter arbitrary DllMain cleanup. */
        if (image_base && !synthetic_shim && !mod->image.Is32Bit) {
            extern void win64_tls_unregister_image(PVOID image_base);
            win64_tls_unregister_image(image_base);
        }
        if (image_base && synthetic_shim)
            pe_free_for_owner(image_base, image_size, owner_pid);
        else if (image_base)
            pe_unload(&mod->image);
        module_release_record(mod);
        released++;
    }

    if (!process_state) {
        for (int i = MAX_LOADED_MODULES - 1; i >= 0; i--) {
            LOADED_MODULE *mod = &modules[i];
            if (!module_visible(mod, owner_pid)) continue;
            PVOID image_base = mod->image.ImageBase;
            SIZE_T image_size = mod->image.SizeOfImage;
            BOOL synthetic_shim = mod->synthetic_shim;
            if (image_base && !synthetic_shim && !mod->image.Is32Bit) {
                extern void win64_tls_unregister_image(PVOID image_base);
                win64_tls_unregister_image(image_base);
            }
            if (image_base && synthetic_shim)
                pe_free_for_owner(image_base, image_size, owner_pid);
            else if (image_base)
                pe_unload(&mod->image);
            module_release_record(mod);
            released++;
        }
    }

    dll_release_search_state_locked(process_state, owner_pid);

    loader_lock_release_for(owner_pid);
    loader_process_forget(owner_pid);

    if (released) {
        serial_puts("[DLL] released process modules pid=");
        serial_putdec(owner_pid);
        serial_puts(" count=");
        serial_putdec((uint64_t)released);
        serial_puts("\n");
    }
}

/* ── Auto-load DLLs from filesystem ────────────────────────── */

extern void *osfs2_find(const char *name);
extern void *osfs2_find_ci(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);
extern const char *osfs2_file_name(void *file);
extern uint64_t osfs2_file_revision(void *file);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern bool win32_normalize_path(PCSTR path, char out[260]);
extern const char *win32_current_image_path(void);

/* Windows keeps file data in the system cache and maps a private image view
 * into each process. We currently copy each PE into private image pages, but
 * retaining the immutable source bytes removes repeated NVMe reads for CEF's
 * helper processes without sharing relocated data, IATs, or TLS. */
#define DLL_FILE_CACHE_ENTRIES 32
#define DLL_FILE_CACHE_LIMIT   (512ULL * 1024 * 1024)
#define DLL_FILE_CACHE_EMPTY   0
#define DLL_FILE_CACHE_LOADING 1
#define DLL_FILE_CACHE_READY   2
#define DLL_FILE_CACHE_EVICTING 3

typedef struct {
    volatile int state;
    void *file;
    uint64_t revision;
    uint64_t size;
    uint64_t pages;
    void *physical;
    const BYTE *data;
    uint64_t last_use;
    uint32_t references;
    char name[64];
} DLL_FILE_CACHE_ENTRY;

typedef struct {
    const BYTE *data;
    uint64_t size;
    uint64_t pages;
    void *physical;
    DLL_FILE_CACHE_ENTRY *entry;
} DLL_FILE_VIEW;

static DLL_FILE_CACHE_ENTRY dll_file_cache[DLL_FILE_CACHE_ENTRIES];
static volatile uint32_t dll_file_cache_lock;
static volatile uint64_t dll_file_cache_clock;
static volatile uint64_t dll_file_cache_bytes;
static volatile uint64_t dll_file_cache_hits;
static volatile uint64_t dll_file_cache_misses;
static volatile uint64_t dll_file_cache_waits;
static volatile uint64_t dll_file_cache_evictions;
static volatile uint64_t dll_file_cache_bypasses;
static volatile uint64_t dll_file_cache_disk_bytes;
static volatile uint64_t dll_file_cache_saved_bytes;

static void dll_file_cache_lock_acquire(void)
{
    while (__atomic_exchange_n(&dll_file_cache_lock, 1U,
                               __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 64; i++)
            __asm__ volatile ("pause");
        sched_yield();
    }
}

static void dll_file_cache_lock_release(void)
{
    __atomic_store_n(&dll_file_cache_lock, 0U, __ATOMIC_RELEASE);
}

static void dll_file_cache_clear_entry(DLL_FILE_CACHE_ENTRY *entry)
{
    entry->file = NULL;
    entry->revision = 0;
    entry->size = 0;
    entry->pages = 0;
    entry->physical = NULL;
    entry->data = NULL;
    entry->last_use = 0;
    entry->references = 0;
    entry->name[0] = 0;
    __atomic_store_n(&entry->state, DLL_FILE_CACHE_EMPTY,
                     __ATOMIC_RELEASE);
}

static BOOL dll_file_cache_evict_one(void)
{
    DLL_FILE_CACHE_ENTRY *victim = NULL;
    void *physical = NULL;
    uint64_t pages = 0;

    dll_file_cache_lock_acquire();
    for (int i = 0; i < DLL_FILE_CACHE_ENTRIES; i++) {
        DLL_FILE_CACHE_ENTRY *entry = &dll_file_cache[i];
        if (entry->state != DLL_FILE_CACHE_READY || entry->references)
            continue;
        if (!victim || entry->last_use < victim->last_use)
            victim = entry;
    }
    if (victim) {
        victim->state = DLL_FILE_CACHE_EVICTING;
        physical = victim->physical;
        pages = victim->pages;
        __atomic_sub_fetch(&dll_file_cache_bytes, pages * 4096,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&dll_file_cache_evictions, 1,
                           __ATOMIC_RELAXED);
    }
    dll_file_cache_lock_release();

    if (!victim) return FALSE;
    if (physical) mem_free_pages(physical, pages);
    dll_file_cache_lock_acquire();
    dll_file_cache_clear_entry(victim);
    dll_file_cache_lock_release();
    return TRUE;
}

static BOOL dll_file_read_uncached(void *file, uint64_t size,
                                   DLL_FILE_VIEW *view)
{
    uint64_t pages = (size + 4095) / 4096;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t before = osfs2_file_revision(file);
        if (before & 1) {
            sched_yield();
            continue;
        }
        void *physical = mem_alloc_pages(pages);
        if (!physical) return FALSE;
        BYTE *data = (BYTE *)PHYS_TO_VIRT(physical);
        int amount = osfs2_read(file, 0, data, size);
        uint64_t after = osfs2_file_revision(file);
        if (amount >= 0 && (uint64_t)amount == size && before == after &&
            !(after & 1)) {
            view->data = data;
            view->size = size;
            view->pages = pages;
            view->physical = physical;
            view->entry = NULL;
            __atomic_add_fetch(&dll_file_cache_disk_bytes, size,
                               __ATOMIC_RELAXED);
            return TRUE;
        }
        mem_free_pages(physical, pages);
        if (amount < 0) return FALSE;
        sched_yield();
    }
    return FALSE;
}

static BOOL dll_file_cache_acquire(void *file, uint64_t size,
                                   const char *name, DLL_FILE_VIEW *view)
{
    uint64_t pages = (size + 4095) / 4096;
    uint64_t bytes = pages * 4096;
    int wait_cycles = 0;

    view->data = NULL;
    view->size = 0;
    view->pages = 0;
    view->physical = NULL;
    view->entry = NULL;

    if (!pages || bytes > DLL_FILE_CACHE_LIMIT) {
        __atomic_add_fetch(&dll_file_cache_bypasses, 1, __ATOMIC_RELAXED);
        return dll_file_read_uncached(file, size, view);
    }

    for (int attempt = 0; attempt < 16; attempt++) {
        uint64_t revision = osfs2_file_revision(file);
        if (revision & 1) {
            if (++wait_cycles >= 8192) break;
            attempt--;
            sched_yield();
            continue;
        }

        DLL_FILE_CACHE_ENTRY *slot = NULL;
        BOOL wait_for_reader = FALSE;
        dll_file_cache_lock_acquire();
        for (int i = 0; i < DLL_FILE_CACHE_ENTRIES; i++) {
            DLL_FILE_CACHE_ENTRY *entry = &dll_file_cache[i];
            if (entry->file == file && entry->revision == revision &&
                entry->size == size) {
                if (entry->state == DLL_FILE_CACHE_READY) {
                    entry->references++;
                    entry->last_use = __atomic_add_fetch(
                        &dll_file_cache_clock, 1, __ATOMIC_RELAXED);
                    view->data = entry->data;
                    view->size = entry->size;
                    view->pages = entry->pages;
                    view->physical = entry->physical;
                    view->entry = entry;
                    __atomic_add_fetch(&dll_file_cache_hits, 1,
                                       __ATOMIC_RELAXED);
                    __atomic_add_fetch(&dll_file_cache_saved_bytes, size,
                                       __ATOMIC_RELAXED);
                    dll_file_cache_lock_release();
                    return TRUE;
                }
                if (entry->state == DLL_FILE_CACHE_LOADING)
                    wait_for_reader = TRUE;
            }
            if (!slot && entry->state == DLL_FILE_CACHE_EMPTY)
                slot = entry;
        }

        if (wait_for_reader) {
            dll_file_cache_lock_release();
            __atomic_add_fetch(&dll_file_cache_waits, 1, __ATOMIC_RELAXED);
            if (++wait_cycles >= 8192) break;
            attempt--;
            sched_yield();
            continue;
        }

        uint64_t used = __atomic_load_n(&dll_file_cache_bytes,
                                        __ATOMIC_RELAXED);
        if (!slot || used > DLL_FILE_CACHE_LIMIT - bytes) {
            dll_file_cache_lock_release();
            if (dll_file_cache_evict_one()) {
                attempt--;
                continue;
            }
            __atomic_add_fetch(&dll_file_cache_bypasses, 1,
                               __ATOMIC_RELAXED);
            return dll_file_read_uncached(file, size, view);
        }

        slot->file = file;
        slot->revision = revision;
        slot->size = size;
        slot->pages = pages;
        slot->physical = NULL;
        slot->data = NULL;
        slot->references = 1;
        slot->last_use = __atomic_add_fetch(&dll_file_cache_clock, 1,
                                            __ATOMIC_RELAXED);
        dl_strcpy_lower(slot->name, name ? name : "<unknown>",
                        sizeof(slot->name));
        __atomic_store_n(&slot->state, DLL_FILE_CACHE_LOADING,
                         __ATOMIC_RELEASE);
        __atomic_add_fetch(&dll_file_cache_bytes, bytes, __ATOMIC_RELAXED);
        dll_file_cache_lock_release();

        void *physical = mem_alloc_pages(pages);
        BYTE *data = physical ? (BYTE *)PHYS_TO_VIRT(physical) : NULL;
        int amount = data ? osfs2_read(file, 0, data, size) : -1;
        uint64_t after = osfs2_file_revision(file);
        if (amount < 0 || (uint64_t)amount != size || revision != after ||
            (after & 1)) {
            if (physical) mem_free_pages(physical, pages);
            dll_file_cache_lock_acquire();
            __atomic_sub_fetch(&dll_file_cache_bytes, bytes,
                               __ATOMIC_RELAXED);
            dll_file_cache_clear_entry(slot);
            dll_file_cache_lock_release();
            if (amount < 0 || !physical) return FALSE;
            sched_yield();
            continue;
        }

        dll_file_cache_lock_acquire();
        slot->physical = physical;
        slot->data = data;
        __atomic_add_fetch(&dll_file_cache_misses, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&dll_file_cache_disk_bytes, size,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&slot->state, DLL_FILE_CACHE_READY,
                         __ATOMIC_RELEASE);
        view->data = data;
        view->size = size;
        view->pages = pages;
        view->physical = physical;
        view->entry = slot;
        dll_file_cache_lock_release();
        return TRUE;
    }

    __atomic_add_fetch(&dll_file_cache_bypasses, 1, __ATOMIC_RELAXED);
    return dll_file_read_uncached(file, size, view);
}

static void dll_file_cache_release(DLL_FILE_VIEW *view)
{
    if (!view) return;
    if (view->entry) {
        dll_file_cache_lock_acquire();
        if (view->entry->references) view->entry->references--;
        dll_file_cache_lock_release();
    } else if (view->physical) {
        mem_free_pages(view->physical, view->pages);
    }
    view->data = NULL;
    view->physical = NULL;
    view->entry = NULL;
}

void dll_file_cache_dump(void)
{
    uint64_t entries = 0;
    dll_file_cache_lock_acquire();
    for (int i = 0; i < DLL_FILE_CACHE_ENTRIES; i++)
        if (dll_file_cache[i].state == DLL_FILE_CACHE_READY) entries++;
    dll_file_cache_lock_release();

    serial_puts("[DLL-CACHE] entries=");
    serial_putdec(entries);
    serial_puts(" bytes=");
    serial_putdec(__atomic_load_n(&dll_file_cache_bytes,
                                  __ATOMIC_RELAXED));
    serial_puts(" hits=");
    serial_putdec(__atomic_load_n(&dll_file_cache_hits, __ATOMIC_RELAXED));
    serial_puts(" misses=");
    serial_putdec(__atomic_load_n(&dll_file_cache_misses, __ATOMIC_RELAXED));
    serial_puts(" waits=");
    serial_putdec(__atomic_load_n(&dll_file_cache_waits, __ATOMIC_RELAXED));
    serial_puts(" evictions=");
    serial_putdec(__atomic_load_n(&dll_file_cache_evictions,
                                  __ATOMIC_RELAXED));
    serial_puts(" bypasses=");
    serial_putdec(__atomic_load_n(&dll_file_cache_bypasses,
                                  __ATOMIC_RELAXED));
    serial_puts(" disk_bytes=");
    serial_putdec(__atomic_load_n(&dll_file_cache_disk_bytes,
                                  __ATOMIC_RELAXED));
    serial_puts(" saved_bytes=");
    serial_putdec(__atomic_load_n(&dll_file_cache_saved_bytes,
                                  __ATOMIC_RELAXED));
    serial_puts("\n");
}

int dll_file_cache_flush_unused(void)
{
    int evicted = 0;
    while (dll_file_cache_evict_one()) evicted++;
    return evicted;
}

/* Recursion guard to prevent infinite dependency loops. The depth is part of
 * the process loader state so unrelated CEF helpers never affect each other. */
#define MAX_LOAD_DEPTH 16

#define DLL_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100U
#define DLL_LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200U
#define DLL_LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400U
#define DLL_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000U

static BOOL dll_name_has_extension(const char *name)
{
    for (; *name; name++)
        if (*name == '.') return TRUE;
    return FALSE;
}

static void *dll_find_in_directory(const char *directory,
                                   const char *basename, BOOL append_dll,
                                   char fs_path[260])
{
    int length = 0;
    const char *source = directory;
    while (*source && length < 259) fs_path[length++] = *source++;
    if (*source) return NULL;
    if (length && fs_path[length - 1] != '\\' &&
        fs_path[length - 1] != '/') {
        if (length >= 259) return NULL;
        fs_path[length++] = '\\';
    }
    source = basename;
    while (*source && length < 259) fs_path[length++] = *source++;
    if (*source) return NULL;
    fs_path[length] = 0;

    void *file = osfs2_find_ci(fs_path);
    if (!file && append_dll && !dll_name_has_extension(basename) &&
        length <= 255) {
        fs_path[length++] = '.';
        fs_path[length++] = 'd';
        fs_path[length++] = 'l';
        fs_path[length++] = 'l';
        fs_path[length] = 0;
        file = osfs2_find_ci(fs_path);
    }
    return file;
}

static void *dll_find_file(const char *dll_name, BOOL append_dll,
                           DWORD search_flags, char fs_path[260])
{
    const char *basename = dll_name;
    for (const char *p = dll_name; *p; p++) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    void *fsfile = NULL;
    fs_path[0] = 0;

    /* LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR applies to dependencies as well as the
     * explicitly loaded image. The directory is process-local and protected
     * by the recursive loader lock. */
    DLL_PROCESS_STATE *process_state =
        loader_process_state(dll_current_owner_pid(), FALSE);
    if (basename == dll_name && process_state &&
        process_state->load_directory[0]) {
        fsfile = dll_find_in_directory(process_state->load_directory,
                                       basename, append_dll, fs_path);
    }

    const char *image_path = win32_current_image_path();
    const char *image_sep = NULL;
    BOOL search_application = search_flags == 0 ||
        (search_flags & (DLL_LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                         DLL_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if (!fsfile && image_path && basename == dll_name && search_application) {
        for (const char *p = image_path; *p; p++)
            if (*p == '\\' || *p == '/') image_sep = p;
        if (image_sep) {
            char image_directory[260];
            int length = 0;
            while (image_path + length < image_sep && length < 259) {
                image_directory[length] = image_path[length];
                length++;
            }
            image_directory[length] = 0;
            fsfile = dll_find_in_directory(image_directory, basename,
                                           append_dll, fs_path);
        }
    }

    if (!fsfile && basename == dll_name && process_state &&
        search_flags == 0 && process_state->search_directory[0]) {
        fsfile = dll_find_in_directory(process_state->search_directory,
                                       basename, append_dll, fs_path);
    }

    BOOL search_user_directories =
        (search_flags & (DLL_LOAD_LIBRARY_SEARCH_USER_DIRS |
                         DLL_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) != 0;
    if (!fsfile && basename == dll_name && process_state &&
        search_user_directories) {
        ULONG owner_key = dll_current_owner_pid() + 1U;
        int slot = process_state->user_directory_head;
        for (int visited = 0;
             slot >= 0 && slot < DLL_USER_DIRECTORY_CAP &&
             visited < DLL_USER_DIRECTORY_CAP;
             visited++) {
            DLL_USER_DIRECTORY *entry = &loader_user_directories[slot];
            int next = entry->next;
            if (__atomic_load_n(&entry->owner_key, __ATOMIC_ACQUIRE) ==
                owner_key) {
                fsfile = dll_find_in_directory(entry->path, basename,
                                               append_dll, fs_path);
                if (fsfile) break;
            }
            slot = next;
        }
    }

    BOOL have_normalized_path = FALSE;
    BOOL search_current_directory = search_flags == 0 &&
        (!process_state || !process_state->current_directory_disabled);
    if (!fsfile && (basename != dll_name || search_current_directory)) {
        have_normalized_path = win32_normalize_path(dll_name, fs_path);
        if (have_normalized_path)
            fsfile = osfs2_find_ci(fs_path);
    }
    if (!fsfile)
        fsfile = osfs2_find_ci(basename);
    if (!fsfile && basename != dll_name)
        fsfile = osfs2_find_ci(dll_name);

    if (!fsfile && append_dll) {
        char with_dll[260];
        const char *stem = have_normalized_path ? fs_path : basename;
        int length = 0;
        int has_dot = 0;
        for (const char *p = basename; *p; p++)
            if (*p == '.') has_dot = 1;
        while (stem[length] && length < 255) {
            with_dll[length] = stem[length];
            length++;
        }
        if (!has_dot) {
            with_dll[length++] = '.';
            with_dll[length++] = 'd';
            with_dll[length++] = 'l';
            with_dll[length++] = 'l';
        }
        with_dll[length] = 0;
        fsfile = osfs2_find_ci(with_dll);
    }

    if (fsfile) {
        const char *stored_path = osfs2_file_name(fsfile);
        if (stored_path && stored_path[0])
            dl_strcpy(fs_path, stored_path, 260);
    }

    return fsfile;
}

PVOID dll_load_from_fs_ex(const char *dll_name, BOOL append_dll,
                          DWORD search_flags)
{
    if (!dll_name || !dll_name[0]) return NULL;

    loader_lock_acquire();

    /* Recheck after taking the process loader lock. This prevents two threads
     * from reading the same DLL concurrently before either publishes it. */
    LOADED_MODULE *existing = dll_find_module(dll_name);
    if (existing) {
        __atomic_add_fetch(&existing->ref_count, 1, __ATOMIC_RELAXED);
        PVOID base = existing->image.ImageBase;
        loader_lock_release();
        return base;
    }

    DLL_PROCESS_STATE *process_state =
        loader_process_state(dll_current_owner_pid(), TRUE);
    DWORD effective_search_flags = search_flags;
    if (!effective_search_flags && process_state)
        effective_search_flags = process_state->default_search_flags;
    unsigned load_depth = process_state
        ? __atomic_load_n(&process_state->load_depth, __ATOMIC_RELAXED)
        : 0;
    if (load_depth >= MAX_LOAD_DEPTH) {
        serial_puts("[DLL] max dependency depth reached: ");
        serial_puts(dll_name);
        serial_puts("\n");
        loader_lock_release();
        return NULL;
    }

    char fs_path[260];
    void *fsfile = dll_find_file(dll_name, append_dll,
                                 effective_search_flags, fs_path);

    if (!fsfile) {
        loader_lock_release();
        return NULL;
    }

    uint64_t fsize = osfs2_file_size(fsfile);
    if (fsize == 0) {
        loader_lock_release();
        return NULL;
    }

    const char *basename = strip_path(dll_name);
    serial_puts("[DLL] auto-load: ");
    serial_puts(basename);
    serial_puts(" (depth ");
    serial_puthex(load_depth, 1);
    serial_puts(")\n");

    DLL_FILE_VIEW file_view;
    if (!dll_file_cache_acquire(fsfile, fsize, basename, &file_view)) {
        loader_lock_release();
        return NULL;
    }

    char previous_directory[260];
    previous_directory[0] = 0;
    if (process_state) {
        int i = 0;
        while (process_state->load_directory[i] && i < 259) {
            previous_directory[i] = process_state->load_directory[i];
            i++;
        }
        previous_directory[i] = 0;

        if ((effective_search_flags &
             DLL_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) ||
            previous_directory[0]) {
            const char *last_separator = NULL;
            for (const char *p = fs_path; *p; p++)
                if (*p == '\\' || *p == '/') last_separator = p;
            if (last_separator) {
                i = 0;
                while (fs_path + i < last_separator && i < 259) {
                    process_state->load_directory[i] = fs_path[i];
                    i++;
                }
                process_state->load_directory[i] = 0;
            }
        }

        __atomic_add_fetch(&process_state->load_depth, 1,
                           __ATOMIC_RELAXED);
    }
    PVOID base = dll_load_impl(fs_path[0] ? fs_path : dll_name,
                               file_view.data,
                               (SIZE_T)file_view.size);
    if (process_state) {
        __atomic_sub_fetch(&process_state->load_depth, 1,
                           __ATOMIC_RELAXED);
        int i = 0;
        while (previous_directory[i]) {
            process_state->load_directory[i] = previous_directory[i];
            i++;
        }
        process_state->load_directory[i] = 0;
    }

    dll_file_cache_release(&file_view);
    loader_lock_release();
    return base;
}

PVOID dll_load_from_fs(const char *dll_name, BOOL append_dll)
{
    return dll_load_from_fs_ex(dll_name, append_dll, 0);
}

/* Called when an import references a DLL that is neither shimmed nor loaded. */
static LOADED_MODULE *dll_try_load_from_fs(const char *dll_name)
{
    PVOID base = dll_load_from_fs(dll_name, FALSE);

    if (base)
        return dll_find_module(dll_name);

    return NULL;
}

/* appUnwindf shim: suppresses the throw from appError.  Logs the
 * caller EIP + first 4 args. appUnwindf is `void appUnwindf(const TCHAR* fmt, ...)`
 * — arg0 is the wide format string, rest are %-conversion targets.
 * Print each arg both as hex and as a wide string (when the pointer is
 * in PE/heap range), so we can see exactly what error the engine is
 * reporting. */
static uint64_t WINAPI shim_appUnwindf(uint64_t fmt)
{
    (void)fmt;
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t val, int digits);
    extern void wdbg_print_wide(uint32_t va);
    extern uint32_t compat32_get_last_caller_eip(void);
    extern uint32_t compat32_get_last_stack_args(void);
    static int count = 0;
    if (++count <= 20) {
        uint32_t eip  = compat32_get_last_caller_eip();
        uint32_t sa   = compat32_get_last_stack_args();
        serial_puts("[APP] appUnwindf suppressed caller=0x");
        serial_puthex(eip, 8);
        if (sa >= 0x100000) {
            const uint32_t *args = (const uint32_t *)(uintptr_t)sa;
            for (int i = 0; i < 4; i++) {
                uint32_t v = args[i];
                serial_puts(" arg");
                serial_puthex((uint64_t)i, 1);
                serial_puts("=0x");
                serial_puthex(v, 8);
            }
            serial_puts("\n");
            /* Decode wide-string args inline. */
            for (int i = 0; i < 4; i++) {
                uint32_t v = args[i];
                if (v >= 0x10000 && v < 0x80000000u) {
                    serial_puts("  arg");
                    serial_puthex((uint64_t)i, 1);
                    serial_puts("=");
                    wdbg_print_wide(v);
                    serial_puts("\n");
                }
            }
        } else {
            serial_puts("\n");
        }
    }
    return 0;
}

/* appFailAssert shim: log expression+file+line, then suppress.  Caller
 * EIP identifies the engine function whose check() failed. */
static uint64_t WINAPI shim_appFailAssert(uint64_t expr, uint64_t file, uint64_t line)
{
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t val, int digits);
    extern void serial_putdec(uint64_t val);
    extern void serial_putchar(char c);
    extern uint32_t compat32_get_last_caller_eip(void);
    static int count = 0;
    if (++count <= 30) {
        uint32_t eip = compat32_get_last_caller_eip();
        serial_puts("[ASSERT] caller=0x");
        serial_puthex(eip, 8);
        serial_puts(" line=");
        serial_putdec((uint64_t)(uint32_t)line);
        if (expr >= 0x100000) {
            const char *e = (const char *)(uintptr_t)expr;
            serial_puts(" expr=\"");
            for (int k = 0; k < 80 && e[k]; k++) serial_putchar(e[k]);
            serial_puts("\"");
        }
        if (file >= 0x100000) {
            const char *f = (const char *)(uintptr_t)file;
            serial_puts(" file=\"");
            for (int k = 0; k < 80 && f[k]; k++) serial_putchar(f[k]);
            serial_puts("\"");
        }
        serial_puts("\n");
    }
    /* Return normally — let the engine continue with stale state via
     * its SEH-unwind path.  Empirically this reaches 10× more INT 0x2E
     * before terminal crash than aborting here via proc_exit. */
    return 0;
}

/* appRequestExit shim: suppresses exit requests from error handlers.
 * After Browse() fails and throw is suppressed, the engine calls
 * appRequestExit(1) which sets GIsRequestingExit=1. The game loop
 * then exits. By suppressing this, the engine stays in its loop. */
static uint64_t WINAPI shim_appRequestExit(uint64_t force)
{
    (void)force;
    extern void serial_puts(const char *);
    static int count = 0;
    if (++count <= 5)
        serial_puts("[APP] appRequestExit suppressed\n");
    return 0;
}

/* ── Master import resolver ────────────────────────────────── */

PVOID dll_resolve_import(const char *dll_name, const char *func_name,
                         USHORT ordinal, BOOL by_ordinal)
{
    /* 1. Try built-in shims first (ntdll, kernel32, msvcrt, etc.) */
    char provider[64];
    const char *shim_name = dll_shim_provider_name(dll_name, provider);
    shim_resolver_fn shim = find_shim(shim_name);
    if (shim) {
        PVOID fn = shim(func_name, ordinal, by_ordinal);
        if (fn) return dll_get_shim_export_thunk(shim_name, fn);
    }

    /* 1b. Function overrides for PE DLL exports.
     * Must return a 32-bit INT 0x2E thunk (not raw 64-bit ptr) because
     * Core.dll is NOT a shim DLL — IAT patcher writes addresses directly
     * without creating thunks. CC_CDECL because appUnwindf is varargs. */
    if (func_name && dl_strcmp(func_name, "?appUnwindf@@YAXPBGZZ") == 0) {
        static uint32_t thunk_addr = 0;
        if (!thunk_addr) {
            extern uint32_t compat32_make_thunk_ex(uint64_t target,
                const char *name, uint8_t num_args, uint8_t callconv);
            thunk_addr = compat32_make_thunk_ex(
                (uint64_t)(uintptr_t)shim_appUnwindf,
                "appUnwindf_shim", 1, 1 /* CC_CDECL */);
            serial_puts("[DLL] appUnwindf thunk at 0x");
            serial_puthex((uint64_t)thunk_addr, 8);
            serial_puts("\n");
        }
        return (PVOID)(uintptr_t)thunk_addr;
    }
    /* appFailAssert: log + suppress (so engine continues past check() */
    if (func_name && dl_strcmp(func_name, "?appFailAssert@@YAXPBD0H@Z") == 0) {
        static uint32_t thunk_addr_assert = 0;
        if (!thunk_addr_assert) {
            extern uint32_t compat32_make_thunk_ex(uint64_t target,
                const char *name, uint8_t num_args, uint8_t callconv);
            thunk_addr_assert = compat32_make_thunk_ex(
                (uint64_t)(uintptr_t)shim_appFailAssert,
                "appFailAssert_shim", 3, 1 /* CC_CDECL */);
            serial_puts("[DLL] appFailAssert thunk at 0x");
            serial_puthex((uint64_t)thunk_addr_assert, 8);
            serial_puts("\n");
        }
        return (PVOID)(uintptr_t)thunk_addr_assert;
    }
    /* appRequestExit: suppress exit after Browse() error */
    if (func_name && dl_strcmp(func_name, "?appRequestExit@@YAXH@Z") == 0) {
        static uint32_t thunk_addr2 = 0;
        if (!thunk_addr2) {
            extern uint32_t compat32_make_thunk_ex(uint64_t target,
                const char *name, uint8_t num_args, uint8_t callconv);
            thunk_addr2 = compat32_make_thunk_ex(
                (uint64_t)(uintptr_t)shim_appRequestExit,
                "appRequestExit_shim", 1, 1 /* CC_CDECL */);
            serial_puts("[DLL] appRequestExit thunk at 0x");
            serial_puthex((uint64_t)thunk_addr2, 8);
            serial_puts("\n");
        }
        return (PVOID)(uintptr_t)thunk_addr2;
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
            if (fn) return dll_get_shim_export_thunk(shims[i].name, fn);
        }
    }

    /* 4. Auto-load DLL from filesystem (recursive dependency resolution)
     *    Skip if we have a registered shim — trust the shim, don't load
     *    conflicting PE DLLs (e.g. real MSVCRT.dll from game directory).
     *    Only auto-load actual .dll files to avoid loading .u packages
     *    or other non-DLL files that waste heap memory. */
    if (dll_name && !find_shim(dll_name) && dll_name[0]) {
        /* Only auto-load if name ends with .dll (case-insensitive) */
        int nlen = 0;
        while (dll_name[nlen]) nlen++;
        int is_dll = (nlen >= 4 &&
            (dll_name[nlen-4] == '.' || dll_name[nlen-4] == '.') &&
            (dll_name[nlen-3] == 'd' || dll_name[nlen-3] == 'D') &&
            (dll_name[nlen-2] == 'l' || dll_name[nlen-2] == 'L') &&
            (dll_name[nlen-1] == 'l' || dll_name[nlen-1] == 'L'));
        LOADED_MODULE *auto_mod = is_dll ? dll_try_load_from_fs(dll_name) : NULL;
        if (auto_mod) {
            PVOID fn = dll_resolve_export(auto_mod, func_name, ordinal, by_ordinal);
            if (fn) return fn;
        }
    }

    /* 5. Compatibility fallback: search unrelated loaded modules only after
     * the DLL named by the import has had a chance to load. */
    if (func_name) {
        ULONG owner_pid = dll_current_owner_pid();
        PVOID fn = dll_resolve_export_any(
            owner_pid, func_name, ordinal, by_ordinal);
        if (fn) return fn;
    }

    /* 6. UT99 appPow mangling fallback: SoftDrv imports float version
     *    (?appPow@@YAMMM@Z) but Core.dll exports double version
     *    (?appPow@@YANNN@Z). Provide a float wrapper. */
    if (func_name && strcmp(func_name, "?appPow@@YAMMM@Z") == 0) {
        /* Search for the double version in loaded modules */
        ULONG owner_pid = dll_current_owner_pid();
        PVOID fn = dll_resolve_export_any(
            owner_pid, "?appPow@@YANNN@Z", 0, FALSE);
        if (fn) {
            serial_puts("[DLL] appPow float->double redirect\n");
            return fn;  /* calling convention compatible (cdecl, x87 float promotion) */
        }
    }

#ifdef PE_LOADER_TRACE
    serial_puts("[DLL] unresolved: ");
    if (dll_name) serial_puts(dll_name);
    serial_puts(" -> ");
    if (func_name) serial_puts(func_name);
    serial_puts("\n");
#endif

    return NULL;
}
