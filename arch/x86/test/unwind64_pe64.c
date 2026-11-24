/* Freestanding PE32+ contract test for AMD64 function tables and unwind. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long QWORD;
typedef long LONG;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI
#define DLLIMPORT __declspec(dllimport)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define UNW_FLAG_NHANDLER 0
#define DISPOSITION_CONTINUE_EXECUTION 0
#define DISPOSITION_CONTINUE_SEARCH 1
#define FILTER_CONTINUE_EXECUTION ((LONG)-1)
#define FILTER_CONTINUE_SEARCH 0
#define FILTER_EXECUTE_HANDLER 1
#define EXCEPTION_NONCONTINUABLE 1
#define EXCEPTION_UNWINDING 2
#define EXCEPTION_EXIT_UNWIND 4
#define EXCEPTION_NESTED_CALL 0x10
#define EXCEPTION_TARGET_UNWIND 0x20
#define CONTEXT_UNWOUND_TO_CALL 0x20000000U
#define EXCEPTION_BREAKPOINT ((DWORD)0x80000003)
#define EXCEPTION_ILLEGAL_INSTRUCTION ((DWORD)0xC000001D)
#define EXCEPTION_SOFTWARE_TEST ((DWORD)0xE0426464)
#define EXCEPTION_NESTED_TEST ((DWORD)0xE0426565)
#define STATUS_LONGJUMP ((DWORD)0x80000026)
#define STATUS_UNWIND_CONSOLIDATE ((DWORD)0x80000029)

typedef struct {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION;

typedef struct __attribute__((aligned(16))) {
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

typedef struct __attribute__((aligned(16))) {
    QWORD Low;
    QWORD High;
} M128_VALUE;

typedef struct __attribute__((aligned(16))) {
    QWORD Frame;
    QWORD Rbx;
    QWORD Rsp;
    QWORD Rbp;
    QWORD Rsi;
    QWORD Rdi;
    QWORD R12;
    QWORD R13;
    QWORD R14;
    QWORD R15;
    QWORD Rip;
    DWORD MxCsr;
    WORD FpCsr;
    WORD Spare;
    M128_VALUE Xmm6[10];
} JUMP_BUFFER64;

typedef struct __attribute__((aligned(16))) {
    QWORD Rbx;
    QWORD Rbp;
    QWORD Rsi;
    QWORD Rdi;
    QWORD R12;
    QWORD R13;
    QWORD R14;
    QWORD R15;
    DWORD MxCsr;
    WORD FpCsr;
    WORD Reserved;
    M128_VALUE Xmm6[10];
} RESTORED_STATE64;

_Static_assert(sizeof(JUMP_BUFFER64) == 0x100,
               "jump buffer layout changed");
_Static_assert(__builtin_offsetof(RESTORED_STATE64, Xmm6) == 0x50,
               "restored XMM layout changed");

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    QWORD ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct _DISPATCHER_CONTEXT {
    QWORD ControlPc;
    QWORD ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    QWORD EstablisherFrame;
    QWORD TargetIp;
    CONTEXT *ContextRecord;
    PVOID LanguageHandler;
    PVOID HandlerData;
    PVOID HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT;

typedef struct _EXCEPTION_POINTERS {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

typedef struct {
    HANDLE process;
    HANDLE thread;
    DWORD process_id;
    DWORD thread_id;
} PROCESS_INFORMATION64;

DLLIMPORT void WINAPI ExitProcess(DWORD code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT const char *WINAPI GetCommandLineA(void);
DLLIMPORT BOOL WINAPI CreateProcessA(
    const char *application, char *command_line, PVOID process_attributes,
    PVOID thread_attributes, BOOL inherit_handles, DWORD creation_flags,
    PVOID environment, const char *current_directory, PVOID startup_info,
    PROCESS_INFORMATION64 *information);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI GetExitCodeProcess(HANDLE process, DWORD *exit_code);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT BOOL WINAPI RtlAddFunctionTable(RUNTIME_FUNCTION *table,
                                          DWORD count, QWORD base);
DLLIMPORT BOOL WINAPI RtlDeleteFunctionTable(RUNTIME_FUNCTION *table);
DLLIMPORT RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(
    QWORD control_pc, QWORD *image_base, PVOID history_table);
DLLIMPORT PVOID WINAPI RtlVirtualUnwind(
    DWORD handler_type, QWORD image_base, QWORD control_pc,
    RUNTIME_FUNCTION *function_entry, CONTEXT *context,
    PVOID *handler_data, QWORD *establisher_frame, PVOID context_pointers);
DLLIMPORT void WINAPI RaiseException(DWORD code, DWORD flags,
                                     DWORD argument_count,
                                     const QWORD *arguments);
DLLIMPORT void WINAPI DebugBreak(void);
DLLIMPORT void WINAPI RtlCaptureContext(CONTEXT *context);
DLLIMPORT void WINAPI RtlRestoreContext(CONTEXT *context,
                                        EXCEPTION_RECORD *record);
DLLIMPORT void WINAPI RtlRaiseException(EXCEPTION_RECORD *record);
DLLIMPORT void WINAPI RtlRaiseStatus(LONG status);
DLLIMPORT void WINAPI RtlUnwindEx(PVOID target_frame, PVOID target_ip,
                                  EXCEPTION_RECORD *record,
                                  PVOID return_value, CONTEXT *context,
                                  PVOID history_table);

extern BYTE unwind64_fixture_after_push;
extern BYTE unwind64_fixture_body;
extern BYTE unwind64_fixture_epilog;
extern BYTE unwind64_frame_fixture_body;
extern BYTE unwind64_dynamic_probe;
extern BYTE unwind64_return_target;
extern BYTE unwind64_dispatch_fault;
extern BYTE unwind64_dispatch_resume;
extern DWORD unwind64_dispatch_probe(void);
extern BYTE unwind64_debug_break_resume;
extern DWORD unwind64_debug_break_probe(void);
extern DWORD unwind64_longjump_probe(CONTEXT *context,
                                     EXCEPTION_RECORD *record,
                                     JUMP_BUFFER64 *jump,
                                     RESTORED_STATE64 *restored);
extern DWORD unwind64_capture_state_probe(CONTEXT *context,
                                          RESTORED_STATE64 *state);
extern DWORD unwind64_consolidate_probe(CONTEXT *context,
                                        EXCEPTION_RECORD *record);
extern BYTE unwind64_consolidate_resume;
extern BYTE unwind64_consolidate_wrong_target;

static volatile DWORD dispatch_handler_seen;
static volatile DWORD c_specific_filter_seen;
static volatile DWORD c_specific_finally_seen;
static volatile LONG c_specific_filter_result;
static volatile DWORD c_specific_reject_seen;
static volatile DWORD c_specific_accept_seen;
static volatile DWORD c_specific_continue_seen;
static volatile DWORD c_specific_continue_valid;
static volatile DWORD c_specific_continue_after;
static volatile DWORD c_specific_continue_handler;
static volatile DWORD c_specific_software_filter_seen;
static volatile DWORD c_specific_software_filter_valid;
static volatile DWORD c_specific_software_expected_flags;
static volatile DWORD c_specific_software_returned;
static volatile DWORD c_specific_software_caught;
static volatile DWORD nested_raising_filter_seen;
static volatile DWORD nested_raising_filter_valid;
static volatile DWORD nested_raising_filter_returned;
static volatile DWORD nested_inner_handler;
static volatile DWORD nested_inner_returned;
static volatile DWORD nested_outer_filter_seen;
static volatile DWORD nested_outer_filter_valid;
static volatile DWORD nested_outer_caught;
static volatile DWORD context_restore_phase;
static volatile DWORD context_restore_returned;
static volatile DWORD debug_break_filter_seen;
static volatile DWORD debug_break_filter_valid;
static volatile DWORD debug_break_handler_seen;
static EXCEPTION_RECORD *consolidate_expected_record;
static volatile DWORD consolidate_callback_seen;
static volatile DWORD consolidate_record_valid;
static volatile DWORD consolidate_frame_valid;
static volatile DWORD rtl_unwind_finally_order;
static volatile DWORD rtl_unwind_returned;
static volatile QWORD rtl_unwind_return_value;
static volatile DWORD rtl_collision_finally_seen;
static volatile DWORD rtl_collision_after_nested;
static volatile DWORD rtl_collision_returned;
static volatile DWORD rtl_collision_started;
static volatile QWORD rtl_collision_return_value;
static volatile DWORD exit_unwind_finally_order;
static EXCEPTION_RECORD exit_unwind_record;

#define EXIT_UNWIND_CHILD_STATUS ((DWORD)0x5A)
#define EXIT_UNWIND_INNER_STATUS ((DWORD)0xE0427102)
#define EXIT_UNWIND_BAD_INNER_ORDER ((DWORD)0xE04271E1)
#define EXIT_UNWIND_BAD_INNER_FLAGS ((DWORD)0xE04271E2)
#define EXIT_UNWIND_BAD_INNER_RECORD ((DWORD)0xE04271E3)
#define EXIT_UNWIND_BAD_OUTER_ORDER ((DWORD)0xE04271E4)
#define EXIT_UNWIND_BAD_OUTER_STATUS ((DWORD)0xE04271E5)
#define EXIT_UNWIND_BAD_OUTER_FLAGS ((DWORD)0xE04271E6)
#define EXIT_UNWIND_RETURNED ((DWORD)0xE04271EF)

LONG unwind64_test_personality(EXCEPTION_RECORD *record, PVOID frame,
                               CONTEXT *context,
                               DISPATCHER_CONTEXT *dispatch)
{
    if (!record || !frame || !context || !dispatch ||
        record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION ||
        record->ExceptionAddress != (PVOID)&unwind64_dispatch_fault ||
        dispatch->ControlPc != (QWORD)&unwind64_dispatch_fault ||
        !dispatch->ImageBase || !dispatch->FunctionEntry ||
        !dispatch->EstablisherFrame ||
        dispatch->EstablisherFrame != (QWORD)frame ||
        !dispatch->ContextRecord ||
        dispatch->ContextRecord->Rip == dispatch->ControlPc)
        return DISPOSITION_CONTINUE_SEARCH;

    dispatch_handler_seen = 1;
    context->Rip = (QWORD)&unwind64_dispatch_resume;
    return DISPOSITION_CONTINUE_EXECUTION;
}

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

static void clear(void *buffer, DWORD size)
{
    BYTE *bytes = (BYTE *)buffer;
    for (DWORD i = 0; i < size; i++) bytes[i] = 0;
}

static BOOL bytes_equal(const void *left, const void *right, DWORD size)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    for (DWORD i = 0; i < size; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static BOOL text_contains(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return 0;
    for (; *text; text++) {
        DWORD i = 0;
        while (needle[i] && text[i] == needle[i]) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}

static DWORD test_body_unwind(QWORD image_base,
                              RUNTIME_FUNCTION *function)
{
    QWORD stack[16] = {0};
    CONTEXT context;
    PVOID handler_data = (PVOID)1;
    QWORD frame = 0;
    const QWORD saved_rbx = 0x1122334455667788ULL;
    const QWORD saved_rsi = 0x8877665544332211ULL;
    const QWORD return_address = (QWORD)&unwind64_return_target;

    stack[5] = saved_rsi;
    stack[7] = saved_rbx;
    stack[8] = return_address;
    clear(&context, sizeof(context));
    context.Rsp = (QWORD)&stack[1];
    context.Rbx = 0xBAD0BAD0BAD0BAD0ULL;
    context.Rsi = 0xBAD1BAD1BAD1BAD1ULL;

    PVOID handler = RtlVirtualUnwind(
        UNW_FLAG_NHANDLER, image_base, (QWORD)&unwind64_fixture_body,
        function, &context, &handler_data, &frame, (PVOID)0);
    if (handler || handler_data) return 10;
    if (frame != (QWORD)&stack[1]) return 11;
    if (context.Rbx != saved_rbx || context.Rsi != saved_rsi) return 12;
    if (context.Rip != return_address ||
        context.Rsp != (QWORD)&stack[9]) return 13;
    return 0;
}

static DWORD test_partial_prolog(QWORD image_base,
                                 RUNTIME_FUNCTION *function)
{
    QWORD stack[8] = {0};
    CONTEXT context;
    QWORD frame = 0;
    const QWORD saved_rbx = 0xAABBCCDDEEFF0011ULL;
    const QWORD return_address = (QWORD)&unwind64_return_target;

    stack[3] = saved_rbx;
    stack[4] = return_address;
    clear(&context, sizeof(context));
    context.Rsp = (QWORD)&stack[3];
    context.Rbx = 0;

    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base,
                     (QWORD)&unwind64_fixture_after_push, function,
                     &context, (PVOID *)0, &frame, (PVOID)0);
    if (context.Rbx != saved_rbx || context.Rip != return_address)
        return 20;
    if (context.Rsp != (QWORD)&stack[5] || frame != (QWORD)&stack[3])
        return 21;
    return 0;
}

static DWORD test_epilog(QWORD image_base, RUNTIME_FUNCTION *function)
{
    QWORD stack[16] = {0};
    CONTEXT context;
    const QWORD saved_rbx = 0x1020304050607080ULL;
    const QWORD saved_rsi = 0x9080706050403020ULL;
    const QWORD return_address = (QWORD)&unwind64_return_target;

    stack[7] = saved_rbx;
    stack[8] = return_address;
    clear(&context, sizeof(context));
    context.Rsp = (QWORD)&stack[1];
    context.Rbx = 0;
    context.Rsi = saved_rsi;

    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base,
                     (QWORD)&unwind64_fixture_epilog, function,
                     &context, (PVOID *)0, (QWORD *)0, (PVOID)0);
    if (context.Rbx != saved_rbx || context.Rsi != saved_rsi)
        return 30;
    if (context.Rip != return_address ||
        context.Rsp != (QWORD)&stack[9]) return 31;
    return 0;
}

static DWORD test_frame_register(void)
{
    QWORD image_base = 0;
    RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(
        (QWORD)&unwind64_frame_fixture_body, &image_base, (PVOID)0);
    if (!function || !image_base) return 40;

    QWORD stack[20] = {0};
    CONTEXT context;
    QWORD frame = 0;
    const QWORD saved_rbp = 0xCAFEBABE12345678ULL;
    const QWORD return_address = (QWORD)&unwind64_return_target;
    stack[11] = saved_rbp;
    stack[12] = return_address;
    clear(&context, sizeof(context));
    context.Rsp = (QWORD)&stack[3];
    context.Rbp = (QWORD)&stack[7];

    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base,
                     (QWORD)&unwind64_frame_fixture_body, function,
                     &context, (PVOID *)0, &frame, (PVOID)0);
    if (frame != (QWORD)&stack[3] || context.Rbp != saved_rbp)
        return 41;
    if (context.Rip != return_address ||
        context.Rsp != (QWORD)&stack[13]) return 42;
    return 0;
}

static DWORD test_dynamic_table(void)
{
    RUNTIME_FUNCTION table = { 0x100, 0x101, 0 };
    QWORD base = (QWORD)&unwind64_dynamic_probe - table.BeginAddress;
    QWORD found_base = 0;

    if (RtlLookupFunctionEntry((QWORD)&unwind64_dynamic_probe,
                               &found_base, (PVOID)0))
        return 50;
    if (!RtlAddFunctionTable(&table, 1, base)) return 51;
    if (RtlAddFunctionTable(&table, 1, base)) return 52;

    RUNTIME_FUNCTION *found = RtlLookupFunctionEntry(
        (QWORD)&unwind64_dynamic_probe, &found_base, (PVOID)0);
    if (found != &table || found_base != base) return 53;
    if (!RtlDeleteFunctionTable(&table)) return 54;
    if (RtlDeleteFunctionTable(&table)) return 55;
    found_base = 0;
    if (RtlLookupFunctionEntry((QWORD)&unwind64_dynamic_probe,
                               &found_base, (PVOID)0))
        return 56;
    return 0;
}

static DWORD test_frame_dispatch(void)
{
    dispatch_handler_seen = 0;
    if (unwind64_dispatch_probe() != 0x64U) return 60;
    if (!dispatch_handler_seen) return 61;
    return 0;
}

__declspec(noinline) static LONG c_specific_filter(void)
{
    c_specific_filter_seen++;
    return c_specific_filter_result;
}

__declspec(noinline) static void c_specific_fault_child(void)
{
    __try {
        __asm__ volatile ("ud2");
    } __finally {
        c_specific_finally_seen++;
    }
}

__declspec(noinline) static DWORD test_c_specific_handler(void)
{
    DWORD caught = 0;
    c_specific_filter_seen = 0;
    c_specific_finally_seen = 0;
    c_specific_filter_result = FILTER_EXECUTE_HANDLER;

    __try {
        c_specific_fault_child();
    } __except (c_specific_filter()) {
        caught = 1;
    }

    if (!caught) return 70;
    if (c_specific_filter_seen != 1) return 71;
    if (c_specific_finally_seen != 1) return 72;
    return 0;
}

__declspec(noinline) static LONG c_specific_reject_filter(void)
{
    c_specific_reject_seen++;
    return FILTER_CONTINUE_SEARCH;
}

__declspec(noinline) static LONG c_specific_accept_filter(void)
{
    c_specific_accept_seen++;
    return FILTER_EXECUTE_HANDLER;
}

__declspec(noinline) static DWORD test_c_specific_continue_search(void)
{
    DWORD caught = 0;
    DWORD inner_caught = 0;
    c_specific_reject_seen = 0;
    c_specific_accept_seen = 0;
    c_specific_finally_seen = 0;

    __try {
        __try {
            c_specific_fault_child();
        } __except (c_specific_reject_filter()) {
            inner_caught = 1;
        }
    } __except (c_specific_accept_filter()) {
        caught = 1;
    }

    if (!caught || inner_caught) return 73;
    if (c_specific_reject_seen != 1) return 74;
    if (c_specific_accept_seen != 1) return 75;
    if (c_specific_finally_seen != 1) return 76;
    return 0;
}

__declspec(noinline) static LONG c_specific_continue_filter(
    EXCEPTION_POINTERS *pointers)
{
    c_specific_continue_seen++;
    if (!pointers || !pointers->ExceptionRecord ||
        !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode !=
            EXCEPTION_ILLEGAL_INSTRUCTION)
        return FILTER_CONTINUE_SEARCH;

    c_specific_continue_valid =
        pointers->ExceptionRecord->ExceptionAddress ==
        (PVOID)(QWORD)pointers->ContextRecord->Rip;
    pointers->ContextRecord->Rip += 2;
    return FILTER_CONTINUE_EXECUTION;
}

__declspec(noinline) static void c_specific_continue_probe(void)
{
    __try {
        __asm__ volatile ("ud2");
        c_specific_continue_after++;
    } __except (c_specific_continue_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        c_specific_continue_handler++;
    }
}

static DWORD test_c_specific_continue_execution(void)
{
    c_specific_continue_seen = 0;
    c_specific_continue_valid = 0;
    c_specific_continue_after = 0;
    c_specific_continue_handler = 0;

    c_specific_continue_probe();

    if (c_specific_continue_seen != 1) return 77;
    if (!c_specific_continue_valid) return 78;
    if (c_specific_continue_after != 1) return 79;
    if (c_specific_continue_handler) return 80;
    return 0;
}

__declspec(noinline) static LONG c_specific_software_filter(
    EXCEPTION_POINTERS *pointers)
{
    c_specific_software_filter_seen++;
    c_specific_software_filter_valid =
        pointers && pointers->ExceptionRecord &&
        pointers->ContextRecord &&
        pointers->ExceptionRecord->ExceptionCode ==
            EXCEPTION_SOFTWARE_TEST &&
        (pointers->ExceptionRecord->ExceptionFlags &
             EXCEPTION_NONCONTINUABLE) ==
            c_specific_software_expected_flags &&
        pointers->ExceptionRecord->ExceptionAddress ==
            (PVOID)(QWORD)pointers->ContextRecord->Rip;
    return c_specific_software_filter_valid ? FILTER_EXECUTE_HANDLER
                                            : FILTER_CONTINUE_SEARCH;
}

__declspec(noinline) static DWORD test_c_specific_software_exception(void)
{
    c_specific_software_filter_seen = 0;
    c_specific_software_filter_valid = 0;
    c_specific_software_expected_flags = 0;
    c_specific_software_returned = 0;
    c_specific_software_caught = 0;

    __try {
        RaiseException(EXCEPTION_SOFTWARE_TEST, 0, 0,
                       (const QWORD *)0);
        c_specific_software_returned = 1;
    } __except (c_specific_software_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        c_specific_software_caught = 1;
    }

    if (c_specific_software_filter_seen != 1) return 81;
    if (!c_specific_software_filter_valid) return 82;
    if (c_specific_software_returned) return 83;
    if (!c_specific_software_caught) return 84;
    return 0;
}

__declspec(noinline) static LONG nested_raising_filter(
    EXCEPTION_POINTERS *pointers)
{
    nested_raising_filter_seen++;
    nested_raising_filter_valid =
        pointers && pointers->ExceptionRecord && pointers->ContextRecord &&
        pointers->ExceptionRecord->ExceptionCode ==
            EXCEPTION_SOFTWARE_TEST &&
        pointers->ExceptionRecord->ExceptionAddress ==
            (PVOID)(QWORD)pointers->ContextRecord->Rip;
    RaiseException(EXCEPTION_NESTED_TEST, 0, 0, (const QWORD *)0);
    nested_raising_filter_returned = 1;
    return FILTER_CONTINUE_SEARCH;
}

__declspec(noinline) static void nested_exception_inner(void)
{
    __try {
        RaiseException(EXCEPTION_SOFTWARE_TEST, 0, 0, (const QWORD *)0);
    } __except (nested_raising_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        nested_inner_handler = 1;
    }
    nested_inner_returned = 1;
}

__declspec(noinline) static LONG nested_outer_filter(
    EXCEPTION_POINTERS *pointers)
{
    nested_outer_filter_seen++;
    BOOL matches = pointers && pointers->ExceptionRecord &&
                   pointers->ExceptionRecord->ExceptionCode ==
                       EXCEPTION_NESTED_TEST;
    nested_outer_filter_valid =
        matches && pointers->ContextRecord &&
        !(pointers->ExceptionRecord->ExceptionFlags &
          EXCEPTION_NESTED_CALL) &&
        pointers->ExceptionRecord->ExceptionAddress ==
            (PVOID)(QWORD)pointers->ContextRecord->Rip;
    return matches ? FILTER_EXECUTE_HANDLER : FILTER_CONTINUE_SEARCH;
}

__declspec(noinline) static DWORD test_nested_exception(void)
{
    nested_raising_filter_seen = 0;
    nested_raising_filter_valid = 0;
    nested_raising_filter_returned = 0;
    nested_inner_handler = 0;
    nested_inner_returned = 0;
    nested_outer_filter_seen = 0;
    nested_outer_filter_valid = 0;
    nested_outer_caught = 0;

    __try {
        nested_exception_inner();
    } __except (nested_outer_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        nested_outer_caught = 1;
    }

    if (nested_raising_filter_seen != 1) return 111;
    if (!nested_raising_filter_valid) return 112;
    if (nested_raising_filter_returned) return 113;
    if (nested_inner_handler) return 114;
    if (nested_inner_returned) return 115;
    if (nested_outer_filter_seen != 1) return 116;
    if (!nested_outer_filter_valid) return 117;
    if (!nested_outer_caught) return 118;
    return 0;
}

__declspec(noinline) static LONG debug_break_continue_filter(
    EXCEPTION_POINTERS *pointers)
{
    debug_break_filter_seen++;
    debug_break_filter_valid =
        pointers && pointers->ExceptionRecord && pointers->ContextRecord &&
        pointers->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        pointers->ExceptionRecord->ExceptionAddress ==
            (PVOID)&unwind64_debug_break_resume &&
        pointers->ContextRecord->Rip ==
            (QWORD)&unwind64_debug_break_resume;
    return debug_break_filter_valid ? FILTER_CONTINUE_EXECUTION
                                    : FILTER_CONTINUE_SEARCH;
}

static DWORD test_debug_break_context(void)
{
    DWORD result = 0;
    debug_break_filter_seen = 0;
    debug_break_filter_valid = 0;
    debug_break_handler_seen = 0;
    __try {
        result = unwind64_debug_break_probe();
    } __except (debug_break_continue_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        debug_break_handler_seen = 1;
    }
    if (debug_break_filter_seen != 1) return 134;
    if (!debug_break_filter_valid) return 135;
    if (debug_break_handler_seen) return 136;
    if (result != 0xDBU) return 137;
    return 0;
}

__declspec(noinline) static DWORD test_capture_restore_context(void)
{
    CONTEXT context;
    clear(&context, sizeof(context));
    context_restore_phase = 0;
    context_restore_returned = 0;

    RtlCaptureContext(&context);
    if (context.ContextFlags != 0x00100007U) return 85;
    if (!context.Rip || !context.Rsp || (context.Rsp & 15U)) return 86;
    if (!context.MxCsr) return 87;

    context_restore_phase = 1;
    context.Rip = (QWORD)&&context_restored;
    RtlRestoreContext(&context, (EXCEPTION_RECORD *)0);
    context_restore_returned = 1;

context_restored:
    if (context_restore_phase != 1) return 88;
    if (context_restore_returned) return 89;
    return 0;
}

static DWORD test_capture_nonvolatile_state(void)
{
    CONTEXT context;
    RESTORED_STATE64 expected;
    clear(&context, sizeof(context));
    clear(&expected, sizeof(expected));
    expected.Rbx = 0x0102030405060708ULL;
    expected.Rbp = 0x1112131415161718ULL;
    expected.Rsi = 0x2122232425262728ULL;
    expected.Rdi = 0x3132333435363738ULL;
    expected.R12 = 0x4142434445464748ULL;
    expected.R13 = 0x5152535455565758ULL;
    expected.R14 = 0x6162636465666768ULL;
    expected.R15 = 0x7172737475767778ULL;
    expected.MxCsr = 0x00005F80U;
    expected.FpCsr = 0x0B7FU;
    for (DWORD i = 0; i < 10; i++) {
        expected.Xmm6[i].Low = 0xC0C0C0C000000000ULL + i;
        expected.Xmm6[i].High = 0xD0D0D0D000000000ULL + i;
    }

    if (unwind64_capture_state_probe(&context, &expected)) return 129;
    if (context.ContextFlags != 0x00100007U) return 130;
    if (context.Rbx != expected.Rbx || context.Rbp != expected.Rbp ||
        context.Rsi != expected.Rsi || context.Rdi != expected.Rdi ||
        context.R12 != expected.R12 || context.R13 != expected.R13 ||
        context.R14 != expected.R14 || context.R15 != expected.R15)
        return 131;
    if (context.MxCsr != expected.MxCsr ||
        !bytes_equal(&context.Reserved[0x18], &expected.MxCsr,
                     sizeof(expected.MxCsr)) ||
        !bytes_equal(&context.Reserved[0], &expected.FpCsr,
                     sizeof(expected.FpCsr)))
        return 132;
    if (!bytes_equal(&context.Reserved[0x100], expected.Xmm6,
                     sizeof(expected.Xmm6)))
        return 133;
    return 0;
}

static DWORD test_restore_longjump(void)
{
    CONTEXT context;
    EXCEPTION_RECORD record;
    JUMP_BUFFER64 jump;
    RESTORED_STATE64 restored;
    clear(&context, sizeof(context));
    clear(&record, sizeof(record));
    clear(&jump, sizeof(jump));
    clear(&restored, sizeof(restored));

    jump.Rbx = 0x1111222233334444ULL;
    jump.Rbp = 0x2222333344445555ULL;
    jump.Rsi = 0x3333444455556666ULL;
    jump.Rdi = 0x4444555566667777ULL;
    jump.R12 = 0x5555666677778888ULL;
    jump.R13 = 0x6666777788889999ULL;
    jump.R14 = 0x777788889999AAAAULL;
    jump.R15 = 0x88889999AAAABBBBULL;
    jump.MxCsr = 0x00003F80U;
    jump.FpCsr = 0x077FU;
    for (DWORD i = 0; i < 10; i++) {
        jump.Xmm6[i].Low = 0xA0A0A0A000000000ULL + i;
        jump.Xmm6[i].High = 0xB0B0B0B000000000ULL + i;
    }

    record.ExceptionCode = STATUS_LONGJUMP;
    record.NumberParameters = 1;
    record.ExceptionInformation[0] = (QWORD)&jump;

    if (unwind64_longjump_probe(&context, &record, &jump, &restored))
        return 119;
    if (restored.Rbx != jump.Rbx || restored.Rbp != jump.Rbp ||
        restored.Rsi != jump.Rsi || restored.Rdi != jump.Rdi)
        return 120;
    if (restored.R12 != jump.R12 || restored.R13 != jump.R13 ||
        restored.R14 != jump.R14 || restored.R15 != jump.R15)
        return 121;
    if (restored.MxCsr != jump.MxCsr || restored.FpCsr != jump.FpCsr)
        return 122;
    if (!bytes_equal(restored.Xmm6, jump.Xmm6, sizeof(jump.Xmm6)))
        return 123;
    return 0;
}

__declspec(noinline) static PVOID consolidate_callback(
    EXCEPTION_RECORD *record)
{
    CONTEXT unwound;
    CONTEXT *expected = (CONTEXT *)0;
    QWORD image_base = 0;
    QWORD frame = 0;
    PVOID handler_data = (PVOID)1;
    consolidate_callback_seen++;
    consolidate_record_valid =
        record && record == consolidate_expected_record &&
        record->ExceptionCode == STATUS_UNWIND_CONSOLIDATE &&
        record->NumberParameters >= 2 &&
        record->ExceptionInformation[0] == (QWORD)&consolidate_callback;
    if (consolidate_record_valid)
        expected = (CONTEXT *)record->ExceptionInformation[1];

    clear(&unwound, sizeof(unwound));
    RtlCaptureContext(&unwound);
    RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(
        unwound.Rip, &image_base, (PVOID)0);
    if (function && image_base) {
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, unwound.Rip,
                         function, &unwound, &handler_data, &frame,
                         (PVOID)0);
        image_base = 0;
        handler_data = (PVOID)1;
        function = RtlLookupFunctionEntry(unwound.Rip, &image_base,
                                          (PVOID)0);
        if (function && image_base) {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, unwound.Rip,
                             function, &unwound, &handler_data, &frame,
                             (PVOID)0);
            consolidate_frame_valid =
                expected && !handler_data &&
                !(unwound.ContextFlags & CONTEXT_UNWOUND_TO_CALL) &&
                unwound.Rip == expected->Rip &&
                unwound.Rsp == expected->Rsp &&
                unwound.Rbx == expected->Rbx &&
                unwound.Rbp == expected->Rbp &&
                unwound.Rsi == expected->Rsi &&
                unwound.Rdi == expected->Rdi &&
                unwound.R12 == expected->R12 &&
                unwound.R13 == expected->R13 &&
                unwound.R14 == expected->R14 &&
                unwound.R15 == expected->R15 &&
                bytes_equal(&unwound.Reserved[0x100],
                            &expected->Reserved[0x100], 10 * 16);
        }
    }
    return (PVOID)&unwind64_consolidate_resume;
}

static DWORD test_restore_unwind_consolidate(void)
{
    CONTEXT context;
    EXCEPTION_RECORD record;
    clear(&context, sizeof(context));
    clear(&record, sizeof(record));
    consolidate_expected_record = &record;
    consolidate_callback_seen = 0;
    consolidate_record_valid = 0;
    consolidate_frame_valid = 0;
    record.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = (QWORD)&consolidate_callback;
    record.ExceptionInformation[1] = (QWORD)&context;

    DWORD probe_result = unwind64_consolidate_probe(&context, &record);
    consolidate_expected_record = (EXCEPTION_RECORD *)0;
    if (probe_result) return 124;
    if (consolidate_callback_seen != 1) return 125;
    if (!consolidate_record_valid) return 126;
    if (!consolidate_frame_valid) return 127;
    if (context.Rip != (QWORD)&unwind64_consolidate_resume) return 128;
    return 0;
}

__declspec(noinline) static DWORD test_direct_rtl_raise_exception(void)
{
    EXCEPTION_RECORD record;
    clear(&record, sizeof(record));
    record.ExceptionCode = EXCEPTION_SOFTWARE_TEST;
    c_specific_software_filter_seen = 0;
    c_specific_software_filter_valid = 0;
    c_specific_software_expected_flags = 0;
    c_specific_software_returned = 0;
    c_specific_software_caught = 0;

    __try {
        RtlRaiseException(&record);
        c_specific_software_returned = 1;
    } __except (c_specific_software_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        c_specific_software_caught = 1;
    }

    if (c_specific_software_filter_seen != 1) return 90;
    if (!c_specific_software_filter_valid) return 91;
    if (c_specific_software_returned) return 92;
    if (!c_specific_software_caught) return 93;
    return 0;
}

__declspec(noinline) static DWORD test_rtl_raise_status(void)
{
    c_specific_software_filter_seen = 0;
    c_specific_software_filter_valid = 0;
    c_specific_software_expected_flags = EXCEPTION_NONCONTINUABLE;
    c_specific_software_returned = 0;
    c_specific_software_caught = 0;

    __try {
        RtlRaiseStatus((LONG)EXCEPTION_SOFTWARE_TEST);
        c_specific_software_returned = 1;
    } __except (c_specific_software_filter(
                    (EXCEPTION_POINTERS *)_exception_info())) {
        c_specific_software_caught = 1;
    }

    if (c_specific_software_filter_seen != 1) return 94;
    if (!c_specific_software_filter_valid) return 95;
    if (c_specific_software_returned) return 96;
    if (!c_specific_software_caught) return 97;
    return 0;
}

__declspec(noinline) static void rtl_unwind_inner(PVOID target_frame,
                                                   PVOID target_ip)
{
    CONTEXT context;
    clear(&context, sizeof(context));
    __try {
        RtlUnwindEx(target_frame, target_ip, (EXCEPTION_RECORD *)0,
                    (PVOID)0x1234ABCD55667788ULL, &context, (PVOID)0);
        rtl_unwind_returned = 1;
    } __finally {
        rtl_unwind_finally_order = rtl_unwind_finally_order * 10U + 2U;
    }
}

__declspec(noinline) static void rtl_unwind_outer(PVOID target_frame,
                                                   PVOID target_ip)
{
    __try {
        rtl_unwind_inner(target_frame, target_ip);
        rtl_unwind_returned = 2;
    } __finally {
        rtl_unwind_finally_order = rtl_unwind_finally_order * 10U + 1U;
    }
}

__declspec(noinline) static DWORD test_rtl_unwind_ex(void)
{
    CONTEXT captured;
    QWORD image_base = 0;
    QWORD target_frame = 0;
    QWORD control_pc;
    PVOID handler_data = (PVOID)0;
    clear(&captured, sizeof(captured));

    RtlCaptureContext(&captured);
    control_pc = captured.Rip;
    RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(
        control_pc, &image_base, (PVOID)0);
    if (!function || !image_base) return 98;

    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, control_pc, function,
                     &captured, &handler_data, &target_frame, (PVOID)0);
    if (!target_frame) return 99;

    rtl_unwind_finally_order = 0;
    rtl_unwind_returned = 0;
    rtl_unwind_return_value = 0;
    rtl_unwind_outer((PVOID)target_frame, (PVOID)&&rtl_unwind_target);
    rtl_unwind_returned = 3;

rtl_unwind_target:
    __asm__ volatile ("movq %%rax, %0"
                      : "=m"(rtl_unwind_return_value) : : "memory");
    if (rtl_unwind_returned) return 100;
    if (rtl_unwind_finally_order != 21U) return 101;
    if (rtl_unwind_return_value != 0x1234ABCD55667788ULL) return 102;
    return 0;
}

__declspec(noinline) static void rtl_collision_leaf(PVOID target_frame,
                                                     PVOID target_ip)
{
    CONTEXT context;
    clear(&context, sizeof(context));
    RtlUnwindEx(target_frame, target_ip, (EXCEPTION_RECORD *)0,
                (PVOID)0x1111222233334444ULL, &context, (PVOID)0);
    rtl_collision_returned = 1;
}

__declspec(noinline) static void rtl_collision_source(PVOID target_frame,
                                                       PVOID first_target,
                                                       PVOID second_target)
{
    __try {
        rtl_collision_leaf(target_frame, first_target);
        rtl_collision_returned = 2;
    } __finally {
        CONTEXT context;
        rtl_collision_finally_seen++;
        clear(&context, sizeof(context));
        RtlUnwindEx(target_frame, second_target, (EXCEPTION_RECORD *)0,
                    (PVOID)0xAAAABBBBCCCCDDDDULL, &context, (PVOID)0);
        rtl_collision_after_nested = 1;
    }
}

__declspec(noinline) static DWORD test_rtl_collided_unwind(void)
{
    CONTEXT captured;
    QWORD image_base = 0;
    QWORD target_frame = 0;
    QWORD target_ip;
    QWORD control_pc;
    PVOID handler_data = (PVOID)0;
    clear(&captured, sizeof(captured));

    RtlCaptureContext(&captured);
    __asm__ volatile ("movq %%rax, %0"
                      : "=m"(rtl_collision_return_value) : : "memory");
    if (rtl_collision_started) {
        if (rtl_collision_return_value == 0x1111222233334444ULL)
            return 106;
        if (rtl_collision_returned) return 107;
        if (rtl_collision_after_nested) return 108;
        if (rtl_collision_finally_seen != 1) return 109;
        if (rtl_collision_return_value != 0xAAAABBBBCCCCDDDDULL)
            return 110;
        return 0;
    }

    target_ip = captured.Rip;
    control_pc = captured.Rip;
    RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(
        control_pc, &image_base, (PVOID)0);
    if (!function || !image_base) return 103;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, control_pc, function,
                     &captured, &handler_data, &target_frame, (PVOID)0);
    if (!target_frame) return 104;

    rtl_collision_finally_seen = 0;
    rtl_collision_after_nested = 0;
    rtl_collision_returned = 0;
    rtl_collision_return_value = 0;
    rtl_collision_started = 1;
    rtl_collision_source((PVOID)target_frame, (PVOID)target_ip,
                         (PVOID)target_ip);
    return 105;
}

static BOOL exit_unwind_flags_valid(void)
{
    DWORD required = EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND;
    return (exit_unwind_record.ExceptionFlags & required) == required &&
           !(exit_unwind_record.ExceptionFlags & EXCEPTION_TARGET_UNWIND);
}

__declspec(noinline) static void exit_unwind_leaf(void)
{
    CONTEXT context;
    clear(&context, sizeof(context));

    /* TargetIp is intentionally invalid: an exit unwind must ignore it. */
    RtlUnwindEx((PVOID)0, (PVOID)1, &exit_unwind_record,
                (PVOID)0x8877665544332211ULL, &context, (PVOID)0);
    exit_unwind_record.ExceptionCode = EXIT_UNWIND_RETURNED;
    ExitProcess(EXIT_UNWIND_RETURNED);
}

