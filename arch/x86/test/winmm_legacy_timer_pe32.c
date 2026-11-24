/*
 * PE32 regression for legacy Win32 DEP and multimedia callbacks. Old audio
 * middleware commonly emits a stdcall timer thunk into writable process
 * memory. With the client OptIn policy, an image without NX_COMPAT must be
 * able to execute that thunk while VirtualAlloc remains PAGE_READWRITE.
 */

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef void *PVOID;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define MEM_COMMIT       0x00001000U
#define MEM_RESERVE      0x00002000U
#define PAGE_READWRITE   0x00000004U
#define TIME_ONESHOT     0x0000U
#define TIME_CALLBACK_FUNCTION 0x0000U

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT void WINAPI Sleep(DWORD milliseconds);
DLLIMPORT PVOID WINAPI VirtualAlloc(PVOID address, DWORD size,
                                    DWORD allocation_type, DWORD protect);
DLLIMPORT UINT WINAPI timeSetEvent(UINT delay, UINT resolution,
                                   PVOID callback, DWORD user, UINT flags);

static volatile DWORD callback_count;

static void store_u32(BYTE *destination, DWORD value)
{
    destination[0] = (BYTE)value;
    destination[1] = (BYTE)(value >> 8);
    destination[2] = (BYTE)(value >> 16);
    destination[3] = (BYTE)(value >> 24);
}

void mainCRTStartup(void)
{
    BYTE *thunk = (BYTE *)VirtualAlloc(
        (PVOID)0, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!thunk)
        ExitProcess(1);

    /* mov dword ptr [callback_count], 1; ret 20 */
    thunk[0] = 0xC7;
    thunk[1] = 0x05;
    store_u32(thunk + 2, (DWORD)&callback_count);
    store_u32(thunk + 6, 1);
    thunk[10] = 0xC2;
    thunk[11] = 20;
    thunk[12] = 0;

    UINT timer = timeSetEvent(10, 1, thunk, 0,
                              TIME_ONESHOT | TIME_CALLBACK_FUNCTION);
    if (!timer)
        ExitProcess(2);

    for (UINT attempt = 0; attempt < 100 && callback_count == 0; attempt++)
        Sleep(2);
    ExitProcess(callback_count == 1 ? 0 : 3);
}
