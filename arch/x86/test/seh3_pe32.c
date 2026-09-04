/*
 * Freestanding PE32 regression for the MSVC i386 EH3 funclet ABI.
 * The filter captures a local from the establishing frame, and an inner
 * __finally must run before the selected handler. The handler then continues
 * normally and returns to its caller, exercising the complete EH3 transfer.
 */

typedef unsigned long DWORD;
typedef int BOOL;
typedef long NTSTATUS;
typedef void *PVOID;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION ((long)-1)
#define MEM_COMMIT       0x00001000UL
#define MEM_RESERVE      0x00002000UL
#define PAGE_NOACCESS    0x00000001UL
#define PAGE_READWRITE   0x00000004UL
#define LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS 0x00000001UL
#define STATUS_SENTINEL ((NTSTATUS)0x12345678L)
#define STATUS_ARRAY_BOUNDS_EXCEEDED ((DWORD)0xC000008CUL)
#define STATUS_SOFTWARE_TEST ((DWORD)0xE0424242UL)

#define FILTER_MAGIC  0x53454833UL
#define HANDLER_MAGIC 0x48414E44UL
#define FINALLY_MAGIC 0x46494E41UL
#define CONTINUE_MAGIC 0x434F4E54UL
#define GUARD_MAGIC    0x47554152UL
#define RAISED_MAGIC   0x52414953UL
#define BOUND_MAGIC    0x424F554EUL

DLLIMPORT void WINAPI ExitProcess(DWORD code);
DLLIMPORT PVOID WINAPI AddVectoredExceptionHandler(DWORD first,
                                                   PVOID handler);
DLLIMPORT DWORD WINAPI RemoveVectoredExceptionHandler(PVOID handle);
DLLIMPORT void WINAPI RaiseException(DWORD code, DWORD flags,
                                     DWORD argument_count,
                                     const DWORD *arguments);
DLLIMPORT PVOID WINAPI VirtualAlloc(PVOID address, DWORD size,
                                    DWORD allocation_type, DWORD protection);
DLLIMPORT BOOL WINAPI VirtualProtect(PVOID address, DWORD size,
                                     DWORD protection,
                                     DWORD *old_protection);
DLLIMPORT NTSTATUS WINAPI LdrLockLoaderLock(DWORD flags,
                                             DWORD *disposition,
                                             DWORD *cookie);
DLLIMPORT NTSTATUS WINAPI LdrUnlockLoaderLock(DWORD flags, DWORD cookie);

typedef struct {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    DWORD ExceptionRecord;
    DWORD ExceptionAddress;
    DWORD NumberParameters;
    DWORD ExceptionInformation[15];
} EXCEPTION_RECORD32;

typedef struct {
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD FloatSave[28];
    DWORD SegGs, SegFs, SegEs, SegDs;
    DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
    DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    unsigned char ExtendedRegisters[512];
} CONTEXT32;

typedef struct {
    EXCEPTION_RECORD32 *ExceptionRecord;
    CONTEXT32 *ContextRecord;
} EXCEPTION_POINTERS32;

_Static_assert(__builtin_offsetof(CONTEXT32, Eip) == 184,
               "CONTEXT32 Eip offset");

static volatile DWORD veh_sequence;
static volatile DWORD veh_first_order;
static volatile DWORD veh_continue_order;
static volatile DWORD veh_observed_code;
static volatile DWORD veh_context_matches_record;

static long WINAPI veh_observe(EXCEPTION_POINTERS32 *pointers)
{
    veh_first_order = ++veh_sequence;
    if (pointers && pointers->ExceptionRecord)
        veh_observed_code = pointers->ExceptionRecord->ExceptionCode;
    return EXCEPTION_CONTINUE_SEARCH;
}

