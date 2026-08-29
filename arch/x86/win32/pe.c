/*
 * OsitoK Windows Compatibility Layer — PE Loader
 *
 * Loads PE32 (i386) and PE32+ (x86-64) executables into memory:
 *   1. Validate DOS + PE headers
 *   2. Allocate memory for the image (SizeOfImage)
 *   3. Copy headers
 *   4. Map each section to its VirtualAddress
 *   5. Apply base relocations (if loaded != preferred base)
 *   6. Resolve imports against our DLL shims
 *   7. Return entry point address
 *
 * This module is designed to be integrated into OsitoK kernel.
 * External dependencies are abstracted via pe_alloc/pe_free/pe_log.
 */

#include "pe.h"

/* ── External dependencies (provided by kernel) ─────────────── */

/* Memory allocation: allocate `size` bytes at `preferred` address (or any if NULL).
 * Returns allocated address, or NULL on failure. */
extern PVOID pe_alloc(PVOID preferred, SIZE_T size, BOOL is_32bit);
extern void  pe_free(PVOID addr, SIZE_T size);

/* Debug output */
extern void  pe_log(const char *msg);
extern void  pe_log_hex(const char *prefix, ULONGLONG val);
extern void  serial_puts(const char *s); /* for inline logging */
extern void  serial_puthex(uint64_t val, int digits);
extern void  serial_putdec(uint64_t val);
#ifndef TEST_HARNESS
extern void  sched_yield(void);
#endif

/* Import resolution: look up a function by DLL name + function name.
 * Returns function pointer, or NULL if not found. */
extern PVOID pe_resolve_import(const char *dll_name, const char *func_name,
                               USHORT ordinal, BOOL by_ordinal);
extern NTSTATUS sys_NtProtectVirtualMemory(ULONG_PTR *args);
extern DWORD win32_current_process_id(void);

static inline void pe_trace(const char *message)
{
#ifdef PE_LOADER_TRACE
    pe_log(message);
#else
    (void)message;
#endif
}

static inline void pe_trace_hex(const char *prefix, ULONGLONG value)
{
#ifdef PE_LOADER_TRACE
    pe_log_hex(prefix, value);
#else
    (void)prefix;
    (void)value;
#endif
}

/* ── Helpers ────────────────────────────────────────────────── */

static inline void pe_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static inline void pe_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
}

static inline void pe_import_yield(void)
{
#ifndef TEST_HARNESS
    sched_yield();
#endif
}

static int pe_stricmp(const char *a, const char *b)
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

#define PE_IMPORT_DIAG_SLOTS 128

typedef struct {
    volatile uint64_t sequence;
    DWORD process_id;
    ULONG iat_rva;
    USHORT ordinal;
    BOOL by_ordinal;
    BOOL is_32bit;
    char image[64];
    char dll[64];
    char symbol[96];
} PE_IMPORT_DIAGNOSTIC;

static PE_IMPORT_DIAGNOSTIC pe_import_diagnostics[PE_IMPORT_DIAG_SLOTS];
static volatile uint64_t pe_import_diagnostic_sequence;
static volatile BOOL pe_import_strict;

