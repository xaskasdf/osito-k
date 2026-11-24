/* Restricted-token contracts, shared by Windows and OsitoK PE32/PE64 runs. */
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
void *memcpy(void *out, const void *in, __SIZE_TYPE__ size)
{
    for (__SIZE_TYPE__ i = 0; i < size; i++) ((BYTE *)out)[i] = ((const BYTE *)in)[i];
    return out;
}
void *memset(void *out, int value, __SIZE_TYPE__ size)
{
    for (__SIZE_TYPE__ i = 0; i < size; i++) ((BYTE *)out)[i] = (BYTE)value;
    return out;
}
typedef struct { BYTE revision, count, authority[6]; DWORD sub[2]; } SID;
typedef struct { void *sid; DWORD attributes; } GROUP;
typedef struct { DWORD read, write, execute, all; } MAPPING;
typedef struct { BYTE revision, reserved; WORD control; void *owner, *group, *sacl, *dacl; } SD;
typedef struct { BYTE revision, reserved; WORD size, count, reserved2; BYTE aces[256]; } ACL;
static SID world = {1,1,{0,0,0,0,0,1},{0}};
static SID system_sid = {1,1,{0,0,0,0,0,5},{18}};
static SID custom = {1,1,{0,0,0,0,0,5},{99}};
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,GROUP *,DWORD,void *,DWORD,GROUP *,HANDLE *);
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *duplicate_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *token_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *access_check)(void *,HANDLE,DWORD,MAPPING *,void *,DWORD *,DWORD *,BOOL *);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *set_thread_token)(HANDLE *,HANDLE);
static BOOL (CALL *open_thread_token)(HANDLE,DWORD,BOOL,HANDLE *);
static BOOL (CALL *revert)(void);
static BOOL (CALL *is_restricted)(HANDLE);
static BOOL (CALL *duplicate_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static HANDLE (CALL *create_thread)(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *thread_exit_code)(HANDLE,DWORD *);
static DWORD failures;
static HANDLE primary, impersonation;
static BYTE user_buffer[128];
static void *user;

static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void hex(DWORD value)
{
    char text[9];
    for (DWORD i = 0; i < 8; i++) text[i] = "0123456789abcdef"[(value >> (28 - 4*i)) & 15];
    text[8] = 0; report(text);
}
static void check(BOOL ok, const char *label)
{
    if (!ok) { failures++; report("RESTRICTED-TOKEN FAIL: "); report(label); report("\n"); }
}
static BOOL sid_equal(const void *left, const void *right)
{
    const BYTE *a = left, *b = right;
    if (a[1] != b[1]) return 0;
    for (DWORD i = 0; i < 8U + 4U * a[1]; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static DWORD dword_info(HANDLE token, DWORD cls)
{
    DWORD result = 0, needed;
    if (!token_info(token, cls, &result, sizeof(result), &needed)) {
        report("query failed cls="); hex(cls); report(" error="); hex(GetLastError()); report("\n");
        failures++;
    }
    if (cls == 21) check(needed == 1, "TokenHasRestrictions is BOOLEAN");
    return result;
}
static void describe(const char *label, HANDLE token)
{
    report(label); report(" type="); hex(dword_info(token,8));
    report(" restricted="); hex(is_restricted(token));
    report(" has="); hex(dword_info(token,21)); report(" inert="); hex(dword_info(token,15));
    report("\n");
    BYTE buffer[2048]; DWORD needed;
    for (DWORD cls = 2; cls <= 11; cls += 9) {
        check(token_info(token,cls,buffer,sizeof(buffer),&needed), "query SID groups");
        DWORD count = *(DWORD *)buffer;
        GROUP *groups = (void *)(buffer + (sizeof(void *) == 8 ? 8 : 4));
        report(" groups cls="); hex(cls); report(" count="); hex(count);
        for (DWORD i = 0; i < count; i++) {
            if (cls == 11 || sid_equal(groups[i].sid,&world)) {
                report(" sid-auth="); hex(((BYTE *)groups[i].sid)[7]);
                report(" rid="); hex(((DWORD *)groups[i].sid)[2]);
                report(" attr="); hex(groups[i].attributes);
            }
        }
        report("\n");
    }
    check(token_info(token,1,buffer,sizeof(buffer),&needed), "query token user");
    report(" user-attr="); hex(((GROUP *)buffer)->attributes); report("\n");
}
static HANDLE create(const char *label, HANDLE source, DWORD flags,
                     DWORD nd, GROUP *disable, DWORD nr, GROUP *restricting)
{
    HANDLE result = 0;
    SetLastError(0x1234);
    BOOL ok = restrict_token(source,flags,nd,disable,0,0,nr,restricting,&result);
    DWORD error = GetLastError();
    report(label); report(" create="); hex(ok); report(" error="); hex(error); report("\n");
    check(ok && result, label);
    if (result) describe(label,result);
    return result;
}
static void reset_acl(ACL *acl)
{
    memset(acl,0,sizeof(*acl)); acl->revision = 2; acl->size = 8;
}
static void ace(ACL *acl, BYTE type, DWORD mask, const void *sid)
{
    WORD size = 8 + 4 * ((const BYTE *)sid)[1];
    BYTE *p = (BYTE *)acl + acl->size;
    p[0] = type; p[1] = 0; *(WORD *)(p+2) = 8 + size;
    *(DWORD *)(p+4) = mask; memcpy(p+8,sid,size);
    acl->size += 8 + size; acl->count++;
}
static void access(const char *label, HANDLE token, ACL *acl, void *owner, DWORD desired,
                   BOOL expected, DWORD expected_grant)
{
    SD sd = {1,0,4,owner,&world,0,acl};
    MAPPING mapping = {1,2,4,0xf0007};
    DWORD privileges[16], length = sizeof(privileges), granted = 0xaaaaaaaa;
    BOOL status = 0x55555555;
    SetLastError(0x1234);
    BOOL ok = access_check(&sd,token,desired,&mapping,privileges,&length,&granted,&status);
    DWORD error = GetLastError();
    report(label); report(" access="); hex(ok); report(" status="); hex(status);
    report(" grant="); hex(granted); report(" error="); hex(error); report("\n");
    check(ok && status == expected && granted == expected_grant &&
          error == (expected ? 0x1234U : 5U), label);
}

static void rejected(const char *label, HANDLE source, DWORD flags, DWORD count, GROUP *groups)
{
    HANDLE result = (HANDLE)(__INTPTR_TYPE__)0x1234;
    SetLastError(0x1234);
    BOOL ok = restrict_token(source,flags,0,0,0,0,count,groups,&result);
    check(!ok && GetLastError() == 87, label);
}

static DWORD attributes(HANDLE token, DWORD cls, const void *sid, DWORD *group_count)
{
    BYTE buffer[2048]; DWORD needed;
    if (!token_info(token,cls,buffer,sizeof(buffer),&needed)) {
        check(0, "query attributes"); return 0xffffffff;
    }
    GROUP *groups = cls == 1 ? (void *)buffer
        : (void *)(buffer + (sizeof(void *) == 8 ? 8 : 4));
    DWORD count = cls == 1 ? 1 : *(DWORD *)buffer;
    if (group_count) *group_count = count;
    for (DWORD i = 0; i < count; i++)
        if (sid_equal(groups[i].sid,sid)) return groups[i].attributes;
    return 0xffffffff;
}

static DWORD CALL token_worker(void *token)
{
    BOOL ok = set_thread_token(0,token);
    close_handle(token);
    return ok ? 0x1234 : 1; /* Thread exit owns the remaining token reference. */
}

static void logon_sid_contract(HANDLE token)
{
    BYTE buffer[2048], logon_buffer[256]; DWORD needed;
    if (!token_info(token,2,buffer,sizeof(buffer),&needed)) {
        check(0, "query groups for logon SID"); return;
    }
    GROUP *groups = (void *)(buffer + (sizeof(void *) == 8 ? 8 : 4));
    void *logon = 0;
    for (DWORD i = 0; i < *(DWORD *)buffer; i++)
        if ((groups[i].attributes & 0xc0000000) == 0xc0000000) logon = groups[i].sid;
    BOOL result = token_info(token,28,logon_buffer,sizeof(logon_buffer),&needed);
    if (!logon) check(!result && GetLastError() == 1168, "absent logon SID reports NOT_FOUND");
    else {
        GROUP *entry = (void *)(logon_buffer + (sizeof(void *) == 8 ? 8 : 4));
        check(result && *(DWORD *)logon_buffer == 1 && sid_equal(entry->sid,logon),
              "logon SID agrees with token groups");
    }
}

void mainCRTStartup(void)
{
    HANDLE library = LoadLibraryA("advapi32.dll"), kernel = LoadLibraryA("kernel32.dll");
    restrict_token = GetProcAddress(library,"CreateRestrictedToken");
    open_token = GetProcAddress(library,"OpenProcessToken");
    duplicate_token = GetProcAddress(library,"DuplicateTokenEx");
    token_info = GetProcAddress(library,"GetTokenInformation");
    access_check = GetProcAddress(library,"AccessCheck");
    set_thread_token = GetProcAddress(library,"SetThreadToken");
    open_thread_token = GetProcAddress(library,"OpenThreadToken");
    revert = GetProcAddress(library,"RevertToSelf");
    is_restricted = GetProcAddress(library,"IsTokenRestricted");
    close_handle = GetProcAddress(kernel,"CloseHandle");
    duplicate_handle = GetProcAddress(kernel,"DuplicateHandle");
    create_thread = GetProcAddress(kernel,"CreateThread");
    wait_object = GetProcAddress(kernel,"WaitForSingleObject");
    thread_exit_code = GetProcAddress(kernel,"GetExitCodeThread");
    if (!restrict_token || !open_token || !duplicate_token || !token_info || !access_check ||
        !set_thread_token || !open_thread_token || !revert || !is_restricted ||
        !close_handle || !duplicate_handle || !create_thread || !wait_object || !thread_exit_code) {
        report("RESTRICTED-TOKEN MISSING EXPORT\n"); ExitProcess(2);
    }
    HANDLE empty_thread = 0;
    check(!open_thread_token((HANDLE)(__INTPTR_TYPE__)-2,8,1,&empty_thread) &&
          GetLastError() == 1008, "previous process impersonation cleaned up");
    check(open_token((HANDLE)(__INTPTR_TYPE__)-1,0xf,&primary), "open primary token");
    check(duplicate_token(primary,0,0,2,2,&impersonation), "impersonation copy");
    DWORD needed;
    check(token_info(primary,1,user_buffer,sizeof(user_buffer),&needed), "source user");
    user = *(void **)user_buffer;
    if (!primary || !impersonation || !user) ExitProcess(3);
    describe("source",impersonation);
    logon_sid_contract(impersonation);
    GROUP disable = {&world,0xffffffff}, restricting[2] = {{&custom,0},{&system_sid,0}};
    ACL acl;
    HANDLE noop = create("noop",impersonation,0,0,0,0,0);
    HANDLE denied = create("disable-world",impersonation,0,1,&disable,0,0);
    check(!is_restricted(noop) && !is_restricted(denied), "deny-only does not imply restricting SIDs");
    check(attributes(denied,2,&world,0) == 0x10, "Everyone deny-only attributes");
    check(attributes(impersonation,2,&world,0) == 7, "source not modified");
    reset_acl(&acl); ace(&acl,0,3,&world);
    access("deny-only-no-allow",denied,&acl,&system_sid,1,0,0);
    ace(&acl,0,3,user); access("deny-only-user-allow",denied,&acl,&system_sid,1,1,1);
    reset_acl(&acl); ace(&acl,1,1,&world); ace(&acl,0,3,user);
    access("deny-only-still-denies",denied,&acl,&system_sid,1,0,0);
    disable.sid = user;
    HANDLE denied_user = create("disable-user",impersonation,0,1,&disable,0,0);
    check(attributes(denied_user,1,user,0) == 0x10, "user deny-only attributes");
    reset_acl(&acl); ace(&acl,0,3,user);
    access("disabled-user-no-allow",denied_user,&acl,&system_sid,1,0,0);
    access("disabled-owner-no-implicit",denied_user,&acl,user,0x60000,0,0);
    HANDLE restricted = create("restrict-custom",impersonation,0,0,0,1,restricting);
    check(is_restricted(restricted) && dword_info(restricted,21), "restricted status");
    reset_acl(&acl); ace(&acl,0,7,&world);
    access("normal-pass-restricted-fail",restricted,&acl,&system_sid,1,0,0);
    ace(&acl,0,3,&custom);
    access("both-pass",restricted,&acl,&system_sid,3,1,3);
    access("both-max",restricted,&acl,&system_sid,0x02000000,1,3);
    access("restricted-null-dacl",restricted,0,&system_sid,1,1,1);
    reset_acl(&acl); ace(&acl,0,7,&custom);
    access("normal-fail-restricted-pass",restricted,&acl,&system_sid,1,0,0);
    reset_acl(&acl); ace(&acl,0,1,&custom);
    access("restricted-owner-read-control",restricted,&acl,user,0x20000,0,0);
    rejected("nested-empty rejected",restricted,0,0,0);
    rejected("nested-disjoint rejected",restricted,0,1,restricting+1);
    HANDLE nested_overlap = create("nested-overlap",restricted,0,0,0,2,restricting);
    DWORD count;
    check(attributes(nested_overlap,11,&custom,&count) == 7 && count == 1,
          "nested SID intersection");
    reset_acl(&acl); ace(&acl,0,7,&world);
    access("nested-overlap-access",nested_overlap,&acl,&system_sid,1,0,0);
    restricting[1] = restricting[0];
    HANDLE repeated = create("duplicate-restricting-sid",impersonation,0,0,0,2,restricting);
    check(attributes(repeated,11,&custom,&count) == 7 && count == 2,
          "duplicate restricting SIDs retained");
    HANDLE write_only = create("write-restricted",impersonation,8,0,0,1,restricting);
    access("write-restricted-read",write_only,&acl,&system_sid,1,1,1);
    access("write-restricted-write",write_only,&acl,&system_sid,2,0,0);
    access("write-restricted-execute",write_only,&acl,&system_sid,4,1,4);
    ace(&acl,0,0xf0000,&world);
    access("write-restricted-read-control",write_only,&acl,&system_sid,0x20000,1,0x20000);
    access("write-restricted-write-dac",write_only,&acl,&system_sid,0x40000,0,0);
    access("write-restricted-delete",write_only,&acl,&system_sid,0x10000,0,0);
    access("write-restricted-max",write_only,&acl,&system_sid,0x02000000,1,0x20005);
    reset_acl(&acl); ace(&acl,1,1,&custom); ace(&acl,0,3,&world); ace(&acl,0,2,&custom);
    access("write-restricted-mixed-read-denial",write_only,&acl,&system_sid,3,1,3);
    HANDLE nested_write = create("nested-write-default",write_only,0,0,0,1,restricting);
    rejected("nested-full-write-flag rejected",restricted,8,1,restricting);
    reset_acl(&acl); ace(&acl,0,7,&world);
    access("nested-write-read",nested_write,&acl,&system_sid,1,0,0);
    GROUP restrict_user = {user,0};
    HANDLE owner_restricted = create("restrict-owner",impersonation,0,0,0,1,&restrict_user);
    reset_acl(&acl);
    access("restrict-owner-implicit",owner_restricted,&acl,user,0x60000,1,0x60000);
    HANDLE max_priv = create("disable-max-privilege",impersonation,1,0,0,0,0);
    HANDLE inert = create("inert",impersonation,2,0,0,0,0);
    HANDLE copy = 0, shared = 0;
    check(duplicate_token(restricted,0,0,2,2,&copy), "duplicate restricted token");
    check(duplicate_handle((HANDLE)(__INTPTR_TYPE__)-1,restricted,(HANDLE)(__INTPTR_TYPE__)-1,
                           &shared,0,0,2), "duplicate restricted handle");
    close_handle(restricted);
    reset_acl(&acl); ace(&acl,0,7,&world);
    access("duplicate-preserves-restrictions",copy,&acl,&system_sid,1,0,0);
    access("handle-preserves-restrictions",shared,&acl,&system_sid,1,0,0);
    check(set_thread_token(0,copy), "impersonate restricted token");
    close_handle(copy); close_handle(shared);
    HANDLE thread = 0;
    check(open_thread_token((HANDLE)(__INTPTR_TYPE__)-2,0xf,1,&thread), "thread owns token after close");
    check(revert(), "revert releases thread reference");
    access("open-thread-reference-survives-revert",thread,&acl,&system_sid,1,0,0);
    close_handle(thread);
    HANDLE query_only = 0, output = 0;
    check(duplicate_handle((HANDLE)(__INTPTR_TYPE__)-1,impersonation,(HANDLE)(__INTPTR_TYPE__)-1,
                           &query_only,8,0,0), "query-only handle");
    check(!restrict_token(query_only,0,0,0,0,0,0,0,&output) && GetLastError() == 5,
          "filter requires TOKEN_DUPLICATE");
    close_handle(query_only);
    check(!restrict_token((HANDLE)(__INTPTR_TYPE__)0x7654,0,0,0,0,0,0,0,&output) &&
          GetLastError() == 6, "invalid source handle");
    SID invalid = custom; invalid.revision = 2;
    GROUP invalid_group = {&invalid,0};
    check(!restrict_token(impersonation,0,0,0,0,0,1,&invalid_group,&output) &&
          GetLastError() == 1337, "invalid restricting SID");
    SID mutable = custom;
    GROUP mutable_group = {&mutable,0};
    HANDLE snapshot = create("SID-snapshot",impersonation,0,0,0,1,&mutable_group);
    mutable.sub[0]++;
    check(attributes(snapshot,11,&custom,&count) == 7 && count == 1, "token owns SID bytes");
    close_handle(snapshot);
    DWORD delete_privilege[3] = {23,0,0};
    for (DWORD i = 0; i < 4096; i++) {
        HANDLE a = 0, b = 0;
        if (!restrict_token(impersonation,1,0,0,1,delete_privilege,1,restricting,&a) ||
            !duplicate_token(a,0,0,2,2,&b) || !set_thread_token(0,b)) {
            report("stress error="); hex(GetLastError()); report("\n");
            if (a) close_handle(a);
            if (b) close_handle(b);
            check(0, "filter/duplicate/impersonation stress"); break;
        }
        close_handle(a); close_handle(b);
        if (i & 1) check(revert(), "stress revert");
    }
    report("RESTRICTED-TOKEN STRESS COMPLETE\n");
    HANDLE worker_token = 0;
    check(duplicate_token(nested_overlap,0,0,2,2,&worker_token), "worker token copy");
    HANDLE worker = create_thread(0,0,token_worker,worker_token,0,0);
    check(worker != 0, "create impersonating worker");
    if (worker) {
        DWORD code = 0;
        check(wait_object(worker,30000) == 0 && thread_exit_code(worker,&code) && code == 0x1234,
              "worker exits while impersonating");
        close_handle(worker);
    } else if (worker_token) close_handle(worker_token);
    check(set_thread_token(0,nested_overlap), "leave token for process-exit cleanup");
    HANDLE all[] = {noop,denied,denied_user,nested_overlap,repeated,nested_write,owner_restricted,
                    write_only,max_priv,inert,impersonation,primary};
    for (DWORD i = 0; i < sizeof(all)/sizeof(all[0]); i++) if (all[i]) close_handle(all[i]);
    report(failures ? "RESTRICTED-TOKEN FAILED\n" : "RESTRICTED-TOKEN PASS\n");
    ExitProcess(failures ? 1 : 0);
}
