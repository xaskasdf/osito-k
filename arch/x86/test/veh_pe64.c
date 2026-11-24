/* Freestanding PE32+ contract test for vectored CPU exception handling. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long QWORD;
typedef unsigned long long ULONG_PTR;
typedef long LONG;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI
#define DLLIMPORT __declspec(dllimport)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define EXCEPTION_BREAKPOINT         0x80000003UL
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001DUL
#define EXCEPTION_SOFTWARE_TEST       0xE0424242UL
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_CONTINUE_EXECUTION ((LONG)-1)

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    DWORD Padding;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct __attribute__((aligned(16))) _CONTEXT {
    QWORD P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    QWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    QWORD Rax, Rcx, Rdx, Rbx;
    QWORD Rsp, Rbp, Rsi, Rdi;
    QWORD R8, R9, R10, R11;
    QWORD R12, R13, R14, R15;
    QWORD Rip;
    BYTE Reserved[0x4D0 - 0x100];
} CONTEXT;

typedef struct {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

_Static_assert(__builtin_offsetof(CONTEXT, Rip) == 0xF8,
               "AMD64 CONTEXT Rip offset");
_Static_assert(sizeof(CONTEXT) == 0x4D0, "AMD64 CONTEXT size");

DLLIMPORT void WINAPI ExitProcess(DWORD code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT PVOID WINAPI AddVectoredExceptionHandler(DWORD first,
                                                   PVOID handler);
DLLIMPORT DWORD WINAPI RemoveVectoredExceptionHandler(PVOID handle);
DLLIMPORT void WINAPI RaiseException(DWORD code, DWORD flags,
                                     DWORD argument_count,
                                     const ULONG_PTR *arguments);

static volatile DWORD sequence;
static volatile DWORD observe_order;
static volatile DWORD continue_order;
static volatile DWORD expected_code;
static volatile DWORD observed_code;
static volatile DWORD context_matches_record;
static volatile DWORD advance_bytes;

static DWORD text_length(const char *text)
{
    DWORD length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text),
              &written, (PVOID)0);
}

static LONG WINAPI veh_observe(EXCEPTION_POINTERS *pointers)
{
    observe_order = ++sequence;
    if (pointers && pointers->ExceptionRecord)
        observed_code = pointers->ExceptionRecord->ExceptionCode;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI veh_continue(EXCEPTION_POINTERS *pointers)
{
    continue_order = ++sequence;
    if (!pointers || !pointers->ExceptionRecord ||
        !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode != expected_code)
        return EXCEPTION_CONTINUE_SEARCH;

    context_matches_record =
        pointers->ExceptionRecord->ExceptionAddress ==
        (PVOID)(ULONG_PTR)pointers->ContextRecord->Rip;
    pointers->ContextRecord->Rip += advance_bytes;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__declspec(noinline) static void trigger_ud2(void)
{
    __asm__ volatile ("ud2");
}

__declspec(noinline) static void trigger_breakpoint(void)
{
    __asm__ volatile ("int3");
}

static DWORD exercise_exception(DWORD code, DWORD bytes,
                                void (*trigger)(void))
{
    sequence = 0;
    observe_order = 0;
    continue_order = 0;
    expected_code = code;
    observed_code = 0;
    context_matches_record = 0;
    advance_bytes = bytes;
    trigger();

    if (observe_order != 1 || continue_order != 2)
        return 10;
    if (observed_code != code)
        return 11;
    if (!context_matches_record)
        return 12;
    return 0;
}

static DWORD exercise_software_exception(void)
{
    sequence = 0;
    observe_order = 0;
    continue_order = 0;
    expected_code = EXCEPTION_SOFTWARE_TEST;
    observed_code = 0;
    context_matches_record = 0;
    advance_bytes = 0;
    RaiseException(EXCEPTION_SOFTWARE_TEST, 0, 0,
                   (const ULONG_PTR *)0);

    if (observe_order != 1 || continue_order != 2)
        return 13;
    if (observed_code != EXCEPTION_SOFTWARE_TEST)
        return 14;
    if (!context_matches_record)
        return 15;
    return 0;
}

void mainCRTStartup(void)
{
    PVOID continue_handle = AddVectoredExceptionHandler(
        0, (PVOID)veh_continue);
    PVOID observe_handle = AddVectoredExceptionHandler(
        1, (PVOID)veh_observe);
    DWORD result = 0;

    if (!continue_handle || !observe_handle)
        result = 1;
    if (!result)
        result = exercise_exception(EXCEPTION_ILLEGAL_INSTRUCTION, 2,
                                    trigger_ud2);
    if (!result)
        result = exercise_exception(EXCEPTION_BREAKPOINT, 1,
                                    trigger_breakpoint);
    if (!result)
        result = exercise_software_exception();
    if (!result &&
        (RemoveVectoredExceptionHandler(observe_handle) != 1 ||
         RemoveVectoredExceptionHandler(continue_handle) != 1))
        result = 20;
    if (!result && RemoveVectoredExceptionHandler(continue_handle) != 0)
        result = 21;

    print(result ? "VEH64 TEST FAIL\r\n" : "VEH64 TEST PASS\r\n");
    ExitProcess(result);
    for (;;) { }
}