static void pe_copy_string(char *destination, SIZE_T capacity,
                           const char *source)
{
    SIZE_T index = 0;
    if (!capacity) return;
    if (source) {
        while (source[index] && index + 1 < capacity) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = 0;
}

static const char *pe_base_name(const char *name)
{
    const char *base = name ? name : "<image>";
    for (const char *current = base; *current; current++)
        if (*current == '\\' || *current == '/') base = current + 1;
    return base;
}

static void pe_record_unresolved_import(const char *image_name,
                                        const char *dll_name,
                                        const char *symbol, USHORT ordinal,
                                        BOOL by_ordinal, BOOL is_32bit,
                                        ULONG iat_rva)
{
    uint64_t sequence = __atomic_add_fetch(
        &pe_import_diagnostic_sequence, 1, __ATOMIC_RELAXED);
    PE_IMPORT_DIAGNOSTIC *entry =
        &pe_import_diagnostics[(sequence - 1) % PE_IMPORT_DIAG_SLOTS];
    __atomic_store_n(&entry->sequence, 0, __ATOMIC_RELAXED);
    entry->process_id = win32_current_process_id();
    entry->iat_rva = iat_rva;
    entry->ordinal = ordinal;
    entry->by_ordinal = by_ordinal;
    entry->is_32bit = is_32bit;
    pe_copy_string(entry->image, sizeof(entry->image),
                   pe_base_name(image_name));
    pe_copy_string(entry->dll, sizeof(entry->dll), dll_name);
    pe_copy_string(entry->symbol, sizeof(entry->symbol),
                   symbol ? symbol : "");
    __atomic_store_n(&entry->sequence, sequence, __ATOMIC_RELEASE);
}

void pe_import_diagnostics_dump(void)
{
    uint64_t last = __atomic_load_n(&pe_import_diagnostic_sequence,
                                    __ATOMIC_ACQUIRE);
    uint64_t first = last > PE_IMPORT_DIAG_SLOTS
        ? last - PE_IMPORT_DIAG_SLOTS + 1 : 1;
    serial_puts("[PE-IMPORT] strict=");
    serial_putdec(__atomic_load_n(&pe_import_strict, __ATOMIC_RELAXED));
    serial_puts(" recorded=");
    serial_putdec(last);
    serial_puts("\n");
    for (uint64_t sequence = first; sequence <= last; sequence++) {
        PE_IMPORT_DIAGNOSTIC *entry =
            &pe_import_diagnostics[(sequence - 1) % PE_IMPORT_DIAG_SLOTS];
        if (__atomic_load_n(&entry->sequence, __ATOMIC_ACQUIRE) != sequence)
            continue;
        serial_puts("[PE-IMPORT] pid=");
        serial_putdec(entry->process_id);
        serial_puts(" image=");
        serial_puts(entry->image);
        serial_puts(" dll=");
        serial_puts(entry->dll);
        serial_puts(" symbol=");
        if (entry->by_ordinal) {
            serial_puts("#");
            serial_putdec(entry->ordinal);
        } else {
            serial_puts(entry->symbol);
        }
        serial_puts(" iat_rva=0x");
        serial_puthex(entry->iat_rva, 8);
        serial_puts(entry->is_32bit ? " pe32\n" : " pe64\n");
    }
}

void pe_import_diagnostics_clear(void)
{
    for (int i = 0; i < PE_IMPORT_DIAG_SLOTS; i++)
        __atomic_store_n(&pe_import_diagnostics[i].sequence, 0,
                         __ATOMIC_RELAXED);
    __atomic_store_n(&pe_import_diagnostic_sequence, 0, __ATOMIC_RELEASE);
}

void pe_import_set_strict(BOOL enabled)
{
    __atomic_store_n(&pe_import_strict, enabled ? TRUE : FALSE,
                     __ATOMIC_RELEASE);
}

BOOL pe_import_get_strict(void)
{
    return __atomic_load_n(&pe_import_strict, __ATOMIC_ACQUIRE);
}

/* ── Parsed header info (bitness-independent) ──────────────── */

typedef struct {
    PIMAGE_FILE_HEADER  FileHeader;
    IMAGE_DATA_DIRECTORY *DataDirectory;
    ULONG       NumberOfRvaAndSizes;
    ULONGLONG   ImageBase;
    ULONG       SizeOfImage;
    ULONG       SizeOfHeaders;
    ULONG       AddressOfEntryPoint;
    USHORT      Subsystem;
    USHORT      DllCharacteristics;
    ULONGLONG   SizeOfStackReserve;
    ULONGLONG   SizeOfStackCommit;
    BOOL        Is32Bit;
} PE_PARSED_HEADERS;

typedef struct {
    ULONG Size;
    ULONG TimeDateStamp;
    USHORT MajorVersion;
    USHORT MinorVersion;
    ULONG GlobalFlagsClear;
    ULONG GlobalFlagsSet;
    ULONG CriticalSectionDefaultTimeout;
    ULONGLONG DeCommitFreeBlockThreshold;
    ULONGLONG DeCommitTotalFreeThreshold;
    ULONGLONG LockPrefixTable;
    ULONGLONG MaximumAllocationSize;
    ULONGLONG VirtualMemoryThreshold;
    ULONGLONG ProcessAffinityMask;
    ULONG ProcessHeapFlags;
    USHORT CSDVersion;
    USHORT DependentLoadFlags;
    ULONGLONG EditList;
    ULONGLONG SecurityCookie;
    ULONGLONG SEHandlerTable;
    ULONGLONG SEHandlerCount;
    ULONGLONG GuardCFCheckFunctionPointer;
    ULONGLONG GuardCFDispatchFunctionPointer;
    ULONGLONG GuardCFFunctionTable;
    ULONGLONG GuardCFFunctionCount;
    ULONG GuardFlags;
} PE_LOAD_CONFIG_CFG64;

_Static_assert(__builtin_offsetof(PE_LOAD_CONFIG_CFG64,
                                  GuardCFCheckFunctionPointer) == 0x70,
               "PE64 CFG check slot offset");
_Static_assert(__builtin_offsetof(PE_LOAD_CONFIG_CFG64,
                                  GuardCFDispatchFunctionPointer) == 0x78,
               "PE64 CFG dispatch slot offset");

extern void pe_guard_check_icall(void);
extern void pe_guard_dispatch_icall(void);

__asm__(
    ".text\n"
    ".globl pe_guard_check_icall\n"
    "pe_guard_check_icall:\n"
    "ret\n"
    ".globl pe_guard_dispatch_icall\n"
    "pe_guard_dispatch_icall:\n"
    "jmp *%rax\n"
);

/* ── Validation & header parsing ───────────────────────────── */

static NTSTATUS pe_parse_headers(const BYTE *file_data, SIZE_T file_size,
                                  PE_PARSED_HEADERS *out)
{
    if (file_size < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)file_data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        pe_log("PE: bad DOS signature");
        return STATUS_INVALID_PARAMETER;
    }

    /* Ensure e_lfanew is within bounds (check against smaller header first) */
    if ((ULONG)dos->e_lfanew + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER) > file_size) {
        pe_log("PE: e_lfanew out of bounds");
        return STATUS_INVALID_PARAMETER;
    }

    /* Read signature and file header (same layout for PE32 and PE32+) */
    BYTE *nt_base = (BYTE *)(file_data + dos->e_lfanew);
    ULONG signature = *(ULONG *)nt_base;
    if (signature != IMAGE_NT_SIGNATURE) {
        pe_log("PE: bad NT signature");
        return STATUS_INVALID_PARAMETER;
    }

    PIMAGE_FILE_HEADER fh = (PIMAGE_FILE_HEADER)(nt_base + sizeof(ULONG));
    out->FileHeader = fh;

    /* Read optional header magic to determine bitness */
    USHORT magic = *(USHORT *)(nt_base + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER));

    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        /* PE32+ (x86-64) */
        if (fh->Machine != IMAGE_FILE_MACHINE_AMD64) {
            pe_log("PE: PE32+ but not AMD64 machine");
            return STATUS_NOT_SUPPORTED;
        }
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)nt_base;
        out->Is32Bit             = FALSE;
        out->ImageBase           = nt->OptionalHeader.ImageBase;
        out->SizeOfImage         = nt->OptionalHeader.SizeOfImage;
        out->SizeOfHeaders       = nt->OptionalHeader.SizeOfHeaders;
        out->AddressOfEntryPoint = nt->OptionalHeader.AddressOfEntryPoint;
        out->Subsystem           = nt->OptionalHeader.Subsystem;
        out->DllCharacteristics  = nt->OptionalHeader.DllCharacteristics;
        out->SizeOfStackReserve  = nt->OptionalHeader.SizeOfStackReserve;
        out->SizeOfStackCommit   = nt->OptionalHeader.SizeOfStackCommit;
        out->NumberOfRvaAndSizes = nt->OptionalHeader.NumberOfRvaAndSizes;
        out->DataDirectory       = nt->OptionalHeader.DataDirectory;
        pe_trace("PE: format PE32+ (x86-64)");
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        /* PE32 (i386) */
        if (fh->Machine != IMAGE_FILE_MACHINE_I386) {
            pe_log("PE: PE32 but not i386 machine");
            return STATUS_NOT_SUPPORTED;
        }
        PIMAGE_NT_HEADERS32 nt = (PIMAGE_NT_HEADERS32)nt_base;
        out->Is32Bit             = TRUE;
        out->ImageBase           = nt->OptionalHeader.ImageBase;
        out->SizeOfImage         = nt->OptionalHeader.SizeOfImage;
        out->SizeOfHeaders       = nt->OptionalHeader.SizeOfHeaders;
        out->AddressOfEntryPoint = nt->OptionalHeader.AddressOfEntryPoint;
        out->Subsystem           = nt->OptionalHeader.Subsystem;
        out->DllCharacteristics  = nt->OptionalHeader.DllCharacteristics;
        out->SizeOfStackReserve  = nt->OptionalHeader.SizeOfStackReserve;
        out->SizeOfStackCommit   = nt->OptionalHeader.SizeOfStackCommit;
        out->NumberOfRvaAndSizes = nt->OptionalHeader.NumberOfRvaAndSizes;
        out->DataDirectory       = nt->OptionalHeader.DataDirectory;
        pe_trace("PE: format PE32 (i386)");
    } else {
        pe_log_hex("PE: unknown optional header magic ", magic);
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

/* ── Get first section header (works for both) ─────────────── */

static PIMAGE_SECTION_HEADER pe_first_section(const BYTE *file_data)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)file_data;
    BYTE *nt_base = (BYTE *)(file_data + dos->e_lfanew);
    PIMAGE_FILE_HEADER fh = (PIMAGE_FILE_HEADER)(nt_base + sizeof(ULONG));
    return (PIMAGE_SECTION_HEADER)((BYTE *)fh + sizeof(IMAGE_FILE_HEADER) +
                                    fh->SizeOfOptionalHeader);
}

