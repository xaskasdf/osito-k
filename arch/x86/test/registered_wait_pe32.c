/* PE32 contract test for thread-pool registered waits and callbacks. */

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define FALSE 0
#define TRUE 1
#define INFINITE 0xFFFFFFFFU
#define WAIT_OBJECT_0 0U
#define WT_EXECUTEONLYONCE 0x00000008U
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT void WINAPI Sleep(DWORD milliseconds);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT HANDLE WINAPI CreateEventA(PVOID attributes, BOOL manual_reset,
                                     BOOL initial_state, const char *name);
DLLIMPORT BOOL WINAPI SetEvent(HANDLE event);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI RegisterWaitForSingleObject(
    HANDLE *new_wait, HANDLE object, PVOID callback, PVOID context,
    DWORD milliseconds, DWORD flags);
DLLIMPORT BOOL WINAPI UnregisterWaitEx(HANDLE wait, HANDLE completion_event);

static volatile DWORD callback_count;
static volatile DWORD callback_context;
static volatile DWORD callback_timed_out;

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

static void fail(UINT code)
{
    char message[] = "PE32 WAIT TEST FAIL 00\r\n";
    message[20] = (char)('0' + (code / 10));
    message[21] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static void WINAPI wait_callback(PVOID context, BYTE timed_out)
{
    callback_context = (DWORD)context;
    callback_timed_out = timed_out;
    callback_count++;
}

static BOOL wait_for_callback(DWORD expected)
{
    for (UINT attempt = 0; attempt < 250; attempt++) {
        if (callback_count == expected)
            return TRUE;
        Sleep(2);
    }
    return FALSE;
}

void mainCRTStartup(void)
{
    const DWORD context_value = 0x13572468U;
    HANDLE source = CreateEventA((PVOID)0, TRUE, FALSE, (const char *)0);
    HANDLE wait = (HANDLE)0;
    if (!source) fail(1);
    if (!RegisterWaitForSingleObject(
            &wait, source, (PVOID)wait_callback, (PVOID)context_value,
            INFINITE, WT_EXECUTEONLYONCE) || !wait)
        fail(2);
    if (!SetEvent(source)) fail(3);
    if (!wait_for_callback(1)) fail(4);
    if (callback_context != context_value || callback_timed_out != FALSE)
        fail(5);
    if (!CloseHandle(source)) fail(6);

    callback_context = 0;
    callback_timed_out = 0;
    source = CreateEventA((PVOID)0, TRUE, FALSE, (const char *)0);
    wait = (HANDLE)0;
    if (!source) fail(7);
    if (!RegisterWaitForSingleObject(
            &wait, source, (PVOID)wait_callback, (PVOID)context_value,
            10, WT_EXECUTEONLYONCE) || !wait)
        fail(8);
    if (!wait_for_callback(2)) fail(9);
    if (callback_context != context_value || callback_timed_out != TRUE)
        fail(10);
    if (!CloseHandle(source)) fail(11);

    source = CreateEventA((PVOID)0, TRUE, FALSE, (const char *)0);
    HANDLE completion = CreateEventA(
        (PVOID)0, TRUE, FALSE, (const char *)0);
    wait = (HANDLE)0;
    if (!source || !completion) fail(12);
    if (!RegisterWaitForSingleObject(
            &wait, source, (PVOID)wait_callback, (PVOID)context_value,
            INFINITE, WT_EXECUTEONLYONCE) || !wait)
        fail(13);
    if (!UnregisterWaitEx(wait, completion)) fail(14);
    if (WaitForSingleObject(completion, 250) != WAIT_OBJECT_0) fail(15);
    Sleep(20);
    if (callback_count != 2) fail(16);
    if (!CloseHandle(completion) || !CloseHandle(source)) fail(17);

    source = CreateEventA((PVOID)0, TRUE, FALSE, (const char *)0);
    wait = (HANDLE)0;
    if (!source) fail(18);
    if (!RegisterWaitForSingleObject(
            &wait, source, (PVOID)wait_callback, (PVOID)context_value,
            INFINITE, WT_EXECUTEONLYONCE) || !wait)
        fail(19);
    if (!UnregisterWaitEx(wait, INVALID_HANDLE_VALUE)) fail(20);
    if (callback_count != 2) fail(21);
    if (!CloseHandle(source)) fail(22);

    Sleep(20);
    source = CreateEventA((PVOID)0, TRUE, FALSE, (const char *)0);
    wait = (HANDLE)0;
    if (!source) fail(23);
    if (!RegisterWaitForSingleObject(
            &wait, source, (PVOID)wait_callback, (PVOID)context_value,
            INFINITE, WT_EXECUTEONLYONCE) || !wait)
        fail(24);
    if (!SetEvent(source)) fail(25);
    if (!wait_for_callback(3)) fail(26);
    if (!CloseHandle(source)) fail(27);

    print("PE32 WAIT TEST PASS\r\n");
    ExitProcess(0);
    for (;;) { }
}
