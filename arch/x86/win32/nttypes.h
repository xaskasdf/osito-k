/*
 * OsitoK Windows Compatibility Layer — NT Base Types
 *
 * Core type definitions for the NT-compatible syscall interface.
 * Based on WRK public/sdk/inc/ntdef.h, ntstatus.h, ntioapi.h.
 *
 * All types use the exact same layout and sizes as Windows x86-64.
 */

#ifndef NTTYPES_H
#define NTTYPES_H

#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stdint.h>
#endif

/* ── Calling convention attributes ──────────────────────────── */
/*
 * PE executables use Microsoft x64 ABI (RCX, RDX, R8, R9).
 * Our shim functions compiled with GCC use System V ABI (RDI, RSI, RDX, RCX).
 * Mark exported functions with WINAPI/NTAPI so GCC generates correct prologues.
 */
#ifdef __GNUC__
#define WINAPI __attribute__((ms_abi))
#define NTAPI  __attribute__((ms_abi))
#else
#define WINAPI
#define NTAPI
#endif

/* ── Fundamental types ──────────────────────────────────────── */

typedef int32_t     LONG;
typedef uint32_t    ULONG;
typedef uint32_t    DWORD;
typedef uint32_t    UINT;
typedef int32_t     BOOL;
typedef uint16_t    USHORT;
typedef uint16_t    WORD;
typedef uint8_t     UCHAR;
typedef uint8_t     BYTE;
typedef int64_t     LONGLONG;
typedef uint64_t    ULONGLONG;
typedef uint64_t    ULONG_PTR;
typedef int64_t     LONG_PTR;
typedef uint64_t    SIZE_T;
typedef void       *PVOID;
typedef const void *PCVOID;
typedef uint16_t    WCHAR;          /* UTF-16LE code unit */
typedef WCHAR      *PWSTR;
typedef const WCHAR *PCWSTR;
typedef char       *PSTR;
typedef const char *PCSTR;

/* The active PE ABI belongs to the scheduled task. Keeping this behind a
 * slot accessor preserves the existing lvalue-style call sites while avoiding
 * a global mode bit that another Win32 thread or CPU can overwrite. */
#ifdef __KERNEL_X86__
int *proc_win32_compat32_mode_slot(void);
#define g_compat32_mode (*proc_win32_compat32_mode_slot())
#else
extern int g_compat32_mode;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── GUID / IID ───────────────────────────────────────────── */

typedef struct _GUID {
    ULONG  Data1;
    USHORT Data2;
    USHORT Data3;
    BYTE   Data4[8];
} GUID, *LPGUID;

typedef const GUID *LPCGUID;
typedef GUID IID;
typedef const IID *REFIID;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

/* ── NTSTATUS ───────────────────────────────────────────────── */
/*
 * Layout (32-bit):
 *   Bits 31-30: Severity (00=Success, 01=Info, 10=Warning, 11=Error)
 *   Bit 29:     Customer flag
 *   Bits 27-16: Facility code
 *   Bits 15-0:  Status code
 */

typedef LONG NTSTATUS;

#define NT_SUCCESS(s)       ((NTSTATUS)(s) >= 0)
#define NT_INFORMATION(s)   ((((ULONG)(s)) >> 30) == 1)
#define NT_WARNING(s)       ((((ULONG)(s)) >> 30) == 2)
#define NT_ERROR(s)         ((((ULONG)(s)) >> 30) == 3)

/* Version identity shared by the PEB and the legacy version APIs. */
#define WIN32_NT_VERSION_MAJOR 10U
#define WIN32_NT_VERSION_MINOR 0U
#define WIN32_NT_VERSION_BUILD 19045U
#define WIN32_NT_PLATFORM_ID   2U

/* Success */
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_OBJECT_NAME_EXISTS       ((NTSTATUS)0x40000000)
#define STATUS_BUFFER_OVERFLOW          ((NTSTATUS)0x80000005)
#define STATUS_PARTIAL_COPY             ((NTSTATUS)0x8000000D)
#define STATUS_LONGJUMP                 ((NTSTATUS)0x80000026)
#define STATUS_UNWIND_CONSOLIDATE       ((NTSTATUS)0x80000029)

