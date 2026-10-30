/* Original Windows runtime + HAL oracle for sampler mip/sRGB semantics.
 * This probe and Microsoft's DLLs are not part of the OS distribution. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include "sampled-program.h"
int _fltused;
void *memset(void *p,int v,size_t n){BYTE *b=p;while(n--)*b++=(BYTE)v;return p;}
static void number(const char *s,unsigned v)
{
    DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,NULL);
    char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(v>>(28-i*4))&15];
    text[8]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,9,&w,NULL);
}
#define CHECK(x) do {if(!(x)){number("SAMPLERORACLE FAIL line=",__LINE__);ExitProcess(1);}}while(0)
void sampler_windows_entry(void)
{
    SetErrorMode(0x8003);
    HMODULE module=LoadLibraryA("d3d9.dll");CHECK(module);
    IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");CHECK(factory);
    IDirect3D9 *api=factory(D3D_SDK_VERSION);CHECK(api);
    HWND window=CreateWindowExA(0,"STATIC","Osito sampler oracle",WS_POPUP,0,0,16,16,NULL,NULL,NULL,NULL);CHECK(window);
    D3DPRESENT_PARAMETERS pp={0};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=16;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *d=NULL;
    CHECK(SUCCEEDED(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&d)) && d);
    IDirect3DPixelShader9 *shader=NULL;
    CHECK(SUCCEEDED(IDirect3DDevice9_CreatePixelShader(d,(const DWORD *)sampled_program+32,&shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShader(d,shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetFVF(d,D3DFVF_XYZRHW|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE4(0))));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,0)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE)));
    IDirect3DTexture9 *textures[2];
    for(unsigned t=0;t<2;t++) {
        CHECK(SUCCEEDED(IDirect3DDevice9_CreateTexture(d,16,16,2,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&textures[t],NULL)));
        for(unsigned level=0;level<2;level++) {
            D3DLOCKED_RECT lock;CHECK(SUCCEEDED(IDirect3DTexture9_LockRect(textures[t],level,&lock,NULL,0)));
            for(unsigned y=0;y<(16u>>level);y++)for(unsigned x=0;x<(16u>>level);x++)
                ((unsigned *)((BYTE *)lock.pBits+y*lock.Pitch))[x]=0xff000000u|(level ? 0x030303 : t ? 0x020202 : 0x010101);
            CHECK(SUCCEEDED(IDirect3DTexture9_UnlockRect(textures[t],level)));
        }
    }
    IDirect3DSurface9 *target,*readback;
    CHECK(SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d,16,16,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL)));
    CHECK(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(d,16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderTarget(d,0,target)));
    const float vertices[4][8]={{-.5f,-.5f,0,1,.5f,.5f,0,1},{15.5f,-.5f,0,1,.5f,.5f,0,1},
        {-.5f,15.5f,0,1,.5f,.5f,0,1},{15.5f,15.5f,0,1,.5f,.5f,0,1}};
    for(unsigned round=0;round<8;round++) {
        for(unsigned i=0;i<16;i++) {
            CHECK(SUCCEEDED(IDirect3DDevice9_SetTexture(d,i,(IDirect3DBaseTexture9 *)textures[round==2 || (round==1 && i==15)])));
            CHECK(SUCCEEDED(IDirect3DDevice9_SetSamplerState(d,i,D3DSAMP_MAXMIPLEVEL,round>=3 && round<=5)));
            CHECK(SUCCEEDED(IDirect3DDevice9_SetSamplerState(d,i,D3DSAMP_MIPFILTER,round==4 ? 1 : round==5 ? 2 : 0)));
            CHECK(SUCCEEDED(IDirect3DDevice9_SetSamplerState(d,i,D3DSAMP_SRGBTEXTURE,round==6 ? 7 : round==7 ? 1 : 0)));
        }
        CHECK(SUCCEEDED(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0,1,0)));
        CHECK(SUCCEEDED(IDirect3DDevice9_BeginScene(d)));
        CHECK(SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,vertices,32)));
        CHECK(SUCCEEDED(IDirect3DDevice9_EndScene(d)));
        CHECK(SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d,target,readback)));
        D3DLOCKED_RECT lock;CHECK(SUCCEEDED(IDirect3DSurface9_LockRect(readback,&lock,NULL,D3DLOCK_READONLY)));
        unsigned pixel=*(const unsigned *)lock.pBits;
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)CHECK(((unsigned *)((BYTE *)lock.pBits+y*lock.Pitch))[x]==pixel);
        number("SAMPLERORACLE case=",round);number("SAMPLERORACLE pixel=",pixel);
        CHECK(SUCCEEDED(IDirect3DSurface9_UnlockRect(readback)));
    }
    IDirect3DSurface9_Release(readback);IDirect3DSurface9_Release(target);
    for(unsigned i=0;i<16;i++)CHECK(SUCCEEDED(IDirect3DDevice9_SetTexture(d,i,NULL)));
    for(unsigned i=0;i<2;i++)IDirect3DTexture9_Release(textures[i]);
    IDirect3DPixelShader9_Release(shader);IDirect3DDevice9_Release(d);IDirect3D9_Release(api);
    DestroyWindow(window);FreeLibrary(module);number("SAMPLERORACLE failures=",0);ExitProcess(0);
}
