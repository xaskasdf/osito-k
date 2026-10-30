/* Exercise polled NIC RX, TX and MMIO from a child's private address space. */
typedef unsigned long DWORD;
#ifdef _WIN64
typedef unsigned long long UINT_PTR;
#define SELF "vnet64.exe"
#define STARTUP_BYTES 104
#else
typedef unsigned long UINT_PTR;
#define SELF "vnet32.exe"
#define STARTUP_BYTES 68
#endif
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
IMPORT void WINAPI ExitProcess(DWORD);
IMPORT void *WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(void *, const void *, DWORD, DWORD *, void *);
IMPORT void *WINAPI LoadLibraryA(const char *);
IMPORT void *WINAPI GetProcAddress(void *, const char *);
IMPORT char *WINAPI GetCommandLineA(void);
IMPORT DWORD WINAPI WaitForSingleObject(void *, DWORD);
IMPORT int WINAPI GetExitCodeProcess(void *, DWORD *);
IMPORT int WINAPI CloseHandle(void *);
typedef struct {
    void *process, *thread;
    DWORD process_id, thread_id;
} PROCESS_INFORMATION;
IMPORT int WINAPI CreateProcessA(const char *, char *, void *, void *, int,
                                DWORD, void *, const char *, void *,
                                PROCESS_INFORMATION *);

void *memset(void *destination, int value, UINT_PTR length)
{
    unsigned char *out = destination;
    for (UINT_PTR i = 0; i < length; i++) out[i] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, UINT_PTR length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (UINT_PTR i = 0; i < length; i++) out[i] = in[i];
    return destination;
}

static void say(const char *s)
{
    DWORD n = 0, done = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &done, (void *)0);
}

static void finish(DWORD code)
{
    say(code ? "VNET-PRIVATE FAIL\n" : "VNET-PRIVATE PASS\n");
    ExitProcess(code);
}

static void child(const char *port_string)
{
    unsigned port = 0;
    while (*port_string >= '0' && *port_string <= '9')
        port = port * 10 + (unsigned)(*port_string++ - '0');
    if (!port || port > 65535 || *port_string) finish(1);
    void *ws = LoadLibraryA("ws2_32.dll");
    int (WINAPI *startup)(unsigned short, void *) = GetProcAddress(ws, "WSAStartup");
    UINT_PTR (WINAPI *socket_fn)(int, int, int) = GetProcAddress(ws, "socket");
    int (WINAPI *connect_fn)(UINT_PTR, const void *, int) = GetProcAddress(ws, "connect");
    int (WINAPI *send_fn)(UINT_PTR, const char *, int, int) = GetProcAddress(ws, "send");
    int (WINAPI *recv_fn)(UINT_PTR, char *, int, int) = GetProcAddress(ws, "recv");
    int (WINAPI *close_fn)(UINT_PTR) = GetProcAddress(ws, "closesocket");
    if (!ws || !startup || !socket_fn || !connect_fn || !send_fn || !recv_fn || !close_fn)
        finish(2);
    unsigned char wsdata[512] = {0};
    if (startup(0x202, wsdata)) finish(3);
    UINT_PTR sock = socket_fn(2, 1, 6);
    if (sock == (UINT_PTR)-1) finish(4);
    unsigned char address[16] = {2, 0, 0, 0, 10, 0, 2, 2};
    address[2] = (unsigned char)(port >> 8);
    address[3] = (unsigned char)port;
    say("VNET-PRIVATE CHILD CONNECT\n");
    if (connect_fn(sock, address, sizeof(address))) finish(5);
    char tx[512], rx[512];
    for (unsigned round = 0; round < 128; round++) {
        for (unsigned i = 0; i < sizeof(tx); i++) tx[i] = (char)(i + round);
        int sent = 0, got = 0;
        while (sent < (int)sizeof(tx)) {
            int n = send_fn(sock, tx + sent, sizeof(tx) - sent, 0);
            if (n <= 0 || n > (int)sizeof(tx) - sent) finish(6);
            sent += n;
        }
        while (got < (int)sizeof(rx)) {
            int n = recv_fn(sock, rx + got, sizeof(rx) - got, 0);
            if (n <= 0 || n > (int)sizeof(rx) - got) finish(7);
            got += n;
        }
        for (unsigned i = 0; i < sizeof(tx); i++)
            if (tx[i] != rx[i]) finish(8);
    }
    if (close_fn(sock)) finish(9);
    say("VNET-PRIVATE CHILD 65536 BYTES OK\n");
    ExitProcess(0);
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    const char *prefix = "--child ";
    unsigned i = 0;
    while (prefix[i] && args[i] == prefix[i]) i++;
    if (!prefix[i]) child(args + i);

    char command[96] = SELF " --child ";
    unsigned n = sizeof(SELF " --child ") - 1;
    while (*args && n < sizeof(command) - 1) command[n++] = *args++;
    command[n] = 0;
    if (*args) finish(10);
    unsigned char startup[STARTUP_BYTES] = {0};
    *(DWORD *)startup = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessA(SELF, command, (void *)0, (void *)0, 0, 0,
                        (void *)0, (void *)0, startup, &process)) finish(11);
    if (WaitForSingleObject(process.process, 30000)) finish(12);
    DWORD code = 0;
    if (!GetExitCodeProcess(process.process, &code)) finish(13);
    CloseHandle(process.thread);
    CloseHandle(process.process);
    finish(code);
}
