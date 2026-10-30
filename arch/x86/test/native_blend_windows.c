/* Original system d3d9.dll, real HAL rendering, offscreen 16x16 surfaces.
 * Only pixel observations are saved. This program is not shipped in the OS. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include "native_blend_cases.h"
int _fltused;
static unsigned failures;
void *memset(void *p,int value,size_t count){unsigned char *q=p;while(count--)*q++=(unsigned char)value;return p;}
static void number(const char *name,unsigned value)
{
    char row[100];unsigned n=0;DWORD written;while(name[n]){row[n]=name[n];n++;}
    for(unsigned i=0;i<8;i++)row[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
    row[n++]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),row,n,&written,NULL);
}
#define CHECK(x) do {if(!(x)){number("BLENDORACLE FAIL line=",__LINE__);ExitProcess(1);}}while(0)
static void state(IDirect3DDevice9 *d,D3DRENDERSTATETYPE kind,unsigned value)
{
    HRESULT result=IDirect3DDevice9_SetRenderState(d,kind,value);
    if(FAILED(result)){number("BLENDORACLE state=",kind);number("BLENDORACLE status=",result);}
    CHECK(SUCCEEDED(result));
}
void blend_windows_entry(void)
{
    SetErrorMode(0x8003);
    HMODULE module=LoadLibraryA("d3d9.dll");CHECK(module);
    IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");CHECK(factory);
    IDirect3D9 *api=factory(D3D_SDK_VERSION);CHECK(api);
    HWND window=CreateWindowExA(0,"STATIC","Osito native blend oracle",WS_POPUP,0,0,16,16,NULL,NULL,NULL,NULL);CHECK(window);
    D3DPRESENT_PARAMETERS pp={0};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=16;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *d=NULL;
    CHECK(SUCCEEDED(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&d)) && d);
    IDirect3DPixelShader9 *shader=NULL;
    CHECK(SUCCEEDED(IDirect3DDevice9_CreatePixelShader(d,(const DWORD *)native_blend_bytecode,&shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShader(d,shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShaderConstantF(d,0,(const float *)native_blend_color_bits,1)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetFVF(d,D3DFVF_XYZRHW)));
    state(d,D3DRS_ZENABLE,0);state(d,D3DRS_CULLMODE,D3DCULL_NONE);state(d,D3DRS_ALPHATESTENABLE,0);
    state(d,D3DRS_FOGENABLE,0);state(d,D3DRS_LIGHTING,0);state(d,D3DRS_DITHERENABLE,0);
    state(d,D3DRS_SRGBWRITEENABLE,0);state(d,D3DRS_BLENDFACTOR,NATIVE_BLEND_FACTOR);
    static const float vertices[4][4]={{-0.5f,-0.5f,0,1},{15.5f,-0.5f,0,1},{-0.5f,15.5f,0,1},{15.5f,15.5f,0,1}};
    for(unsigned format=0;format<2;format++) {
        D3DFORMAT type=format?D3DFMT_X8R8G8B8:D3DFMT_A8R8G8B8;
        IDirect3DSurface9 *target=NULL,*readback=NULL;
        CHECK(SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d,16,16,type,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL)));
        CHECK(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(d,16,16,type,D3DPOOL_SYSTEMMEM,&readback,NULL)));
        CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderTarget(d,0,target)));
        D3DVIEWPORT9 viewport={0,0,16,16,0,1};CHECK(SUCCEEDED(IDirect3DDevice9_SetViewport(d,&viewport)));
        for(unsigned i=0;i<NATIVE_BLEND_CASE_COUNT;i++) {
            const native_blend_case_t *c=&native_blend_cases[i];
            number("BLENDORACLE format=",format);number("BLENDORACLE case=",i);
            state(d,D3DRS_ALPHABLENDENABLE,c->enable);state(d,D3DRS_SRCBLEND,c->src);state(d,D3DRS_DESTBLEND,c->dst);
            state(d,D3DRS_BLENDOP,c->op);state(d,D3DRS_SEPARATEALPHABLENDENABLE,c->separate);
            state(d,D3DRS_SRCBLENDALPHA,c->asrc);state(d,D3DRS_DESTBLENDALPHA,c->adst);
            state(d,D3DRS_BLENDOPALPHA,c->aop);state(d,D3DRS_COLORWRITEENABLE,c->mask);
            CHECK(SUCCEEDED(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,NATIVE_BLEND_DESTINATION,1,0)));
            CHECK(SUCCEEDED(IDirect3DDevice9_BeginScene(d)));
            CHECK(SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,vertices,16)));
            CHECK(SUCCEEDED(IDirect3DDevice9_EndScene(d)));
            CHECK(SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d,target,readback)));
            D3DLOCKED_RECT locked;CHECK(SUCCEEDED(IDirect3DSurface9_LockRect(readback,&locked,NULL,D3DLOCK_READONLY)));
            unsigned pixel=*(const unsigned *)locked.pBits;
            for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)
                CHECK(((const unsigned *)((const BYTE *)locked.pBits+y*locked.Pitch))[x]==pixel);
            number("BLENDORACLE pixel=",pixel);CHECK(SUCCEEDED(IDirect3DSurface9_UnlockRect(readback)));
        }
        IDirect3DSurface9_Release(readback);IDirect3DSurface9_Release(target);
    }
    IDirect3DPixelShader9_Release(shader);IDirect3DDevice9_Release(d);IDirect3D9_Release(api);
    DestroyWindow(window);FreeLibrary(module);number("BLENDORACLE failures=",failures);ExitProcess(0);
}
