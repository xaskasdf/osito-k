/*
 * OsitoK Windows Compatibility Layer — PE Format Structures
 *
 * Portable Executable (PE/PE32+) format definitions for x86-64.
 * Based on WRK public/sdk/inc/ntimage.h and PE/COFF spec.
 */

#ifndef PE_H
#define PE_H

#include "nttypes.h"

/* ── DOS Header ─────────────────────────────────────────────── */

#define IMAGE_DOS_SIGNATURE     0x5A4D      /* "MZ" */

typedef struct _IMAGE_DOS_HEADER {
    USHORT  e_magic;        /* 0x5A4D */
    USHORT  e_cblp;
    USHORT  e_cp;
    USHORT  e_crlc;
    USHORT  e_cparhdr;
    USHORT  e_minalloc;
    USHORT  e_maxalloc;
    USHORT  e_ss;
    USHORT  e_sp;
    USHORT  e_csum;
    USHORT  e_ip;
    USHORT  e_cs;
    USHORT  e_lfarlc;
    USHORT  e_ovno;
    USHORT  e_res[4];
    USHORT  e_oemid;
    USHORT  e_oeminfo;
    USHORT  e_res2[10];
    LONG    e_lfanew;       /* offset to PE signature */
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

/* ── PE Signature ───────────────────────────────────────────── */

#define IMAGE_NT_SIGNATURE      0x00004550  /* "PE\0\0" */

/* ── COFF File Header ───────────────────────────────────────── */

#define IMAGE_FILE_MACHINE_AMD64    0x8664
#define IMAGE_FILE_MACHINE_I386     0x014C

#define IMAGE_FILE_EXECUTABLE_IMAGE     0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE  0x0020
#define IMAGE_FILE_DLL                  0x2000

typedef struct _IMAGE_FILE_HEADER {
    USHORT  Machine;
    USHORT  NumberOfSections;
    ULONG   TimeDateStamp;
    ULONG   PointerToSymbolTable;
    ULONG   NumberOfSymbols;
    USHORT  SizeOfOptionalHeader;
    USHORT  Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

/* ── Optional Header ───────────────────────────────────────── */

#define IMAGE_NT_OPTIONAL_HDR64_MAGIC   0x020B  /* PE32+ */
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC   0x010B  /* PE32 */

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

typedef struct _IMAGE_DATA_DIRECTORY {
    ULONG   VirtualAddress;
    ULONG   Size;
} IMAGE_DATA_DIRECTORY, *PIMAGE_DATA_DIRECTORY;

/* Data directory indices */
#define IMAGE_DIRECTORY_ENTRY_EXPORT        0
#define IMAGE_DIRECTORY_ENTRY_IMPORT        1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE      2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION     3
#define IMAGE_DIRECTORY_ENTRY_SECURITY      4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC     5
#define IMAGE_DIRECTORY_ENTRY_DEBUG         6
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE  7
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR     8
#define IMAGE_DIRECTORY_ENTRY_TLS           9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG   10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT  11
#define IMAGE_DIRECTORY_ENTRY_IAT           12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT  13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14

typedef struct _IMAGE_OPTIONAL_HEADER64 {
    USHORT  Magic;                      /* 0x020B for PE32+ */
    BYTE    MajorLinkerVersion;
    BYTE    MinorLinkerVersion;
    ULONG   SizeOfCode;
    ULONG   SizeOfInitializedData;
    ULONG   SizeOfUninitializedData;
    ULONG   AddressOfEntryPoint;        /* RVA of entry point */
    ULONG   BaseOfCode;

    /* PE32+ fields (no BaseOfData) */
    ULONGLONG   ImageBase;              /* preferred load address */
    ULONG       SectionAlignment;       /* in-memory section alignment */
    ULONG       FileAlignment;          /* on-disk section alignment */
    USHORT      MajorOperatingSystemVersion;
    USHORT      MinorOperatingSystemVersion;
    USHORT      MajorImageVersion;
    USHORT      MinorImageVersion;
    USHORT      MajorSubsystemVersion;
    USHORT      MinorSubsystemVersion;
    ULONG       Win32VersionValue;
    ULONG       SizeOfImage;            /* total in-memory size */
    ULONG       SizeOfHeaders;          /* headers + section table */
    ULONG       CheckSum;
    USHORT      Subsystem;
    USHORT      DllCharacteristics;
    ULONGLONG   SizeOfStackReserve;
    ULONGLONG   SizeOfStackCommit;
    ULONGLONG   SizeOfHeapReserve;
    ULONGLONG   SizeOfHeapCommit;
    ULONG       LoaderFlags;
    ULONG       NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64, *PIMAGE_OPTIONAL_HEADER64;

/* ── Optional Header (PE32 / i386) ─────────────────────────── */

typedef struct _IMAGE_OPTIONAL_HEADER32 {
    USHORT  Magic;                      /* 0x010B for PE32 */
    BYTE    MajorLinkerVersion;
    BYTE    MinorLinkerVersion;
    ULONG   SizeOfCode;
    ULONG   SizeOfInitializedData;
    ULONG   SizeOfUninitializedData;
    ULONG   AddressOfEntryPoint;
    ULONG   BaseOfCode;
    ULONG   BaseOfData;                 /* PE32 only */

    ULONG   ImageBase;                  /* 32-bit preferred load address */
    ULONG   SectionAlignment;
    ULONG   FileAlignment;
    USHORT  MajorOperatingSystemVersion;
    USHORT  MinorOperatingSystemVersion;
    USHORT  MajorImageVersion;
    USHORT  MinorImageVersion;
    USHORT  MajorSubsystemVersion;
    USHORT  MinorSubsystemVersion;
    ULONG   Win32VersionValue;
    ULONG   SizeOfImage;
    ULONG   SizeOfHeaders;
    ULONG   CheckSum;
    USHORT  Subsystem;
    USHORT  DllCharacteristics;
    ULONG   SizeOfStackReserve;
    ULONG   SizeOfStackCommit;
    ULONG   SizeOfHeapReserve;
    ULONG   SizeOfHeapCommit;
    ULONG   LoaderFlags;
    ULONG   NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32, *PIMAGE_OPTIONAL_HEADER32;

/* Subsystem values */
#define IMAGE_SUBSYSTEM_NATIVE              1
#define IMAGE_SUBSYSTEM_WINDOWS_GUI         2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI         3   /* console app */
#define IMAGE_SUBSYSTEM_EFI_APPLICATION      10

/* DLL characteristics */
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE       0x0040  /* ASLR */
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT          0x0100  /* DEP */
#define IMAGE_DLLCHARACTERISTICS_NO_SEH             0x0400
#define IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE 0x8000

/* ── NT Headers ─────────────────────────────────────────────── */

typedef struct _IMAGE_NT_HEADERS64 {
    ULONG                   Signature;      /* 0x00004550 */
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64, *PIMAGE_NT_HEADERS64;

typedef struct _IMAGE_NT_HEADERS32 {
    ULONG                   Signature;      /* 0x00004550 */
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32, *PIMAGE_NT_HEADERS32;

/* ── Section Header ─────────────────────────────────────────── */

#define IMAGE_SIZEOF_SHORT_NAME 8

typedef struct _IMAGE_SECTION_HEADER {
    BYTE    Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        ULONG PhysicalAddress;
        ULONG VirtualSize;
    } Misc;
    ULONG   VirtualAddress;         /* RVA when loaded */
    ULONG   SizeOfRawData;         /* size on disk */
    ULONG   PointerToRawData;      /* file offset */
    ULONG   PointerToRelocations;
    ULONG   PointerToLinenumbers;
    USHORT  NumberOfRelocations;
    USHORT  NumberOfLinenumbers;
    ULONG   Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

/* Section characteristics */
#define IMAGE_SCN_CNT_CODE                  0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA      0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA    0x00000080
#define IMAGE_SCN_MEM_EXECUTE               0x20000000
#define IMAGE_SCN_MEM_READ                  0x40000000
#define IMAGE_SCN_MEM_WRITE                 0x80000000
#define IMAGE_SCN_MEM_DISCARDABLE           0x02000000

/* ── Import Directory ───────────────────────────────────────── */

typedef struct _IMAGE_IMPORT_DESCRIPTOR {
    union {
        ULONG Characteristics;
        ULONG OriginalFirstThunk;   /* RVA to INT (Import Name Table) */
    };
    ULONG   TimeDateStamp;
    ULONG   ForwarderChain;
    ULONG   Name;                   /* RVA to DLL name (ASCII) */
    ULONG   FirstThunk;             /* RVA to IAT (patched at load) */
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;

/* Import lookup table entry (PE32+: 64-bit) */
typedef struct _IMAGE_THUNK_DATA64 {
    union {
        ULONGLONG ForwarderString;
        ULONGLONG Function;
        ULONGLONG Ordinal;
        ULONGLONG AddressOfData;    /* RVA to IMAGE_IMPORT_BY_NAME */
    } u1;
} IMAGE_THUNK_DATA64, *PIMAGE_THUNK_DATA64;

#define IMAGE_ORDINAL_FLAG64    0x8000000000000000ULL
#define IMAGE_SNAP_BY_ORDINAL64(o)  (((o) & IMAGE_ORDINAL_FLAG64) != 0)
#define IMAGE_ORDINAL64(o)          ((o) & 0xFFFF)

/* Import lookup table entry (PE32: 32-bit) */
typedef struct _IMAGE_THUNK_DATA32 {
    union {
        ULONG ForwarderString;
        ULONG Function;
        ULONG Ordinal;
        ULONG AddressOfData;
    } u1;
} IMAGE_THUNK_DATA32, *PIMAGE_THUNK_DATA32;

#define IMAGE_ORDINAL_FLAG32    0x80000000UL
#define IMAGE_SNAP_BY_ORDINAL32(o)  (((o) & IMAGE_ORDINAL_FLAG32) != 0)
#define IMAGE_ORDINAL32(o)          ((o) & 0xFFFF)

typedef struct _IMAGE_IMPORT_BY_NAME {
    USHORT  Hint;
    char    Name[1];    /* variable-length ASCII name */
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;

/* ── Export Directory ───────────────────────────────────────── */

typedef struct _IMAGE_EXPORT_DIRECTORY {
    ULONG   Characteristics;
    ULONG   TimeDateStamp;
    USHORT  MajorVersion;
    USHORT  MinorVersion;
    ULONG   Name;                   /* RVA to DLL name */
    ULONG   Base;                   /* ordinal base */
    ULONG   NumberOfFunctions;      /* entries in EAT */
    ULONG   NumberOfNames;          /* entries in name table */
    ULONG   AddressOfFunctions;     /* RVA to EAT (ULONG[]) */
    ULONG   AddressOfNames;         /* RVA to name pointers (ULONG[]) */
    ULONG   AddressOfNameOrdinals;  /* RVA to ordinal table (USHORT[]) */
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

/* ── Base Relocations ───────────────────────────────────────── */

typedef struct _IMAGE_BASE_RELOCATION {
    ULONG   VirtualAddress;     /* page RVA */
    ULONG   SizeOfBlock;        /* total size including header */
    /* followed by USHORT TypeOffset[] entries */
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

/* Relocation types (high 4 bits of TypeOffset entry) */
#define IMAGE_REL_BASED_ABSOLUTE    0   /* padding, skip */
#define IMAGE_REL_BASED_HIGH        1
#define IMAGE_REL_BASED_LOW         2
#define IMAGE_REL_BASED_HIGHLOW     3   /* 32-bit fixup (PE32) */
#define IMAGE_REL_BASED_DIR64       10  /* 64-bit fixup (PE32+) */

/* ── TLS Directory ──────────────────────────────────────────── */

typedef struct _IMAGE_TLS_DIRECTORY64 {
    ULONGLONG StartAddressOfRawData;
    ULONGLONG EndAddressOfRawData;
    ULONGLONG AddressOfIndex;       /* PULONG */
    ULONGLONG AddressOfCallBacks;   /* PIMAGE_TLS_CALLBACK* */
    ULONG     SizeOfZeroFill;
    ULONG     Characteristics;
} IMAGE_TLS_DIRECTORY64, *PIMAGE_TLS_DIRECTORY64;

/* ── PE Image Info (returned by loader) ─────────────────────── */

typedef struct _PE_IMAGE_INFO {
    PVOID       ImageBase;          /* actual load address */
    ULONGLONG   PreferredBase;      /* from optional header */
    ULONG       SizeOfImage;
    ULONG       EntryPointRVA;
    PVOID       EntryPoint;         /* ImageBase + EntryPointRVA */
    USHORT      Subsystem;          /* CUI, GUI, native */
    USHORT      DllCharacteristics;
    ULONGLONG   StackReserve;
    ULONGLONG   StackCommit;
    BOOL        IsDLL;
    BOOL        Is32Bit;            /* PE32 (i386) vs PE32+ (x86-64) */
} PE_IMAGE_INFO, *PPE_IMAGE_INFO;

/* ── Loader API ────────────────────────────────────────────── */

NTSTATUS pe_load(const BYTE *file_data, SIZE_T file_size,
                 PPE_IMAGE_INFO info);
void     pe_unload(PPE_IMAGE_INFO info);

/* ── Helper macros ──────────────────────────────────────────── */

/* Get pointer to first section header after NT headers.
 * Works for both 32-bit and 64-bit: FileHeader is at the same offset. */
#define IMAGE_FIRST_SECTION(nt) \
    ((PIMAGE_SECTION_HEADER)((BYTE *)&(nt)->FileHeader + \
        sizeof(IMAGE_FILE_HEADER) + \
        (nt)->FileHeader.SizeOfOptionalHeader))

/* RVA to pointer (given loaded image base) */
#define RVA_TO_PTR(base, rva) ((PVOID)((BYTE *)(base) + (ULONG)(rva)))

#endif /* PE_H */
