/* Diagnostic fixture: preserve a failed lookup before unhandled INT3 exit. */
typedef unsigned int DWORD;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL ExitProcess(DWORD);
#define SAY(text) do { DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), text, sizeof(text)-1, &written, 0); } while (0)

void breakpoint_persist_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
    if (!kernel || GetProcAddress(kernel, "OsitoMissingBreakpointProbe") ||
        GetLastError() != 127) {
        SAY("BREAKPOINT-PERSIST FAIL lookup contract\n");
        ExitProcess(1);
    }
#ifdef LOOKUP_CHECKPOINT_PROBE
    /* The diagnostic build selects CEF's verified GetProcAddress return
     * address. Reproduce that address without requiring a Steam session. */
    /* The builder supplies a writable executable section at RVA 0xe000.
     * Load it as image storage, like CEF, rather than requiring an unrelated
     * VirtualAlloc reservation to replace physical identity aliases. */
    extern unsigned char __ImageBase;
    unsigned char *page = &__ImageBase;
    if (page != (void *)0x183b40000ULL) {
        SAY("BREAKPOINT-PERSIST FAIL diagnostic image base\n");
        ExitProcess(3);
    }
    unsigned char code[] = {
        0x48,0x83,0xec,0x28,             /* sub rsp,40 */
        0x48,0xb9,0,0,0,0,0,0,0,0,     /* mov rcx,module */
        0x48,0xba,0,0,0,0,0,0,0,0,     /* mov rdx,name */
        0x48,0xb8,0,0,0,0,0,0,0,0,     /* mov rax,GetProcAddress */
        0xff,0xd0,                     /* call rax: return 0x183b4e899 */
        0x48,0x83,0xc4,0x28,0xc3
    };
    __UINTPTR_TYPE__ values[] = {(__UINTPTR_TYPE__)kernel,
        (__UINTPTR_TYPE__)"OsitoTargetedLookupProbe", (__UINTPTR_TYPE__)GetProcAddress};
    for (DWORD i = 0; i < 3; i++)
        for (DWORD j = 0; j < 8; j++)
            code[6 + i * 10 + j] = (unsigned char)(values[i] >> (j * 8));
    unsigned char *entry = page + 0xe875;
    for (DWORD i = 0; i < sizeof(code); i++) entry[i] = code[i];
    if (((void *(CALL *)(void))entry)() || GetLastError() != 127) {
        SAY("BREAKPOINT-PERSIST FAIL targeted lookup contract\n");
        ExitProcess(4);
    }
    SAY("BREAKPOINT-PERSIST targeted lookup returned\n");
    ExitProcess(0);
#else
    SAY("BREAKPOINT-PERSIST expected unhandled INT3\n");
    __asm__ volatile ("int3");
    SAY("BREAKPOINT-PERSIST FAIL resumed after INT3\n");
    ExitProcess(2);
#endif
}