/* Error */
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_INFO_CLASS       ((NTSTATUS)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH     ((NTSTATUS)0xC0000004)
#define STATUS_ACCESS_VIOLATION         ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_FILE             ((NTSTATUS)0xC000000F)
#define STATUS_END_OF_FILE              ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017)
#define STATUS_CONFLICTING_ADDRESSES    ((NTSTATUS)0xC0000018)
#define STATUS_UNABLE_TO_FREE_VM        ((NTSTATUS)0xC000001A)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022)
#define STATUS_NONCONTINUABLE_EXCEPTION ((NTSTATUS)0xC0000025)
#define STATUS_INVALID_DISPOSITION      ((NTSTATUS)0xC0000026)
#define STATUS_UNWIND                   ((NTSTATUS)0xC0000027)
#define STATUS_BAD_STACK                ((NTSTATUS)0xC0000028)
#define STATUS_INVALID_UNWIND_TARGET    ((NTSTATUS)0xC0000029)
#define STATUS_OBJECT_NAME_INVALID      ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xC000003A)
#define STATUS_OBJECT_PATH_SYNTAX_BAD   ((NTSTATUS)0xC000003B)
#define STATUS_FILE_LOCK_CONFLICT       ((NTSTATUS)0xC0000054)
#define STATUS_LOCK_NOT_GRANTED         ((NTSTATUS)0xC0000055)
#define STATUS_PROCEDURE_NOT_FOUND      ((NTSTATUS)0xC000007A)
#define STATUS_INVALID_IMAGE_FORMAT     ((NTSTATUS)0xC000007B)
#define STATUS_RANGE_NOT_LOCKED         ((NTSTATUS)0xC000007E)
#define STATUS_ARRAY_BOUNDS_EXCEEDED    ((NTSTATUS)0xC000008C)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009A)
#define STATUS_IO_TIMEOUT               ((NTSTATUS)0xC00000B5)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_BAD_FUNCTION_TABLE       ((NTSTATUS)0xC00000FF)
#define STATUS_NAME_TOO_LONG            ((NTSTATUS)0xC0000106)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120)
#define STATUS_DLL_NOT_FOUND            ((NTSTATUS)0xC0000135)
#define STATUS_UNHANDLED_EXCEPTION      ((NTSTATUS)0xC0000144)
#define STATUS_CONNECTION_RESET         ((NTSTATUS)0xC000020D)
#define STATUS_CONNECTION_REFUSED       ((NTSTATUS)0xC0000236)
#define STATUS_INVALID_PARAMETER_1      ((NTSTATUS)0xC00000EF)
#define STATUS_INVALID_PARAMETER_2      ((NTSTATUS)0xC00000F0)
#define STATUS_INVALID_PARAMETER_3      ((NTSTATUS)0xC00000F1)
#define STATUS_HANDLE_NOT_CLOSABLE      ((NTSTATUS)0xC0000235)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000046)
#define STATUS_ILLEGAL_FUNCTION         ((NTSTATUS)0xC00000AF)

/* ── HANDLE ─────────────────────────────────────────────────── */

typedef PVOID HANDLE;
typedef HANDLE *PHANDLE;

#define INVALID_HANDLE_VALUE    ((HANDLE)(LONG_PTR)-1)
#define NT_CURRENT_PROCESS      ((HANDLE)(LONG_PTR)-1)
#define NT_CURRENT_THREAD       ((HANDLE)(LONG_PTR)-2)

/* ── UNICODE_STRING ─────────────────────────────────────────── */

typedef struct _ANSI_STRING {
    USHORT Length;          /* bytes, excluding terminator */
    USHORT MaximumLength;   /* bytes, including terminator */
    PSTR   Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef const ANSI_STRING *PCANSI_STRING;

typedef struct _UNICODE_STRING {
    USHORT Length;              /* current length in BYTES */
    USHORT MaximumLength;      /* allocated size in BYTES */
    PWSTR  Buffer;             /* UTF-16LE, NOT null-terminated */
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;

/* ── OBJECT_ATTRIBUTES ──────────────────────────────────────── */

#define OBJ_PROTECT_CLOSE       0x00000001
#define OBJ_INHERIT             0x00000002
#define OBJ_PERMANENT           0x00000010
#define OBJ_EXCLUSIVE           0x00000020
#define OBJ_CASE_INSENSITIVE    0x00000040
#define OBJ_OPENIF              0x00000080
#define OBJ_OPENLINK            0x00000100
#define OBJ_KERNEL_HANDLE       0x00000200

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef const OBJECT_ATTRIBUTES *PCOBJECT_ATTRIBUTES;

#define InitializeObjectAttributes(p, n, a, r, s) do { \
    (p)->Length                    = sizeof(OBJECT_ATTRIBUTES); \
    (p)->RootDirectory            = (r); \
    (p)->Attributes               = (a); \
    (p)->ObjectName               = (n); \
    (p)->SecurityDescriptor       = (s); \
    (p)->SecurityQualityOfService = NULL; \
} while (0)

/* ── IO_STATUS_BLOCK ────────────────────────────────────────── */

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;     /* bytes transferred */
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* ── LARGE_INTEGER ──────────────────────────────────────────── */

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        ULONG LowPart;
        ULONG HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

/* ── ACCESS_MASK ────────────────────────────────────────────── */

typedef ULONG ACCESS_MASK;

typedef struct _GENERIC_MAPPING {
    ACCESS_MASK GenericRead;
    ACCESS_MASK GenericWrite;
    ACCESS_MASK GenericExecute;
    ACCESS_MASK GenericAll;
} GENERIC_MAPPING, *PGENERIC_MAPPING;

/* Generic access rights */
#define GENERIC_READ            0x80000000
#define GENERIC_WRITE           0x40000000
#define GENERIC_EXECUTE         0x20000000
#define GENERIC_ALL             0x10000000

/* Standard access rights */
#define DELETE                  0x00010000
#define READ_CONTROL            0x00020000
#define WRITE_DAC               0x00040000
#define WRITE_OWNER             0x00080000
#define SYNCHRONIZE             0x00100000

/* File-specific access rights */
#define FILE_READ_DATA          0x0001
#define FILE_LIST_DIRECTORY     0x0001
#define FILE_WRITE_DATA         0x0002
#define FILE_ADD_FILE           0x0002
#define FILE_APPEND_DATA        0x0004
#define FILE_READ_EA            0x0008
#define FILE_WRITE_EA           0x0010
#define FILE_EXECUTE            0x0020
#define FILE_TRAVERSE           0x0020
#define FILE_DELETE_CHILD       0x0040
#define FILE_READ_ATTRIBUTES    0x0080
#define FILE_WRITE_ATTRIBUTES   0x0100

#define FILE_GENERIC_READ       (READ_CONTROL | FILE_READ_DATA | \
                                 FILE_READ_ATTRIBUTES | FILE_READ_EA | \
                                 SYNCHRONIZE)
#define FILE_GENERIC_WRITE      (READ_CONTROL | FILE_WRITE_DATA | \
                                 FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | \
                                 FILE_APPEND_DATA | SYNCHRONIZE)
#define FILE_GENERIC_EXECUTE    (READ_CONTROL | FILE_READ_ATTRIBUTES | \
                                 FILE_EXECUTE | SYNCHRONIZE)

/* ── File creation disposition ──────────────────────────────── */

#define FILE_SUPERSEDE          0x00000000
#define FILE_OPEN               0x00000001
#define FILE_CREATE             0x00000002
#define FILE_OPEN_IF            0x00000003
#define FILE_OVERWRITE          0x00000004
#define FILE_OVERWRITE_IF       0x00000005

/* Successful create/open result stored in IO_STATUS_BLOCK.Information. */
#define FILE_SUPERSEDED         0x00000000
#define FILE_OPENED             0x00000001
#define FILE_CREATED            0x00000002
#define FILE_OVERWRITTEN        0x00000003
#define FILE_EXISTS             0x00000004
#define FILE_DOES_NOT_EXIST     0x00000005

/* Special ByteOffset.LowPart values used with HighPart == -1. */
#define FILE_USE_FILE_POINTER_POSITION 0xFFFFFFFEU
#define FILE_WRITE_TO_END_OF_FILE      0xFFFFFFFFU

