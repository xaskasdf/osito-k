/* Public Win32 ABI: finite IOCP waits must retain their final time slice. */
typedef unsigned long DWORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
#define NIL ((void *)0)

typedef struct {
    UPTR key;
    void *overlapped;
    UPTR internal;
    DWORD bytes;
} ENTRY;

IMPORT HANDLE WINAPI CreateIoCompletionPort(HANDLE, HANDLE, UPTR, DWORD);
IMPORT BOOL WINAPI GetQueuedCompletionStatus(HANDLE, DWORD *, UPTR *, void **, DWORD);
IMPORT BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE, ENTRY *, DWORD, DWORD *, DWORD, BOOL);
IMPORT BOOL WINAPI PostQueuedCompletionStatus(HANDLE, DWORD, UPTR, void *);
IMPORT HANDLE WINAPI CreateThread(void *, SIZE_T, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT DWORD WINAPI GetTickCount(void);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI Sleep(DWORD);
IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT void WINAPI ExitProcess(DWORD);

static DWORD failures;
static volatile BOOL posted;

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void number(DWORD value)
{
    char text[11];
    unsigned pos = sizeof(text) - 1;
    text[pos] = 0;
    do {
        text[--pos] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(text + pos);
}

static void check(BOOL ok, const char *name)
{
    if (!ok) {
        failures++;
        print("IOCP-DEADLINE FAIL ");
        print(name);
        print("\r\n");
    }
}

static BOOL dequeue(HANDLE port, BOOL extended, DWORD timeout, BOOL expected)
{
    DWORD bytes = 0, count = 0;
    UPTR key = 0;
    void *overlapped = (void *)(UPTR)0xfeed;
    ENTRY entry;
    BOOL ok;
    if (extended) {
        ok = GetQueuedCompletionStatusEx(port, &entry, 1, &count, timeout, 0);
    } else {
        ok = GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, timeout);
    }
    DWORD error = GetLastError();
    if (expected) {
        if (extended && ok) {
            check(count == 1 && entry.internal == 0, "extended entry");
            bytes = entry.bytes;
            key = entry.key;
            overlapped = entry.overlapped;
        }
        check(ok && bytes == 0x1357 && key == 0x2468 &&
              overlapped == (void *)(UPTR)0x1234, "packet contents");
    } else {
        check(!ok && error == 258, "empty timeout status");
        check(extended ? count == 0 : overlapped == NIL, "empty timeout output");
    }
    return ok;
}

static DWORD WINAPI producer(void *port)
{
    Sleep(425);
    posted = PostQueuedCompletionStatus(port, 0x1357, 0x2468, (void *)(UPTR)0x1234);
    return 0;
}

void mainCRTStartup(void)
{
    static const DWORD timeouts[] = {0, 50, 150, 200, 250, 350, 500};
    print(sizeof(void *) == 8 ? "IOCP-DEADLINE PE64\r\n" : "IOCP-DEADLINE PE32\r\n");
    for (BOOL extended = 0; extended <= 1; extended++) {
        HANDLE port = CreateIoCompletionPort((HANDLE)(UPTR)-1, NIL, 0, 1);
        if (!port) {
            check(0, "create port");
            continue;
        }
        for (unsigned i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); i++) {
            DWORD start = GetTickCount();
            dequeue(port, extended, timeouts[i], 0);
            DWORD elapsed = GetTickCount() - start;
            print("IOCP-DEADLINE ");
            print(extended ? "ex " : "base ");
            print("requested="); number(timeouts[i]);
            print(" elapsed="); number(elapsed); print("\r\n");
            if (elapsed + 20 < timeouts[i]) {
                print(extended ? "IOCP-DEADLINE EARLY ex\r\n" :
                                 "IOCP-DEADLINE EARLY base\r\n");
                check(0, "finite deadline");
            }
            check(elapsed < 3000, "bounded empty wait");
        }
        posted = 0;
        HANDLE thread = CreateThread(NIL, 0, producer, port, 0, NIL);
        if (thread) {
            dequeue(port, extended, 500, 1);
            check(WaitForSingleObject(thread, 3000) == 0 && posted, "producer completion");
            CloseHandle(thread);
        } else {
            check(0, "create producer");
        }
        CloseHandle(port);

        port = CreateIoCompletionPort((HANDLE)(UPTR)-1, NIL, 0, 1);
        if (port) {
            check(PostQueuedCompletionStatus(port, 0x1357, 0x2468,
                                             (void *)(UPTR)0x1234), "post immediate");
            dequeue(port, extended, 0, 1);
            CloseHandle(port);
        } else {
            check(0, "create immediate port");
        }
    }
    print(failures ? "IOCP-DEADLINE FAILURES=" : "IOCP-DEADLINE PASS checks=");
    number(failures ? failures : 18);
    print("\r\n");
    ExitProcess(failures ? 1 : 0);
}
