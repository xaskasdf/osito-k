/* Launch the PE64 PEB contract in a distinct Win32 process. */

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI
#define DLLIMPORT __declspec(dllimport)
#define FALSE 0
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0 0U
#define INFINITE 0xFFFFFFFFU

typedef struct {
    HANDLE process;
    HANDLE thread;
    DWORD process_id;
    DWORD thread_id;
} PROCESS_INFORMATION64;

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT BOOL WINAPI CreateProcessA(const char *application,
                                     char *command_line,
                                     PVOID process_attributes,
                                     PVOID thread_attributes,
                                     BOOL inherit_handles,
                                     DWORD creation_flags,
                                     PVOID environment,
                                     const char *current_directory,
                                     PVOID startup_info,
                                     PROCESS_INFORMATION64 *information);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI GetExitCodeProcess(HANDLE process, DWORD *exit_code);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);

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
    char message[] = "PEB64 CHILD FAIL 00\r\n";
    message[17] = (char)('0' + (code / 10));
    message[18] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

void mainCRTStartup(void)
{
    BYTE startup_info[104] = {0};
    PROCESS_INFORMATION64 information = {0};
    *(DWORD *)startup_info = sizeof(startup_info);

    if (!CreateProcessA("peb_teb_contract_pe64.exe", (char *)0,
                        (PVOID)0, (PVOID)0, FALSE, 0, (PVOID)0,
                        (const char *)0, startup_info, &information))
        fail(1);
    if (!information.process || !information.thread ||
        !information.process_id || !information.thread_id)
        fail(2);
    if (!CloseHandle(information.thread)) fail(3);
    if (WaitForSingleObject(information.process, INFINITE) != WAIT_OBJECT_0)
        fail(4);

    DWORD exit_code = 0xFFFFFFFFU;
    if (!GetExitCodeProcess(information.process, &exit_code)) fail(5);
    if (exit_code != 0) fail(6);
    if (!CloseHandle(information.process)) fail(7);

    print("PEB64 CHILD PASS\r\n");
    ExitProcess(0);
}
