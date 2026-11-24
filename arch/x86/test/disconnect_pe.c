typedef unsigned long DWORD;
#ifdef _WIN64
typedef unsigned long long UINT_PTR;
#define SELF "disconnect64.exe"
#define STARTUP_SIZE 104
#else
typedef unsigned long UINT_PTR;
#define SELF "disconnect32.exe"
#define STARTUP_SIZE 68
#endif
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
IMPORT void WINAPI ExitProcess(DWORD);
IMPORT void *WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(void *, const void *, DWORD, DWORD *, void *);
IMPORT void *WINAPI LoadLibraryA(const char *);
IMPORT void *WINAPI GetProcAddress(void *, const char *);
IMPORT char *WINAPI GetCommandLineA(void);
IMPORT void *WINAPI CreateIoCompletionPort(void *, void *, UINT_PTR, DWORD);
IMPORT int WINAPI GetQueuedCompletionStatus(void *, DWORD *, UINT_PTR *, void **, DWORD);
IMPORT void *WINAPI CreateEventA(void *, int, int, const char *);
IMPORT DWORD WINAPI WaitForSingleObject(void *, DWORD);
IMPORT int WINAPI CloseHandle(void *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT int WINAPI CancelIoEx(void *, void *);
IMPORT void WINAPI Sleep(DWORD);
typedef struct {
    void *process, *thread;
    DWORD process_id, thread_id;
} PROCESS_INFORMATION;
IMPORT int WINAPI CreateProcessA(const char *, char *, void *, void *, int,
                                DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
IMPORT int WINAPI GetExitCodeProcess(void *, DWORD *);
IMPORT DWORD WINAPI ResumeThread(void *);

typedef struct { UINT_PTR status, bytes; DWORD offset, offset_high; void *event; } OVERLAPPED;
typedef UINT_PTR SOCKET;
typedef struct { DWORD len; char *buf; } WSABUF;
static int (WINAPI *startup)(unsigned short, void *);
static int (WINAPI *cleanup)(void);
static SOCKET (WINAPI *socket_fn)(int, int, int);
static int (WINAPI *bind_fn)(SOCKET, const void *, int);
static int (WINAPI *listen_fn)(SOCKET, int);
static int (WINAPI *connect_fn)(SOCKET, const void *, int);
static SOCKET (WINAPI *accept_fn)(SOCKET, void *, int *);
static int (WINAPI *name_fn)(SOCKET, void *, int *);
static int (WINAPI *close_fn)(SOCKET);
static int (WINAPI *error_fn)(void);
static int (WINAPI *ioctl_fn)(SOCKET, DWORD, void *, DWORD, void *, DWORD, DWORD *, void *, void *);
static int (WINAPI *disconnect_fn)(SOCKET, void *, DWORD, DWORD);
static int (WINAPI *connect_ex_fn)(SOCKET, const void *, int, void *, DWORD, DWORD *, void *);
static int (WINAPI *duplicate_fn)(SOCKET, DWORD, void *);
static SOCKET (WINAPI *wsa_socket_fn)(int, int, int, void *, DWORD, DWORD);
static int (WINAPI *send_fn)(SOCKET, const char *, int, int);
static int (WINAPI *recv_fn)(SOCKET, char *, int, int);
static int (WINAPI *wsa_recv_fn)(SOCKET, WSABUF *, DWORD, DWORD *, DWORD *, void *, void *);
static SOCKET listener;
static unsigned char address[16];
static unsigned checks;

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
    DWORD n = 0, done;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &done, (void *)0);
}

static void number(unsigned n)
{
    char text[16]; unsigned p = sizeof(text) - 1;
    text[p] = 0;
    do { text[--p] = (char)('0' + n % 10); n /= 10; } while (n);
    say(text + p);
}

static void check(int condition, const char *name)
{
    checks++;
    if (condition) return;
    say("DISCONNECT FAIL: "); say(name);
    say(" check="); number(checks);
    say(" wsa="); number(error_fn ? (unsigned)error_fn() : 0);
    say(" win="); number(GetLastError()); say("\n");
    ExitProcess(1);
}

static void pair(SOCKET *client, SOCKET *peer)
{
    *client = socket_fn(2, 1, 6);
    check(*client != (SOCKET)-1, "client socket");
    check(connect_fn(*client, address, sizeof(address)) == 0, "connect");
    *peer = accept_fn(listener, (void *)0, (void *)0);
    check(*peer != (SOCKET)-1, "accept");
}

