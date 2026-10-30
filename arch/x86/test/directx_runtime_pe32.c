/* Exercise original Microsoft DirectX redistributable DLLs without a GPU. */
typedef unsigned int UINT;
typedef unsigned int DWORD;
typedef int HRESULT;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
void *memcpy(void *p,const void *q,UINT n)
{unsigned char *b=p;const unsigned char *a=q;for(UINT i=0;i<n;i++)b[i]=a[i];return p;}
void *memset(void *p,int c,UINT n)
{unsigned char *b=p;for(UINT i=0;i<n;i++)b[i]=(unsigned char)c;return p;}
static DWORD failures;
static void say(const char *text)
{DWORD n=0,written;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&written,0);}
static UINT length(const char *text){UINT n=0;while(text[n])n++;return n;}
static void check(int ok,const char *name)
{say(ok?"DXRUN PASS ":"DXRUN FAIL ");say(name);say("\n");if(!ok)failures++;}
static void hex(UINT value)
{char s[12]={'0','x'};for(UINT i=0;i<8;i++)s[2+i]="0123456789abcdef"[(value>>(28-4*i))&15];s[10]='\n';s[11]=0;say(s);}
typedef struct Blob Blob;
typedef struct {
    HRESULT (CALL *query)(Blob *,const void *,void **);
    UINT (CALL *addref)(Blob *);
    UINT (CALL *release)(Blob *);
    void *(CALL *pointer)(Blob *);
    UINT (CALL *size)(Blob *);
} BlobVtable;
struct Blob{BlobVtable *vtable;};
static int valid_shader(Blob *blob)
{
    if(!blob||!blob->vtable||blob->vtable->size(blob)<8)return 0;
    UINT *words=blob->vtable->pointer(blob);
    return words&&(words[0]&0xffff0000U)==0xffff0000U;
}
static void error_blob(Blob *errors)
{
    if(!errors)return;
    UINT bytes=errors->vtable->size(errors),written;
    if(bytes>2000)bytes=2000;
    WriteFile(GetStdHandle((DWORD)-11),errors->vtable->pointer(errors),bytes,&written,0);
    errors->vtable->release(errors);
}
void directx_runtime_entry(void)
{
    void *compiler=LoadLibraryA("D3DCompiler_43.dll");
    check(compiler!=0,"load-original-shader-compiler");
    if(!compiler){hex(GetLastError());ExitProcess(1);}
    typedef HRESULT (CALL *compile_fn)(const void *,UINT,const char *,const void *,
        void *,const char *,const char *,UINT,UINT,Blob **,Blob **);
    compile_fn compile=GetProcAddress(compiler,"D3DCompile");
    check(compile!=0,"compiler-entry");
    if(!compile)ExitProcess(1);
    const char *sources[]={
        "float4 main(float2 uv:TEXCOORD0):COLOR0{return float4(uv,0.25,1);}",
        "float4 main():COLOR0{return float4(sin(0.5),cos(0.5),atan(0.5),1);}"};
    for(UINT i=0;i<2;i++){
        Blob *shader=0,*errors=0;
        say("DXRUN INFO calling D3DCompile\n");
        HRESULT status=compile(sources[i],length(sources[i]),"osito-test.hlsl",0,0,
                               "main","ps_2_0",0,0,&shader,&errors);
        hex((UINT)status);error_blob(errors);
        check(status>=0&&valid_shader(shader),i?"constant-folded-shader":"pixel-shader-bytecode");
        if(shader)shader->vtable->release(shader);
    }
    void *d3dx=LoadLibraryA("d3dx9_43.dll");
    check(d3dx!=0,"load-original-d3dx");
    if(!d3dx){hex(GetLastError());ExitProcess(1);}
    typedef HRESULT (CALL *buffer_fn)(UINT,Blob **);
    buffer_fn create_buffer=GetProcAddress(d3dx,"D3DXCreateBuffer");
    check(create_buffer!=0,"d3dx-buffer-entry");
    Blob *buffer=0;
    if(create_buffer){
        HRESULT status=create_buffer(256,&buffer);
        check(status>=0&&buffer&&buffer->vtable->size(buffer)==256&&buffer->vtable->pointer(buffer),"d3dx-owned-buffer");
        if(buffer)buffer->vtable->release(buffer);
    }
    typedef HRESULT (CALL *effect_fn)(const char *,UINT,const void *,void *,DWORD,void **,Blob **);
    effect_fn effect=GetProcAddress(d3dx,"D3DXCreateEffectCompiler");
    check(effect!=0,"effect-compiler-entry");
    if(effect){
        const char source[]="float4 main():COLOR0{return float4(1,0,0,1);} technique T { pass P { PixelShader = compile ps_2_0 main(); } }";
        void *object=0;Blob *errors=0;
        HRESULT status=effect(source,sizeof(source)-1,0,0,0,&object,&errors);
        hex((UINT)status);error_blob(errors);
        check(status>=0&&object,"effect-compiler-parse");
        if(object)((Blob *)object)->vtable->release(object);
    }
    if(!failures)say("DXRUN DONE\n");
    ExitProcess(failures?1:0);
}