/* ── Map sections ───────────────────────────────────────────── */

static NTSTATUS pe_map_sections(BYTE *image_base,
                                const BYTE *file_data, SIZE_T file_size,
                                PE_PARSED_HEADERS *ph)
{
    PIMAGE_SECTION_HEADER sec = pe_first_section(file_data);
    USHORT num_sections = ph->FileHeader->NumberOfSections;

    for (USHORT i = 0; i < num_sections; i++) {
        ULONG vaddr    = sec[i].VirtualAddress;
        ULONG vsize    = sec[i].Misc.VirtualSize;
        ULONG raw_off  = sec[i].PointerToRawData;
        ULONG raw_size = sec[i].SizeOfRawData;

        /* Zero the entire virtual range first */
        if (vsize > 0)
            pe_memset(image_base + vaddr, 0, vsize);

        /* Copy raw data */
        ULONG copy_size = raw_size < vsize ? raw_size : vsize;
        if (raw_off + copy_size > file_size) {
            pe_log("PE: section raw data out of bounds");
            return STATUS_INVALID_PARAMETER;
        }

        if (copy_size > 0)
            pe_memcpy(image_base + vaddr, file_data + raw_off, copy_size);

        pe_trace_hex("PE: mapped section at RVA ", vaddr);
    }

    return STATUS_SUCCESS;
}

/* ── Apply base relocations ─────────────────────────────────── */