static void test_reuse(SOCKET client, SOCKET alias, void *port)
{
    unsigned char other_address[16] = {2, 0, 0, 0, 127, 0, 0, 1};
    SOCKET other = socket_fn(2, 1, 6);
    check(other != (SOCKET)-1, "reuse listener");
    check(bind_fn(other, other_address, sizeof(other_address)) == 0,
          "reuse listener bind");
    check(listen_fn(other, 4) == 0, "reuse listener listen");
    int size = sizeof(other_address);
    check(name_fn(other, other_address, &size) == 0, "reuse listener address");
    OVERLAPPED ov = {0};
    DWORD bytes = 0;
    int result = connect_ex_fn(client, other_address, size, (void *)0, 0, &bytes, &ov);
    check(result || error_fn() == 997, "ConnectEx on reused handle");
    SOCKET peer = accept_fn(other, (void *)0, (void *)0);
    check(peer != (SOCKET)-1, "accept reused connection");
    UINT_PTR key = 0; void *returned = (void *)0;
    check(GetQueuedCompletionStatus(port, &bytes, &key, &returned, 10000),
          "reused ConnectEx completion");
    check(returned == &ov && key == 0x1234 && bytes == 0 && ov.status == 0,
          "reuse preserves IOCP association");
    check(send_fn(peer, "R", 1, 0) == 1, "send on reused connection");
    char byte = 0;
    check(recv_fn(alias, &byte, 1, 0) == 1 && byte == 'R',
          "duplicate follows reused backend");
    check(close_fn(alias) == 0, "close duplicate after reuse");
    check(!disconnect_fn(client, (void *)0, 0x80, 0) && error_fn() == 10022,
          "reject unknown flags");
    check(!disconnect_fn(client, (void *)0, 2, 1) && error_fn() == 10022,
          "reject reserved argument");
    check(!disconnect_fn(client, (void *)1, 2, 0) && error_fn() == 10014,
          "reject invalid overlapped");
    check(send_fn(peer, "S", 1, 0) == 1 &&
          recv_fn(client, &byte, 1, 0) == 1 && byte == 'S',
          "invalid requests preserve connection");
    check(close_fn(peer) == 0, "close reused peer");
    check(disconnect_fn(client, (void *)0, 0, 0), "disconnect without reuse");
    result = connect_ex_fn(client, other_address, size, (void *)0, 0, &bytes, &ov);
    check(!result && error_fn() == 10056,
          "reuse requires TF_REUSE_SOCKET");
    check(close_fn(other) == 0, "close reuse listener");
}

static void test_pending_read(void)
{
    SOCKET client, peer;
    pair(&client, &peer);
    void *port = CreateIoCompletionPort((void *)client, (void *)0, 0x5678, 1);
    check(port != (void *)0, "pending read port");
    OVERLAPPED read_ov = {0}, disconnect_ov = {0};
    char data[8] = {0};
    WSABUF buffer = {sizeof(data), data};
    DWORD bytes = 0, flags = 0;
    check(wsa_recv_fn(client, &buffer, 1, &bytes, &flags, &read_ov, (void *)0) == -1 &&
          error_fn() == 997, "receive pending before disconnect");
    int result = disconnect_fn(client, &disconnect_ov, 2, 0);
    check(result || error_fn() == 997, "disconnect with pending read");
    check(close_fn(peer) == 0, "close pending read peer");
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; i++) {
        UINT_PTR key = 0; void *returned = (void *)0;
        result = GetQueuedCompletionStatus(port, &bytes, &key, &returned, 10000);
        DWORD error = GetLastError();
        check(key == 0x5678 && bytes == 0, "pending operation completion key");
        if (returned == &read_ov) {
            check(!(seen & 1) && (result || error == 995), "read completed once");
            seen |= 1;
        } else {
            check(returned == &disconnect_ov && !(seen & 2) && result,
                  "disconnect completed once");
            seen |= 2;
        }
    }
    check(seen == 3, "both pending operations complete");
    check(close_fn(client) == 0, "close pending read socket");
    CloseHandle(port);
}

