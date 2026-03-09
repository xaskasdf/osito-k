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
extern PVOID pe_alloc(PVOID preferred, SIZE_T size);
extern void  pe_free(PVOID addr, SIZE_T size);

/* Debug output */
extern void  pe_log(const char *msg);
extern void  pe_log_hex(const char *prefix, ULONGLONG val);

/* Import resolution: look up a function by DLL name + function name.
 * Returns function pointer, or NULL if not found. */
extern PVOID pe_resolve_import(const char *dll_name, const char *func_name,
                               USHORT ordinal, BOOL by_ordinal);

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
        pe_log("PE: format PE32+ (x86-64)");
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
        pe_log("PE: format PE32 (i386)");
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

        pe_log_hex("PE: mapped section at RVA ", vaddr);
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

    pe_log_hex("PE: applied relocations, delta = ", (ULONGLONG)delta);
    return STATUS_SUCCESS;
}

/* ── Resolve imports (PE32+ / 64-bit thunks) ───────────────── */

static NTSTATUS pe_resolve_imports64(BYTE *image_base,
                                     IMAGE_DATA_DIRECTORY *import_dir)
{
    if (import_dir->VirtualAddress == 0 || import_dir->Size == 0)
        return STATUS_SUCCESS;

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(image_base + import_dir->VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(image_base + desc->Name);
        pe_log("PE: resolving imports from: ");
        pe_log(dll_name);

        PIMAGE_THUNK_DATA64 int_entry = (PIMAGE_THUNK_DATA64)(
            image_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk));
        PIMAGE_THUNK_DATA64 iat_entry =
            (PIMAGE_THUNK_DATA64)(image_base + desc->FirstThunk);

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            PVOID resolved = NULL;

            if (IMAGE_SNAP_BY_ORDINAL64(int_entry->u1.Ordinal)) {
                USHORT ordinal = (USHORT)IMAGE_ORDINAL64(int_entry->u1.Ordinal);
                resolved = pe_resolve_import(dll_name, NULL, ordinal, TRUE);
                pe_log_hex("PE:   ordinal ", ordinal);
            } else {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(image_base + (ULONG)int_entry->u1.AddressOfData);
                resolved = pe_resolve_import(dll_name, name_entry->Name,
                                             name_entry->Hint, FALSE);
                pe_log("PE:   ");
                pe_log(name_entry->Name);
            }

            if (!resolved) {
                pe_log("PE: WARN — unresolved import (stubbed)");
                resolved = NULL;
            }

            iat_entry->u1.Function = (ULONGLONG)resolved;
        }
    }

    return STATUS_SUCCESS;
}

/* ── Resolve imports (PE32 / 32-bit thunks) ────────────────── */

static NTSTATUS pe_resolve_imports32(BYTE *image_base,
                                     IMAGE_DATA_DIRECTORY *import_dir)
{
    if (import_dir->VirtualAddress == 0 || import_dir->Size == 0)
        return STATUS_SUCCESS;

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(image_base + import_dir->VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(image_base + desc->Name);
        pe_log("PE: resolving imports from: ");
        pe_log(dll_name);

        PIMAGE_THUNK_DATA32 int_entry = (PIMAGE_THUNK_DATA32)(
            image_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk));
        PIMAGE_THUNK_DATA32 iat_entry =
            (PIMAGE_THUNK_DATA32)(image_base + desc->FirstThunk);

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            PVOID resolved = NULL;

            if (IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal)) {
                USHORT ordinal = (USHORT)IMAGE_ORDINAL32(int_entry->u1.Ordinal);
                resolved = pe_resolve_import(dll_name, NULL, ordinal, TRUE);
                pe_log_hex("PE:   ordinal ", ordinal);
            } else {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(image_base + int_entry->u1.AddressOfData);
                resolved = pe_resolve_import(dll_name, name_entry->Name,
                                             name_entry->Hint, FALSE);
                pe_log("PE:   ");
                pe_log(name_entry->Name);
            }

            if (!resolved) {
                pe_log("PE: WARN — unresolved import (stubbed)");
                resolved = NULL;
            }

            /* Write resolved address as 32-bit pointer into IAT.
             * For PE32 running in 64-bit host, the shim thunks are 64-bit
             * addresses — we store the low 32 bits. The PE32 code will
             * call through these 32-bit pointers via our thunk wrappers. */
            iat_entry->u1.Function = (ULONG)(ULONG_PTR)resolved;
        }
    }

    return STATUS_SUCCESS;
}

/* ── Main loader entry point ────────────────────────────────── */

NTSTATUS pe_load(const BYTE *file_data, SIZE_T file_size,
                 PPE_IMAGE_INFO info)
{
    NTSTATUS status;

    if (!file_data || !info || file_size < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_PARAMETER;

    /* Step 1: Parse headers */
    PE_PARSED_HEADERS ph;
    status = pe_parse_headers(file_data, file_size, &ph);
    if (!NT_SUCCESS(status))
        return status;

    ULONG size_of_image = ph.SizeOfImage;
    ULONGLONG preferred  = ph.ImageBase;

    pe_log_hex("PE: SizeOfImage = ", size_of_image);
    pe_log_hex("PE: ImageBase   = ", preferred);
    pe_log_hex("PE: EntryPoint  = ", ph.AddressOfEntryPoint);

    /* Step 2: Allocate memory for image */
    BYTE *image_base = (BYTE *)pe_alloc((PVOID)(ULONG_PTR)preferred, size_of_image);
    if (!image_base) {
        /* Try any address if preferred is taken */
        image_base = (BYTE *)pe_alloc(NULL, size_of_image);
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

    /* Step 6: Resolve imports (from mapped image's data directories) */
    /* Re-parse mapped image headers to get relocated data directory */
    PE_PARSED_HEADERS mph;
    status = pe_parse_headers(image_base, size_of_image, &mph);
    if (!NT_SUCCESS(status)) {
        pe_free(image_base, size_of_image);
        return status;
    }

    IMAGE_DATA_DIRECTORY *import_dir = NULL;
    if (mph.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
        import_dir = &mph.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (import_dir) {
        if (ph.Is32Bit)
            status = pe_resolve_imports32(image_base, import_dir);
        else
            status = pe_resolve_imports64(image_base, import_dir);

        if (!NT_SUCCESS(status)) {
            pe_free(image_base, size_of_image);
            return status;
        }
    }

    /* Step 7: Fill in image info */
    info->ImageBase          = image_base;
    info->PreferredBase      = preferred;
    info->SizeOfImage        = size_of_image;
    info->EntryPointRVA      = ph.AddressOfEntryPoint;
    info->EntryPoint         = image_base + ph.AddressOfEntryPoint;
    info->Subsystem          = ph.Subsystem;
    info->DllCharacteristics = ph.DllCharacteristics;
    info->StackReserve       = ph.SizeOfStackReserve;
    info->StackCommit        = ph.SizeOfStackCommit;
    info->IsDLL              = (ph.FileHeader->Characteristics & IMAGE_FILE_DLL) != 0;
    info->Is32Bit            = ph.Is32Bit;

    pe_log_hex("PE: loaded at   = ", (ULONGLONG)(ULONG_PTR)image_base);
    pe_log("PE: load complete");

    return STATUS_SUCCESS;
}

/* ── Unload ─────────────────────────────────────────────────── */

void pe_unload(PPE_IMAGE_INFO info)
{
    if (info && info->ImageBase) {
        pe_free(info->ImageBase, info->SizeOfImage);
        info->ImageBase  = NULL;
        info->EntryPoint = NULL;
    }
}
