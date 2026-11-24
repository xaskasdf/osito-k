/* Check the same missing imports through the PE32 stdcall/cdecl thunks. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef void *HANDLE;
typedef int BOOL;
#define IMPORT __declspec(dllimport)
#define WINAPI __attribute__((stdcall))

IMPORT void WINAPI ExitProcess(DWORD);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT BOOL WINAPI QueryProcessCycleTime(HANDLE, U64 *);
IMPORT int __attribute__((cdecl)) _ismbblead(unsigned int);

static void finish(DWORD code)
{
    char failure[] = "PF-IMPORTS32 FAIL 0\r\n";
    failure[18] = (char)('0' + code);
    const char *text = code ? failure : "PF-IMPORTS32 PASS\r\n";
    DWORD written;
    WriteFile(GetStdHandle((DWORD)-11), text, code ? 21 : 19, &written, (void *)0);
    ExitProcess(code);
}

void mainCRTStartup(void)
{
    U64 output = 0;
    for (unsigned i = 0; i < 300; i++) {
        U64 previous = output;
        if (_ismbblead(i)) finish(1);
        if (!QueryProcessCycleTime((HANDLE)-1, &output)) finish(2);
        if (output < previous) finish(3);
        if (output == 0xfeedfacecafebeefULL) finish(4);
    }
    if (_ismbblead(~0U)) finish(5);
    if (QueryProcessCycleTime((HANDLE)-1, (void *)0) || GetLastError() != 87)
        finish(6);
    if (QueryProcessCycleTime((HANDLE)0, &output) || GetLastError() != 6)
        finish(7);
    finish(0);
}