static NTSTATUS pe_apply_relocations(BYTE *image_base,
                                     PE_PARSED_HEADERS *ph,
                                     LONGLONG delta)
{
    if (delta == 0)
        return STATUS_SUCCESS;  /* loaded at preferred base */

    if (ph->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC)
        goto no_reloc;

    IMAGE_DATA_DIRECTORY *reloc_dir =
        &ph->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

    if (reloc_dir->VirtualAddress == 0 || reloc_dir->Size == 0) {
no_reloc:
        if (delta != 0) {
            pe_log("PE: needs relocation but no .reloc section");
            return STATUS_CONFLICTING_ADDRESSES;
        }
        return STATUS_SUCCESS;
    }

    BYTE *reloc_base = image_base + reloc_dir->VirtualAddress;
    BYTE *reloc_end  = reloc_base + reloc_dir->Size;
    BYTE *ptr = reloc_base;

    while (ptr < reloc_end) {
        PIMAGE_BASE_RELOCATION block = (PIMAGE_BASE_RELOCATION)ptr;
        if (block->SizeOfBlock == 0)
            break;

        ULONG page_rva = block->VirtualAddress;
        ULONG num_entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
        USHORT *entries = (USHORT *)(block + 1);

        for (ULONG i = 0; i < num_entries; i++) {
            USHORT type   = entries[i] >> 12;
            USHORT offset = entries[i] & 0x0FFF;

            switch (type) {
            case IMAGE_REL_BASED_ABSOLUTE:
                /* Padding, skip */
                break;

            case IMAGE_REL_BASED_DIR64: {
                /* 64-bit fixup */
                ULONGLONG *fixup = (ULONGLONG *)(image_base + page_rva + offset);
                *fixup += (ULONGLONG)delta;
                break;
            }

            case IMAGE_REL_BASED_HIGHLOW: {
                /* 32-bit fixup */
                ULONG *fixup = (ULONG *)(image_base + page_rva + offset);
                *fixup += (ULONG)delta;
                break;
            }

            default:
                pe_log_hex("PE: unsupported reloc type ", type);
                break;
            }
        }

        ptr += block->SizeOfBlock;
    }

    pe_trace_hex("PE: applied relocations, delta = ", (ULONGLONG)delta);
    return STATUS_SUCCESS;
}

/* ── Resolve imports (PE32+ / 64-bit thunks) ───────────────── */

static NTSTATUS pe_resolve_imports64(BYTE *image_base,
                                     IMAGE_DATA_DIRECTORY *import_dir,
                                     const char *image_name,
                                     ULONG *unresolved_count)
{
    if (import_dir->VirtualAddress == 0 || import_dir->Size == 0)
        return STATUS_SUCCESS;

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(image_base + import_dir->VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(image_base + desc->Name);
        pe_trace("PE: resolving imports from: ");
        pe_trace(dll_name);

        PIMAGE_THUNK_DATA64 int_entry = (PIMAGE_THUNK_DATA64)(
            image_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk));
        PIMAGE_THUNK_DATA64 iat_entry =
            (PIMAGE_THUNK_DATA64)(image_base + desc->FirstThunk);
        ULONG import_count = 0;

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            PVOID resolved = NULL;

            if (IMAGE_SNAP_BY_ORDINAL64(int_entry->u1.Ordinal)) {
                USHORT ordinal = (USHORT)IMAGE_ORDINAL64(int_entry->u1.Ordinal);
                resolved = pe_resolve_import(dll_name, NULL, ordinal, TRUE);
                pe_trace_hex("PE:   ordinal ", ordinal);
            } else {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(image_base + (ULONG)int_entry->u1.AddressOfData);
                resolved = pe_resolve_import(dll_name, name_entry->Name,
                                             name_entry->Hint, FALSE);
                pe_trace("PE:   ");
                pe_trace(name_entry->Name);
            }

            if (!resolved) {
                BOOL by_ordinal =
                    IMAGE_SNAP_BY_ORDINAL64(int_entry->u1.Ordinal);
                USHORT ordinal = by_ordinal
                    ? (USHORT)IMAGE_ORDINAL64(int_entry->u1.Ordinal) : 0;
                const char *symbol = NULL;
                if (!by_ordinal) {
                    PIMAGE_IMPORT_BY_NAME n = (PIMAGE_IMPORT_BY_NAME)(
                        image_base + (ULONG)int_entry->u1.AddressOfData);
                    symbol = n->Name;
                }
                pe_record_unresolved_import(
                    image_name, dll_name, symbol, ordinal, by_ordinal, FALSE,
                    (ULONG)((BYTE *)iat_entry - image_base));
                (*unresolved_count)++;
#ifdef PE_LOADER_TRACE
                serial_puts("PE: WARN unresolved: ");
                serial_puts(dll_name);
                serial_puts("!");
                if (!IMAGE_SNAP_BY_ORDINAL64(int_entry->u1.Ordinal)) {
                    PIMAGE_IMPORT_BY_NAME n =
                        (PIMAGE_IMPORT_BY_NAME)(image_base + (ULONG)int_entry->u1.AddressOfData);
                    serial_puts(n->Name);
                } else {
                    serial_puts("ordinal ");
                    serial_puthex(IMAGE_ORDINAL64(int_entry->u1.Ordinal), 4);
                }
                serial_puts("\n");
#endif
                resolved = NULL;
            }

            iat_entry->u1.Function = (ULONGLONG)resolved;
            if ((++import_count & 63U) == 0)
                pe_import_yield();
        }
    }

    return STATUS_SUCCESS;
}

/* ── Resolve imports (PE32 / 32-bit thunks) ────────────────── */

/* Scan section headers for a section named ".idata" and return its RVA.
 * Used as fallback when the PE data directory doesn't point to imports. */
