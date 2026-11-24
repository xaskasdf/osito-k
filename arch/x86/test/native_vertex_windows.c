/* Native Windows D3D9 HAL observations; never shipped as an OS dependency. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include "native_vertex_cases.h"
int _fltused;
void *memset(void *p,int v,size_t n){BYTE *b=p;while(n--)*b++=(BYTE)v;return p;}
void *memcpy(void *d,const void *s,size_t n){BYTE *a=d;const BYTE *b=s;while(n--)*a++=*b++;return d;}
static void number(const char *s,unsigned v)
{
    DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,NULL);
    char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(v>>(28-i*4))&15];
    text[8]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,9,&w,NULL);
}
#define CHECK(x) do {if(!(x)){number("VERTEXORACLE FAIL line=",__LINE__);ExitProcess(1);}}while(0)
void vertex_windows_entry(void)
{
    SetErrorMode(0x8003);
    HMODULE module=LoadLibraryA("d3d9.dll");CHECK(module);
    IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");CHECK(factory);
    IDirect3D9 *api=factory(D3D_SDK_VERSION);CHECK(api);
    HWND window=CreateWindowExA(0,"STATIC","Osito vertex oracle",WS_POPUP,0,0,16,16,NULL,NULL,NULL,NULL);CHECK(window);
    D3DPRESENT_PARAMETERS pp={0};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=16;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *d=NULL;
    CHECK(SUCCEEDED(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&d)) && d);
    IDirect3DPixelShader9 *shader=NULL;
    CHECK(SUCCEEDED(IDirect3DDevice9_CreatePixelShader(d,(const DWORD *)native_vertex_shader,&shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShader(d,shader)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,0)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE)));
    IDirect3DSurface9 *target,*readback;
    CHECK(SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d,16,16,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL)));
    CHECK(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(d,16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL)));
    CHECK(SUCCEEDED(IDirect3DDevice9_SetRenderTarget(d,0,target)));
    const float position[4][4]={{-.5f,-.5f,0,1},{15.5f,-.5f,0,1},{-.5f,15.5f,0,1},{15.5f,15.5f,0,1}};
    for(unsigned type=0;type<NATIVE_VERTEX_CASE_COUNT;type++)for(unsigned layout=0;layout<2;layout++) {
        const native_vertex_case_t *c=&native_vertex_cases[type];
        unsigned coord=layout ? 0 : 16,pos=layout ? c->bytes : 0,stride=16+c->bytes;
        number("VERTEXORACLE type=",type);number("VERTEXORACLE layout=",layout);
        D3DVERTEXELEMENT9 elements[3]={{0,(WORD)pos,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0},
            {0,(WORD)coord,(BYTE)type,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
        if(layout){D3DVERTEXELEMENT9 t=elements[0];elements[0]=elements[1];elements[1]=t;}
        IDirect3DVertexDeclaration9 *decl=NULL;
        HRESULT status=IDirect3DDevice9_CreateVertexDeclaration(d,elements,&decl);
        number("VERTEXORACLE declaration=",status);
        unsigned reference=FAILED(status);
        if(reference) {
            coord=layout ? 0 : 16;pos=layout ? 16 : 0;stride=32;
            elements[layout ? 1 : 0].Offset=(WORD)pos;
            elements[layout ? 0 : 1].Offset=(WORD)coord;
            elements[layout ? 0 : 1].Type=D3DDECLTYPE_FLOAT4;
            CHECK(SUCCEEDED(IDirect3DDevice9_CreateVertexDeclaration(d,elements,&decl)));
        }
        CHECK(decl);number("VERTEXORACLE reference_only=",reference);
        CHECK(SUCCEEDED(IDirect3DDevice9_SetVertexDeclaration(d,decl)));
        CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShaderConstantF(d,0,c->scale,1)));
        CHECK(SUCCEEDED(IDirect3DDevice9_SetPixelShaderConstantF(d,1,c->bias,1)));
        BYTE vertices[128];memset(vertices,0xa5,sizeof(vertices));
        for(unsigned i=0;i<4;i++) {
            memcpy(vertices+i*stride+pos,position[i],16);
            memcpy(vertices+i*stride+coord,reference ? (const void *)native_vertex_values[type] : c->data,reference ? 16 : c->bytes);
        }
        CHECK(SUCCEEDED(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0,1,0)));
        CHECK(SUCCEEDED(IDirect3DDevice9_BeginScene(d)));
        CHECK(SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,vertices,stride)));
        CHECK(SUCCEEDED(IDirect3DDevice9_EndScene(d)));
        CHECK(SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d,target,readback)));
        D3DLOCKED_RECT lock;CHECK(SUCCEEDED(IDirect3DSurface9_LockRect(readback,&lock,NULL,D3DLOCK_READONLY)));
        unsigned pixel=*(const unsigned *)lock.pBits;
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)CHECK(((unsigned *)((BYTE *)lock.pBits+y*lock.Pitch))[x]==pixel);
        number("VERTEXORACLE pixel=",pixel);
        CHECK(SUCCEEDED(IDirect3DSurface9_UnlockRect(readback)));IDirect3DVertexDeclaration9_Release(decl);
    }
    IDirect3DSurface9_Release(readback);IDirect3DSurface9_Release(target);
    IDirect3DPixelShader9_Release(shader);IDirect3DDevice9_Release(d);IDirect3D9_Release(api);
    DestroyWindow(window);FreeLibrary(module);number("VERTEXORACLE failures=",0);ExitProcess(0);
}
