/* Native/guest oracle: invalid handles only, no ETW session is started. */
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __SIZE_TYPE__ SIZE_T;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API char *CALL GetCommandLineA(void);
#define SAY(s) do { DWORD n_; WriteFile(GetStdHandle((DWORD)-11), s, sizeof(s)-1, &n_, 0); } while (0)
#define HEX(v) do { DWORD v_ = (DWORD)(v), w_; char b_[9]; \
    for (int j_ = 7; j_ >= 0; j_--) { b_[j_] = "0123456789abcdef"[v_ & 15]; v_ >>= 4; } \
    b_[8] = ' '; WriteFile(GetStdHandle((DWORD)-11), b_, sizeof(b_), &w_, 0); \
} while (0)
void trace_message_entry(void)
{
    typedef DWORD (*trace_fn)(U64, DWORD, const void *, unsigned short, ...);
    void *module = LoadLibraryA("advapi32.dll");
    trace_fn trace = module ? GetProcAddress(module, "TraceMessage") : 0;
    if (!trace) { SAY("TRACEMSG MISSING\n"); ExitProcess(1); }
    const U64 handles[] = {0, 1, 0xffffffffffffffffULL, 0x123456789abcdef0ULL,
        0x100000000ULL, 0xe700ffffULL};
    const DWORD flags[] = {0, 1, 2, 4, 6, 8, 16, 32, 64, 128, 0x100,
        0x10000, 0xffffffff};
    const DWORD guid[4] = {0x829a7c55, 0x44d44a71, 0x56efeaac, 0x7123ccf5};
    DWORD payload = 0x75319531;
    for (DWORD i = 0; i < sizeof(handles)/sizeof(handles[0]); i++) {
        for (DWORD j = 0; j < sizeof(flags)/sizeof(flags[0]); j++) {
            for (DWORD k = 0; k < 2; k++) {
                SetLastError(0x13579);
                DWORD result = trace(handles[i], flags[j], (flags[j] & 6) ? guid : 0,
                    0x7135, k ? &payload : 0, (SIZE_T)(k ? sizeof(payload) : 0),
                    (void *)0, (SIZE_T)0);
                DWORD error = GetLastError();
                SAY("TRACEMSG ROW "); HEX(i); HEX(j); HEX(k); HEX(result); HEX(error); SAY("\n");
            }
        }
    }
    /* This optional branch exercises Osito's existing session model, not a
     * Windows controller. Native oracle runs never request this argument. */
    const char *command = GetCommandLineA();
    int guest_session = 0;
    for (DWORD i = 0; command && command[i]; i++) {
        const char option[] = "--guest-session";
        DWORD j = 0;
        while (option[j] && command[i+j] == option[j]) j++;
        if (!option[j]) { guest_session = 1; break; }
    }
    if (guest_session) {
        typedef DWORD (CALL *start_fn)(U64 *, const char *, void *);
        typedef DWORD (CALL *stop_fn)(U64, const char *, void *);
        start_fn start = GetProcAddress(module, "StartTraceA");
        stop_fn stop = GetProcAddress(module, "StopTraceA");
        U64 session = 0;
        if (!start || !stop || start(&session, "OsitoTraceMessageProbe", 0)) {
            SAY("TRACEMSG SESSION FAIL start\n"); ExitProcess(2);
        }
        SetLastError(0x13579);
        DWORD supported = trace(session, 0, 0, 7, (void *)0, (SIZE_T)0);
        DWORD high = trace(session | 0x100000000ULL, 0, 0, 7,
            (void *)0, (SIZE_T)0);
        DWORD ended = stop(session, 0, 0);
        DWORD stale = trace(session, 0, 0, 7, (void *)0, (SIZE_T)0);
        DWORD error = GetLastError();
        SAY("TRACEMSG SESSION "); HEX(supported); HEX(high); HEX(ended);
        HEX(stale); HEX(error); SAY("\n");
        if (supported != 50 || high != 6 || ended || stale != 6 || error != 0x13579) {
            SAY("TRACEMSG SESSION FAIL contract\n"); ExitProcess(3);
        }
    }
    SAY("TRACEMSG DONE\n");
    ExitProcess(0);
}
