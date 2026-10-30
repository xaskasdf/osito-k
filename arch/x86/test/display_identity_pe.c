/* Read-only Windows/Osito-K display enumeration client; no SDK/runtime code. */
#define native_d3dkmt_entry unused_identity_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry
#undef CHECK
#define CHECK(test,label) do { checks++; if (!(test)) { \
    REPORT("DISPLAY FAIL " label "\n"); failures++; } } while (0)

typedef struct {
    OKMT_U32 cb; char name[32],description[128]; OKMT_U32 flags;
    char id[128],key[128];
} DISPLAY_A;
typedef struct {
    OKMT_U32 cb; OKMT_WCHAR name[32],description[128]; OKMT_U32 flags;
    OKMT_WCHAR id[128],key[128];
} DISPLAY_W;
_Static_assert(sizeof(DISPLAY_A)==424,"DISPLAY_DEVICEA size");
_Static_assert(sizeof(DISPLAY_W)==840,"DISPLAY_DEVICEW size");
_Static_assert(__builtin_offsetof(DISPLAY_A,flags)==164,"DISPLAY_DEVICEA flags");
_Static_assert(__builtin_offsetof(DISPLAY_W,flags)==324,"DISPLAY_DEVICEW flags");

void *memset(void *p,int value,__SIZE_TYPE__ size)
{
    volatile unsigned char *bytes=p;
    while (size--) *bytes++=(unsigned char)value;
    return p;
}

static void text(const char *prefix,const char *value)
{
    char line[256]; unsigned n=0;
    while (*prefix && n<250) line[n++]=*prefix++;
    while (*value && n<254) line[n++]=*value++;
    line[n++]='\n'; OKMT_U32 written;
    WriteFile(GetStdHandle((OKMT_U32)-11),line,n,&written,0);
}
static int equal(const char *a,const char *b)
{ while (*a && *a==*b) { a++; b++; } return *a==*b; }
static int same_wide(const char *a,const OKMT_WCHAR *w,unsigned size)
{
    for (unsigned i=0;i<size;i++) {
        if ((unsigned char)a[i]!=w[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}
static int bounded(const char *p,unsigned size)
{ for (unsigned i=0;i<size;i++) if (!p[i]) return 1; return 0; }
static int bounded_wide(const OKMT_WCHAR *p,unsigned size)
{ for (unsigned i=0;i<size;i++) if (!p[i]) return 1; return 0; }

void display_identity_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *module=LoadLibraryA("user32.dll");
    int (CALL *enumerate_a)(const char *,OKMT_U32,DISPLAY_A *,OKMT_U32)=GetProcAddress(module,"EnumDisplayDevicesA");
    int (CALL *enumerate_w)(const OKMT_WCHAR *,OKMT_U32,DISPLAY_W *,OKMT_U32)=GetProcAddress(module,"EnumDisplayDevicesW");
    if (!enumerate_a || !enumerate_w) { REPORT("DISPLAY UNAVAILABLE\n"); ExitProcess(2); }
    struct { DISPLAY_A info; OKMT_U32 guard[4]; } a={0};
    struct { DISPLAY_W info; OKMT_U32 guard[4]; } w={0};
    a.info.cb=sizeof(a.info); w.info.cb=sizeof(w.info);
    for (unsigned i=0;i<4;i++) a.guard[i]=w.guard[i]=0xcafed00d;
    CHECK(enumerate_a(0,0,&a.info,0)!=0,"ANSI primary enumeration");
    CHECK(enumerate_w(0,0,&w.info,0)!=0,"Unicode primary enumeration");
    CHECK(a.info.cb==424 && w.info.cb==840,"structure size preserved");
    CHECK(a.info.name[0] && a.info.description[0],"adapter strings present");
    CHECK(w.info.name[0] && w.info.description[0],"wide adapter strings present");
    CHECK(bounded(a.info.name,32) && bounded(a.info.description,128) && bounded(a.info.id,128) && bounded(a.info.key,128),"ANSI bounded strings");
    CHECK(bounded_wide(w.info.name,32) && bounded_wide(w.info.description,128) && bounded_wide(w.info.id,128) && bounded_wide(w.info.key,128),"Unicode bounded strings");
    CHECK(same_wide(a.info.name,w.info.name,32) && same_wide(a.info.id,w.info.id,128),"ANSI Unicode identity agrees");
    CHECK(a.info.flags==w.info.flags,"ANSI Unicode state agrees");
    text("DISPLAY name=",a.info.name); text("DISPLAY description=",a.info.description); text("DISPLAY id=",a.info.id);
    VALUE("DISPLAY flags=",a.info.flags);
#ifdef DISPLAY_EXPECT_ID
    CHECK(equal(a.info.name,"\\\\.\\DISPLAY1"),"guest logical primary name");
    CHECK(equal(a.info.id,DISPLAY_EXPECT_ID),"actual primary PCI or unknown");
    CHECK(equal(a.info.description,DISPLAY_EXPECT_DESCRIPTION),"actual primary description");
    CHECK((a.info.flags&5)==5,"guest attached primary");
    if (!DISPLAY_EXPECT_ID[0]) CHECK(a.info.flags==5,"unknown device does not claim VGA");
#endif
    DISPLAY_A other_a={0}; DISPLAY_W other_w={0};
    other_a.cb=sizeof(other_a); other_w.cb=sizeof(other_w);
    CHECK(!enumerate_a(0,0xffffffffu,&other_a,0),"ANSI enumeration end");
    CHECK(!enumerate_w(0,0xffffffffu,&other_w,0),"Unicode enumeration end");
    CHECK(!enumerate_a("OSITO-missing-display",0,&other_a,0),"unknown ANSI parent");
    const OKMT_WCHAR missing[]={'O','S','I','T','O','-','m','i','s','s','i','n','g',0};
    CHECK(!enumerate_w(missing,0,&other_w,0),"unknown Unicode parent");
    for (unsigned i=0;i<4;i++) CHECK(a.guard[i]==0xcafed00d && w.guard[i]==0xcafed00d,"output bounds");
    CHECK(enumerate_a(0,0,&other_a,0)!=0 && equal(other_a.id,a.info.id) && equal(other_a.name,a.info.name),"stable repeated identity");
    VALUE("DISPLAY checks=",checks); VALUE("DISPLAY failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
