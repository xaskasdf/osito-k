/* Read-only Windows oracle for the installed runtime's KMT mode query.
 * Uses local SDK declarations; never changes a display mode or publishes
 * capabilities for Osito-K. No Microsoft binary is included in this fixture. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stddef.h>

#define MODE_LIMIT 65536u
static unsigned checks,failures,successful_sources;
static struct {
    UINT prefix[4];
    D3DKMT_DISPLAYMODE modes[MODE_LIMIT+1];
    UINT suffix[4];
} guarded;

void *memset(void *data,int value,size_t bytes)
{
    volatile BYTE *p=data;
    while (bytes--) *p++=(BYTE)value;
    return data;
}
static void number(const char *label,UINT value)
{
    char text[128]; UINT n=0; DWORD written;
    while (label[n]) { text[n]=label[n]; n++; }
    for (UINT i=0;i<8;i++) text[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
    text[n++]='\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,NULL);
}
#define CHECK(value) do { checks++; if (!(value)) { failures++; number("MODES FAIL line=",__LINE__); } } while (0)
#define GET(name,type) type name=(type)GetProcAddress(gdi,#name); if (!name) { number("MODES missing line=",__LINE__); ExitProcess(2); }

static BOOL untouched(const void *data,SIZE_T bytes)
{
    const BYTE *p=data;
    while (bytes--) if (*p++!=0x5a) return FALSE;
    return TRUE;
}

/* Log uncertain error/copyout behavior instead of encoding an assumed result.
 * Guard and input preservation remain checked independently of NTSTATUS. */
static NTSTATUS query(PFND3DKMT_GETDISPLAYMODELIST get,D3DKMT_HANDLE adapter,
    UINT source,UINT variant,UINT capacity,BOOL output,UINT *count)
{
    memset(&guarded,0x5a,sizeof(guarded));
    D3DKMT_GETDISPLAYMODELIST data;
    memset(&data,0x5a,sizeof(data));
    data.hAdapter=adapter; data.VidPnSourceId=source;
    data.pModeList=output ? guarded.modes : NULL; data.ModeCount=capacity;
    NTSTATUS status=get(&data);
    number("MODES case=",variant); number("MODES status=",(UINT)status);
    number("MODES capacity=",capacity); number("MODES count=",data.ModeCount);
    CHECK(data.hAdapter==adapter && data.VidPnSourceId==source);
    CHECK(data.pModeList==(output ? guarded.modes : NULL));
    CHECK(untouched(guarded.prefix,sizeof(guarded.prefix)));
    CHECK(untouched(guarded.suffix,sizeof(guarded.suffix)));
    CHECK(capacity<=MODE_LIMIT+1);
    if (capacity<=MODE_LIMIT+1) {
        CHECK(untouched(guarded.modes+capacity,
            (MODE_LIMIT+1-capacity)*sizeof(guarded.modes[0])));
    }
    UINT changed=0,checksum=2166136261u;
    for (SIZE_T i=0;i<sizeof(guarded.modes);i++)
        changed+=((BYTE *)guarded.modes)[i]!=0x5a;
    number("MODES changed-bytes=",changed);
    if (status>=0 && output && data.ModeCount<=capacity) {
        for (SIZE_T i=0;i<data.ModeCount*sizeof(guarded.modes[0]);i++)
            checksum=(checksum^((BYTE *)guarded.modes)[i])*16777619u;
        number("MODES list-checksum=",checksum);
        for (UINT i=0;i<data.ModeCount;i++) {
            const D3DKMT_DISPLAYMODE *mode=guarded.modes+i;
            CHECK(mode->Width && mode->Height && mode->RefreshRate.Denominator);
            if (i>=8 && i+1<data.ModeCount) continue;
            number("MODES item=",i);
            number("MODES width=",mode->Width); number("MODES height=",mode->Height);
            number("MODES format=",mode->Format); number("MODES hz=",mode->IntegerRefreshRate);
            number("MODES numerator=",mode->RefreshRate.Numerator);
            number("MODES denominator=",mode->RefreshRate.Denominator);
            number("MODES scan=",mode->ScanLineOrdering);
            number("MODES orientation=",mode->DisplayOrientation);
            number("MODES fixed-output=",mode->DisplayFixedOutput);
            const BYTE *flag_bytes=(const BYTE *)&mode->Flags;
            for (UINT word=0;word<sizeof(mode->Flags)/4;word++) {
                UINT value=0;
                for (UINT byte=0;byte<4;byte++)
                    value|=(UINT)flag_bytes[word*4+byte]<<(byte*8);
                number("MODES flags-word=",word); number("MODES flags=",value);
            }
        }
        CHECK(untouched(guarded.modes+data.ModeCount,
            (MODE_LIMIT+1-data.ModeCount)*sizeof(guarded.modes[0])));
    }
    *count=data.ModeCount;
    return status;
}