static ULONG pe_find_idata_rva(BYTE *image_base)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)image_base;
    ULONG pe_off = dos->e_lfanew;
    PIMAGE_FILE_HEADER fh = (PIMAGE_FILE_HEADER)(image_base + pe_off + 4);
    ULONG opt_size = fh->SizeOfOptionalHeader;
    BYTE *sec_start = image_base + pe_off + 4 + sizeof(IMAGE_FILE_HEADER) + opt_size;

    for (USHORT i = 0; i < fh->NumberOfSections; i++) {
        PIMAGE_SECTION_HEADER sh = (PIMAGE_SECTION_HEADER)(sec_start + i * sizeof(IMAGE_SECTION_HEADER));
        if (sh->Name[0] == '.' && sh->Name[1] == 'i' && sh->Name[2] == 'd' &&
            sh->Name[3] == 'a' && sh->Name[4] == 't' && sh->Name[5] == 'a') {
            /* Verify the section looks like it contains import descriptors:
             * first entry should have non-zero Name and FirstThunk fields. */
            PIMAGE_IMPORT_DESCRIPTOR probe =
                (PIMAGE_IMPORT_DESCRIPTOR)(image_base + sh->VirtualAddress);
            if (probe->Name != 0 && probe->FirstThunk != 0) {
                pe_trace_hex("PE: .idata fallback at RVA ",
                             sh->VirtualAddress);
                return sh->VirtualAddress;
            }
        }
    }
    return 0;
}

static NTSTATUS pe_resolve_imports32(BYTE *image_base,
                                     IMAGE_DATA_DIRECTORY *import_dir,
                                     const char *image_name,
                                     ULONG *unresolved_count)
{
    ULONG import_rva = import_dir->VirtualAddress;

    /* Fallback: if data directory doesn't point to imports, scan for .idata
     * section directly.  Unreal Engine 1 DLLs (Engine.dll, etc.) have valid
     * import descriptors in .idata but set the data directory entry to 0. */
    if (import_rva == 0 || import_dir->Size == 0) {
        import_rva = pe_find_idata_rva(image_base);
        if (import_rva == 0)
            return STATUS_SUCCESS;
    }

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(image_base + import_rva);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(image_base + desc->Name);
        pe_trace("PE: resolving imports from: ");
        pe_trace(dll_name);

        PIMAGE_THUNK_DATA32 int_entry = (PIMAGE_THUNK_DATA32)(
            image_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk));
        PIMAGE_THUNK_DATA32 iat_entry =
            (PIMAGE_THUNK_DATA32)(image_base + desc->FirstThunk);
        ULONG import_count = 0;

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            PVOID resolved = NULL;

            if (IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal)) {
                USHORT ordinal = (USHORT)IMAGE_ORDINAL32(int_entry->u1.Ordinal);
                resolved = pe_resolve_import(dll_name, NULL, ordinal, TRUE);
                pe_trace_hex("PE:   ordinal ", ordinal);
            } else {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(image_base + int_entry->u1.AddressOfData);
                resolved = pe_resolve_import(dll_name, name_entry->Name,
                                             name_entry->Hint, FALSE);
                pe_trace("PE:   ");
                pe_trace(name_entry->Name);
            }

            if (!resolved) {
                BOOL by_ordinal =
                    IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal);
                USHORT ordinal = by_ordinal
                    ? (USHORT)IMAGE_ORDINAL32(int_entry->u1.Ordinal) : 0;
                const char *symbol = NULL;
                if (!by_ordinal) {
                    PIMAGE_IMPORT_BY_NAME n = (PIMAGE_IMPORT_BY_NAME)(
                        image_base + int_entry->u1.AddressOfData);
                    symbol = n->Name;
                }
                pe_record_unresolved_import(
                    image_name, dll_name, symbol, ordinal, by_ordinal, TRUE,
                    (ULONG)((BYTE *)iat_entry - image_base));
                (*unresolved_count)++;
#ifdef PE_LOADER_TRACE
                serial_puts("PE: WARN unresolved: ");
                serial_puts(dll_name);
                serial_puts("!");
                if (!IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal)) {
                    PIMAGE_IMPORT_BY_NAME n =
                        (PIMAGE_IMPORT_BY_NAME)(image_base + int_entry->u1.AddressOfData);
                    serial_puts(n->Name);
                }
                serial_puts("\n");