/* ── File creation options ──────────────────────────────────── */

#define FILE_DIRECTORY_FILE             0x00000001
#define FILE_WRITE_THROUGH              0x00000002
#define FILE_SEQUENTIAL_ONLY            0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT       0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_DELETE_ON_CLOSE            0x00001000

/* ── File attributes ────────────────────────────────────────── */

#define FILE_ATTRIBUTE_READONLY         0x00000001
#define FILE_ATTRIBUTE_HIDDEN           0x00000002
#define FILE_ATTRIBUTE_SYSTEM           0x00000004
#define FILE_ATTRIBUTE_DIRECTORY        0x00000010
#define FILE_ATTRIBUTE_ARCHIVE          0x00000020
#define FILE_ATTRIBUTE_NORMAL           0x00000080

/* ── Share access ───────────────────────────────────────────── */

#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004

/* ── Memory allocation ──────────────────────────────────────── */

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_RELEASE             0x00008000
#define MEM_FREE                0x00010000
#define MEM_RESET               0x00080000

/* ── Page protection ────────────────────────────────────────── */

#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08
#define PAGE_EXECUTE            0x10
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40
#define PAGE_EXECUTE_WRITECOPY  0x80
#define PAGE_GUARD              0x100

/* ── CLIENT_ID (process/thread identifier pair) ─────────────── */

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

/* User stack bounds supplied to NtCreateThread. The stack storage itself is
 * owned by the process virtual-memory manager, not by the thread object. */
typedef struct _INITIAL_TEB {
    PVOID StackBase;
    PVOID StackLimit;
    PVOID StackCommit;
    PVOID StackCommitMax;
    PVOID StackReserved;
} INITIAL_TEB, *PINITIAL_TEB;

/* ── LIST_ENTRY (doubly-linked list) ────────────────────────── */

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

typedef struct _RTL_CRITICAL_SECTION {
    PVOID       DebugInfo;
    LONG        LockCount;
    LONG        RecursionCount;
    HANDLE      OwningThread;
    HANDLE      LockSemaphore;
    ULONG_PTR   SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION,
  CRITICAL_SECTION, *LPCRITICAL_SECTION;

#define InitializeListHead(head) do { \
    (head)->Flink = (head); \
    (head)->Blink = (head); \
} while (0)

#define IsListEmpty(head) ((head)->Flink == (head))

#define InsertTailList(head, entry) do { \
    PLIST_ENTRY _blink = (head)->Blink; \
    (entry)->Flink = (head); \
    (entry)->Blink = _blink; \
    _blink->Flink = (entry); \
    (head)->Blink  = (entry); \
} while (0)

#define RemoveEntryList(entry) do { \
    PLIST_ENTRY _flink = (entry)->Flink; \
    PLIST_ENTRY _blink = (entry)->Blink; \
    _blink->Flink = _flink; \
    _flink->Blink = _blink; \
} while (0)

#define CONTAINING_RECORD(addr, type, field) \
    ((type *)((char *)(addr) - __builtin_offsetof(type, field)))

/* ── File information classes (NtQueryInformationFile) ──────── */

typedef enum _FILE_INFORMATION_CLASS {
    FileBasicInformation          = 4,
    FileStandardInformation       = 5,
    FileRenameInformation         = 10,
    FileDispositionInformation    = 13,
    FilePositionInformation       = 14,
    FileAllocationInformation     = 19,
    FileEndOfFileInformation      = 20,
    FileNameInformation           = 9,
} FILE_INFORMATION_CLASS;

typedef struct _FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         NumberOfLinks;
    BOOL          DeletePending;
    BOOL          Directory;
} FILE_STANDARD_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION {
    LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION;

typedef struct _FILE_BASIC_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG         FileAttributes;
} FILE_BASIC_INFORMATION;

/* ── Process information ────────────────────────────────────── */

typedef enum _PROCESSINFOCLASS {
    ProcessBasicInformation = 0,
    ProcessImageFileName    = 27,
} PROCESSINFOCLASS;

typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS  ExitStatus;
    PVOID     PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG      BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

/* Process parameters. The layout matches the native x64 ABI through the
 * Windows 10/11 fields that are read directly by CRT, SDL and Chromium. */

#define RTL_USER_PROC_PARAMS_NORMALIZED 0x00000001UL

typedef struct _CURDIR {
    UNICODE_STRING DosPath;
    HANDLE         Handle;
} CURDIR, *PCURDIR;

typedef struct _RTL_DRIVE_LETTER_CURDIR {
    USHORT         Flags;
    USHORT         Length;
    ULONG          TimeStamp;
    UNICODE_STRING DosPath;
} RTL_DRIVE_LETTER_CURDIR, *PRTL_DRIVE_LETTER_CURDIR;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
    ULONG          MaximumLength;                 /* +0x000 */
    ULONG          Length;                        /* +0x004 */
    ULONG          Flags;                         /* +0x008 */
    ULONG          DebugFlags;                    /* +0x00C */
    HANDLE         ConsoleHandle;                 /* +0x010 */
    ULONG          ConsoleFlags;                  /* +0x018 */
    ULONG          Padding0;
    HANDLE         StandardInput;                 /* +0x020 */
    HANDLE         StandardOutput;                /* +0x028 */
    HANDLE         StandardError;                 /* +0x030 */
    CURDIR         CurrentDirectory;              /* +0x038 */
    UNICODE_STRING DllPath;                       /* +0x050 */
    UNICODE_STRING ImagePathName;                 /* +0x060 */
    UNICODE_STRING CommandLine;                   /* +0x070 */
    PVOID          Environment;                   /* +0x080 */
    ULONG          StartingX;                     /* +0x088 */
    ULONG          StartingY;
    ULONG          CountX;
    ULONG          CountY;
    ULONG          CountCharsX;
    ULONG          CountCharsY;
    ULONG          FillAttribute;
    ULONG          WindowFlags;
    ULONG          ShowWindowFlags;               /* +0x0A8 */
    ULONG          Padding1;
    UNICODE_STRING WindowTitle;                   /* +0x0B0 */
    UNICODE_STRING DesktopInfo;
    UNICODE_STRING ShellInfo;
    UNICODE_STRING RuntimeData;
    RTL_DRIVE_LETTER_CURDIR CurrentDirectories[32]; /* +0x0F0 */
    ULONG_PTR      EnvironmentSize;               /* +0x3F0 */
    ULONG_PTR      EnvironmentVersion;
    PVOID          PackageDependencyData;
    ULONG          ProcessGroupId;
    ULONG          LoaderThreads;
    UNICODE_STRING RedirectionDllName;
    UNICODE_STRING HeapPartitionName;
    ULONGLONG     *DefaultThreadpoolCpuSetMasks;
    ULONG          DefaultThreadpoolCpuSetMaskCount;
    ULONG          DefaultThreadpoolThreadMaximum;
    ULONG          HeapMemoryTypeMask;             /* +0x440 */
    ULONG          Padding2;
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