void display_modes_windows_entry(void)
{
    SetErrorMode(0x8003);
    HMODULE gdi=LoadLibraryA("gdi32.dll"); CHECK(gdi);
    GET(D3DKMTEnumAdapters2,PFND3DKMT_ENUMADAPTERS2);
    GET(D3DKMTGetDisplayModeList,PFND3DKMT_GETDISPLAYMODELIST);
    GET(D3DKMTCloseAdapter,PFND3DKMT_CLOSEADAPTER);
    number("MODES pointer-bytes=",sizeof(void *));
    number("MODES request-bytes=",sizeof(D3DKMT_GETDISPLAYMODELIST));
    number("MODES pointer-offset=",offsetof(D3DKMT_GETDISPLAYMODELIST,pModeList));
    number("MODES count-offset=",offsetof(D3DKMT_GETDISPLAYMODELIST,ModeCount));
    number("MODES entry-bytes=",sizeof(D3DKMT_DISPLAYMODE));
    D3DKMT_ADAPTERINFO adapters[16]; memset(adapters,0,sizeof(adapters));
    D3DKMT_ENUMADAPTERS2 list={16,adapters};
    NTSTATUS status=D3DKMTEnumAdapters2(&list);
    CHECK(status==0 && list.NumAdapters && list.NumAdapters<=16);
    if (status<0 || list.NumAdapters>16) ExitProcess(2);
    UINT ignored;
    query(D3DKMTGetDisplayModeList,0,0,0x100,0,FALSE,&ignored);
    for (UINT index=0;index<list.NumAdapters;index++) {
        number("MODES adapter=",index);
        number("MODES luid-low=",adapters[index].AdapterLuid.LowPart);
        number("MODES luid-high=",adapters[index].AdapterLuid.HighPart);
        number("MODES sources=",adapters[index].NumOfSources);
        UINT sources=adapters[index].NumOfSources;
        CHECK(sources<=16);
        for (UINT source=0;source<sources && source<16;source++) {
            number("MODES source=",source);
            UINT count=0;
            status=query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,0,0,FALSE,&count);
            if (status<0) continue;
            CHECK(count>0 && count<=MODE_LIMIT);
            if (!count || count>MODE_LIMIT) continue;
            query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,1,1,FALSE,&ignored);
            query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,2,0,TRUE,&ignored);
            query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,3,count-1,TRUE,&ignored);
            status=query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,4,count,TRUE,&ignored);
            CHECK(status==0 && ignored==count);
            if (!status && ignored==count) successful_sources++;
            status=query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,source,5,count+1,TRUE,&ignored);
            CHECK(status==0 && ignored==count);
        }
        query(D3DKMTGetDisplayModeList,adapters[index].hAdapter,0xffffffffu,0x101,0,FALSE,&ignored);
        D3DKMT_CLOSEADAPTER close={adapters[index].hAdapter};
        CHECK(D3DKMTCloseAdapter(&close)==0);
        query(D3DKMTGetDisplayModeList,close.hAdapter,0,0x102,0,FALSE,&ignored);
    }
    CHECK(successful_sources);
    CHECK(FreeLibrary(gdi));
    number("MODES successful-sources=",successful_sources);
    number("MODES checks=",checks); number("MODES failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
