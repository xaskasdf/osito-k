/* Native/OsitoK ownership and ABI control for ReleaseStgMedium. */
typedef unsigned int DWORD;
typedef int HRESULT;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ WORDPTR;
void *memcpy(void *destination, const void *source, WORDPTR size)
{
    unsigned char *out=(unsigned char *)destination;
    const unsigned char *in=(const unsigned char *)source;
    for (WORDPTR i=0;i<size;i++) out[i]=in[i];
    return destination;
}
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
#define SAY(text) do { DWORD n; WriteFile(GetStdHandle((DWORD)-11),text,sizeof(text)-1,&n,0); } while (0)
#define CHECK(test,label) do { if (test) SAY("STGMEDIUM PASS " label "\n"); \
    else { SAY("STGMEDIUM FAIL " label "\n"); failures++; } } while (0)
typedef struct { DWORD type; void *resource; void *owner; } MEDIUM;
_Static_assert(sizeof(MEDIUM)==(sizeof(void *)==4 ? 12 : 24),"STGMEDIUM layout");
typedef struct { void **vtable; DWORD refs, releases, role; } OBJECT;
static DWORD sequence;
static HRESULT CALL stg_query(OBJECT *self, const void *iid, void **out)
{
    (void)self; (void)iid;
    if (out) *out=0;
    return (HRESULT)0x80004002;
}
static DWORD CALL stg_addref(OBJECT *self) { return ++self->refs; }
static DWORD CALL stg_release(OBJECT *self)
{
    self->releases++;
    sequence=sequence*10+self->role;
    return --self->refs;
}
static void *vtable[14]={stg_query,stg_addref,stg_release};
static OBJECT resource_object={vtable,1,0,1};
static OBJECT owner_object={vtable,1,0,2};
#define RESET() do { resource_object.refs=owner_object.refs=1; \
    resource_object.releases=owner_object.releases=0; sequence=0; } while (0)

