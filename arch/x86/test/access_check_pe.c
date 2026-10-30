/* The same ACL/token contract probe runs on Windows and OsitoK, PE32/PE64. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);

void *memcpy(void *out, const void *in, __SIZE_TYPE__ n)
{
    for (__SIZE_TYPE__ i = 0; i < n; i++) ((BYTE *)out)[i] = ((const BYTE *)in)[i];
    return out;
}
void *memset(void *out, int value, __SIZE_TYPE__ n)
{
    for (__SIZE_TYPE__ i = 0; i < n; i++) ((BYTE *)out)[i] = (BYTE)value;
    return out;
}

typedef struct { DWORD read, write, execute, all; } MAPPING;
typedef struct { BYTE revision, reserved; WORD control; void *owner, *group, *sacl, *dacl; } SD;
typedef struct { BYTE revision, reserved; WORD size, count, reserved2; BYTE aces[256]; } ACL;
typedef struct { BYTE revision, count, authority[6]; DWORD sub[2]; } SID;
static const SID world = {1, 1, {0,0,0,0,0,1}, {0}};
static const SID other = {1, 1, {0,0,0,0,0,5}, {18}};
static const SID authenticated = {1, 1, {0,0,0,0,0,5}, {11}};
static const SID users = {1, 2, {0,0,0,0,0,5}, {32,545}};
static const SID owner_rights = {1, 1, {0,0,0,0,0,3}, {4}};
static MAPPING mapping = {1,2,4,0x000f0007};
static BOOL (CALL *access_check)(void *, HANDLE, DWORD, MAPPING *, void *, DWORD *, DWORD *, BOOL *);
static BOOL (CALL *open_token)(HANDLE, DWORD, HANDLE *);
static BOOL (CALL *duplicate_token)(HANDLE, DWORD, void *, DWORD, DWORD, HANDLE *);
static BOOL (CALL *token_info)(HANDLE, DWORD, void *, DWORD, DWORD *);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *anonymous_token)(HANDLE);
static BOOL (CALL *thread_token)(HANDLE, DWORD, BOOL, HANDLE *);
static BOOL (CALL *revert)(void);
static BOOL (CALL *handle_info)(HANDLE, DWORD *);
static DWORD failures;
static DWORD current_level;

typedef struct { const char *name; DWORD error, grant; BOOL result; } EXPECTED;
static const EXPECTED expected[] = {
    {"null",0x1234,3,1}, {"null-max",0x1234,0xf0007,1},
    {"null-max-extra",0x1234,0xf000f,1}, {"zero",5,0,1},
    {"generic",1360,0xaaaaaaaa,0}, {"system-security",1314,0,1},
    {"primary",1309,0xaaaaaaaa,0}, {"bad-handle",6,0xaaaaaaaa,0},
    {"short0",122,0xaaaaaaaa,0}, {"short7",122,0xaaaaaaaa,0},
    {"short8",122,0xaaaaaaaa,0}, {"short19",122,0xaaaaaaaa,0},
    {"exact20",0x1234,1,1}, {"no-buffer0",122,0xaaaaaaaa,0},
    {"no-buffer128",998,0xaaaaaaaa,0}, {"no-query",5,0xaaaaaaaa,0},
    {"no-owner",1338,0xaaaaaaaa,0}, {"no-group",1338,0xaaaaaaaa,0},
    {"absent",0x1234,1,1}, {"empty",5,0,1}, {"empty-zero",5,0,1},
    {"empty-max",5,0,1}, {"user-allow",0x1234,1,1}, {"user-partial",5,0,1},
    {"user-max",0x1234,1,1}, {"user-max-required",5,0,1},
    {"allow-before-deny",0x1234,1,1}, {"allow-before-deny-max",0x1234,1,1},
    {"deny-before-allow",5,0,1}, {"deny-other-bit",0x1234,2,1},
    {"deny-before-allow-max",0x1234,2,1}, {"accumulate",0x1234,3,1},
    {"everyone",0x1234,1,1}, {"authenticated",0x1234,1,1}, {"users",0x1234,1,1},
    {"unrelated-sid",5,0,1}, {"inherit-only",5,0,1}, {"inherited",0x1234,1,1},
    {"generic-ace",5,0,1}, {"generic-ace-max",0x1234,0x80000000,1},
    {"owner-read-control",0x1234,0x20000,1}, {"owner-write-dac",0x1234,0x40000,1},
    {"owner-max",0x1234,0x60000,1}, {"owner-implicit-before-deny",0x1234,0x60000,1},
    {"owner-rights-allow",0x1234,1,1}, {"owner-rights-suppresses-implicit",5,0,1},
    {"owner-rights-inherit-only",0x1234,0x60000,1}, {"self-relative",0x1234,1,1},
};

static BOOL equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static BOOL sid_equal(const void *a, const void *b)
{
    const BYTE *left = a, *right = b;
    if (left[1] != right[1]) return 0;
    for (DWORD i = 0; i < 8U + 4U * left[1]; i++)
        if (left[i] != right[i]) return 0;
    return 1;
}

static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void hex(DWORD value)
{
    char s[9];
    for (DWORD i = 0; i < 8; i++) s[i] = "0123456789abcdef"[(value >> (28-4*i)) & 15];
    s[8] = 0; report(s);
}
static void check(BOOL ok, const char *name)
{
    if (!ok) { failures++; report("ACCESS-CHECK FAIL: "); report(name); report("\n"); }
}
static void reset_acl(ACL *acl)
{
    memset(acl, 0, sizeof(*acl));
    acl->revision = 2; acl->size = 8;
}
static void ace(ACL *acl, BYTE type, BYTE flags, DWORD mask, const void *sid)
{
    WORD sid_size = 8 + 4 * ((const BYTE *)sid)[1];
    BYTE *p = (BYTE *)acl + acl->size;
    p[0] = type; p[1] = flags; *(WORD *)(p+2) = 8 + sid_size;
    *(DWORD *)(p+4) = mask; memcpy(p+8, sid, sid_size);
    acl->count++; acl->size += 8 + sid_size;
}
static void probe(const char *label, void *sd, HANDLE token, DWORD access,
                  DWORD length, BOOL with_buffer)
{
    DWORD privileges[32];
    memset(privileges, 0xcc, sizeof(privileges));
    DWORD granted = 0xaaaaaaaa;
    BOOL status = 0x55555555;
    DWORD supplied_length = length;
    SetLastError(0x1234);
    BOOL result = access_check(sd, token, access, &mapping,
                              with_buffer ? privileges : 0, &length, &granted, &status);
    DWORD error = GetLastError();
    report(label); report(" ret="); hex(result); report(" err="); hex(error);
    report(" grant="); hex(granted); report(" status="); hex(status);
    report(" len="); hex(length); report(" count="); hex(privileges[0]);
    report(" control="); hex(privileges[1]); report("\n");
    const EXPECTED *want = 0;
    EXPECTED level = {"level", current_level ? 0x1234 : 1346,
                      current_level ? 1 : 0xaaaaaaaa, current_level != 0};
    if (equal(label, "level")) want = &level;
    for (DWORD i = 0; i < sizeof(expected)/sizeof(expected[0]); i++)
        if (equal(label, expected[i].name)) want = &expected[i];
    check(want != 0, "expected result recorded");
    if (want) {
        check(result == want->result && error == want->error && granted == want->grant &&
              status == (want->result ? want->grant != 0 : 0x55555555), label);
        check(length == (want->error == 122 ? 20 : supplied_length), "privilege buffer length");
        check(privileges[0] == ((!with_buffer || want->error == 122) ? 0xcccccccc : 0),
              "privilege count and failure buffer");
        check(privileges[1] == ((want->result && want->error != 1314) ? 0 : 0xcccccccc),
              "privilege control and failure buffer");
    }
}

static void denied_descriptor(const char *label, void *sd, HANDLE token)
{
    DWORD privileges[8], length = sizeof(privileges), granted = 0xaaaaaaaa;
    BOOL status = 0x55555555;
    BOOL result = access_check(sd, token, 1, &mapping, privileges, &length, &granted, &status);
    DWORD error = GetLastError();
    report(label); report(" fail-closed ret="); hex(result); report(" err="); hex(error);
    report("\n");
    check(result ? !status && !granted : granted == 0xaaaaaaaa, label);
    if (equal(label, "ace-outside-acl") || equal(label, "sid-outside-ace"))
        check(!result && error == 1336, "malformed ACL error");
}

void mainCRTStartup(void)
{
    HANDLE library = LoadLibraryA("advapi32.dll");
    access_check = GetProcAddress(library, "AccessCheck");
    open_token = GetProcAddress(library, "OpenProcessToken");
    duplicate_token = GetProcAddress(library, "DuplicateTokenEx");
    token_info = GetProcAddress(library, "GetTokenInformation");
    close_handle = GetProcAddress(LoadLibraryA("kernel32.dll"), "CloseHandle");
    handle_info = GetProcAddress(LoadLibraryA("kernel32.dll"), "GetHandleInformation");
    anonymous_token = GetProcAddress(library, "ImpersonateAnonymousToken");
    thread_token = GetProcAddress(library, "OpenThreadToken");
    revert = GetProcAddress(library, "RevertToSelf");
    if (!access_check || !open_token || !duplicate_token || !token_info || !close_handle) {
        report("ACCESS-CHECK MISSING EXPORT\n"); ExitProcess(2);
    }
    if (thread_token) {
        HANDLE stale = 0;
        check(!thread_token((HANDLE)(__INTPTR_TYPE__)-2, 8, 1, &stale) && GetLastError() == 1008,
              "new execution has no stale impersonation");
    }
    HANDLE primary = 0, token = 0, no_query = 0;
    check(open_token((HANDLE)(__INTPTR_TYPE__)-1, 0xa, &primary), "open primary");
    check(duplicate_token(primary, 0xa, 0, 2, 2, &token), "duplicate impersonation");
    if (!primary || !token) ExitProcess(3);
    BYTE user_buffer[128]; DWORD needed, value;
    check(token_info(token, 1, user_buffer, sizeof(user_buffer), &needed), "query user");
    void *user = *(void **)user_buffer;
    check(token_info(token, 8, &value, sizeof(value), &needed) && value == 2,
          "duplicate has requested type");
    check(token_info(primary, 8, &value, sizeof(value), &needed) && value == 1,
          "duplication leaves source unchanged");
    static BYTE groups[4096];
    check(token_info(token, 2, groups, sizeof(groups), &needed), "query groups");
    DWORD groups_count = *(DWORD *)groups;
    DWORD present = 0;
    struct GROUP { void *sid; DWORD attributes; };
    const struct GROUP *entries = (const void *)(groups + (sizeof(void *) == 8 ? 8 : 4));
    for (DWORD i = 0; i < groups_count; i++) {
        if (!(entries[i].attributes & 4) || (entries[i].attributes & 0x10)) continue;
        if (sid_equal(entries[i].sid, &world)) present |= 1;
        if (sid_equal(entries[i].sid, &authenticated)) present |= 2;
        if (sid_equal(entries[i].sid, &users)) present |= 4;
    }
    check(present == 7, "reported groups agree with ACL membership");
    DWORD stats[14];
    check(token_info(token, 10, stats, sizeof(stats), &needed) && stats[10] == groups_count,
          "statistics group count");
    ACL acl; reset_acl(&acl);
    SD sd = {1,0,4,(void *)&other,(void *)&world,0,0};
    probe("null", &sd, token, 3, 128, 1);
    probe("null-max", &sd, token, 0x02000000, 128, 1);
    probe("null-max-extra", &sd, token, 0x02000008, 128, 1);
    probe("zero", &sd, token, 0, 128, 1);
    probe("generic", &sd, token, 0x80000000, 128, 1);
    probe("system-security", &sd, token, 0x01000000, 128, 1);
    probe("primary", &sd, primary, 1, 128, 1);
    probe("bad-handle", &sd, (HANDLE)(__INTPTR_TYPE__)0x1234, 1, 128, 1);
    probe("short0", &sd, token, 1, 0, 1);
    probe("short7", &sd, token, 1, 7, 1);
    probe("short8", &sd, token, 1, 8, 1);
    probe("short19", &sd, token, 1, 19, 1);
    probe("exact20", &sd, token, 1, 20, 1);
    probe("no-buffer0", &sd, token, 1, 0, 0);
    probe("no-buffer128", &sd, token, 1, 128, 0);
    check(duplicate_token(primary, 2, 0, 2, 2, &no_query), "duplicate no query");
    probe("no-query", &sd, no_query, 1, 128, 1);
    check(!token_info(no_query, 8, &value, sizeof(value), &needed) && GetLastError() == 5,
          "query requires TOKEN_QUERY");
    close_handle(no_query);
    HANDLE query_only = 0, rejected = 0;
    check(open_token((HANDLE)(__INTPTR_TYPE__)-1, 8, &query_only), "query-only token");
    check(!duplicate_token(query_only, 0, 0, 2, 2, &rejected) && GetLastError() == 5 && !rejected,
          "duplication requires TOKEN_DUPLICATE");
    close_handle(query_only);
    if (handle_info) {
        struct { DWORD size; void *sd; BOOL inherit; } attributes = {sizeof(attributes),0,1};
        HANDLE inherited = 0;
        check(duplicate_token(primary, 0, &attributes, 2, 2, &inherited), "inheritable token");
        check(handle_info(inherited, &value) && (value & 1), "token handle inherit flag");
        close_handle(inherited);
    }
    for (DWORD level = 0; level <= 3; level++) {
        current_level = level;
        HANDLE t = 0, higher = 0;
        check(duplicate_token(primary, 0, 0, level, 2, &t), "duplicate level");
        check(token_info(t, 9, &value, sizeof(value), &needed) && value == level, "requested level");
        report("LEVEL "); hex(level); report("\n");
        probe("level", &sd, t, 1, 128, 1);
        SetLastError(0x1234);
        BOOL ok = duplicate_token(t, 0, 0, 3, 2, &higher);
        DWORD error = GetLastError();
        check(ok == (level == 3) && error == (level == 3 ? 0x1234 : 1346),
              "cannot raise impersonation level");
        report("upgrade ret="); hex(ok); report(" err="); hex(error); report("\n");
        if (higher) close_handle(higher);
        close_handle(t);
    }
    sd.owner = 0; probe("no-owner", &sd, token, 1, 128, 1); sd.owner = (void *)&other;
    sd.group = 0; probe("no-group", &sd, token, 1, 128, 1); sd.group = (void *)&world;
    sd.control = 0; probe("absent", &sd, token, 1, 128, 1); sd.control = 4;
    sd.dacl = &acl;
    probe("empty", &sd, token, 1, 128, 1);
    probe("empty-zero", &sd, token, 0, 128, 1);
    probe("empty-max", &sd, token, 0x02000000, 128, 1);
    ace(&acl, 0, 0, 1, user);
    probe("user-allow", &sd, token, 1, 128, 1);
    probe("user-partial", &sd, token, 3, 128, 1);
    probe("user-max", &sd, token, 0x02000000, 128, 1);
    probe("user-max-required", &sd, token, 0x02000002, 128, 1);
    ace(&acl, 1, 0, 1, user);
    probe("allow-before-deny", &sd, token, 1, 128, 1);
    probe("allow-before-deny-max", &sd, token, 0x02000000, 128, 1);
    reset_acl(&acl); ace(&acl, 1, 0, 1, user); ace(&acl, 0, 0, 3, user);
    probe("deny-before-allow", &sd, token, 1, 128, 1);
    probe("deny-other-bit", &sd, token, 2, 128, 1);
    probe("deny-before-allow-max", &sd, token, 0x02000000, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, user); ace(&acl, 0, 0, 2, &world);
    probe("accumulate", &sd, token, 3, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, &world);
    probe("everyone", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, &authenticated);
    probe("authenticated", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, &users);
    probe("users", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, &other);
    probe("unrelated-sid", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 8, 1, user);
    probe("inherit-only", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0x10, 1, user);
    probe("inherited", &sd, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 0x80000000, user);
    probe("generic-ace", &sd, token, 1, 128, 1);
    probe("generic-ace-max", &sd, token, 0x02000000, 128, 1);
    reset_acl(&acl); sd.owner = user;
    probe("owner-read-control", &sd, token, 0x00020000, 128, 1);
    probe("owner-write-dac", &sd, token, 0x00040000, 128, 1);
    probe("owner-max", &sd, token, 0x02000000, 128, 1);
    ace(&acl, 1, 0, 0x00060000, user);
    probe("owner-implicit-before-deny", &sd, token, 0x00060000, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, &owner_rights);
    probe("owner-rights-allow", &sd, token, 1, 128, 1);
    probe("owner-rights-suppresses-implicit", &sd, token, 0x00060000, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 8, 1, &owner_rights);
    probe("owner-rights-inherit-only", &sd, token, 0x00060000, 128, 1);
    sd.owner = (void *)&other;
    reset_acl(&acl); ace(&acl, 0, 0, 1, user);
    struct { BYTE revision, reserved; WORD control; DWORD owner, group, sacl, dacl;
             BYTE data[512]; } relative;
    memset(&relative, 0, sizeof(relative));
    relative.revision = 1; relative.control = 0x8004;
    relative.owner = 20; relative.group = 36; relative.dacl = 52;
    memcpy((BYTE *)&relative + 20, &other, sizeof(other));
    memcpy((BYTE *)&relative + 36, &world, sizeof(world));
    memcpy((BYTE *)&relative + 52, &acl, sizeof(acl));
    probe("self-relative", &relative, token, 1, 128, 1);
    reset_acl(&acl); ace(&acl, 0, 0, 1, user);
    *(WORD *)(acl.aces + 2) = acl.size;
    denied_descriptor("ace-outside-acl", &sd, token);
    reset_acl(&acl); ace(&acl, 0, 0, 1, user);
    acl.aces[9] = 15;
    denied_descriptor("sid-outside-ace", &sd, token);
    reset_acl(&acl); ace(&acl, 0x7f, 0, 1, user);
    denied_descriptor("unknown-ace", &sd, token);
    if (anonymous_token && thread_token && revert) {
        HANDLE anonymous = 0;
        check(anonymous_token((HANDLE)(__INTPTR_TYPE__)-2), "anonymous impersonation");
        check(thread_token((HANDLE)(__INTPTR_TYPE__)-2, 0xa, 1, &anonymous), "anonymous token");
        check(revert(), "revert to self");
        check(token_info(anonymous, 1, user_buffer, sizeof(user_buffer), &needed), "anonymous user");
        const SID anonymous_sid = {1,1,{0,0,0,0,0,5},{7}};
        check(sid_equal(*(void **)user_buffer, &anonymous_sid), "anonymous identity preserved");
        reset_acl(&acl); ace(&acl, 0, 0, 1, &authenticated);
        denied_descriptor("anonymous-not-authenticated", &sd, anonymous);
        reset_acl(&acl); ace(&acl, 0, 0, 1, &world);
        denied_descriptor("anonymous-not-everyone", &sd, anonymous);
        close_handle(anonymous);
        /* The next PE launch must not inherit this thread's impersonation. */
        check(anonymous_token((HANDLE)(__INTPTR_TYPE__)-2), "exit while impersonating");
    }
    close_handle(token); close_handle(primary);
    report(failures ? "ACCESS-CHECK FAILED\n" : "ACCESS-CHECK PASS\n");
    ExitProcess(failures ? 1 : 0);
}