_Static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS, Flags) == 0x08,
               "RTL process parameters Flags offset changed");
_Static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS,
                                  CurrentDirectory) == 0x38,
               "RTL process parameters CWD offset changed");
_Static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS,
                                  ImagePathName) == 0x60,
               "RTL process parameters image offset changed");
_Static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS,
                                  CommandLine) == 0x70,
               "RTL process parameters command line offset changed");
_Static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS,
                                  Environment) == 0x80,
               "RTL process parameters environment offset changed");
_Static_assert(sizeof(RTL_USER_PROCESS_PARAMETERS) == 0x448,
               "RTL process parameters x64 size changed");

/* Stable x64 PEB prefix through SessionId. Runtime libraries commonly read
 * these fields directly, so preserve the native offsets rather than exposing
 * a shortened private structure. */
typedef struct _PEB {
    BYTE        InheritedAddressSpace;             /* +0x000 */
    BYTE        ReadImageFileExecOptions;
    BYTE        BeingDebugged;
    BYTE        BitField;
    ULONG       Padding0;
    PVOID       Mutant;                            /* +0x008 */
    PVOID       ImageBaseAddress;                  /* +0x010 */
    PVOID       Ldr;                               /* +0x018 */
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters; /* +0x020 */
    PVOID       SubSystemData;
    PVOID       ProcessHeap;                       /* +0x030 */
    PVOID       FastPebLock;
    PVOID       AtlThunkSListPtr;
    PVOID       IFEOKey;
    ULONG       CrossProcessFlags;                 /* +0x050 */
    ULONG       Padding1;
    PVOID       KernelCallbackTable;
    ULONG       SystemReserved;                    /* +0x060 */
    ULONG       AtlThunkSListPtr32;
    PVOID       ApiSetMap;
    ULONG       TlsExpansionCounter;               /* +0x070 */
    ULONG       Padding2;
    PVOID       TlsBitmap;
    ULONG       TlsBitmapBits[2];                  /* +0x080 */
    PVOID       ReadOnlySharedMemoryBase;
    PVOID       SharedData;
    PVOID       ReadOnlyStaticServerData;
    PVOID       AnsiCodePageData;
    PVOID       OemCodePageData;
    PVOID       UnicodeCaseTableData;
    ULONG       NumberOfProcessors;                /* +0x0B8 */
    ULONG       NtGlobalFlag;                      /* +0x0BC */
    LARGE_INTEGER CriticalSectionTimeout;
    ULONG_PTR   HeapSegmentReserve;
    ULONG_PTR   HeapSegmentCommit;
    ULONG_PTR   HeapDeCommitTotalFreeThreshold;
    ULONG_PTR   HeapDeCommitFreeBlockThreshold;
    ULONG       NumberOfHeaps;
    ULONG       MaximumNumberOfHeaps;
    PVOID       ProcessHeaps;
    PVOID       GdiSharedHandleTable;              /* +0x0F8 */
    PVOID       ProcessStarterHelper;               /* +0x100 */
    ULONG       GdiDCAttributeList;                 /* +0x108 */
    ULONG       Padding3;
    PVOID       LoaderLock;                         /* +0x110 */
    ULONG       OSMajorVersion;                     /* +0x118 */
    ULONG       OSMinorVersion;                     /* +0x11C */
    USHORT      OSBuildNumber;                      /* +0x120 */
    USHORT      OSCSDVersion;
    ULONG       OSPlatformId;                       /* +0x124 */
    ULONG       ImageSubsystem;                     /* +0x128 */
    ULONG       ImageSubsystemMajorVersion;         /* +0x12C */
    ULONG       ImageSubsystemMinorVersion;         /* +0x130 */
    ULONG       Padding4;
    ULONG_PTR   ActiveProcessAffinityMask;          /* +0x138 */
    ULONG       GdiHandleBuffer[60];                /* +0x140 */
    PVOID       PostProcessInitRoutine;             /* +0x230 */
    PVOID       TlsExpansionBitmap;                 /* +0x238 */
    ULONG       TlsExpansionBitmapBits[32];         /* +0x240 */
    ULONG       SessionId;                          /* +0x2C0 */
    ULONG       Padding5;
} PEB, *PPEB;

_Static_assert(__builtin_offsetof(PEB, ImageBaseAddress) == 0x10,
               "PEB image base offset changed");
_Static_assert(__builtin_offsetof(PEB, ProcessParameters) == 0x20,
               "PEB process parameters offset changed");
_Static_assert(__builtin_offsetof(PEB, ProcessHeap) == 0x30,
               "PEB process heap offset changed");
_Static_assert(__builtin_offsetof(PEB, NtGlobalFlag) == 0xBC,
               "PEB NtGlobalFlag offset changed");
_Static_assert(__builtin_offsetof(PEB, GdiSharedHandleTable) == 0xF8,
               "PEB GDI table offset changed");
_Static_assert(__builtin_offsetof(PEB, LoaderLock) == 0x110,
               "PEB x64 loader lock offset changed");
_Static_assert(__builtin_offsetof(PEB, OSMajorVersion) == 0x118,
               "PEB x64 OS version offset changed");
_Static_assert(__builtin_offsetof(PEB, PostProcessInitRoutine) == 0x230,
               "PEB x64 post-init offset changed");