void stgmedium_probe_entry(void)
{
    DWORD failures=0;
    void *ole=LoadLibraryA("ole32.dll"),*kernel=LoadLibraryA("kernel32.dll"),*gdi=LoadLibraryA("gdi32.dll");
    typedef void (CALL *release_fn)(MEDIUM *);
    typedef void *(CALL *allocate_fn)(DWORD,WORDPTR);
    typedef void *(CALL *pointer_fn)(void *);
    typedef int (CALL *unlock_fn)(void *);
    typedef void *(CALL *heap_fn)(void);
    typedef int (CALL *validate_fn)(void *,DWORD,const void *);
    typedef void *(CALL *task_alloc_fn)(WORDPTR);
    typedef void *(CALL *file_fn)(const WCHAR *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *close_fn)(void *);
    typedef int (CALL *delete_file_fn)(const WCHAR *);
    typedef DWORD (CALL *attrs_fn)(const WCHAR *);
    typedef void *(CALL *bitmap_fn)(int,int,DWORD,DWORD,const void *);
    typedef DWORD (CALL *object_type_fn)(void *);
    typedef int (CALL *flush_fn)(void);
    release_fn release=(release_fn)GetProcAddress(ole,"ReleaseStgMedium");
    CHECK(release,"export");
    if (!release) ExitProcess(1);
    allocate_fn allocate=(allocate_fn)GetProcAddress(kernel,"GlobalAlloc");
    pointer_fn lock=(pointer_fn)GetProcAddress(kernel,"GlobalLock");
    pointer_fn free_global=(pointer_fn)GetProcAddress(kernel,"GlobalFree");
    unlock_fn unlock=(unlock_fn)GetProcAddress(kernel,"GlobalUnlock");
    heap_fn heap=(heap_fn)GetProcAddress(kernel,"GetProcessHeap");
    validate_fn validate=(validate_fn)GetProcAddress(kernel,"HeapValidate");
    task_alloc_fn task_allocate=(task_alloc_fn)GetProcAddress(ole,"CoTaskMemAlloc");
    file_fn create_file=(file_fn)GetProcAddress(kernel,"CreateFileW");
    close_fn close=(close_fn)GetProcAddress(kernel,"CloseHandle");
    delete_file_fn delete_file=(delete_file_fn)GetProcAddress(kernel,"DeleteFileW");
    attrs_fn attributes=(attrs_fn)GetProcAddress(kernel,"GetFileAttributesW");
    bitmap_fn bitmap=(bitmap_fn)GetProcAddress(gdi,"CreateBitmap");
    object_type_fn object_type=(object_type_fn)GetProcAddress(gdi,"GetObjectType");
    close_fn delete_object=(close_fn)GetProcAddress(gdi,"DeleteObject");
    flush_fn flush=(flush_fn)GetProcAddress(gdi,"GdiFlush");
    CHECK(allocate && lock && free_global && unlock && heap && validate && task_allocate &&
          create_file && close && delete_file && attributes && bitmap && object_type && delete_object && flush,"support-exports");
    if (failures) ExitProcess(1);
    struct { WORDPTR before; MEDIUM medium; WORDPTR after; } checked;
    checked.before=(WORDPTR)0x13579bdf; checked.after=(WORDPTR)0x2468ace0;
    MEDIUM *m=&checked.medium;
    *m=(MEDIUM){0,(void *)(WORDPTR)0x12345678,0};
    release(m);
    CHECK(m->type==0 && !m->owner && m->resource==(void *)(WORDPTR)0x12345678,"null-medium-type");
    const DWORD types[]={0,1,2,4,8,16,32,64};
    for (DWORD i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        RESET();*m=(MEDIUM){types[i],0,&owner_object};release(m);
        CHECK(owner_object.releases==1 && owner_object.refs==0 && sequence==2 &&
              m->type==0 && !m->owner,"empty-resource-owner");
        release(m);
        CHECK(owner_object.releases==1,"second-release-inert");
    }
    for (DWORD type=4;type<=8;type*=2) {
        RESET();*m=(MEDIUM){type,&resource_object,0};release(m);
        CHECK(resource_object.releases==1 && resource_object.refs==0 && sequence==1,"interface-reference");
        RESET();*m=(MEDIUM){type,&resource_object,&owner_object};release(m);
        CHECK(resource_object.releases==1 && owner_object.releases==1 && sequence==12,"interface-before-owner");
        RESET();resource_object.refs=2;*m=(MEDIUM){type,&resource_object,&resource_object};release(m);
        CHECK(resource_object.releases==2 && resource_object.refs==0 && sequence==11,"aliased-interface-owner");
    }
    void *memory=allocate(0,256);
    CHECK(memory && validate(heap(),0,memory),"global-fixed-allocation");
    if (!memory) ExitProcess(1);
    *m=(MEDIUM){1,memory,0};release(m);
    CHECK(!validate(heap(),0,memory) && m->type==0,"global-fixed-freed");
    for (DWORD flags=0;flags<=2;flags+=2) {
        memory=allocate(flags,256);unsigned char *bytes=(unsigned char *)lock(memory);
        CHECK(memory && bytes,"global-owned-allocation");
        if (!memory || !bytes) ExitProcess(1);
        bytes[0]=0xa5;bytes[255]=0x5a;unlock(memory);
        RESET();*m=(MEDIUM){1,memory,&owner_object};release(m);
        bytes=(unsigned char *)lock(memory);
        CHECK(owner_object.releases==1 && bytes && bytes[0]==0xa5 && bytes[255]==0x5a,"global-owner-retains-data");
        unlock(memory);CHECK(!free_global(memory),"global-owner-cleanup");
    }
    for (DWORD delegated=0;delegated<2;delegated++) {
        void *handle=bitmap(2,2,1,32,0);
        CHECK(handle && object_type(handle)==7,"bitmap-allocation");
        if (!handle) ExitProcess(1);
        RESET();*m=(MEDIUM){16,handle,delegated ? &owner_object : 0};release(m);
        flush(); /* Native GDI may batch deletion until a flush boundary. */
        CHECK((!delegated || object_type(handle)==7U) && m->type==0 &&
              owner_object.releases==delegated,"bitmap-ownership");
        if (delegated) CHECK(delete_object(handle),"bitmap-owner-cleanup");
    }
    /* Native GDI can still answer queries about a deleted bitmap. Verify
     * reclamation by recycling more objects than the OsitoK bitmap pool. */
    DWORD recycled=0;
    for (;recycled<1024;recycled++) {
        void *handle=bitmap(2,2,1,32,0);
        if (!handle) break;
        *m=(MEDIUM){16,handle,0};release(m);
    }
    flush();CHECK(recycled==1024,"bitmap-recycling");
    /* No metafile objects exist on OsitoK yet. Delegation must not inspect or
     * free their opaque handles; direct metafile destruction is not claimed. */
    for (DWORD type=32;type<=64;type*=2) {
        RESET();*m=(MEDIUM){type,(void *)(WORDPTR)0x23456789,&owner_object};release(m);
        CHECK(owner_object.releases==1 && m->type==0 && !m->owner,"metafile-delegated");
    }
    const WCHAR filename[]={'s','t','g','m','e','d','i','u','m','-',
#ifdef __i386__
        '3','2',
#else
        '6','4',
#endif
        '.','t','m','p',0};
    CHECK(attributes(filename)==0xffffffffU,"temporary-file-absent");
    if (failures) ExitProcess(1);
    for (DWORD delegated=0;delegated<2;delegated++) {
        void *file=create_file(filename,0x40000000U,0,0,1,0x80,0); /* CREATE_NEW */
        CHECK(file!=(void *)(WORDPTR)-1 && file,"temporary-file-create");
        if (!file || file==(void *)(WORDPTR)-1) ExitProcess(1);
        CHECK(close(file),"temporary-file-close");
        WCHAR *name=(WCHAR *)task_allocate(sizeof(filename));
        CHECK(name,"filename-allocation");
        if (!name) { delete_file(filename); ExitProcess(1); }
        for (DWORD i=0;i<sizeof(filename)/sizeof(filename[0]);i++) name[i]=filename[i];
        RESET();*m=(MEDIUM){2,name,delegated ? &owner_object : 0};release(m);
        CHECK((attributes(filename)!=0xffffffffU)==delegated && owner_object.releases==delegated,"file-ownership");
        if (delegated) CHECK(delete_file(filename),"file-owner-cleanup");
    }
    WORDPTR before,after;
#ifdef __i386__
    __asm__ volatile("movl %%esp,%0":"=r"(before));
#else
    __asm__ volatile("movq %%rsp,%0":"=r"(before));
#endif
    for (DWORD i=0;i<64;i++) {*m=(MEDIUM){0,0,0};release(m);}
#ifdef __i386__
    __asm__ volatile("movl %%esp,%0":"=r"(after));
#else
    __asm__ volatile("movq %%rsp,%0":"=r"(after));
#endif
    CHECK(before==after,"repeated-call-stack");
    CHECK(checked.before==(WORDPTR)0x13579bdf && checked.after==(WORDPTR)0x2468ace0,"layout-canaries");
    if (failures) { SAY("STGMEDIUM FAILED\n");ExitProcess(1); }
    SAY("STGMEDIUM DONE\n");ExitProcess(0);
}
