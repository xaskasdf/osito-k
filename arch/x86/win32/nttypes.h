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

/* Success */
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_BUFFER_OVERFLOW          ((NTSTATUS)0x80000005)
#define STATUS_PARTIAL_COPY             ((NTSTATUS)0x8000000D)

/* Error */
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_INFO_CLASS       ((NTSTATUS)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH     ((NTSTATUS)0xC0000004)
#define STATUS_ACCESS_VIOLATION         ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_FILE             ((NTSTATUS)0xC000000F)
#define STATUS_END_OF_FILE              ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017)
#define STATUS_CONFLICTING_ADDRESSES    ((NTSTATUS)0xC0000018)
#define STATUS_UNABLE_TO_FREE_VM        ((NTSTATUS)0xC000001A)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022)
#define STATUS_OBJECT_NAME_INVALID      ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xC000003A)
#define STATUS_OBJECT_PATH_SYNTAX_BAD   ((NTSTATUS)0xC000003B)
#define STATUS_PROCEDURE_NOT_FOUND      ((NTSTATUS)0xC000007A)
#define STATUS_INVALID_IMAGE_FORMAT     ((NTSTATUS)0xC000007B)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009A)
#define STATUS_IO_TIMEOUT               ((NTSTATUS)0xC00000B5)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120)
#define STATUS_DLL_NOT_FOUND            ((NTSTATUS)0xC0000135)
#define STATUS_CONNECTION_RESET         ((NTSTATUS)0xC000020D)
#define STATUS_CONNECTION_REFUSED       ((NTSTATUS)0xC0000236)
#define STATUS_INVALID_PARAMETER_1      ((NTSTATUS)0xC00000EF)
#define STATUS_INVALID_PARAMETER_2      ((NTSTATUS)0xC00000F0)
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

/* PEB prefix through GdiSharedHandleTable. This is the stable x64 layout
 * commonly read without API mediation by runtime libraries. */
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
_Static_assert(sizeof(PEB) == 0x100, "PEB x64 prefix size changed");

/* ── SEH (Structured Exception Handling) ────────────────────── */

/* Exception codes */
#define EXCEPTION_ACCESS_VIOLATION      0xC0000005
#define EXCEPTION_BREAKPOINT            0x80000003
#define EXCEPTION_SINGLE_STEP           0x80000004
#define EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094
#define EXCEPTION_INT_OVERFLOW          0xC0000095
#define EXCEPTION_STACK_OVERFLOW        0xC00000FD
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

/* CONTEXT — x86-64 register context (minimal for SEH dispatch) */
typedef struct _CONTEXT {
    /* Control flags */
    DWORD   ContextFlags;

    /* Integer registers */
    ULONGLONG Rax, Rcx, Rdx, Rbx;
    ULONGLONG Rsp, Rbp, Rsi, Rdi;
    ULONGLONG R8, R9, R10, R11;
    ULONGLONG R12, R13, R14, R15;

    /* Program counter */
    ULONGLONG Rip;

    /* Segment registers + flags */
    DWORD   SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD   EFlags;
} CONTEXT, *PCONTEXT;

/* Context flags */
#define CONTEXT_AMD64               0x00100000
#define CONTEXT_CONTROL             (CONTEXT_AMD64 | 0x0001)
#define CONTEXT_INTEGER             (CONTEXT_AMD64 | 0x0002)
#define CONTEXT_SEGMENTS            (CONTEXT_AMD64 | 0x0004)
#define CONTEXT_FULL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)

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
 * This struct matches the Win32 TEB layout at critical offsets:
 *   +0x00 ExceptionList (SEH chain)
 *   +0x18 Self
 *   +0x20 ClientId
 *   +0x30 ProcessEnvironmentBlock
 *   +0x34 LastErrorValue
 *
 * Must be allocated in <4GB memory.
 */
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
} TEB32, *PTEB32;

/*
 * 32-bit PEB for PE32 compatibility mode.
 * Only the fields that UT99/UE1 actually access.
 */
typedef struct __attribute__((packed)) _PEB32 {
    uint8_t     InheritedAddressSpace;      /* +0x00 */
    uint8_t     ReadImageFileExecOptions;   /* +0x01 */
    uint8_t     BeingDebugged;              /* +0x02 */
    uint8_t     Spare;                      /* +0x03 */
    uint32_t    Mutant;                     /* +0x04 */
    uint32_t    ImageBaseAddress;           /* +0x08 */
    uint32_t    Ldr;                        /* +0x0C */
    uint32_t    ProcessParameters;          /* +0x10 */
    uint32_t    SubSystemData;              /* +0x14 */
    uint32_t    ProcessHeap;               /* +0x18 */
} PEB32, *PPEB32;

#endif /* NTTYPES_H */