_Static_assert(__builtin_offsetof(PEB, SessionId) == 0x2C0,
               "PEB x64 session offset changed");
_Static_assert(sizeof(PEB) == 0x2C8, "PEB x64 prefix size changed");

/* ── SEH (Structured Exception Handling) ────────────────────── */

/* Exception codes */
#define EXCEPTION_ACCESS_VIOLATION      0xC0000005
#define EXCEPTION_BREAKPOINT            0x80000003
#define EXCEPTION_SINGLE_STEP           0x80000004
#define EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094
#define EXCEPTION_INT_OVERFLOW          0xC0000095
#define EXCEPTION_STACK_OVERFLOW        0xC00000FD
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED 0xC000008C
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    0xC000008E
#define EXCEPTION_ILLEGAL_INSTRUCTION   0xC000001D
#define EXCEPTION_NONCONTINUABLE_EXCEPTION 0xC0000025
#define EXCEPTION_IN_PAGE_ERROR         0xC0000006
#define EXCEPTION_GUARD_PAGE            0x80000001
#define EXCEPTION_INVALID_DISPOSITION   0xC0000026

/* Exception flags */
#define EXCEPTION_NONCONTINUABLE        0x01
#define EXCEPTION_UNWINDING             0x02
#define EXCEPTION_EXIT_UNWIND           0x04
#define EXCEPTION_STACK_INVALID         0x08
#define EXCEPTION_NESTED_CALL           0x10
#define EXCEPTION_TARGET_UNWIND         0x20
#define EXCEPTION_COLLIDED_UNWIND       0x40
#define EXCEPTION_UNWIND   (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | \
                            EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND)

/* Exception handler return values */
#define ExceptionContinueExecution      0
#define ExceptionContinueSearch         1
#define ExceptionNestedException        2
#define ExceptionCollidedUnwind         3

/* EXCEPTION_DISPOSITION */
typedef LONG EXCEPTION_DISPOSITION;

/* Maximum parameters in EXCEPTION_RECORD */
#define EXCEPTION_MAXIMUM_PARAMETERS    15

/* EXCEPTION_RECORD — describes the exception */
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;     /* chained record */
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct __attribute__((aligned(16))) _M128A {
    ULONGLONG Low;
    LONGLONG High;
} M128A, *PM128A;

typedef struct __attribute__((aligned(16))) _XMM_SAVE_AREA32 {
    WORD ControlWord;
    WORD StatusWord;
    BYTE TagWord;
    BYTE Reserved1;
    WORD ErrorOpcode;
    DWORD ErrorOffset;
    WORD ErrorSelector;
    WORD Reserved2;
    DWORD DataOffset;
    WORD DataSelector;
    WORD Reserved3;
    DWORD MxCsr;
    DWORD MxCsr_Mask;
    M128A FloatRegisters[8];
    M128A XmmRegisters[16];
    BYTE Reserved4[96];
} XMM_SAVE_AREA32, *PXMM_SAVE_AREA32;

/* AMD64 CONTEXT layout exposed to native Win32 handlers. The floating-point
 * state follows the architectural FXSAVE layout used by Windows. */
typedef struct __attribute__((aligned(16))) _CONTEXT {
    ULONGLONG P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    ULONGLONG Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    ULONGLONG Rax, Rcx, Rdx, Rbx;
    ULONGLONG Rsp, Rbp, Rsi, Rdi;
    ULONGLONG R8, R9, R10, R11;
    ULONGLONG R12, R13, R14, R15;
    ULONGLONG Rip;
    union {
        XMM_SAVE_AREA32 FltSave;
        BYTE FloatingPointState[512];
    };
    BYTE VectorRegister[26][16];
    ULONGLONG VectorControl;
    ULONGLONG DebugControl;
    ULONGLONG LastBranchToRip;
    ULONGLONG LastBranchFromRip;
    ULONGLONG LastExceptionToRip;
    ULONGLONG LastExceptionFromRip;
} CONTEXT, *PCONTEXT;

_Static_assert(__builtin_offsetof(CONTEXT, ContextFlags) == 0x30,
               "AMD64 CONTEXT flags offset changed");
_Static_assert(__builtin_offsetof(CONTEXT, Rax) == 0x78,
               "AMD64 CONTEXT Rax offset changed");
_Static_assert(__builtin_offsetof(CONTEXT, Rsp) == 0x98,
               "AMD64 CONTEXT Rsp offset changed");
_Static_assert(__builtin_offsetof(CONTEXT, Rip) == 0xF8,
               "AMD64 CONTEXT Rip offset changed");
_Static_assert(sizeof(CONTEXT) == 0x4D0,
               "AMD64 CONTEXT size changed");

/* Context flags */
#define CONTEXT_AMD64               0x00100000
#define CONTEXT_CONTROL             (CONTEXT_AMD64 | 0x0001)
#define CONTEXT_INTEGER             (CONTEXT_AMD64 | 0x0002)
#define CONTEXT_SEGMENTS            (CONTEXT_AMD64 | 0x0004)
#define CONTEXT_FULL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)
#define CONTEXT_UNWOUND_TO_CALL     0x20000000U

/* AMD64 table-based unwind metadata. RUNTIME_FUNCTION fields are RVAs from
 * the image or dynamic-table base supplied to RtlAddFunctionTable. */