__declspec(noinline) static void exit_unwind_inner(void)
{
    __try {
        exit_unwind_leaf();
    } __finally {
        if (exit_unwind_finally_order != 0)
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_INNER_ORDER;
        else if (!exit_unwind_flags_valid())
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_INNER_FLAGS;
        else if (exit_unwind_record.ExceptionAddress !=
                 (PVOID)&exit_unwind_leaf)
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_INNER_RECORD;
        else {
            exit_unwind_finally_order = 2;
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_INNER_STATUS;
        }
    }
}

__declspec(noinline) static void exit_unwind_outer(void)
{
    __try {
        exit_unwind_inner();
    } __finally {
        if (exit_unwind_finally_order != 2)
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_OUTER_ORDER;
        else if (exit_unwind_record.ExceptionCode !=
                 EXIT_UNWIND_INNER_STATUS)
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_OUTER_STATUS;
        else if (!exit_unwind_flags_valid())
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_BAD_OUTER_FLAGS;
        else {
            exit_unwind_finally_order = 21;
            exit_unwind_record.ExceptionCode = EXIT_UNWIND_CHILD_STATUS;
        }
    }
}

static void run_exit_unwind_child(void)
{
    clear(&exit_unwind_record, sizeof(exit_unwind_record));
    exit_unwind_finally_order = 0;
    exit_unwind_record.ExceptionCode = (DWORD)0xE0427100;
    exit_unwind_record.ExceptionAddress = (PVOID)&exit_unwind_leaf;
    exit_unwind_outer();
    ExitProcess(EXIT_UNWIND_RETURNED);
}

