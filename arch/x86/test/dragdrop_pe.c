/* Cancel-only OLE contract probe: identical binary on Windows and OsitoK. */
typedef unsigned int DWORD;
typedef int HRESULT;
typedef __UINTPTR_TYPE__ WORDPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
#define SAY(text) do { DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), text, sizeof(text)-1, &written, 0); } while (0)
#define CHECK(test, label) do { \
    if (test) SAY("DODRAGDROP PASS " label "\n"); \
    else { SAY("DODRAGDROP FAIL " label "\n"); failures++; } \
} while (0)

typedef struct { DWORD first; unsigned short second, third; unsigned char tail[8]; } GUID;
typedef struct { void **vtable; DWORD refs, role, reads; } OBJECT;

static HRESULT CALL probe_query_interface(OBJECT *self, const GUID *iid, void **out)
{
    if (!out) return (HRESULT)0x80004003;
    *out = 0;
    if (!iid || iid->second || iid->third || iid->tail[0] != 0xc0 ||
        iid->tail[7] != 0x46) return (HRESULT)0x80004002;
    for (DWORD i = 1; i < 7; i++)
        if (iid->tail[i]) return (HRESULT)0x80004002;
    if (iid->first && iid->first != (self->role == 1 ? 0x10eU : 0x121U))
        return (HRESULT)0x80004002;
    self->refs++;
    *out = self;
    return 0;
}

static DWORD CALL probe_addref(OBJECT *self) { return ++self->refs; }
static DWORD CALL probe_release(OBJECT *self) { return --self->refs; }
static HRESULT CALL probe_cancel(OBJECT *self, int escape, DWORD keys)
{
    (void)self; (void)escape; (void)keys;
    return 0x00040101; /* DRAGDROP_S_CANCEL; never start a real transfer. */
}
static HRESULT CALL probe_feedback(OBJECT *self, DWORD effect)
{
    (void)self; (void)effect;
    return 0x00040102; /* DRAGDROP_S_USEDEFAULTCURSORS */
}
static HRESULT CALL probe_unsupported2(OBJECT *self, WORDPTR a)
{
    (void)a; self->reads++;
    return (HRESULT)0x80004001;
}
static HRESULT CALL probe_unsupported3(OBJECT *self, WORDPTR a, WORDPTR b)
{
    (void)a; (void)b; self->reads++;
    return (HRESULT)0x80004001;
}
static HRESULT CALL probe_unsupported4(OBJECT *self, WORDPTR a, WORDPTR b, WORDPTR c)
{
    (void)a; (void)b; (void)c; self->reads++;
    return (HRESULT)0x80004001;
}
static HRESULT CALL probe_unsupported5(OBJECT *self, WORDPTR a, WORDPTR b, WORDPTR c, WORDPTR d)
{
    (void)a; (void)b; (void)c; (void)d; self->reads++;
    return (HRESULT)0x80004001;
}

static void *data_vtable[] = {
    probe_query_interface, probe_addref, probe_release,
    probe_unsupported3, probe_unsupported3, probe_unsupported2,
    probe_unsupported3, probe_unsupported4, probe_unsupported3,
    probe_unsupported5, probe_unsupported2, probe_unsupported2
};
static void *source_vtable[] = {
    probe_query_interface, probe_addref, probe_release, probe_cancel, probe_feedback
};
static OBJECT data_object = {data_vtable, 1, 1, 0};
static OBJECT drop_source = {source_vtable, 1, 2, 0};

void dragdrop_probe_entry(void)
{
    DWORD failures = 0;
    void *module = LoadLibraryA("ole32.dll");
    typedef HRESULT (CALL *drop_fn)(void *, void *, DWORD, DWORD *);
    typedef HRESULT (CALL *init_fn)(void *);
    typedef void (CALL *uninit_fn)(void);
    typedef DWORD (CALL *thread_fn)(void);
    typedef int (CALL *post_fn)(DWORD, DWORD, WORDPTR, WORDPTR);
    drop_fn drop = (drop_fn)GetProcAddress(module, "DoDragDrop");
    init_fn initialize = (init_fn)GetProcAddress(module, "OleInitialize");
    uninit_fn uninitialize = (uninit_fn)GetProcAddress(module, "OleUninitialize");
    CHECK(module && drop && initialize && uninitialize, "exports");
    if (failures) ExitProcess(1);
    HRESULT status = initialize(0);
    CHECK(status >= 0, "initialize");
    if (status < 0) ExitProcess(1);
    thread_fn thread_id = (thread_fn)GetProcAddress(LoadLibraryA("kernel32.dll"), "GetCurrentThreadId");
    post_fn post = (post_fn)GetProcAddress(LoadLibraryA("user32.dll"), "PostThreadMessageW");
    CHECK(thread_id && post, "message-api");
    if (failures) ExitProcess(1);
    DWORD tid = thread_id();
    struct { DWORD before, effect, after; } output = {0x13579bdf, 0xcccccccc, 0x2468ace0};
    status = drop(0, &drop_source, 7, &output.effect);
    CHECK(status == (HRESULT)0x80070057 && output.effect == 0xcccccccc, "null-data");
    status = drop(&data_object, 0, 7, &output.effect);
    CHECK(status == (HRESULT)0x80070057 && output.effect == 0xcccccccc, "null-source");
    CHECK(drop(&data_object, &drop_source, 7, 0) == (HRESULT)0x80070057, "null-effect");
    const DWORD masks[] = {0, 1, 2, 4, 7, 0x80000000U, 0x80000003U};
    for (DWORD i = 0; i < sizeof(masks)/sizeof(masks[0]); i++) {
        output.effect = 0xcccccccc;
        /* Wake the native modal loop without injecting input into other apps. */
        CHECK(post(tid, 0x0100, 0x1b, 0), "queued-cancel"); /* WM_KEYDOWN, VK_ESCAPE */
        status = drop(&data_object, &drop_source, masks[i], &output.effect);
        CHECK(status == 0x00040101 && output.effect == 0, "cancel-none");
    }
    WORDPTR before, after;
#ifdef __i386__
    __asm__ volatile ("movl %%esp,%0" : "=r"(before));
#else
    __asm__ volatile ("movq %%rsp,%0" : "=r"(before));
#endif
    for (DWORD i = 0; i < 32; i++) {
        output.effect = 0xffffffff;
        if (!post(tid, 0x0100, 0x1b, 0)) failures++;
        if (drop(&data_object, &drop_source, 3, &output.effect) != 0x00040101 ||
            output.effect) failures++;
    }
#ifdef __i386__
    __asm__ volatile ("movl %%esp,%0" : "=r"(after));
#else
    __asm__ volatile ("movq %%rsp,%0" : "=r"(after));
#endif
    CHECK(before == after, "repeated-call-stack");
    CHECK(output.before == 0x13579bdf && output.after == 0x2468ace0, "effect-width");
    CHECK(data_object.refs == 1 && drop_source.refs == 1, "object-lifetimes");
    uninitialize();
    if (failures) { SAY("DODRAGDROP FAILED\n"); ExitProcess(1); }
    SAY("DODRAGDROP DONE\n");
    ExitProcess(0);
}