typedef struct _RUNTIME_FUNCTION {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

typedef union _UNWIND_CODE {
    struct {
        BYTE CodeOffset;
        BYTE UnwindOp : 4;
        BYTE OpInfo : 4;
    };
    USHORT FrameOffset;
} UNWIND_CODE, *PUNWIND_CODE;

typedef struct _UNWIND_INFO {
    BYTE Version : 3;
    BYTE Flags : 5;
    BYTE SizeOfProlog;
    BYTE CountOfCodes;
    BYTE FrameRegister : 4;
    BYTE FrameOffset : 4;
    UNWIND_CODE UnwindCode[1];
} UNWIND_INFO, *PUNWIND_INFO;

#define UNW_FLAG_NHANDLER   0x0
#define UNW_FLAG_EHANDLER   0x1
#define UNW_FLAG_UHANDLER   0x2
#define UNW_FLAG_CHAININFO  0x4

#define UWOP_PUSH_NONVOL       0
#define UWOP_ALLOC_LARGE       1
#define UWOP_ALLOC_SMALL       2
#define UWOP_SET_FPREG         3
#define UWOP_SAVE_NONVOL       4
#define UWOP_SAVE_NONVOL_FAR   5
#define UWOP_SAVE_XMM128       8
#define UWOP_SAVE_XMM128_FAR   9
#define UWOP_PUSH_MACHFRAME   10

typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    union {
        PM128A FloatingContext[16];
        struct {
            PM128A Xmm0, Xmm1, Xmm2, Xmm3;
            PM128A Xmm4, Xmm5, Xmm6, Xmm7;
            PM128A Xmm8, Xmm9, Xmm10, Xmm11;
            PM128A Xmm12, Xmm13, Xmm14, Xmm15;
        };
    };
    union {
        ULONGLONG *IntegerContext[16];
        struct {
            ULONGLONG *Rax, *Rcx, *Rdx, *Rbx;
            ULONGLONG *Rsp, *Rbp, *Rsi, *Rdi;
            ULONGLONG *R8, *R9, *R10, *R11;
            ULONGLONG *R12, *R13, *R14, *R15;
        };
    };
} KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

#define UNWIND_HISTORY_TABLE_SIZE 12
typedef struct _UNWIND_HISTORY_TABLE_ENTRY {
    ULONGLONG ImageBase;
    PRUNTIME_FUNCTION FunctionEntry;
} UNWIND_HISTORY_TABLE_ENTRY, *PUNWIND_HISTORY_TABLE_ENTRY;

typedef struct _UNWIND_HISTORY_TABLE {
    DWORD Count;
    BYTE LocalHint;
    BYTE GlobalHint;
    BYTE Search;
    BYTE Once;
    ULONGLONG LowAddress;
    ULONGLONG HighAddress;
    UNWIND_HISTORY_TABLE_ENTRY Entry[UNWIND_HISTORY_TABLE_SIZE];
} UNWIND_HISTORY_TABLE, *PUNWIND_HISTORY_TABLE;

struct _DISPATCHER_CONTEXT;
typedef EXCEPTION_DISPOSITION (WINAPI *PEXCEPTION_ROUTINE)(
    PEXCEPTION_RECORD ExceptionRecord, PVOID EstablisherFrame,
    PCONTEXT ContextRecord, struct _DISPATCHER_CONTEXT *DispatcherContext);

typedef struct _DISPATCHER_CONTEXT {
    ULONGLONG ControlPc;
    ULONGLONG ImageBase;
    PRUNTIME_FUNCTION FunctionEntry;
    ULONGLONG EstablisherFrame;
    ULONGLONG TargetIp;
    PCONTEXT ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID HandlerData;
    PUNWIND_HISTORY_TABLE HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;

typedef struct _SCOPE_RECORD_AMD64 {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD HandlerAddress;
    DWORD JumpTarget;
} SCOPE_RECORD_AMD64, *PSCOPE_RECORD_AMD64;

typedef struct _SCOPE_TABLE_AMD64 {
    DWORD Count;
    SCOPE_RECORD_AMD64 ScopeRecord[1];
} SCOPE_TABLE_AMD64, *PSCOPE_TABLE_AMD64;

_Static_assert(sizeof(RUNTIME_FUNCTION) == 12,
               "AMD64 runtime function size changed");
_Static_assert(sizeof(UNWIND_CODE) == 2,
               "AMD64 unwind code size changed");
_Static_assert(sizeof(SCOPE_RECORD_AMD64) == 16,
               "AMD64 scope record size changed");
_Static_assert(__builtin_offsetof(CONTEXT, FloatingPointState) == 0x100,
               "AMD64 floating-point context offset changed");
_Static_assert(sizeof(XMM_SAVE_AREA32) == 0x200,
               "AMD64 FXSAVE area size changed");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, MxCsr) == 0x18,
               "AMD64 FXSAVE MXCSR offset changed");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, XmmRegisters) == 0xA0,
               "AMD64 FXSAVE XMM offset changed");
_Static_assert(sizeof(KNONVOLATILE_CONTEXT_POINTERS) == 0x100,
               "AMD64 context-pointer table size changed");

/* EXCEPTION_POINTERS — passed to unhandled exception filters */
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

/* EXCEPTION_REGISTRATION_RECORD — SEH frame chain entry (x86-style) */
typedef struct _EXCEPTION_REGISTRATION_RECORD {
    struct _EXCEPTION_REGISTRATION_RECORD *Next;
    PVOID   Handler;        /* _except_handler3 or similar */
} EXCEPTION_REGISTRATION_RECORD, *PEXCEPTION_REGISTRATION_RECORD;

#define EXCEPTION_CHAIN_END ((PEXCEPTION_REGISTRATION_RECORD)(ULONG_PTR)-1)

/* _except_handler3 scopetable entry */
typedef struct _SCOPETABLE_ENTRY {
    DWORD EnclosingLevel;
    PVOID FilterFunc;       /* filter expression — returns EXCEPTION_EXECUTE_HANDLER etc. */
    PVOID HandlerFunc;      /* __except block body */
} SCOPETABLE_ENTRY, *PSCOPETABLE_ENTRY;

/* _except_handler3 establisher frame (pushed by compiler) */
typedef struct _EH3_EXCEPTION_REGISTRATION {
    EXCEPTION_REGISTRATION_RECORD registration;
    PSCOPETABLE_ENTRY ScopeTable;
    DWORD TryLevel;
} EH3_EXCEPTION_REGISTRATION, *PEH3_EXCEPTION_REGISTRATION;

/* Filter return values */
#define EXCEPTION_EXECUTE_HANDLER       1
#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_CONTINUE_EXECUTION   -1

/* ── TEB (Thread Environment Block) — minimal ──────────────── */

#define TEB64_STORAGE_SIZE 0x2000u