static void test_abort(void)
{
    for (unsigned round = 0; round < 96; round++) {
        SOCKET client, peer;
        pair(&client, &peer);
        void *port = CreateIoCompletionPort((void *)client, (void *)0, 0x2468, 1);
        check(port != (void *)0, "abort port");
        OVERLAPPED ov = {0};
        ov.event = CreateEventA((void *)0, 1, 0, (void *)0);
        check(ov.event != (void *)0, "abort event");
        int result = disconnect_fn(client, &ov, 2, 0);
        check(result || error_fn() == 997, "abort disconnect starts");
        if (round & 1) {
            check(close_fn(client) == 0, "close during disconnect");
        } else {
            result = CancelIoEx((void *)client, &ov);
            check(result || GetLastError() == 1168, "cancel pending disconnect");
        }
        check(close_fn(peer) == 0, "close abort peer");
        UINT_PTR key = 0; void *returned = (void *)0; DWORD bytes = 99;
        result = GetQueuedCompletionStatus(port, &bytes, &key, &returned, 10000);
        check(result || GetLastError() == 995, "abort completion status");
        check(returned == &ov && key == 0x2468 && bytes == 0,
              "abort completion identity");
        check(WaitForSingleObject(ov.event, 0) == 0, "abort event signaled");
        check(!GetQueuedCompletionStatus(port, &bytes, &key, &returned, 0) &&
              GetLastError() == 258, "abort completed only once");
        if (!(round & 1)) check(close_fn(client) == 0, "close canceled socket");
        CloseHandle(ov.event); CloseHandle(port);
        Sleep(40);
    }
}

static void test_exit_connections(unsigned port, int leave_open)
{
    address[2] = (unsigned char)(port >> 8);
    address[3] = (unsigned char)port;
    OVERLAPPED operations[3] = {0};
    unsigned pending = 0;
    for (unsigned i = 0; i < 3; i++) {
        SOCKET client = socket_fn(2, 1, 6);
        check(client != (SOCKET)-1, "exit-pending socket");
        check(connect_fn(client, address, sizeof(address)) == 0,
              leave_open ? "exit-open connect" : "exit-pending connect");
        if (!leave_open) {
            int result = disconnect_fn(client, &operations[i], 2, 0);
            check(result || error_fn() == 997, "exit-pending disconnect");
            if (!result) pending++;
        }
    }
    say(leave_open ? "DISCONNECT EXIT open=" : "DISCONNECT EXIT pending=");
    number(leave_open ? 3 : pending); say("\n");
    ExitProcess(0);
}

static void test_lifetime(int leave_open)
{
    for (unsigned round = 0; round < 32; round++) {
        check(listen_fn(listener, 4) == 0, "lifetime backlog");
        UINT_PTR start[STARTUP_SIZE / sizeof(UINT_PTR)] = {0};
        *(DWORD *)start = STARTUP_SIZE;
        PROCESS_INFORMATION info = {0};
        char command[96] = SELF " ";
        unsigned port = (unsigned)address[2] * 256 + address[3];
        char digits[8]; unsigned count = 0;
        do { digits[count++] = (char)('0' + port % 10); port /= 10; } while (port);
        char *end = command;
        while (*end) end++;
        const char *mode = leave_open ? "--exit-open " : "--exit-pending ";
        while (*mode) *end++ = *mode++;
        while (count) *end++ = digits[--count];
        *end = 0;
        check(CreateProcessA(SELF, command, (void *)0, (void *)0, 0, 0,
                             (void *)0, (void *)0, start, &info), "lifetime child launch");
        check(WaitForSingleObject(info.process, 90000) == 0, "lifetime child finished");
        DWORD code = 99;
        check(GetExitCodeProcess(info.process, &code) && code == 0,
              "lifetime child status");
        CloseHandle(info.thread); CloseHandle(info.process);
        check(close_fn(listener) == 0, "close lifetime listener");
        listener = socket_fn(2, 1, 6);
        check(listener != (SOCKET)-1, "next lifetime listener");
        address[2] = 0; address[3] = 0;
        check(bind_fn(listener, address, sizeof(address)) == 0, "next lifetime bind");
        int size = sizeof(address);
        check(name_fn(listener, address, &size) == 0, "next lifetime address");
    }
}

