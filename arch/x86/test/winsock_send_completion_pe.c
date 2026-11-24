/* A successful overlapped send consumes the complete scatter/gather request. */
typedef unsigned long DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define NIL ((void *)0)
#define LENGTH (2u * 1024u * 1024u)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetTickCount(void);
API void *CALL CreateIoCompletionPort(void *, void *, UPTR, DWORD);
API int CALL GetQueuedCompletionStatus(void *, DWORD *, UPTR *, void **, DWORD);
API int CALL CloseHandle(void *);
typedef struct { UPTR status, bytes; DWORD low, high; void *event; } OVERLAPPED;
typedef struct { DWORD length; char *data; } WSABUF;
static unsigned char payload[LENGTH];
static int (CALL *error_fn)(void);

void *memset(void *p, int value, UPTR count)
{
    unsigned char *out = p;
    for (UPTR i = 0; i < count; i++) out[i] = (unsigned char)value;
    return p;
}

static void say(const char *s)
{
    DWORD count = 0, written;
    while (s[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), s, count, &written, NIL);
}

static void number(DWORD value)
{
    char text[16]; unsigned at = sizeof(text) - 1;
    text[at] = 0;
    do { text[--at] = (char)('0' + value % 10); value /= 10; } while (value);
    say(text + at);
}

static void check(int valid, const char *name)
{
    if (valid) return;
    say("FULLSEND FAIL "); say(name); say(" wsa=");
    number(error_fn ? (DWORD)error_fn() : 0); say("\n");
    ExitProcess(1);
}

static DWORD take_number(char **cursor)
{
    char *p = *cursor;
    check(*p >= '0' && *p <= '9', "numeric argument");
    DWORD value = 0;
    while (*p >= '0' && *p <= '9') {
        check(value < 1000000, "argument range");
        value = value * 10 + (DWORD)(*p++ - '0');
    }
    *cursor = p;
    return value;
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    if (*args == '"') {
        args++;
        while (*args && *args != '"') args++;
        if (*args) args++;
    } else while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    unsigned char address[16] = {2, 0};
    for (unsigned i = 0; i < 4; i++) {
        DWORD octet = take_number(&args);
        check(octet < 256 && *args == (i == 3 ? ' ' : '.'), "IPv4 argument");
        address[4 + i] = (unsigned char)octet;
        args++;
    }
    DWORD port_number = take_number(&args);
    check(port_number && port_number < 65536 && *args++ == ' ', "port argument");
    address[2] = (unsigned char)(port_number >> 8);
    address[3] = (unsigned char)port_number;
    DWORD nonblocking = take_number(&args);
    check(nonblocking <= 1 && !*args, "blocking mode argument");

    void *ws = LoadLibraryA("ws2_32.dll");
    check(ws != NIL, "Winsock library");
    int (CALL *startup)(unsigned short, void *) = GetProcAddress(ws, "WSAStartup");
    UPTR (CALL *open_socket)(int, int, int) = GetProcAddress(ws, "socket");
    int (CALL *connect_socket)(UPTR, const void *, int) = GetProcAddress(ws, "connect");
    int (CALL *send_socket)(UPTR, const char *, int, int) = GetProcAddress(ws, "send");
    int (CALL *recv_socket)(UPTR, char *, int, int) = GetProcAddress(ws, "recv");
    int (CALL *close_socket)(UPTR) = GetProcAddress(ws, "closesocket");
    int (CALL *option)(UPTR, int, int, const char *, int) = GetProcAddress(ws, "setsockopt");
    int (CALL *ioctl)(UPTR, long, DWORD *) = GetProcAddress(ws, "ioctlsocket");
    int (CALL *transmit)(UPTR, WSABUF *, DWORD, DWORD *, DWORD, void *, void *) = GetProcAddress(ws, "WSASend");
    error_fn = GetProcAddress(ws, "WSAGetLastError");
    check(startup && open_socket && connect_socket && send_socket && recv_socket && close_socket &&
          option && ioctl && transmit && error_fn, "Winsock exports");
    unsigned char wsdata[512] = {0};
    check(!startup(0x202, wsdata), "WSAStartup");
    UPTR socket = open_socket(2, 1, 6);
    check(socket != (UPTR)-1, "socket");
    DWORD small = 4096;
    check(!option(socket, 0xFFFF, 0x1001, (const char *)&small, 4), "send buffer");
    check(!connect_socket(socket, address, sizeof(address)), "connect");
    unsigned char header[12] = {'O', 'S', 'C', '1'};
    DWORD values[2] = {LENGTH,
#ifdef _WIN64
                      64
#else
                      32
#endif
                      | (nonblocking << 16)};
    for (unsigned i = 0; i < 8; i++)
        header[4 + i] = (unsigned char)(values[i / 4] >> ((i & 3) * 8));
    for (DWORD at = 0; at < sizeof(header);) {
        int sent = send_socket(socket, (const char *)header + at,
                               (int)(sizeof(header) - at), 0);
        check(sent > 0, "header send");
        at += (DWORD)sent;
    }
    check(!ioctl(socket, (long)0x8004667Eu, &nonblocking), "FIONBIO");
    void *port = CreateIoCompletionPort((void *)socket, NIL, 0x51A7, 1);
    check(port != NIL, "IOCP association");
    for (DWORD i = 0; i < LENGTH; i++)
        payload[i] = (unsigned char)(i * 29u + (i >> 8) * 11u + 0x5Au);
    OVERLAPPED ov = {0};
    WSABUF buffers[4] = {{13, (char *)payload}, {0, NIL},
                        {8193, (char *)payload + 13},
                        {LENGTH - 8206, (char *)payload + 8206}};
    DWORD before = GetTickCount();
    int rc = transmit(socket, buffers, 4, NIL, 0, &ov, NIL);
    DWORD call_ms = GetTickCount() - before;
    check(!rc || (rc == -1 && error_fn() == 997), "post WSASend");
    /* Winsock must capture descriptors even when the caller used its stack. */
    volatile unsigned char *expired = (volatile unsigned char *)buffers;
    for (unsigned i = 0; i < sizeof(buffers); i++) expired[i] = 0;
    DWORD bytes = 0; UPTR key = 0; void *returned = NIL;
    check(GetQueuedCompletionStatus(port, &bytes, &key, &returned, 30000), "completion success");
    check(key == 0x51A7 && returned == &ov, "completion identity");
    say("FULLSEND RESULT requested="); number(LENGTH); say(" completed="); number(bytes);
    say(" nonblocking="); number(nonblocking); say(" call_ms="); number(call_ms); say("\n");
    check(bytes == LENGTH, "complete scatter/gather count");
    DWORD blocking = 0;
    check(!ioctl(socket, (long)0x8004667Eu, &blocking), "blocking result receive");
    DWORD timeout = 30000;
    check(!option(socket, 0xFFFF, 0x1006, (const char *)&timeout, 4), "result receive timeout");
    unsigned char ack[8];
    for (DWORD got = 0; got < sizeof(ack);) {
        int received = recv_socket(socket, (char *)ack + got,
                                   (int)(sizeof(ack) - got), 0);
        check(received > 0, "peer result receive");
        got += (DWORD)received;
    }
    check(ack[0] == 'P' && ack[1] == 'A' && ack[2] == 'S' && ack[3] == 'S',
          "peer verified bytes");
    check(!close_socket(socket), "socket close");
    check(CloseHandle(port), "port close");
    say("FULLSEND PASS\n");
    ExitProcess(0);
}
