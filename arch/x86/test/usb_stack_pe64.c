/* Exercise synchronous filesystem DMA from a remapped Win32 x64 stack. */
typedef unsigned long DWORD;
typedef unsigned long long UINT_PTR;
typedef void *HANDLE;
typedef int BOOL;
#define IMPORT __declspec(dllimport)
IMPORT HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
IMPORT DWORD SetFilePointer(HANDLE, long, long *, DWORD);
IMPORT BOOL CloseHandle(HANDLE);
IMPORT HANDLE GetStdHandle(DWORD);
IMPORT void ExitProcess(unsigned);

static void finish(unsigned error)
{
    const char *message = error ? "USB-STACK64 FAIL\n" : "USB-STACK64 PASS\n";
    DWORD count;
    WriteFile(GetStdHandle((DWORD)-11), message, 17, &count, 0);
    ExitProcess(error);
}

void mainCRTStartup(void)
{
    unsigned char buffer[1024];
    DWORD count;
    if ((UINT_PTR)buffer < 0x100000000ULL || (UINT_PTR)buffer >= 0x800000000000ULL)
        finish(1);
    HANDLE file = CreateFileA("usb-stack64.bin", 0xc0000000U, 0, 0, 2, 0x80, 0);
    if (!file || file == (HANDLE)(UINT_PTR)-1) finish(2);
    for (unsigned block = 0; block < 17; block++) {
        for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = (unsigned char)(i ^ block);
        if (!WriteFile(file, buffer, sizeof(buffer), &count, 0) || count != sizeof(buffer))
            finish(3);
    }
    if (SetFilePointer(file, 0, 0, 0) != 0) finish(4);
    for (unsigned block = 0; block < 17; block++) {
        for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xff;
        if (!ReadFile(file, buffer, sizeof(buffer), &count, 0) || count != sizeof(buffer))
            finish(5);
        for (unsigned i = 0; i < sizeof(buffer); i++)
            if (buffer[i] != (unsigned char)(i ^ block)) finish(6);
    }
    if (!CloseHandle(file)) finish(7);
    finish(0);
}