static void test_duplicate_exit(void)
{
    for (unsigned round = 0; round < 128; round++) {
        SOCKET client, peer;
        pair(&client, &peer);
        UINT_PTR start[STARTUP_SIZE / sizeof(UINT_PTR)] = {0};
        *(DWORD *)start = STARTUP_SIZE;
        PROCESS_INFORMATION info = {0};
        char command[] = SELF " --token-child";
        check(CreateProcessA(SELF, command, (void *)0, (void *)0, 0, 4,
                             (void *)0, (void *)0, start, &info),
              "duplicate target launch");
        DWORD protocol_info[157] = {0};
        check(duplicate_fn(client, info.process_id, protocol_info) == 0,
              "duplicate into child");
        if (!(round & 1))
            check(close_fn(client) == 0, "close source before target exits");
        check(ResumeThread(info.thread) == 1, "resume duplicate target");
        check(WaitForSingleObject(info.process, 10000) == 0,
              "duplicate target exits");
        DWORD code = 99;
        check(GetExitCodeProcess(info.process, &code) && code == 0,
              "duplicate target status");
        CloseHandle(info.thread); CloseHandle(info.process);
        if (round & 1) {
            char byte = 0;
            check(send_fn(peer, "T", 1, 0) == 1 &&
                  recv_fn(client, &byte, 1, 0) == 1 && byte == 'T',
                  "source survives target exit");
            check(close_fn(client) == 0, "close source after target exit");
        }
        check(close_fn(peer) == 0, "close token peer");
        Sleep(100);
        if ((round + 1) % 32 == 0) {
            say("DISCONNECT tokens="); number(round + 1); say("\n");
        }
    }
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    int churn = 0, abort = 0, private = 0, lifetime = 0, orphans = 0;
    int exit_open = 0, tokens = 0, token_child = 0;
    unsigned exit_port = 0;
    while (*args) {
        if (args[0] == '-' && args[1] == '-' && args[2] == 'c' && args[3] == 'h' &&
            args[4] == 'u') churn = 1;
        if (args[0] == '-' && args[1] == '-' && args[2] == 'a') abort = 1;
        if (args[0] == '-' && args[1] == '-' && args[2] == 'p') private = 1;
        if (args[0] == '-' && args[1] == '-' && args[2] == 'l') lifetime = 1;
        if (args[0] == '-' && args[1] == '-' && args[2] == 'o') orphans = 1;
        const char *token_mode = "--token-child";
        unsigned token_i = 0;
        while (token_mode[token_i] && token_mode[token_i] == args[token_i]) token_i++;
        if (!token_mode[token_i]) token_child = 1;
        if (args[0] == '-' && args[1] == '-' && args[2] == 't' &&
            args[3] == 'o' && args[4] == 'k' && args[5] == 'e' &&
            args[6] == 'n' && args[7] == 's') tokens = 1;
        const char *prefix = "--exit-pending ";
        unsigned i = 0;
        while (prefix[i] && prefix[i] == args[i]) i++;
        if (prefix[i]) {
            prefix = "--exit-open ";
            i = 0;
            while (prefix[i] && prefix[i] == args[i]) i++;
            if (!prefix[i]) exit_open = 1;
        }
        if (!prefix[i]) {
            const char *digit = args + i;
            while (*digit >= '0' && *digit <= '9')
                exit_port = exit_port * 10 + (unsigned)(*digit++ - '0');
        }
        args++;
    }
    if (token_child) ExitProcess(0);
    if (private) {
        UINT_PTR start[STARTUP_SIZE / sizeof(UINT_PTR)] = {0};
        *(DWORD *)start = STARTUP_SIZE;
        PROCESS_INFORMATION info = {0};
        char command[96] = SELF " --child";
        char *end = command;
        while (*end) end++;
        const char *extra = churn ? " --churn" : abort ? " --abort" : "";
        while ((*end++ = *extra++)) {}
        check(CreateProcessA(SELF, command, (void *)0, (void *)0, 0, 0,
                             (void *)0, (void *)0, start, &info), "private child launch");
        check(WaitForSingleObject(info.process, 90000) == 0, "private child finished");
        DWORD code = 99;
        check(GetExitCodeProcess(info.process, &code) && code == 0,
              "private child exit status");
        CloseHandle(info.thread); CloseHandle(info.process);
        say("DISCONNECT PRIVATE PASS\n");
        ExitProcess(0);
    }
    void *ws = LoadLibraryA("ws2_32.dll");
    startup = GetProcAddress(ws, "WSAStartup");
    cleanup = GetProcAddress(ws, "WSACleanup");
    socket_fn = GetProcAddress(ws, "socket");
    bind_fn = GetProcAddress(ws, "bind");
    listen_fn = GetProcAddress(ws, "listen");
    connect_fn = GetProcAddress(ws, "connect");
    accept_fn = GetProcAddress(ws, "accept");
    name_fn = GetProcAddress(ws, "getsockname");
    close_fn = GetProcAddress(ws, "closesocket");
    error_fn = GetProcAddress(ws, "WSAGetLastError");
    ioctl_fn = GetProcAddress(ws, "WSAIoctl");
    duplicate_fn = GetProcAddress(ws, "WSADuplicateSocketW");
    wsa_socket_fn = GetProcAddress(ws, "WSASocketW");
    send_fn = GetProcAddress(ws, "send");
    recv_fn = GetProcAddress(ws, "recv");
    wsa_recv_fn = GetProcAddress(ws, "WSARecv");
    check(ws && startup && cleanup && socket_fn && bind_fn && listen_fn &&
          connect_fn && accept_fn && name_fn && close_fn && error_fn && ioctl_fn &&
          duplicate_fn && wsa_socket_fn && send_fn && recv_fn && wsa_recv_fn,
          "exports");
    unsigned char wsdata[512] = {0};
    check(startup(0x202, wsdata) == 0, "startup");
    listener = socket_fn(2, 1, 6);
    check(listener != (SOCKET)-1, "listener socket");
    address[0] = 2; address[4] = 127; address[7] = 1;
    check(bind_fn(listener, address, sizeof(address)) == 0, "bind loopback");
    check(listen_fn(listener, 4) == 0, "listen");
    int address_size = sizeof(address);
    check(name_fn(listener, address, &address_size) == 0 && address_size == 16,
          "listener address");
    DWORD guid[4] = {0x7fda2e11, 0x436f8630, 0x36f531a0, 0x57c1eea6};
    DWORD bytes = 0;
    check(ioctl_fn(listener, 0xc8000006, guid, sizeof(guid), &disconnect_fn,
                   sizeof(disconnect_fn), &bytes, (void *)0, (void *)0) == 0 &&
          bytes == sizeof(disconnect_fn) && disconnect_fn, "extension lookup");
    DWORD connect_guid[4] = {0x25a207b9, 0x4660ddf3, 0xe576e98e, 0x3e06748c};
    check(ioctl_fn(listener, 0xc8000006, connect_guid, sizeof(connect_guid),
                   &connect_ex_fn, sizeof(connect_ex_fn), &bytes, (void *)0,
                   (void *)0) == 0 && connect_ex_fn, "ConnectEx lookup");
    if (exit_port) {
        test_exit_connections(exit_port, exit_open);
    } else if (tokens) {
        test_duplicate_exit();
    } else if (lifetime || orphans) {
        test_lifetime(orphans);
    } else if (abort) {
        test_abort();
    } else if (churn) {
        for (unsigned round = 0; round < 96; round++) {
            SOCKET client, peer;
            pair(&client, &peer);
            check(close_fn(peer) == 0, "close peer");
            check(disconnect_fn(client, (void *)0, 2, 0), "synchronous disconnect");
            check(close_fn(client) == 0, "close disconnected socket");
            Sleep(40);
            if ((round + 1) % 16 == 0) {
                say("DISCONNECT churn="); number(round + 1); say("\n");
            }
        }
    } else {
        SOCKET client, peer;
        pair(&client, &peer);
        void *port = CreateIoCompletionPort((void *)client, (void *)0, 0x1234, 1);
        check(port != (void *)0, "completion port");
        DWORD protocol_info[157] = {0};
        check(duplicate_fn(client, GetCurrentProcessId(), protocol_info) == 0,
              "duplicate connected socket");
        SOCKET alias = wsa_socket_fn(-1, -1, -1, protocol_info, 0, 1);
        check(alias != (SOCKET)-1, "open duplicate");
        OVERLAPPED overlapped = {0};
        overlapped.event = CreateEventA((void *)0, 1, 0, (void *)0);
        check(overlapped.event != (void *)0, "completion event");
        int result = disconnect_fn(client, &overlapped, 2, 0);
        check(result || error_fn() == 997, "overlapped disconnect starts");
        check(close_fn(peer) == 0, "close peer");
        UINT_PTR key = 0; void *returned = (void *)0; bytes = 99;
        check(GetQueuedCompletionStatus(port, &bytes, &key, &returned, 10000),
              "disconnect posts completion");
        check(bytes == 0 && key == 0x1234 && returned == &overlapped,
              "completion identity and byte count");
        check(overlapped.status == 0 && overlapped.bytes == 0,
              "overlapped status and byte count");
        check(WaitForSingleObject(overlapped.event, 0) == 0, "event signaled");
        check(!GetQueuedCompletionStatus(port, &bytes, &key, &returned, 0) &&
              GetLastError() == 258, "no duplicate completion");
        test_reuse(client, alias, port);
        check(close_fn(client) == 0, "close disconnected socket");
        CloseHandle(overlapped.event); CloseHandle(port);
        test_pending_read();
    }
    check(close_fn(listener) == 0, "close listener");
    check(cleanup() == 0, "cleanup");
    say("DISCONNECT PASS checks="); number(checks); say("\n");
    ExitProcess(0);
}