#endif
                resolved = NULL;
            }

            /* Write resolved address as 32-bit pointer into IAT.
             * For PE32 running in 64-bit host, the shim thunks are 64-bit
             * addresses — we store the low 32 bits. The PE32 code will
             * call through these 32-bit pointers via our thunk wrappers. */
            iat_entry->u1.Function = (ULONG)(ULONG_PTR)resolved;
            if ((++import_count & 63U) == 0)
                pe_import_yield();
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS pe_initialize_cfg64(BYTE *image_base,
                                    PE_PARSED_HEADERS *ph)
{
    if (ph->Is32Bit ||
        ph->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
        return STATUS_SUCCESS;

    IMAGE_DATA_DIRECTORY *dir =
        &ph->DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    SIZE_T cfg_fields = __builtin_offsetof(PE_LOAD_CONFIG_CFG64, GuardFlags) +
                        sizeof(ULONG);
    if (!dir->VirtualAddress || dir->Size < cfg_fields)
        return STATUS_SUCCESS;
    if (dir->VirtualAddress > ph->SizeOfImage ||
        cfg_fields > ph->SizeOfImage - dir->VirtualAddress)
        return STATUS_INVALID_PARAMETER;

    PE_LOAD_CONFIG_CFG64 *cfg =
        (PE_LOAD_CONFIG_CFG64 *)(image_base + dir->VirtualAddress);
    if (cfg->Size < cfg_fields || !(cfg->GuardFlags & 0x100))
        return STATUS_SUCCESS;

    ULONGLONG image_start = (ULONGLONG)(ULONG_PTR)image_base;
    ULONGLONG image_end = image_start + ph->SizeOfImage;
    ULONGLONG slots[2] = {
        cfg->GuardCFCheckFunctionPointer,
        cfg->GuardCFDispatchFunctionPointer,
    };
    PVOID targets[2] = {
        (PVOID)pe_guard_check_icall,
        (PVOID)pe_guard_dispatch_icall,
    };

    for (int i = 0; i < 2; i++) {
        if (!slots[i]) continue;
        if (slots[i] < image_start || slots[i] > image_end - sizeof(PVOID))
            return STATUS_INVALID_PARAMETER;
        *(PVOID *)(ULONG_PTR)slots[i] = targets[i];
    }

    /* ponytail: pass-through CFG until process-wide GFIDS are tracked. */
    return STATUS_SUCCESS;
}

/* ── Main loader entry point ────────────────────────────────── */

static ULONG pe_characteristics_to_protection(ULONG characteristics)
{
    BOOL executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    BOOL readable = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    BOOL writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;

    if (executable) {
        if (writable) return PAGE_EXECUTE_READWRITE;
        if (readable) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (writable) return PAGE_READWRITE;
    if (readable) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static NTSTATUS pe_protect_image_run(BYTE *image_base, uint64_t page_rva,
                                     SIZE_T size, ULONG protection)
{
    PVOID base = image_base + page_rva;
    ULONG old_protection = 0;
    ULONG_PTR args[5] = {
        (ULONG_PTR)NT_CURRENT_PROCESS,
        (ULONG_PTR)&base,
        (ULONG_PTR)&size,
        (ULONG_PTR)protection,
        (ULONG_PTR)&old_protection,
    };
    return sys_NtProtectVirtualMemory(args);
}

static NTSTATUS pe_page_protection(const PE_PARSED_HEADERS *ph,
                                   const IMAGE_SECTION_HEADER *sections,
                                   uint64_t image_size, uint64_t headers_end,
                                   uint64_t page_rva, ULONG *protection)
{
    uint64_t page_end = page_rva + 4096;
    BOOL mapped = page_rva < headers_end;
    ULONG characteristics = mapped ? IMAGE_SCN_MEM_READ : 0;

    if (page_end > image_size) page_end = image_size;
    for (USHORT i = 0; i < ph->FileHeader->NumberOfSections; i++) {
        uint64_t section_start = sections[i].VirtualAddress;
        uint64_t section_size = sections[i].Misc.VirtualSize;
        if (!section_size) section_size = sections[i].SizeOfRawData;
        if (!section_size) continue;
        if (section_start >= image_size ||
            section_size > image_size - section_start)
            return STATUS_INVALID_PARAMETER;
        uint64_t section_end = section_start + section_size;
        if (page_rva < section_end && page_end > section_start) {
            mapped = TRUE;
            characteristics |= sections[i].Characteristics &
                (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE |
                 IMAGE_SCN_MEM_EXECUTE);
        }
    }

    *protection = mapped
        ? pe_characteristics_to_protection(characteristics)
        : PAGE_NOACCESS;
    return STATUS_SUCCESS;
}

NTSTATUS pe_finalize_image_protections(PPE_IMAGE_INFO info)
{
    if (!info || !info->ImageBase || !info->SizeOfImage)
        return STATUS_INVALID_PARAMETER;

    BYTE *image_base = (BYTE *)info->ImageBase;
    PE_PARSED_HEADERS ph;
    NTSTATUS status = pe_parse_headers(image_base, info->SizeOfImage, &ph);
    if (!NT_SUCCESS(status) || !ph.SizeOfImage ||
        ph.SizeOfImage != info->SizeOfImage)
        return STATUS_INVALID_PARAMETER;

    IMAGE_SECTION_HEADER *sections = pe_first_section(image_base);
    uint64_t section_offset = (uint64_t)((BYTE *)sections - image_base);
    uint64_t section_bytes =
        (uint64_t)ph.FileHeader->NumberOfSections * sizeof(*sections);
    if (ph.FileHeader->NumberOfSections > 96 ||
        section_offset > info->SizeOfImage ||
        section_bytes > info->SizeOfImage - section_offset)
        return STATUS_INVALID_PARAMETER;

    uint64_t headers_end = ph.SizeOfHeaders;
    uint64_t section_table_end = section_offset + section_bytes;
    if (headers_end < section_table_end) headers_end = section_table_end;
    if (headers_end > info->SizeOfImage)
        return STATUS_INVALID_PARAMETER;

    uint64_t page_count = (info->SizeOfImage + 0xFFFULL) / 4096;
    ULONG run_protection = 0;
    uint64_t run_start = 0;
    uint64_t run_count = 0;

    for (uint64_t page = 0; page <= page_count; page++) {
        ULONG protection = 0;
        if (page < page_count) {
            status = pe_page_protection(&ph, sections, info->SizeOfImage,
                                        headers_end, page * 4096,
                                        &protection);
            if (!NT_SUCCESS(status)) return status;
        }

        if (page == 0) {
            run_protection = protection;
            continue;
        }
        if (page < page_count && protection == run_protection)
            continue;

        status = pe_protect_image_run(image_base, run_start * 4096,
                                      (page - run_start) * 4096,
                                      run_protection);
        if (!NT_SUCCESS(status)) return status;
        run_count++;
        run_start = page;
        run_protection = protection;
    }

    if (info->SizeOfImage >= 64ULL * 1024 * 1024) {
        serial_puts("[PE-PROTECT] image=0x");
        serial_puthex((uint64_t)(ULONG_PTR)info->ImageBase, 16);
        serial_puts(" pages=");
        serial_putdec(page_count);
        serial_puts(" runs=");
        serial_putdec(run_count);
        serial_puts("\n");
    }
    return STATUS_SUCCESS;
}

static NTSTATUS pe_test_protect(PVOID address, ULONG protection,
                                ULONG *old_protection)
{
    PVOID base = address;
    SIZE_T size = 1;
    ULONG_PTR args[5] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)&base,
        (ULONG_PTR)&size, (ULONG_PTR)protection,
        (ULONG_PTR)old_protection,
    };
    return sys_NtProtectVirtualMemory(args);
}

static void pe_test_expect(BOOL condition, const char *name,
                           int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[PEPROT-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int pe_protection_selftest(void)
{
    const SIZE_T image_size = 4 * 4096;
    int checks = 0;
    int failures = 0;
    BYTE *base = (BYTE *)pe_alloc(NULL, image_size, FALSE);
    pe_test_expect(base != NULL, "allocate synthetic image", &checks,
                   &failures);
    if (!base) return failures;

    pe_memset(base, 0, image_size);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS64 *nt =
        (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = (ULONGLONG)(ULONG_PTR)base;
    nt->OptionalHeader.SectionAlignment = 4096;
    nt->OptionalHeader.FileAlignment = 512;
    nt->OptionalHeader.SizeOfImage = image_size;
    nt->OptionalHeader.SizeOfHeaders = 4096;
    nt->OptionalHeader.NumberOfRvaAndSizes =
        IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
    sections[0].Misc.VirtualSize = 4096;
    sections[0].VirtualAddress = 4096;
    sections[0].Characteristics = IMAGE_SCN_CNT_CODE |
                                  IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_EXECUTE;
    sections[1].Misc.VirtualSize = 4096;
    sections[1].VirtualAddress = 8192;
    sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
                                  IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_WRITE;

    PE_IMAGE_INFO info;
    pe_memset(&info, 0, sizeof(info));
    info.ImageBase = base;
    info.SizeOfImage = image_size;
    NTSTATUS status = pe_finalize_image_protections(&info);
    pe_test_expect(NT_SUCCESS(status), "finalize section protections",
                   &checks, &failures);

    ULONG old_protection = 0;
    status = pe_test_protect(base, PAGE_READONLY, &old_protection);
    pe_test_expect(NT_SUCCESS(status) && old_protection == PAGE_READONLY,
                   "headers are read-only and NX", &checks, &failures);
    status = pe_test_protect(base + 4096, PAGE_EXECUTE_READ,
                             &old_protection);
    pe_test_expect(NT_SUCCESS(status) &&
                   old_protection == PAGE_EXECUTE_READ,
                   "code is executable and read-only", &checks, &failures);
    status = pe_test_protect(base + 8192, PAGE_READONLY, &old_protection);
    pe_test_expect(NT_SUCCESS(status) && old_protection == PAGE_READWRITE,
                   "data reports read-write and NX", &checks, &failures);
    status = pe_test_protect(base + 8192, PAGE_READWRITE, &old_protection);
    pe_test_expect(NT_SUCCESS(status) && old_protection == PAGE_READONLY,
                   "data protection transition is preserved", &checks,
                   &failures);
    status = pe_test_protect(base + 12288, PAGE_NOACCESS,
                             &old_protection);
    pe_test_expect(NT_SUCCESS(status) && old_protection == PAGE_NOACCESS,
                   "unassigned image pages are inaccessible", &checks,
                   &failures);

    pe_free(base, image_size);
    serial_puts("[PEPROT-TEST] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec(failures);
    serial_puts("\n");
    return failures;
}

static void pe_abort_load(PPE_IMAGE_INFO info, BYTE *image_base,
                          ULONG image_size, BOOL published_executable)
{
    if (published_executable) {
        extern void win32_publish_current_image_base(PVOID image_base);
        win32_publish_current_image_base(NULL);
    }
    if (image_base) pe_free(image_base, image_size);
    if (info) pe_memset(info, 0, sizeof(*info));
}

NTSTATUS pe_load_named(const BYTE *file_data, SIZE_T file_size,
                       PPE_IMAGE_INFO info, const char *image_name)
{
    NTSTATUS status;

    if (info) pe_memset(info, 0, sizeof(*info));
    if (!file_data || !info || file_size < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_PARAMETER;

    /* Step 1: Parse headers */
    PE_PARSED_HEADERS ph;
    status = pe_parse_headers(file_data, file_size, &ph);
    if (!NT_SUCCESS(status))
        return status;

    ULONG size_of_image = ph.SizeOfImage;
    ULONGLONG preferred  = ph.ImageBase;

    pe_trace_hex("PE: SizeOfImage = ", size_of_image);
    pe_trace_hex("PE: ImageBase   = ", preferred);
    pe_trace_hex("PE: EntryPoint  = ", ph.AddressOfEntryPoint);

    /* Step 2: Allocate memory for image */
    BYTE *image_base = (BYTE *)pe_alloc((PVOID)(ULONG_PTR)preferred,
                                        size_of_image, ph.Is32Bit);
    if (!image_base) {
        /* Try any address if preferred is taken */
        image_base = (BYTE *)pe_alloc(NULL, size_of_image, ph.Is32Bit);
        if (!image_base) {
            pe_log("PE: failed to allocate image memory");
            return STATUS_NO_MEMORY;
        }
    }

    pe_memset(image_base, 0, size_of_image);

    /* Step 3: Copy headers */
    ULONG headers_size = ph.SizeOfHeaders;
    if (headers_size > file_size) headers_size = (ULONG)file_size;
    pe_memcpy(image_base, file_data, headers_size);

    /* Step 4: Map sections */
    status = pe_map_sections(image_base, file_data, file_size, &ph);
    if (!NT_SUCCESS(status)) {
        pe_free(image_base, size_of_image);
        return status;
    }

    /* Step 5: Apply relocations */
    LONGLONG delta = (LONGLONG)((ULONGLONG)(ULONG_PTR)image_base - preferred);
    status = pe_apply_relocations(image_base, &ph, delta);
    if (!NT_SUCCESS(status)) {
        pe_free(image_base, size_of_image);
        return status;
    }

    /* NT inserts an LDR entry after mapping/relocation and before walking
     * imports. Publishing the complete mapped identity here lets a circular
     * A -> B -> A dependency resolve A's exports while A is still loading. */
    info->ImageBase          = image_base;
    info->PreferredBase      = preferred;
    info->SizeOfImage        = size_of_image;
    info->EntryPointRVA      = ph.AddressOfEntryPoint;
    info->EntryPoint         = image_base + ph.AddressOfEntryPoint;
    info->Subsystem          = ph.Subsystem;
    info->DllCharacteristics = ph.DllCharacteristics;
    info->StackReserve       = ph.SizeOfStackReserve;
    info->StackCommit        = ph.SizeOfStackCommit;
    info->IsDLL =
        (ph.FileHeader->Characteristics & IMAGE_FILE_DLL) != 0;
    info->Is32Bit = ph.Is32Bit;

    /* GetModuleHandle(NULL) and PEB readers are valid from DLL entry points.
     * Publish an executable's actual mapped base before resolving imports,
     * because recursive DLL loads can invoke DllMain during that step. */
    BOOL published_executable =
        (ph.FileHeader->Characteristics & IMAGE_FILE_DLL) == 0;
    if (published_executable) {
        extern void win32_publish_current_image_base(PVOID image_base);
        win32_publish_current_image_base(image_base);
    }

    /* Step 6: Resolve imports (from mapped image's data directories) */
    /* Re-parse mapped image headers to get relocated data directory */
    PE_PARSED_HEADERS mph;
    status = pe_parse_headers(image_base, size_of_image, &mph);
    if (!NT_SUCCESS(status)) {
        pe_abort_load(info, image_base, size_of_image, published_executable);
        return status;
    }

    IMAGE_DATA_DIRECTORY *import_dir = NULL;
    if (mph.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
        import_dir = &mph.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    ULONG unresolved_count = 0;
    if (import_dir) {
        if (ph.Is32Bit)
            status = pe_resolve_imports32(image_base, import_dir, image_name,
                                          &unresolved_count);
        else
            status = pe_resolve_imports64(image_base, import_dir, image_name,
                                          &unresolved_count);

        if (!NT_SUCCESS(status)) {
            pe_abort_load(info, image_base, size_of_image,
                          published_executable);
            return status;
        }
    }

    if (unresolved_count) {
        serial_puts("[PE-IMPORT] image=");
        serial_puts(pe_base_name(image_name));
        serial_puts(" unresolved=");
        serial_putdec(unresolved_count);
        serial_puts(pe_import_get_strict()
            ? " policy=strict\n" : " policy=compat\n");
        if (pe_import_get_strict()) {
            pe_abort_load(info, image_base, size_of_image,
                          published_executable);
            return STATUS_PROCEDURE_NOT_FOUND;
        }
    }

    status = pe_initialize_cfg64(image_base, &mph);
    if (!NT_SUCCESS(status)) {
        pe_abort_load(info, image_base, size_of_image, published_executable);
        return status;
    }

    pe_trace_hex("PE: loaded at   = ", (ULONGLONG)(ULONG_PTR)image_base);
    pe_trace("PE: load complete");

    return STATUS_SUCCESS;
}

NTSTATUS pe_load(const BYTE *file_data, SIZE_T file_size,
                 PPE_IMAGE_INFO info)
{
    return pe_load_named(file_data, file_size, info, "<image>");
}

/* ── Unload ─────────────────────────────────────────────────── */

void pe_unload(PPE_IMAGE_INFO info)
{
    if (info && info->ImageBase) {
        /* Win32 children currently share the kernel address space. Async
         * callbacks and shared objects can therefore retain pointers into an
         * image after process teardown. Reusing those physical pages for a
         * kernel stack caused live scheduler frames to be overwritten. Keep
         * successfully loaded images pinned until Win32 has per-process page
         * tables; pe_load error paths still release unpublished images. */
        info->ImageBase  = NULL;
        info->EntryPoint = NULL;
    }
}