typedef struct _TEB {
    PVOID       ExceptionList;      /* +0x0000: NT_TIB */
    PVOID       StackBase;
    PVOID       StackLimit;
    PVOID       SubSystemTib;
    PVOID       FiberData;
    PVOID       ArbitraryUserPointer;
    struct _TEB *Self;              /* +0x0030 */
    PVOID       EnvironmentPointer;
    CLIENT_ID   ClientId;           /* +0x0040: PID + TID */
    PVOID       ActiveRpcHandle;
    PVOID       ThreadLocalStoragePointer; /* +0x0058: static TLS vector */
    PPEB        ProcessEnvironmentBlock;   /* +0x0060 */
    ULONG       LastErrorValue;             /* +0x0068 */
    ULONG       CountOfOwnedCriticalSections;
    BYTE        ReservedToLastStatusValue[0x1250 - 0x70];
    NTSTATUS    LastStatusValue;            /* +0x1250 */
    BYTE        ReservedToDeallocationStack[0x1478 - 0x1254];
    PVOID       DeallocationStack;          /* +0x1478 */
    PVOID       TlsSlots[64];               /* +0x1480 */
    BYTE        ReservedToGuaranteedStackBytes[0x1748 - 0x1680];
    ULONG       GuaranteedStackBytes;       /* +0x1748 */
    BYTE        ReservedToTlsExpansionSlots[0x1780 - 0x174C];
    PVOID      *TlsExpansionSlots;          /* +0x1780 */
    BYTE        ReservedTail[TEB64_STORAGE_SIZE - 0x1788];
} TEB, *PTEB;

_Static_assert(__builtin_offsetof(TEB, Self) == 0x30,
               "TEB64 Self offset changed");
_Static_assert(__builtin_offsetof(TEB, ProcessEnvironmentBlock) == 0x60,
               "TEB64 PEB offset changed");
_Static_assert(__builtin_offsetof(TEB, LastErrorValue) == 0x68,
               "TEB64 LastErrorValue offset changed");
_Static_assert(__builtin_offsetof(TEB, LastStatusValue) == 0x1250,
               "TEB64 LastStatusValue offset changed");
_Static_assert(__builtin_offsetof(TEB, DeallocationStack) == 0x1478,
               "TEB64 DeallocationStack offset changed");
_Static_assert(__builtin_offsetof(TEB, TlsSlots) == 0x1480,
               "TEB64 TlsSlots offset changed");
_Static_assert(__builtin_offsetof(TEB, GuaranteedStackBytes) == 0x1748,
               "TEB64 GuaranteedStackBytes offset changed");
_Static_assert(__builtin_offsetof(TEB, TlsExpansionSlots) == 0x1780,
               "TEB64 TlsExpansionSlots offset changed");
_Static_assert(sizeof(TEB) == TEB64_STORAGE_SIZE,
               "TEB64 backing storage must remain two pages");

/*
 * 32-bit TEB for PE32 (i386) compatibility mode.
 *
 * Windows i386 uses FS:0 → TEB with 4-byte pointers.
 * Our 64-bit TEB has 8-byte PVOID fields, so offsets are wrong.
 * This struct matches the Win32 TEB layout through the public TLS tail:
 *   +0x00 ExceptionList (SEH chain)
 *   +0x18 Self
 *   +0x20 ClientId
 *   +0x30 ProcessEnvironmentBlock
 *   +0x34 LastErrorValue
 *   +0xE10 TlsSlots
 *   +0xF94 TlsExpansionSlots
 *
 * Must be allocated in <4GB memory.
 */
#define TEB32_PUBLIC_SIZE  0x0F98u
#define TEB32_STORAGE_SIZE 0x1000u

typedef struct __attribute__((packed)) _TEB32 {
    uint32_t    ExceptionList;              /* +0x00 SEH chain head */
    uint32_t    StackBase;                  /* +0x04 */
    uint32_t    StackLimit;                 /* +0x08 */
    uint32_t    SubSystemTib;               /* +0x0C */
    uint32_t    FiberData;                  /* +0x10 */
    uint32_t    ArbitraryUserPointer;       /* +0x14 */
    uint32_t    Self;                       /* +0x18 linear address of TEB32 */
    uint32_t    EnvironmentPointer;         /* +0x1C */
    uint32_t    ClientId_UniqueProcess;     /* +0x20 */
    uint32_t    ClientId_UniqueThread;      /* +0x24 */
    uint32_t    ActiveRpcHandle;            /* +0x28 */
    uint32_t    ThreadLocalStoragePointer;  /* +0x2C */
    uint32_t    ProcessEnvironmentBlock;    /* +0x30 → PEB32 */
    uint32_t    LastErrorValue;             /* +0x34 */
    BYTE        ReservedToTlsSlots[0x0E10 - 0x0038];
    uint32_t    TlsSlots[64];               /* +0xE10 */
    BYTE        Reserved4[8];               /* +0xF10 */
    uint32_t    Reserved5[26];              /* +0xF18 */
    uint32_t    ReservedForOle;             /* +0xF80 */
    uint32_t    Reserved6[4];               /* +0xF84 */
    uint32_t    TlsExpansionSlots;          /* +0xF94 */
} TEB32, *PTEB32;

_Static_assert(__builtin_offsetof(TEB32, Self) == 0x18,
               "TEB32 Self offset changed");
_Static_assert(__builtin_offsetof(TEB32, ProcessEnvironmentBlock) == 0x30,
               "TEB32 PEB offset changed");
_Static_assert(__builtin_offsetof(TEB32, LastErrorValue) == 0x34,
               "TEB32 LastErrorValue offset changed");
_Static_assert(__builtin_offsetof(TEB32, TlsSlots) == 0x0E10,
               "TEB32 TlsSlots offset changed");
_Static_assert(__builtin_offsetof(TEB32, TlsExpansionSlots) == 0x0F94,
               "TEB32 TlsExpansionSlots offset changed");
_Static_assert(sizeof(TEB32) == TEB32_PUBLIC_SIZE,
               "TEB32 public layout size changed");

/*
 * 32-bit PEB for PE32 compatibility mode.
 * Includes the stable i386 prefix through SessionId; the containing process
 * block reserves a complete page so future fields cannot overlap other data.
 */
#define PEB32_PUBLIC_SIZE  0x01D8u
#define PEB32_STORAGE_SIZE 0x1000u

