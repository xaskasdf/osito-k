/* Read-only Windows ABI/behavior reference; no runtime binary is copied. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stddef.h>
static unsigned checks,failures,sources;
void *memset(void *data,int value,size_t size)
{
    volatile BYTE *p=data;while (size--) *p++=(BYTE)value;return data;
}
static void number(const char *label,UINT value)
{
    char text[128];UINT n=0;DWORD written;
    while (label[n]) { text[n]=label[n];n++; }
    for (UINT i=0;i<8;i++) text[n++]="0123456789abcdef"[(value>>(28-4*i))&15];
    text[n++]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,NULL);
}
#define CHECK(x) do { checks++;if (!(x)) { failures++;number("TIMING FAIL line=",__LINE__); } } while (0)
#define GET(name,type) type name=(type)GetProcAddress(gdi,#name);if (!name) ExitProcess(2)
static NTSTATUS scan(PFND3DKMT_GETSCANLINE get,D3DKMT_HANDLE adapter,UINT source,UINT variant)
{
    struct { UINT before;D3DKMT_GETSCANLINE data;UINT after; } guard;
    memset(&guard,0x5a,sizeof(guard));
    guard.data.hAdapter=adapter;guard.data.VidPnSourceId=source;
    NTSTATUS status=get(&guard.data);
    CHECK(guard.before==0x5a5a5a5a && guard.after==0x5a5a5a5a);
    CHECK(guard.data.hAdapter==adapter && guard.data.VidPnSourceId==source);
    number("TIMING case=",variant);number("TIMING status=",status);
    number("TIMING blank=",guard.data.InVerticalBlank);number("TIMING line=",guard.data.ScanLine);
    const BYTE *bytes=(const BYTE *)&guard.data;
    number("TIMING padding=",bytes[9]|bytes[10]<<8|bytes[11]<<16);
    if (!status) CHECK(guard.data.InVerticalBlank<=1);
    return status;
}
void timing_windows_entry(void)
{
    SetErrorMode(0x8003);HMODULE gdi=LoadLibraryA("gdi32.dll");CHECK(gdi);
    GET(D3DKMTEnumAdapters2,PFND3DKMT_ENUMADAPTERS2);
    GET(D3DKMTGetScanLine,PFND3DKMT_GETSCANLINE);
    GET(D3DKMTWaitForVerticalBlankEvent,PFND3DKMT_WAITFORVERTICALBLANKEVENT);
    GET(D3DKMTCloseAdapter,PFND3DKMT_CLOSEADAPTER);
    number("TIMING pointer-bytes=",sizeof(void *));
    number("TIMING scan-bytes=",sizeof(D3DKMT_GETSCANLINE));
    number("TIMING blank-offset=",offsetof(D3DKMT_GETSCANLINE,InVerticalBlank));
    number("TIMING line-offset=",offsetof(D3DKMT_GETSCANLINE,ScanLine));
    number("TIMING wait-bytes=",sizeof(D3DKMT_WAITFORVERTICALBLANKEVENT));
    number("TIMING null-scan=",D3DKMTGetScanLine(NULL));
    number("TIMING null-wait=",D3DKMTWaitForVerticalBlankEvent(NULL));
    scan(D3DKMTGetScanLine,0,0,0x100);
    D3DKMT_WAITFORVERTICALBLANKEVENT invalid={0,0,0};
    number("TIMING invalid-wait=",D3DKMTWaitForVerticalBlankEvent(&invalid));
    D3DKMT_ADAPTERINFO adapters[16];D3DKMT_ENUMADAPTERS2 list={16,adapters};
    NTSTATUS status=D3DKMTEnumAdapters2(&list);CHECK(!status && list.NumAdapters<=16);
    if (status || list.NumAdapters>16) ExitProcess(2);
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));
    number("TIMING qpc-hz-low=",frequency.LowPart);number("TIMING qpc-hz-high=",frequency.HighPart);
    for (UINT i=0;i<list.NumAdapters;i++) {
        number("TIMING adapter=",i);number("TIMING sources=",adapters[i].NumOfSources);
        CHECK(adapters[i].NumOfSources<=16);
        for (UINT source=0;source<adapters[i].NumOfSources && source<16;source++) {
            number("TIMING source=",source);
            if (scan(D3DKMTGetScanLine,adapters[i].hAdapter,source,0)) continue;
            sources++;
            for (UINT j=0;j<8;j++) {
                D3DKMT_WAITFORVERTICALBLANKEVENT wait={adapters[i].hAdapter,0,source};
                LARGE_INTEGER before,after;QueryPerformanceCounter(&before);
                status=D3DKMTWaitForVerticalBlankEvent(&wait);QueryPerformanceCounter(&after);
                number("TIMING wait-status=",status);
                number("TIMING elapsed-ticks=",(UINT)(after.QuadPart-before.QuadPart));
                scan(D3DKMTGetScanLine,adapters[i].hAdapter,source,j+1);
                if (status) break;
            }
        }
        scan(D3DKMTGetScanLine,adapters[i].hAdapter,0xffffffffu,0x101);
        invalid=(D3DKMT_WAITFORVERTICALBLANKEVENT){adapters[i].hAdapter,0,0xffffffffu};
        number("TIMING invalid-source-wait=",D3DKMTWaitForVerticalBlankEvent(&invalid));
        D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};CHECK(!D3DKMTCloseAdapter(&close));
        scan(D3DKMTGetScanLine,close.hAdapter,0,0x102);
        invalid=(D3DKMT_WAITFORVERTICALBLANKEVENT){close.hAdapter,0,0};
        number("TIMING stale-wait=",D3DKMTWaitForVerticalBlankEvent(&invalid));
    }
    CHECK(FreeLibrary(gdi));number("TIMING successful-sources=",sources);
    number("TIMING checks=",checks);number("TIMING failures=",failures);ExitProcess(failures?1:0);
}
