/* Public Winsock send ordering, cancellation, thread lifetime and event I/O. */
typedef unsigned long DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define NIL ((void *)0)
#define LENGTH (2u * 1024u * 1024u)
#define TAIL 4096u
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(void *, char *, DWORD);
API DWORD CALL GetLastError(void);
API void *CALL CreateIoCompletionPort(void *, void *, UPTR, DWORD);
API int CALL GetQueuedCompletionStatus(void *, DWORD *, UPTR *, void **, DWORD);
API DWORD CALL WaitForSingleObject(void *, DWORD);
API int CALL CloseHandle(void *);
typedef struct { void *process, *thread; DWORD pid, tid; } PROCESS_INFORMATION;
API int CALL CreateProcessA(const char *, char *, void *, void *, int,
                           DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API int CALL GetExitCodeProcess(void *, DWORD *);
typedef struct { UPTR status, bytes; DWORD low, high; void *event; } OVERLAPPED;
typedef struct { DWORD length; char *data; } WSABUF;
static unsigned char payload[LENGTH + TAIL * 3];
static OVERLAPPED requests[4];
static UPTR stream;
static int (CALL *error_fn)(void);
static int (CALL *transmit)(UPTR, WSABUF *, DWORD, DWORD *, DWORD, void *, void *);

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
    say("SENDQUEUE FAIL "); say(name); say(" wsa=");
    number(error_fn ? (DWORD)error_fn() : 0);
    say(" win="); number(GetLastError()); say("\n");
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

static void post(unsigned index, DWORD offset, DWORD length)
{
    WSABUF buffers[2] = {{13, (char *)payload + offset},
                        {length - 13, (char *)payload + offset + 13}};
    DWORD sentinel = 0xBADC0DE;
    int rc = transmit(stream, buffers, 2, &sentinel, 0, &requests[index], NIL);
    check(!rc || (rc == -1 && error_fn() == 997), "post send");
    if (rc) check(sentinel == 0xBADC0DE, "pending preserves bytes output");
    volatile unsigned char *expired = (volatile unsigned char *)buffers;
    for (unsigned i = 0; i < sizeof(buffers); i++) expired[i] = 0;
}

static DWORD CALL worker(void *unused)
{
    (void)unused;
    post(0, 0, LENGTH);
    return 0;
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
    char *original = args;
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
    DWORD mode = take_number(&args);
    check(mode <= 5, "test mode");
    if (*args) {
        const char suffix[] = " --private";
        unsigned i = 0;
        while (suffix[i] && args[i] == suffix[i]) i++;
        check(!suffix[i] && !args[i], "private mode suffix");
        char module[512], command[640];
        DWORD size = GetModuleFileNameA(NIL, module, sizeof(module));
        check(size && size < sizeof(module), "module name");
        unsigned at = 0;
        command[at++] = '"';
        for (DWORD j = 0; j < size; j++) command[at++] = module[j];
        command[at++] = '"'; command[at++] = ' ';
        for (char *p = original; p < args; p++) command[at++] = *p;
        command[at] = 0;
        unsigned char startup[104] = {0};
#ifdef _WIN64
        *(DWORD *)startup = 104;
#else
        *(DWORD *)startup = 68;
#endif
        PROCESS_INFORMATION child = {0};
        check(CreateProcessA(module, command, NIL, NIL, 0, 0, NIL, NIL,
                             startup, &child), "private child start");
        check(!WaitForSingleObject(child.process, 60000), "private child wait");
        DWORD code = 259;
        check(GetExitCodeProcess(child.process, &code) && !code, "private child result");
        CloseHandle(child.thread); CloseHandle(child.process);
        say("SENDQUEUE PRIVATE PASS\n");
        ExitProcess(0);
    }
    void *ws = LoadLibraryA("ws2_32.dll");
    void *k32 = LoadLibraryA("kernel32.dll");
    check(ws && k32, "libraries");
    int (CALL *startup)(unsigned short, void *) = GetProcAddress(ws, "WSAStartup");
    UPTR (CALL *open_socket)(int, int, int) = GetProcAddress(ws, "socket");
    int (CALL *connect_socket)(UPTR, const void *, int) = GetProcAddress(ws, "connect");
    int (CALL *send_socket)(UPTR, const char *, int, int) = GetProcAddress(ws, "send");
    int (CALL *recv_socket)(UPTR, char *, int, int) = GetProcAddress(ws, "recv");
    int (CALL *close_socket)(UPTR) = GetProcAddress(ws, "closesocket");
    int (CALL *shutdown_socket)(UPTR, int) = GetProcAddress(ws, "shutdown");
    int (CALL *option)(UPTR, int, int, const char *, int) = GetProcAddress(ws, "setsockopt");
    int (CALL *cancel)(void *, void *) = GetProcAddress(k32, "CancelIoEx");
    void *(CALL *thread_create)(void *, UPTR, DWORD (CALL *)(void *), void *, DWORD, DWORD *) = GetProcAddress(k32, "CreateThread");
    void *(CALL *event_create)(void *, int, int, const char *) = GetProcAddress(k32, "CreateEventA");
    int (CALL *get_result)(UPTR, void *, DWORD *, int, DWORD *) = GetProcAddress(ws, "WSAGetOverlappedResult");
    transmit = GetProcAddress(ws, "WSASend");
    error_fn = GetProcAddress(ws, "WSAGetLastError");
    check(startup && open_socket && connect_socket && send_socket && recv_socket &&
          close_socket && shutdown_socket && option && cancel && thread_create && event_create &&
          get_result && transmit && error_fn, "exports");
    unsigned char wsdata[512] = {0};
    check(!startup(0x202, wsdata), "WSAStartup");
    stream = open_socket(2, 1, 6);
    check(stream != (UPTR)-1, "socket");
    DWORD small = 4096;
    check(!option(stream, 0xFFFF, 0x1001, (const char *)&small, 4), "send buffer");
    check(!connect_socket(stream, address, sizeof(address)), "connect");
    unsigned char header[12] = {'O', 'S', 'Q', '1'};
    DWORD values[2] = {LENGTH,
#ifdef _WIN64
                      64
#else
                      32
#endif
                      | (mode << 16)};
    for (unsigned i = 0; i < 8; i++)
        header[4 + i] = (unsigned char)(values[i / 4] >> ((i & 3) * 8));
    for (DWORD at = 0; at < sizeof(header);) {
        int sent = send_socket(stream, (const char *)header + at,
                               (int)(sizeof(header) - at), 0);
        check(sent > 0, "header send"); at += (DWORD)sent;
    }
    for (DWORD i = 0; i < sizeof(payload); i++)
        payload[i] = (unsigned char)(i * 29u + (i >> 8) * 11u + 0x5Au);
    void *port = NIL, *event = NIL;
    if (mode == 4) {
        event = event_create(NIL, 1, 1, NIL);
        check(event != NIL, "completion event");
        requests[0].event = event;
    } else if (mode != 5) {
        port = CreateIoCompletionPort((void *)stream, NIL, 0xCA71, 1);
        check(port != NIL, "IOCP association");
    }
    if (mode == 3) {
        void *thread = thread_create(NIL, 0, worker, NIL, 0, NIL);
        check(thread && !WaitForSingleObject(thread, 10000), "issuing thread exit");
        CloseHandle(thread);
    } else {
        post(0, 0, LENGTH);
    }
    if (mode == 5) {
        say("SENDQUEUE EXIT-PENDING PASS\n");
        ExitProcess(0);
    }
    unsigned mask = 1, cancelled = 0;
    if (mode <= 1) {
        post(1, mode == 1 ? 0 : LENGTH, mode == 1 ? LENGTH : TAIL);
        post(2, LENGTH + TAIL, TAIL);
        mask = 7;
    }
    if (mode == 1) {
        check(cancel((void *)stream, &requests[1]), "cancel middle send");
        /* Reuse the cancelled array slot, while older requests remain queued. */
        post(3, LENGTH + TAIL * 2, TAIL);
        mask |= 8;
    }
    if (mode == 2) check(!close_socket(stream), "close pending sends");
    if (mode == 4) {
        check(!WaitForSingleObject(event, 30000), "event-only completion");
        DWORD bytes = 0, flags = 0;
        check(get_result(stream, &requests[0], &bytes, 0, &flags) && bytes == LENGTH,
              "event full send result");
        mask = 0;
    }
    while (mask) {
        DWORD bytes = 0; UPTR key = 0; void *returned = NIL;
        int ok = GetQueuedCompletionStatus(port, &bytes, &key, &returned, 30000);
        DWORD completion_error = GetLastError();
        check(key == 0xCA71, "completion key");
        unsigned index = 0;
        while (index < 4 && returned != &requests[index]) index++;
        check(index < 4 && (mask & (1u << index)), "completion once per request");
        if (mode == 1 && index == 1) {
            check(!ok && completion_error == 995 && bytes < LENGTH, "cancel completion");
            cancelled |= 2;
        } else if (mode == 2 || mode == 3) {
            /* Completion may win the race with close/thread termination.
             * Successful short completions remain a contract violation. */
            check(ok ? bytes == LENGTH : completion_error == 995 && bytes < LENGTH,
                  "close or thread completion race");
            if (!ok) cancelled |= 1u << index;
        } else {
            check(ok && bytes == (index ? TAIL : LENGTH), "full queued completion");
        }
        mask &= ~(1u << index);
    }
    if (port) {
        DWORD bytes; UPTR key; void *returned;
        check(!GetQueuedCompletionStatus(port, &bytes, &key, &returned, 0) &&
              GetLastError() == 258 && !returned, "no duplicate completion");
    }
    if (mode == 0 || mode == 1 || mode == 4) {
        check(!shutdown_socket(stream, 1), "send stream boundary");
        unsigned char ack[8];
        for (DWORD got = 0; got < sizeof(ack);) {
            int n = recv_socket(stream, (char *)ack + got, (int)(sizeof(ack) - got), 0);
            check(n > 0, "peer integrity acknowledgement"); got += (DWORD)n;
        }
        check(ack[0] == 'P' && ack[1] == 'A' && ack[2] == 'S' && ack[3] == 'S',
              "peer byte order");
    }
    if (mode != 2) check(!close_socket(stream), "socket close");
    if (port) CloseHandle(port);
    if (event) CloseHandle(event);
    say("SENDQUEUE RESULT mode="); number(mode);
    say(" cancelled="); number(cancelled); say("\nSENDQUEUE PASS\n");
    ExitProcess(0);
}
