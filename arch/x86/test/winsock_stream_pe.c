/* Exact stream bytes through public Winsock, including private child mappings. */
typedef unsigned long DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
#define NIL ((void *)0)
#define CAPACITY 32768
#define SEED 0x61C88647u
IMPORT void WINAPI ExitProcess(DWORD);
IMPORT void *WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(void *, const void *, DWORD, DWORD *, void *);
IMPORT void *WINAPI LoadLibraryA(const char *);
IMPORT void *WINAPI GetProcAddress(void *, const char *);
IMPORT char *WINAPI GetCommandLineA(void);
IMPORT DWORD WINAPI GetModuleFileNameA(void *, char *, DWORD);
IMPORT DWORD WINAPI GetTickCount(void);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI Sleep(DWORD);
IMPORT void *WINAPI CreateIoCompletionPort(void *, void *, UPTR, DWORD);
IMPORT int WINAPI GetQueuedCompletionStatus(void *, DWORD *, UPTR *, void **, DWORD);
IMPORT DWORD WINAPI WaitForSingleObject(void *, DWORD);
IMPORT int WINAPI CloseHandle(void *);
typedef struct { void *process, *thread; DWORD pid, tid; } PROCESS_INFORMATION;
IMPORT int WINAPI CreateProcessA(const char *, char *, void *, void *, int,
                                DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
IMPORT int WINAPI GetExitCodeProcess(void *, DWORD *);
typedef struct { UPTR status, bytes; DWORD low, high; void *event; } OVERLAPPED;
typedef struct { DWORD length; char *data; } WSABUF;
typedef struct {
    OVERLAPPED ov;
    DWORD length, received, flags;
    unsigned char guard0[16], data[CAPACITY], guard1[16];
} SLOT;
static SLOT slots[3];
static unsigned char tx[CAPACITY], ack[8];
static int (WINAPI *startup)(unsigned short, void *);
static int (WINAPI *cleanup)(void);
static UPTR (WINAPI *socket_fn)(int, int, int);
static int (WINAPI *connect_fn)(UPTR, const void *, int);
static int (WINAPI *send_fn)(UPTR, const char *, int, int);
static int (WINAPI *recv_fn)(UPTR, char *, int, int);
static int (WINAPI *close_fn)(UPTR);
static int (WINAPI *shutdown_fn)(UPTR, int);
static int (WINAPI *error_fn)(void);
static int (WINAPI *option_fn)(UPTR, int, int, const char *, int);
static int (WINAPI *receive_fn)(UPTR, WSABUF *, DWORD, DWORD *, DWORD *, void *, void *);
static int (WINAPI *transmit_fn)(UPTR, WSABUF *, DWORD, DWORD *, DWORD, void *, void *);
static unsigned checks, pending, immediate, reads, writes;

void *memset(void *p, int value, UPTR count)
{
    unsigned char *out = p;
    for (UPTR i = 0; i < count; i++) out[i] = (unsigned char)value;
    return p;
}

void *memcpy(void *p, const void *source, UPTR count)
{
    unsigned char *out = p;
    const unsigned char *in = source;
    for (UPTR i = 0; i < count; i++) out[i] = in[i];
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
    char text[16]; unsigned pos = sizeof(text) - 1;
    text[pos] = 0;
    do { text[--pos] = (char)('0' + value % 10); value /= 10; } while (value);
    say(text + pos);
}

static void check(int valid, const char *name)
{
    checks++;
    if (valid) return;
    say("STREAM FAIL "); say(name); say(" check="); number(checks);
    say(" wsa="); number(error_fn ? (DWORD)error_fn() : 0);
    say(" win="); number(GetLastError()); say("\n");
    ExitProcess(1);
}

static unsigned char pattern(DWORD offset, DWORD seed)
{
    DWORD n = (offset >> 2) + seed;
    n ^= n >> 16; n *= 0x7FEB352Du;
    n ^= n >> 15; n *= 0x846CA68Bu;
    n ^= n >> 16;
    return (unsigned char)(n >> ((offset & 3) * 8));
}

static void verify(const unsigned char *data, DWORD length, DWORD offset, DWORD seed)
{
    for (DWORD i = 0; i < length; i++) {
        unsigned char want = pattern(offset + i, seed);
        if (data[i] != want) {
            say("STREAM MISMATCH offset="); number(offset + i);
            say(" expected="); number(want); say(" actual="); number(data[i]); say("\n");
            check(0, "byte integrity");
        }
    }
}

static DWORD parse_number(char **cursor)
{
    char *p = *cursor;
    DWORD value = 0;
    check(*p >= '0' && *p <= '9', "numeric argument");
    while (*p >= '0' && *p <= '9') {
        check(value <= 10000000, "argument range");
        value = value * 10 + (DWORD)(*p++ - '0');
    }
    *cursor = p;
    return value;
}

static void send_all(UPTR socket, const unsigned char *data, DWORD size)
{
    for (DWORD sent = 0; sent < size;) {
        int n = send_fn(socket, (const char *)data + sent, (int)(size - sent), 0);
        check(n > 0 && (DWORD)n <= size - sent, "send range");
        sent += (DWORD)n;
        writes++;
    }
}

static void receive_blocking(UPTR socket, DWORD total)
{
    static const DWORD amounts[] = {1, 31, 16381, CAPACITY, 7, 4093};
    DWORD offset = 0, iteration = 0;
    while (offset < total) {
        DWORD amount = amounts[iteration % 6];
        if (amount > total - offset) amount = total - offset;
        if (iteration % 13 == 0) {
            int n = recv_fn(socket, (char *)slots[0].data, (int)amount, 2);
            check(n > 0 && (DWORD)n <= amount, "MSG_PEEK range");
            verify(slots[0].data, (DWORD)n, offset, SEED);
        }
        int n = recv_fn(socket, (char *)slots[0].data, (int)amount, 0);
        check(n > 0 && (DWORD)n <= amount, "recv range");
        verify(slots[0].data, (DWORD)n, offset, SEED);
        offset += (DWORD)n;
        reads++;
        if (++iteration % 17 == 0) Sleep(100);
    }
}

static void receive_overlapped(UPTR socket, void *port, DWORD total)
{
    DWORD offset = 0, round = 0;
    while (offset < total) {
        DWORD capacity = 0, count = 0;
        for (unsigned i = 0; i < 3 && capacity < total - offset; i++) {
            SLOT *slot = &slots[i];
            memset(slot, 0, sizeof(*slot));
            memset(slot->guard0, 0xA5, sizeof(slot->guard0));
            memset(slot->guard1, 0x5A, sizeof(slot->guard1));
            slot->length = total - offset - capacity;
            if (slot->length > CAPACITY) slot->length = CAPACITY;
            DWORD first = slot->length < 7 ? slot->length : 7;
            WSABUF buffers[3] = {{first, (char *)slot->data}, {0, NIL},
                                {slot->length - first, (char *)slot->data + first}};
            DWORD bytes = 0;
            int rc = receive_fn(socket, buffers, 3, &bytes, &slot->flags, &slot->ov, NIL);
            check(rc == 0 || (rc == -1 && error_fn() == 997), "post WSARecv");
            if (rc == 0) immediate++; else pending++;
            /* Keep descriptor invalidation observable under optimization. */
            volatile unsigned char *expired = (volatile unsigned char *)buffers;
            for (unsigned j = 0; j < sizeof(buffers); j++) expired[j] = 0;
            capacity += slot->length;
            count++;
        }
        unsigned seen = 0;
        for (unsigned i = 0; i < count; i++) {
            DWORD bytes = 0; UPTR key = 0; void *ov = NIL;
            check(GetQueuedCompletionStatus(port, &bytes, &key, &ov, 60000), "receive completion");
            unsigned index = 0;
            while (index < count && ov != &slots[index].ov) index++;
            check(index < count && key == 0x51A7, "receive identity");
            check(!(seen & (1u << index)), "receive completes once");
            seen |= 1u << index;
            check(bytes > 0 && bytes <= slots[index].length, "completion range");
            check(slots[index].ov.status == 0 && slots[index].ov.bytes == bytes,
                  "OVERLAPPED status and count");
            slots[index].received = bytes;
        }
        /* Completion order can differ; stream order is submission order. */
        for (unsigned i = 0; i < count; i++) {
            SLOT *slot = &slots[i];
            for (unsigned j = 0; j < 16; j++)
                check(slot->guard0[j] == 0xA5 && slot->guard1[j] == 0x5A, "buffer guards");
            verify(slot->data, slot->received, offset, SEED);
            offset += slot->received;
            reads++;
        }
        if (++round % 13 == 0) Sleep(100);
    }
}

static void child(char *args)
{
    unsigned char address[16] = {2, 0};
    for (unsigned i = 0; i < 4; i++) {
        DWORD octet = parse_number(&args);
        check(octet < 256 && *args == (i == 3 ? ' ' : '.'), "IPv4 argument");
        address[4 + i] = (unsigned char)octet;
        args++;
    }
    DWORD port_number = parse_number(&args);
    check(port_number > 0 && port_number <= 65535 && *args++ == ' ', "port argument");
    address[2] = (unsigned char)(port_number >> 8);
    address[3] = (unsigned char)port_number;
    DWORD mode = parse_number(&args);
    check(mode <= 1 && *args++ == ' ', "mode argument (0=recv, 1=iocp)");
    DWORD total = parse_number(&args);
    check(!*args && total >= 1 && total <= 8, "MiB argument");
    total *= 1024 * 1024;
    void *ws = LoadLibraryA("ws2_32.dll");
    check(ws != NIL, "Winsock library");
#define LOAD(variable, name) variable = GetProcAddress(ws, name); check(variable != NIL, name)
    LOAD(startup, "WSAStartup"); LOAD(cleanup, "WSACleanup");
    LOAD(socket_fn, "socket"); LOAD(connect_fn, "connect");
    LOAD(send_fn, "send"); LOAD(recv_fn, "recv");
    LOAD(close_fn, "closesocket"); LOAD(shutdown_fn, "shutdown");
    LOAD(error_fn, "WSAGetLastError"); LOAD(option_fn, "setsockopt");
    LOAD(receive_fn, "WSARecv"); LOAD(transmit_fn, "WSASend");
#undef LOAD
    unsigned char wsdata[512] = {0};
    check(startup(0x202, wsdata) == 0, "WSAStartup");
    UPTR socket = socket_fn(2, 1, 6);
    check(socket != (UPTR)-1, "socket");
    DWORD timeout = 60000;
    check(!option_fn(socket, 0xFFFF, 0x1006, (const char *)&timeout, 4), "receive timeout");
    check(!option_fn(socket, 0xFFFF, 0x1005, (const char *)&timeout, 4), "send timeout");
    check(!connect_fn(socket, address, sizeof(address)), "connect");
    unsigned char header[12] = {'O', 'S', 'S', '1'};
    for (unsigned i = 0; i < 4; i++) {
        header[4 + i] = (unsigned char)(total >> (8 * i));
        header[8 + i] = (unsigned char)(SEED >> (8 * i));
    }
    send_all(socket, header, sizeof(header));
    DWORD started = GetTickCount();
    void *port = mode ? CreateIoCompletionPort((void *)socket, NIL, 0x51A7, 1) : NIL;
    if (mode) check(port != NIL, "create IOCP");
    say("STREAM RECEIVING bytes="); number(total); say(" mode="); number(mode); say("\n");
    if (mode) receive_overlapped(socket, port, total);
    else receive_blocking(socket, total);
    say("STREAM RX OK bytes="); number(total); say(" ms="); number(GetTickCount() - started); say("\n");
    for (DWORD offset = 0; offset < total;) {
        DWORD count = total - offset;
        if (count > CAPACITY) count = CAPACITY;
        for (DWORD i = 0; i < count; i++) tx[i] = pattern(offset + i, SEED ^ 0xA5A5A5A5u);
        if (mode) {
            OVERLAPPED ov = {0};
            DWORD first = count < 13 ? count : 13;
            WSABUF buffers[2] = {{first, (char *)tx}, {count - first, (char *)tx + first}};
            DWORD bytes = 0;
            int rc = transmit_fn(socket, buffers, 2, &bytes, 0, &ov, NIL);
            check(!rc || (rc == -1 && error_fn() == 997), "post WSASend");
            UPTR key = 0; void *returned = NIL;
            check(GetQueuedCompletionStatus(port, &bytes, &key, &returned, 60000), "send completion");
            check(key == 0x51A7 && returned == &ov && bytes > 0 && bytes <= count,
                  "send completion range and identity");
            offset += bytes;
            writes++;
        } else {
            send_all(socket, tx, count);
            offset += count;
        }
    }
    check(!shutdown_fn(socket, 1), "shutdown send");
    for (DWORD got = 0; got < sizeof(ack);) {
        int n = recv_fn(socket, (char *)ack + got, (int)(sizeof(ack) - got), 0);
        check(n > 0 && (DWORD)n <= sizeof(ack) - got, "server result");
        got += (DWORD)n;
    }
    check(ack[0] == 'P' && ack[1] == 'A' && ack[2] == 'S' && ack[3] == 'S', "server verified TX bytes");
    DWORD confirmed = 0;
    for (unsigned i = 0; i < 4; i++) confirmed |= (DWORD)ack[4 + i] << (8 * i);
    check(confirmed == total, "server TX count");
    check(recv_fn(socket, (char *)ack, 1, 0) == 0, "clean peer EOF");
    check(!close_fn(socket), "close socket");
    if (port) check(CloseHandle(port), "close IOCP");
    check(!cleanup(), "WSACleanup");
    say("STREAM PASS bytes_each_direction="); number(total);
    say(" reads="); number(reads); say(" writes="); number(writes);
    say(" pending="); number(pending); say(" immediate="); number(immediate);
    say(" elapsed_ms="); number(GetTickCount() - started); say("\n");
    ExitProcess(0);
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    if (*args == '"') { args++; while (*args && *args != '"') args++; if (*args) args++; }
    else while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    if (args[0] == '-' && args[1] == '-' && args[2] == 'c' && args[3] == ' ') child(args + 4);
    static char self[1024], command[1400];
    DWORD length = GetModuleFileNameA(NIL, self, sizeof(self));
    check(length > 0 && length < sizeof(self), "self path");
    DWORD n = 0;
    command[n++] = '"';
    for (DWORD i = 0; i < length; i++) command[n++] = self[i];
    const char *suffix = "\" --c ";
    while (*suffix) command[n++] = *suffix++;
    while (*args && n + 1 < sizeof(command)) command[n++] = *args++;
    check(!*args, "argument length");
    command[n] = 0;
    unsigned char startup_info[sizeof(void *) == 8 ? 104 : 68] = {0};
    *(DWORD *)startup_info = sizeof(startup_info);
    PROCESS_INFORMATION process = {0};
    say(sizeof(void *) == 8 ? "STREAM PE64\n" : "STREAM PE32\n");
    check(CreateProcessA(self, command, NIL, NIL, 0, 0, NIL, NIL, startup_info, &process), "private child");
    check(WaitForSingleObject(process.process, 600000) == 0, "bounded child completion");
    DWORD code = 1;
    check(GetExitCodeProcess(process.process, &code), "child exit code");
    CloseHandle(process.thread); CloseHandle(process.process);
    check(code == 0, "child success");
    say("STREAM PRIVATE PASS\n");
    ExitProcess(0);
}
