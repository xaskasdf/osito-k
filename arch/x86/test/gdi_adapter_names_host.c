#define GDI_HOST_TEST 1
#define main screen_suite_main
#include "gdi_screen_host.c"
#undef main

/* ASan retains the complete export table. These unrelated APIs are forbidden
 * in this GDI metadata test; production KMT is exercised by its separate suite. */
bool win32_resolve_file_path(PCSTR path,char out[260]) { (void)path; (void)out; abort(); }
BOOL opengl32_has_render_backend(void) { abort(); }
int win32_user_range_readable(const void *p,SIZE_T n,BOOL compat) { (void)p; (void)n; (void)compat; abort(); }
int win32_user_range_writable(void *p,SIZE_T n,BOOL compat) { (void)p; (void)n; (void)compat; abort(); }
#define UNREACHED_KMT(name,type) NTSTATUS WINAPI name(type p) { (void)p; abort(); }
UNREACHED_KMT(D3DKMTEnumAdapters,PVOID)
UNREACHED_KMT(D3DKMTEnumAdapters2,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromLuid,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromDeviceName,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromGdiDisplayName,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromHdc,PVOID)
UNREACHED_KMT(D3DKMTCloseAdapter,PCVOID)
UNREACHED_KMT(D3DKMTQueryAdapterInfo,PCVOID)
UNREACHED_KMT(D3DKMTCreateDevice,PVOID)
UNREACHED_KMT(D3DKMTDestroyDevice,PCVOID)
UNREACHED_KMT(D3DKMTCreateAllocation,PVOID)
UNREACHED_KMT(D3DKMTCreateAllocation2,PVOID)
UNREACHED_KMT(D3DKMTDestroyAllocation,PCVOID)
UNREACHED_KMT(D3DKMTDestroyAllocation2,PCVOID)
UNREACHED_KMT(D3DKMTLock,PVOID)
UNREACHED_KMT(D3DKMTUnlock,PCVOID)
UNREACHED_KMT(D3DKMTCreateContext,PVOID)
UNREACHED_KMT(D3DKMTDestroyContext,PCVOID)
UNREACHED_KMT(D3DKMTRender,PVOID)
#undef UNREACHED_KMT

int main(void)
{
    if (screen_suite_main()) return 1;
    WCHAR name[32];
    const WCHAR expected[]=u"\\\\.\\DISPLAY1";
    HDC screen=CreateDCA("DISPLAY",NULL,NULL,NULL);
    HDC memory=CreateCompatibleDC(screen);
    CHECK(screen && memory);
    for (unsigned i=0;i<32;i++) name[i]=0x5a5a;
    CHECK(gdi32_dc_display_name(screen,name));
    CHECK(!memcmp(name,expected,sizeof(expected)) && name[sizeof(expected)/2]==0x5a5a);
    CHECK(gdi32_dc_display_name(memory,name) && !memcmp(name,expected,sizeof(expected)));
    CHECK(!gdi32_dc_display_name(NULL,name));
    CHECK(!gdi32_dc_display_name((HDC)(ULONG_PTR)1,name));
    CHECK(!gdi32_dc_display_name(screen,NULL));
    gdi_dcs[DC_INDEX(screen)].owner_pid=2;
    CHECK(!gdi32_dc_display_name(screen,name));
    gdi_dcs[DC_INDEX(screen)].owner_pid=1;
    DeleteDC(screen); DeleteDC(memory);
    CHECK(!gdi32_dc_display_name(screen,name));
    CHECK(!gdi32_dc_display_name(memory,name));
    printf("GDI adapter names: checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
}
