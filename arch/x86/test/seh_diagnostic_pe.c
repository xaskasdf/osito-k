/* Exercise Osito's diagnostic reads, not Windows' interactive crash policy. */
typedef unsigned long DWORD;
typedef long LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
IMPORT LONG WINAPI UnhandledExceptionFilter(void *);
IMPORT void *WINAPI VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
IMPORT int WINAPI VirtualFree(void *, SIZE_T, DWORD);
IMPORT int WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT void WINAPI ExitProcess(DWORD);

typedef struct {
    DWORD code, flags;
    UPTR nested, address;
    DWORD count;
    UPTR arguments[15];
} RECORD;
typedef struct { RECORD *record; void *context; } POINTERS;

void *memset(void *destination, int value, SIZE_T size)
{
    unsigned char *bytes = destination;
    for (SIZE_T i = 0; i < size; i++) bytes[i] = (unsigned char)value;
    return destination;
}

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, (void *)0);
}

void mainCRTStartup(void)
{
    unsigned char context[1232] __attribute__((aligned(16))) = {0};
    UPTR stack[256];
    for (unsigned i = 0; i < 256; i++)
        stack[i] = sizeof(UPTR) == 8 ? (UPTR)0xAAAAAAAAAAAAAAAAULL : 0xAAAAAAAAU;
    stack[0] = (UPTR)&mainCRTStartup;
    RECORD record = {0};
    record.code = 0x40000015;
    record.address = (UPTR)&mainCRTStartup;
    POINTERS info = {&record, context};
    void *argument = &info;
    unsigned char *pages = VirtualAlloc((void *)0, 8192, 0x2000, 4);
    if (!pages || VirtualAlloc(pages, 4096, 0x1000, 4) != pages)
        ExitProcess(80);
    UPTR rsp = (UPTR)stack;
    if (SEH_DIAG_CASE == 1) {
        rsp = (UPTR)(pages + 4096 - sizeof(UPTR));
        *(UPTR *)rsp = (UPTR)&mainCRTStartup;
    } else if (SEH_DIAG_CASE == 2) {
        info.context = pages + 4096;
    } else if (SEH_DIAG_CASE == 3) {
        info.record = (RECORD *)(pages + 4096);
    } else if (SEH_DIAG_CASE == 4) {
        argument = pages + 4096;
    } else if (SEH_DIAG_CASE == 5) {
        rsp = (UPTR)0xAAAAAAAAAAAAAAAAULL;
    } else if (SEH_DIAG_CASE == 6) {
        argument = pages + 4092;
    } else if (SEH_DIAG_CASE == 7) {
        info.record = (RECORD *)(pages + 4088);
    }
#ifdef _WIN64
    *(DWORD *)(context + 0x30) = 0x100001;
    *(UPTR *)(context + 0x98) = rsp;
    *(UPTR *)(context + 0xF8) = (UPTR)&mainCRTStartup;
#else
    *(DWORD *)context = 0x10001;
    *(DWORD *)(context + 180) = (DWORD)rsp;
    *(DWORD *)(context + 184) = (DWORD)&mainCRTStartup;
    *(DWORD *)(context + 196) = (DWORD)rsp;
#endif
    print("SEH-DIAGNOSTIC BEGIN\r\n");
#if !defined(_WIN64) && SEH_DIAG_CASE == 5
    LONG disposition;
    /* EBP is not required to be a frame pointer by the x86 calling convention. */
    __asm__ volatile ("pushl %%ebp\n\t"
                      "movl %2, %%ebp\n\t"
                      "pushl %1\n\t"
                      "calll *%3\n\t"
                      "popl %%ebp"
                      : "=a"(disposition)
                      : "r"(argument), "r"(pages + 4096),
                        "r"(&UnhandledExceptionFilter)
                      : "ecx", "edx", "cc", "memory");
#else
    LONG disposition = UnhandledExceptionFilter(argument);
#endif
    print(disposition == 1 ? "SEH-DIAGNOSTIC PASS\r\n"
                           : "SEH-DIAGNOSTIC FAIL disposition\r\n");
    VirtualFree(pages, 0, 0x8000);
    ExitProcess(disposition == 1 ? 0 : 81);
}
