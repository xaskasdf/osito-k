typedef unsigned int UINT;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("GDIDX PASS " name "\n");else{SAY("GDIDX FAIL " name "\n");failures++;}}while(0)
void *memset(void *p,int c,UPTR n){unsigned char *b=p;for(UPTR i=0;i<n;i++)b[i]=(unsigned char)c;return p;}
void *memcpy(void *p,const void *q,UPTR n){unsigned char *b=p;const unsigned char *a=q;for(UPTR i=0;i<n;i++)b[i]=a[i];return p;}
typedef struct{int height,width,esc,orientation,weight;unsigned char flags[8];char face[32];} FontA;
typedef struct{UINT charset,cp,usb[4],csb[2];} Charset;
void gdi_directx_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll");
    typedef void *(CALL *font_fn)(const FontA *);
    typedef int (CALL *object_fn)(void *,int,void *);
    typedef int (CALL *delete_fn)(void *);
    typedef int (CALL *charset_fn)(const void *,Charset *,UINT);
    typedef UINT (CALL *glyph_fn)(void *,UINT,UINT,void *,UINT,void *,const void *);
    typedef UINT (CALL *outline_fn)(void *,UINT,void *);
    typedef UINT (CALL *placement_fn)(void *,const void *,int,int,void *,UINT);
    typedef UINT (CALL *language_fn)(void *);
    font_fn font=GetProcAddress(gdi,"CreateFontIndirectA");
    object_fn object=GetProcAddress(gdi,"GetObjectA");
    delete_fn remove=GetProcAddress(gdi,"DeleteObject");
    charset_fn charset=GetProcAddress(gdi,"TranslateCharsetInfo");
    glyph_fn glyphA=GetProcAddress(gdi,"GetGlyphOutlineA"),glyphW=GetProcAddress(gdi,"GetGlyphOutlineW");
    outline_fn outline=GetProcAddress(gdi,"GetOutlineTextMetricsA");
    placement_fn placeA=GetProcAddress(gdi,"GetCharacterPlacementA"),placeW=GetProcAddress(gdi,"GetCharacterPlacementW");
    language_fn language=GetProcAddress(gdi,"GetFontLanguageInfo");
    CHECK(font&&object&&remove&&charset&&glyphA&&glyphW&&outline&&placeA&&placeW&&language,"exports");
    if(failures)ExitProcess(1);
    FontA request={0},result={0};request.height=-16;request.weight=400;
    request.face[0]='A';request.face[1]='r';request.face[2]='i';request.face[3]='a';request.face[4]='l';
    void *handle=font(&request);
    CHECK(handle&&object(handle,sizeof(result),&result)==sizeof(result)&&result.height==-16&&result.weight==400&&result.face[0]=='A',"font-roundtrip");
    CHECK(handle&&remove(handle),"font-release");
    Charset info={0};
    CHECK(charset(0,&info,1)&&info.charset==0&&info.cp==1252&&info.csb[0]==1,"ansi-charset");
    CHECK(charset((void*)932,&info,2)&&info.charset==128&&info.cp==932&&info.csb[0]==(1U<<17),"shiftjis-codepage");
    UINT signature[6]={0,0,0,0,4,0};
    CHECK(charset(signature+4,&info,3)&&info.charset==204&&info.cp==1251,"font-signature");
    CHECK(!charset((void*)1,&info,1),"default-charset-is-not-concrete");
    unsigned char metrics[64]={0x55};int matrix[4]={0x10000,0,0,0x10000};
    CHECK(glyphA(0,'A',0,metrics,0,0,matrix)==0xffffffffU,"glyph-a-invalid-dc");
    CHECK(glyphW(0,'A',0,metrics,0,0,matrix)==0xffffffffU,"glyph-w-invalid-dc");
    CHECK(!outline(0,0,0),"outline-invalid-dc");
    CHECK(!placeA(0,"abc",3,0,0,0),"placement-a-invalid-dc");
    WCHAR text[]={'a','b','c',0};
    CHECK(!placeW(0,text,3,0,0,0),"placement-w-invalid-dc");
    CHECK(language(0)==0x8000U,"language-invalid-dc");
    if(!failures)SAY("GDIDX DONE\n");
    ExitProcess(failures?1:0);
}
