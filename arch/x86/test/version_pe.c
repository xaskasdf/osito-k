/* Native/guest oracle for the process manifest version contract. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef unsigned long long U64;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
typedef int (CALL *EXFN)(void *);
typedef int (CALL *VERIFYFN)(void *, DWORD, U64);
typedef U64 (CALL *CONDFN)(U64, DWORD, BYTE);
static unsigned failures, checks;
static DWORD observed[2][5];
static WORD suites[2];
static void vp_report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void vp_hex(DWORD value)
{
    char s[11] = "0x00000000";
    for (unsigned i = 0; i < 8; i++) {
        unsigned v = (value >> (i * 4)) & 15;
        s[9-i] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
    }
    vp_report(s);
}
static void vp_check(const char *label, int pass)
{
    checks++;
    if (!pass) failures++;
    vp_report(pass ? "VERSION PASS " : "VERSION FAIL ");
    vp_report(label); vp_report("\n");
}
static void vp_query(EXFN fn, unsigned wide, DWORD size)
{
    union { U64 alignment; BYTE b[320]; } storage;
    BYTE *p = storage.b;
    DWORD base = wide ? 276 : 148, extended = base + 8;
    for (unsigned i = 0; i < sizeof(storage); i++) p[i] = 0xa5;
    *(DWORD *)p = size;
    SetLastError(0x1234abcd);
    int ok = fn(p);
    DWORD error = GetLastError();
    vp_report(wide ? "VERSION QUERY W " : "VERSION QUERY A ");
    vp_hex(size); vp_report(" ok="); vp_hex((DWORD)ok);
    vp_report(" error="); vp_hex(error);
    for (unsigned i = 0; i < 5; i++) { vp_report(" "); vp_hex(((DWORD *)p)[i]); }
    vp_report(" extended="); vp_hex(*(DWORD *)(p + base));
    vp_report(" "); vp_hex(*(DWORD *)(p + base + 4)); vp_report("\n");
    int valid = size == base || size == extended;
    vp_check("size-result", (!!ok) == valid);
    vp_check("size-preserved", *(DWORD *)p == size);
    unsigned end = valid ? size : 4;
    int intact = 1;
    for (unsigned i = end; i < sizeof(storage); i++) if (p[i] != 0xa5) intact = 0;
    vp_check("no-write-outside-structure", intact);
    if (!valid) {
        vp_check("invalid-size-error", error == 122);
        return;
    }
    vp_check("success-last-error", error == 0x1234abcd);
    vp_check("platform-and-service-pack-text", *(DWORD *)(p + 16) == 2 && !p[20] && (!wide || !p[21]));
    DWORD major = *(DWORD *)(p + 4), minor = *(DWORD *)(p + 8), build = *(DWORD *)(p + 12);
#if VERSION_TARGET == 100
    vp_check("manifest-version", major == 10 && minor == 0 && build >= 10240);
#elif VERSION_TARGET == 63
    vp_check("manifest-version", major == 6 && minor == 3 && build == 9600);
#else
    vp_check("manifest-version", major == 6 && minor == 2 && build == 9200);
#endif
    if (size == extended) {
        for (unsigned i = 0; i < 5; i++) observed[wide][i] = ((DWORD *)p)[i];
        suites[wide] = *(WORD *)(p + base + 4);
        vp_check("extended-service-pack", !*(DWORD *)(p + base));
        vp_check("extended-product-type", p[base + 6] == 1);
        vp_check("extended-reserved", p[base + 7] == 0);
    }
}
static void vp_verify(VERIFYFN fn, CONDFN cond, unsigned wide)
{
    union { U64 alignment; BYTE b[284]; } storage;
    BYTE *p = storage.b;
    unsigned base = wide ? 276 : 148;
    for (unsigned i = 0; i < sizeof(storage); i++) p[i] = 0;
    for (unsigned i = 0; i < 5; i++) ((DWORD *)p)[i] = observed[wide][i];
    *(WORD *)(p + base + 4) = suites[wide]; p[base + 6] = 1;
    U64 mask = 0;
    for (unsigned bit = 0; bit < 8; bit++) mask = cond(mask, 1U << bit, 1);
    mask = cond(mask, 0x40, 6);
    SetLastError(0x1234abcd);
    vp_check("verify-exact", fn(p, 0xff, mask) != 0);
    vp_check("verify-success-last-error", GetLastError() == 0x1234abcd);
    *(DWORD *)(p + 4) = 5; *(DWORD *)(p + 8) = 99;
    *(WORD *)(p + base) = 99; *(WORD *)(p + base + 2) = 99;
    mask = 0;
    for (unsigned bit = 0; bit < 6; bit++) if ((1U << bit) & 0x33) mask = cond(mask, 1U << bit, 3);
    vp_check("verify-hierarchical-major", fn(p, 0x33, mask) != 0);
    *(DWORD *)(p + 4) = observed[wide][1]; *(DWORD *)(p + 8) = observed[wide][2] + 1;
    SetLastError(0);
    vp_check("verify-newer-minor-fails", fn(p, 0x33, mask) == 0 && GetLastError() == 1150);
    *(DWORD *)(p + 8) = observed[wide][2];
    *(WORD *)(p + base) = 0; *(WORD *)(p + base + 2) = 0;
    vp_check("verify-equal-ge", fn(p, 0x33, mask) != 0);
    mask = 0;
    for (unsigned bit = 0; bit < 6; bit++) if ((1U << bit) & 0x33) mask = cond(mask, 1U << bit, 2);
    vp_check("verify-equal-not-greater", fn(p, 0x33, mask) == 0 && GetLastError() == 1150);
    mask = 0;
    for (unsigned bit = 0; bit < 6; bit++) if ((1U << bit) & 0x33) mask = cond(mask, 1U << bit, 4);
    *(DWORD *)(p + 4) = observed[wide][1] + 1; *(DWORD *)(p + 8) = 0;
    vp_check("verify-hierarchical-less", fn(p, 0x33, mask) != 0);
    SetLastError(0);
    int result = fn(p, 0, mask); DWORD error = GetLastError();
    vp_report("VERSION VERIFY zero-type "); vp_hex((DWORD)result); vp_report(" "); vp_hex(error); vp_report("\n");
    vp_check("verify-zero-type-error", result == 0 && error == 160);
    SetLastError(0);
    result = fn(p, 2, 0); error = GetLastError();
    vp_report("VERSION VERIFY zero-condition "); vp_hex((DWORD)result); vp_report(" "); vp_hex(error); vp_report("\n");
    vp_check("verify-zero-condition-error", result == 0 && error == 1150);
    vp_report("VERSION CONDITION multi ");
    mask = cond(0, 0xff, 3); vp_hex((DWORD)mask); vp_report(" hi="); vp_hex((DWORD)(mask >> 32)); vp_report("\n");
    vp_check("condition-highest-field", (mask & 0xffffff) == ((U64)3 << 21));
    vp_report("VERSION CONDITION repeat ");
    mask = cond(cond(0, 2, 3), 2, 1); vp_hex((DWORD)mask); vp_report("\n");
    vp_check("condition-accumulates", (mask & 0xffffff) == 0x18);
    vp_report("VERSION CONDITION zero ");
    mask = cond(cond(0, 2, 3), 2, 0); vp_hex((DWORD)mask); vp_report("\n");
    vp_check("condition-zero-preserves", (mask & 0xffffff) == 0x18);
    DWORD digest = 2166136261U, trials = 0;
    for (unsigned major_condition = 1; major_condition <= 5; major_condition++)
    for (unsigned minor_condition = 1; minor_condition <= 5; minor_condition++)
    for (int major_delta = -1; major_delta <= 1; major_delta++)
    for (int minor_delta = -1; minor_delta <= 1; minor_delta++) {
        *(DWORD *)(p + 4) = observed[wide][1] + major_delta;
        *(DWORD *)(p + 8) = observed[wide][2] + minor_delta;
        mask = cond(cond(0, 2, (BYTE)major_condition), 1, (BYTE)minor_condition);
        SetLastError(0x1234abcd);
        result = fn(p, 3, mask); error = GetLastError();
        digest = (digest ^ (DWORD)result) * 16777619U;
        digest = (digest ^ error) * 16777619U; trials++;
    }
    vp_report("VERSION MATRIX "); vp_hex(trials); vp_report(" "); vp_hex(digest); vp_report("\n");
    /* Native Windows x86/x64 agree for all 225 mixed-condition requests,
     * including minor=-1. Hash includes both BOOL and the saved last error. */
    vp_check("verify-mixed-condition-matrix", trials == 225 && digest == 0x4c05cd6b);
}
void version_probe_entry(void)
{
    void *k = LoadLibraryA("kernel32.dll");
    EXFN a = GetProcAddress(k, "GetVersionExA"), w = GetProcAddress(k, "GetVersionExW");
    VERIFYFN va = GetProcAddress(k, "VerifyVersionInfoA"), vw = GetProcAddress(k, "VerifyVersionInfoW");
    CONDFN cond = GetProcAddress(k, "VerSetConditionMask");
    typedef DWORD (CALL *GETFN)(void);
    GETFN get = GetProcAddress(k, "GetVersion");
    vp_check("exports", a && w && va && vw && cond && get);
    if (failures) ExitProcess(1);
    for (unsigned wide = 0; wide < 2; wide++) {
        DWORD base = wide ? 276 : 148;
        DWORD sizes[] = {0, 20, base-1, base, base+1, base+7, base+8, base+9};
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) vp_query(wide ? w : a, wide, sizes[i]);
        vp_verify(wide ? vw : va, cond, wide);
    }
    SetLastError(0x1234abcd);
    DWORD v = get();
    vp_check("getversion-packed", v == ((observed[0][3] << 16) | (observed[0][2] << 8) | observed[0][1]));
    vp_check("getversion-last-error", GetLastError() == 0x1234abcd);
    vp_report("VERSION TOTAL "); vp_hex(checks); vp_report(" failures="); vp_hex(failures); vp_report("\n");
    if (!failures) vp_report("VERSION DONE\n");
    ExitProcess(failures ? 1 : 0);
}