static long WINAPI veh_continue_bound(EXCEPTION_POINTERS32 *pointers)
{
    veh_continue_order = ++veh_sequence;
    if (!pointers || !pointers->ExceptionRecord ||
        !pointers->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    veh_context_matches_record =
        pointers->ExceptionRecord->ExceptionAddress ==
        pointers->ContextRecord->Eip;
    pointers->ContextRecord->Eip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static long WINAPI veh_continue_software(EXCEPTION_POINTERS32 *pointers)
{
    veh_continue_order = ++veh_sequence;
    if (!pointers || !pointers->ExceptionRecord ||
        !pointers->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    veh_context_matches_record =
        pointers->ExceptionRecord->ExceptionAddress ==
        pointers->ContextRecord->Eip;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int mark_filter(volatile DWORD *marker)
{
    *marker = FILTER_MAGIC;
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) static void trigger_access_violation(
    volatile DWORD *address)
{
    *address = 0xBAD0BAD0UL;
}

__declspec(naked) static void trigger_bound_range(const long *bounds)
{
    __asm {
        mov ecx, dword ptr [esp + 4]
        mov eax, 2
        bound eax, [ecx]
        ret
    }
}

__declspec(noinline) static DWORD exercise_seh3(
    volatile DWORD *fault_page)
{
    volatile DWORD filter_marker = 0;
    volatile DWORD finally_marker = 0;
    volatile DWORD handler_marker = 0;
    volatile DWORD continuation_marker = 0;

    __try {
        __try {
            trigger_access_violation(fault_page);
        } __finally {
            finally_marker = FINALLY_MAGIC;
        }
    } __except (mark_filter(&filter_marker)) {
        handler_marker = HANDLER_MAGIC;
    }

    continuation_marker = CONTINUE_MAGIC;
    if (filter_marker != FILTER_MAGIC)
        return 10;
    if (finally_marker != FINALLY_MAGIC)
        return 11;
    if (handler_marker != HANDLER_MAGIC)
        return 12;
    if (continuation_marker != CONTINUE_MAGIC)
        return 13;
    return 0;
}

__declspec(noinline) static DWORD exercise_loader_lock_raise(void)
{
    volatile struct {
        DWORD cookie;
        DWORD guard;
    } lock_result = { 0xFFFFFFFFUL, GUARD_MAGIC };
    volatile DWORD disposition = 0xFFFFFFFFUL;
    volatile DWORD lock_raised = 0;
    volatile DWORD unlock_raised = 0;
    volatile DWORD lock_returned = 0;
    volatile DWORD unlock_returned = 0;
    volatile NTSTATUS status = STATUS_SENTINEL;

    __try {
        status = LdrLockLoaderLock(
            LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS | 0x4UL,
            (DWORD *)&disposition, (DWORD *)&lock_result.cookie);
        lock_returned = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lock_raised = RAISED_MAGIC;
    }

    if (lock_returned || lock_raised != RAISED_MAGIC)
        return 30;
    if (status != STATUS_SENTINEL)
        return 31;
    if (disposition != 0 || lock_result.cookie != 0 ||
        lock_result.guard != GUARD_MAGIC)
        return 32;

    status = STATUS_SENTINEL;
    __try {
        status = LdrUnlockLoaderLock(
            LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS, 0xF0000001UL);
        unlock_returned = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        unlock_raised = RAISED_MAGIC;
    }

    if (unlock_returned || unlock_raised != RAISED_MAGIC)
        return 33;
    if (status != STATUS_SENTINEL)
        return 34;
    return 0;
}

__declspec(noinline) static DWORD exercise_bound_range(void)
{
    volatile long bounds[2] = { 0, 1 };
    volatile DWORD exception_code = 0;
    volatile DWORD handler_marker = 0;

    __try {
        trigger_bound_range((const long *)bounds);
    } __except ((exception_code = (DWORD)_exception_code()),
                EXCEPTION_EXECUTE_HANDLER) {
        handler_marker = BOUND_MAGIC;
    }

    if (handler_marker != BOUND_MAGIC)
        return 40;
    if (exception_code != STATUS_ARRAY_BOUNDS_EXCEEDED)
        return 41;
    return 0;
}

__declspec(noinline) static DWORD exercise_vectored_bound_range(void)
{
    volatile long bounds[2] = { 0, 1 };
    PVOID continue_handle = AddVectoredExceptionHandler(
        0, (PVOID)veh_continue_bound);
    PVOID observe_handle = AddVectoredExceptionHandler(
        1, (PVOID)veh_observe);

    if (!continue_handle || !observe_handle)
        return 50;

    veh_sequence = 0;
    veh_first_order = 0;
    veh_continue_order = 0;
    veh_observed_code = 0;
    veh_context_matches_record = 0;
    trigger_bound_range((const long *)bounds);

    if (veh_first_order != 1 || veh_continue_order != 2)
        return 51;
    if (veh_observed_code != STATUS_ARRAY_BOUNDS_EXCEEDED)
        return 52;
    if (!veh_context_matches_record)
        return 53;
    if (RemoveVectoredExceptionHandler(observe_handle) != 1 ||
        RemoveVectoredExceptionHandler(continue_handle) != 1)
        return 54;
    if (RemoveVectoredExceptionHandler(continue_handle) != 0)
        return 55;
    return 0;
}

__declspec(noinline) static DWORD exercise_vectored_software(void)
{
    PVOID continue_handle = AddVectoredExceptionHandler(
        0, (PVOID)veh_continue_software);
    PVOID observe_handle = AddVectoredExceptionHandler(
        1, (PVOID)veh_observe);

    if (!continue_handle || !observe_handle)
        return 60;

    veh_sequence = 0;
    veh_first_order = 0;
    veh_continue_order = 0;
    veh_observed_code = 0;
    veh_context_matches_record = 0;
    RaiseException(STATUS_SOFTWARE_TEST, 0, 0, (const DWORD *)0);

    if (veh_first_order != 1 || veh_continue_order != 2)
        return 61;
    if (veh_observed_code != STATUS_SOFTWARE_TEST)
        return 62;
    if (!veh_context_matches_record)
        return 63;
    if (RemoveVectoredExceptionHandler(observe_handle) != 1 ||
        RemoveVectoredExceptionHandler(continue_handle) != 1)
        return 64;
    return 0;
}

void mainCRTStartup(void)
{
    DWORD result;
    DWORD old_protection = 0;
    volatile DWORD *fault_page = (volatile DWORD *)VirtualAlloc(
        (PVOID)0, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (!fault_page)
        ExitProcess(20);
    if (!VirtualProtect((PVOID)fault_page, 4096, PAGE_NOACCESS,
                        &old_protection))
        ExitProcess(21);

    result = exercise_seh3(fault_page);
    if (!result)
        result = exercise_loader_lock_raise();
    if (!result)
        result = exercise_vectored_software();
    if (!result)
        result = exercise_vectored_bound_range();
    if (!result)
        result = exercise_bound_range();
    ExitProcess(result);
}