typedef struct _RTL_CRITICAL_SECTION32 {
    uint32_t DebugInfo;
    LONG     LockCount;
    LONG     RecursionCount;
    uint32_t OwningThread;
    uint32_t LockSemaphore;
    uint32_t SpinCount;
} RTL_CRITICAL_SECTION32, *PRTL_CRITICAL_SECTION32;

_Static_assert(sizeof(RTL_CRITICAL_SECTION32) == 0x18,
               "Win32 RTL_CRITICAL_SECTION layout size changed");

typedef struct __attribute__((packed)) _PEB32 {
    uint8_t     InheritedAddressSpace;      /* +0x00 */
    uint8_t     ReadImageFileExecOptions;   /* +0x01 */
    uint8_t     BeingDebugged;              /* +0x02 */
    uint8_t     BitField;                   /* +0x03 */
    uint32_t    Mutant;                     /* +0x04 */
    uint32_t    ImageBaseAddress;           /* +0x08 */
    uint32_t    Ldr;                        /* +0x0C */
    uint32_t    ProcessParameters;          /* +0x10 */
    uint32_t    SubSystemData;              /* +0x14 */
    uint32_t    ProcessHeap;               /* +0x18 */
    uint32_t    FastPebLock;                /* +0x1C */
    uint32_t    AtlThunkSListPtr;           /* +0x20 */
    uint32_t    IFEOKey;                    /* +0x24 */
    uint32_t    CrossProcessFlags;          /* +0x28 */
    uint32_t    KernelCallbackTable;        /* +0x2C */
    uint32_t    SystemReserved;             /* +0x30 */
    uint32_t    AtlThunkSListPtr32;         /* +0x34 */
    uint32_t    ApiSetMap;                  /* +0x38 */
    uint32_t    TlsExpansionCounter;        /* +0x3C */
    uint32_t    TlsBitmap;                  /* +0x40 */
    uint32_t    TlsBitmapBits[2];           /* +0x44 */
    uint32_t    ReadOnlySharedMemoryBase;   /* +0x4C */
    uint32_t    SharedData;                 /* +0x50 */
    uint32_t    ReadOnlyStaticServerData;   /* +0x54 */
    uint32_t    AnsiCodePageData;           /* +0x58 */
    uint32_t    OemCodePageData;            /* +0x5C */
    uint32_t    UnicodeCaseTableData;       /* +0x60 */
    uint32_t    NumberOfProcessors;         /* +0x64 */
    uint32_t    NtGlobalFlag;               /* +0x68 */
    uint32_t    AlignmentPadding;           /* +0x6C */
    int64_t     CriticalSectionTimeout;     /* +0x70 */
    uint32_t    HeapSegmentReserve;         /* +0x78 */
    uint32_t    HeapSegmentCommit;          /* +0x7C */
    uint32_t    HeapDeCommitTotalFreeThreshold; /* +0x80 */
    uint32_t    HeapDeCommitFreeBlockThreshold; /* +0x84 */
    uint32_t    NumberOfHeaps;              /* +0x88 */
    uint32_t    MaximumNumberOfHeaps;       /* +0x8C */
    uint32_t    ProcessHeaps;               /* +0x90 */
    uint32_t    GdiSharedHandleTable;       /* +0x94 */
    uint32_t    ProcessStarterHelper;       /* +0x98 */
    uint32_t    GdiDCAttributeList;         /* +0x9C */
    uint32_t    LoaderLock;                 /* +0xA0 */
    uint32_t    OSMajorVersion;             /* +0xA4 */
    uint32_t    OSMinorVersion;             /* +0xA8 */
    uint16_t    OSBuildNumber;              /* +0xAC */
    uint16_t    OSCSDVersion;               /* +0xAE */
    uint32_t    OSPlatformId;               /* +0xB0 */
    uint32_t    ImageSubsystem;             /* +0xB4 */
    uint32_t    ImageSubsystemMajorVersion; /* +0xB8 */
    uint32_t    ImageSubsystemMinorVersion; /* +0xBC */
    uint32_t    ActiveProcessAffinityMask;  /* +0xC0 */
    uint32_t    GdiHandleBuffer[34];        /* +0xC4 */
    uint32_t    PostProcessInitRoutine;     /* +0x14C */
    uint32_t    TlsExpansionBitmap;         /* +0x150 */
    uint32_t    TlsExpansionBitmapBits[32]; /* +0x154 */
    uint32_t    SessionId;                  /* +0x1D4 */
} PEB32, *PPEB32;

_Static_assert(__builtin_offsetof(PEB32, ImageBaseAddress) == 0x08,
               "PEB32 image base offset changed");
_Static_assert(__builtin_offsetof(PEB32, ProcessParameters) == 0x10,
               "PEB32 process parameters offset changed");
_Static_assert(__builtin_offsetof(PEB32, ProcessHeap) == 0x18,
               "PEB32 process heap offset changed");
_Static_assert(__builtin_offsetof(PEB32, NumberOfProcessors) == 0x64,
               "PEB32 processor count offset changed");
_Static_assert(__builtin_offsetof(PEB32, NumberOfHeaps) == 0x88,
               "PEB32 heap count offset changed");
_Static_assert(__builtin_offsetof(PEB32, GdiSharedHandleTable) == 0x94,
               "PEB32 GDI table offset changed");
_Static_assert(__builtin_offsetof(PEB32, LoaderLock) == 0xA0,
               "PEB32 loader lock offset changed");
_Static_assert(__builtin_offsetof(PEB32, OSMajorVersion) == 0xA4,
               "PEB32 OS version offset changed");
_Static_assert(__builtin_offsetof(PEB32, ImageSubsystem) == 0xB4,
               "PEB32 image subsystem offset changed");
_Static_assert(__builtin_offsetof(PEB32, PostProcessInitRoutine) == 0x14C,
               "PEB32 post-process callback offset changed");
_Static_assert(__builtin_offsetof(PEB32, SessionId) == 0x1D4,
               "PEB32 session offset changed");
_Static_assert(sizeof(PEB32) == PEB32_PUBLIC_SIZE,
               "PEB32 public layout size changed");

#endif /* NTTYPES_H */