static DWORD test_exit_unwind_child(void)
{
    BYTE startup_info[104] = {0};
    PROCESS_INFORMATION64 information = {0};
    char command_line[] =
        "unwind64_pe64.exe --exit-unwind-child";
    DWORD exit_code = 0xFFFFFFFFU;
    *(DWORD *)startup_info = sizeof(startup_info);

    if (!CreateProcessA("unwind64_pe64.exe", command_line, (PVOID)0,
                        (PVOID)0, 0, 0, (PVOID)0, (const char *)0,
                        startup_info, &information))
        return 129;
    if (!information.process || !information.thread ||
        !information.process_id || !information.thread_id)
        return 130;
    if (!CloseHandle(information.thread)) return 131;
    if (WaitForSingleObject(information.process, 0xFFFFFFFFU) != 0)
        return 132;
    if (!GetExitCodeProcess(information.process, &exit_code)) return 133;
    if (!CloseHandle(information.process)) return 134;
    if (exit_code != EXIT_UNWIND_CHILD_STATUS) return 135;
    return 0;
}

void mainCRTStartup(void)
{
    if (text_contains(GetCommandLineA(), "--exit-unwind-child")) {
        run_exit_unwind_child();
        ExitProcess(EXIT_UNWIND_RETURNED);
    }

    DWORD result = 0;
    QWORD image_base = 0;
    RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(
        (QWORD)&unwind64_fixture_body, &image_base, (PVOID)0);
    if (!function || !image_base ||
        (QWORD)&unwind64_fixture_body - image_base < function->BeginAddress ||
        (QWORD)&unwind64_fixture_body - image_base >= function->EndAddress)
        result = 1;
    if (!result) result = test_body_unwind(image_base, function);
    if (!result) result = test_partial_prolog(image_base, function);
    if (!result) result = test_epilog(image_base, function);
    if (!result) result = test_frame_register();
    if (!result) result = test_dynamic_table();
    if (!result) result = test_frame_dispatch();
    if (!result) result = test_debug_break_context();
    if (!result) result = test_c_specific_handler();
    if (!result) result = test_c_specific_continue_search();
    if (!result) result = test_c_specific_continue_execution();
    if (!result) result = test_c_specific_software_exception();
    if (!result) result = test_capture_restore_context();
    if (!result) result = test_capture_nonvolatile_state();
    if (!result) result = test_restore_longjump();
    if (!result) result = test_restore_unwind_consolidate();
    if (!result) result = test_direct_rtl_raise_exception();
    if (!result) result = test_rtl_raise_status();
    if (!result) result = test_rtl_unwind_ex();
    if (!result) result = test_rtl_collided_unwind();
    if (!result) result = test_exit_unwind_child();
    if (!result) result = test_nested_exception();

    print(result ? "UNWIND64 TEST FAIL\r\n" : "UNWIND64 TEST PASS\r\n");
    ExitProcess(result);
    for (;;) { }
}
