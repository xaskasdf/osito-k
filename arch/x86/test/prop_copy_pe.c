/* PROPVARIANT copy ownership and guest-width layout, native and Osito-K. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct {
    WORD type,r1,r2,r3;
    union {U64 bits;void *pointer;struct {DWORD count;void *data;} array;} value;
} PROP;
_Static_assert(sizeof(PROP)==(sizeof(void *)==4?16:24),"PROPVARIANT layout");
typedef struct {void **vtable;DWORD refs,adds,releases;} OBJECT;
static unsigned failures;
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void prop_copy_check(int ok)
{if(ok)SAY("PROPCOPY PASS\n");else{failures++;SAY("PROPCOPY FAIL\n");}}
static int prop_copy_equal(const void *a,const void *b,UP size)
{const unsigned char *x=a,*y=b;if(size&&(!x||!y))return 0;for(UP i=0;i<size;i++)if(x[i]!=y[i])return 0;return 1;}
static void prop_copy_row(DWORD id,DWORD type,DWORD hr,DWORD a,DWORD b,DWORD c)
{SAY("PROPCOPY ROW ");HEX(id);HEX(type);HEX(hr);HEX(a);HEX(b);HEX(c);SAY("\n");}
static DWORD CALL prop_copy_addref(OBJECT *object)
{object->adds++;return ++object->refs;}
static DWORD CALL prop_copy_release(OBJECT *object)
{object->releases++;return --object->refs;}
void prop_copy_entry(void)
{
    void *ole=LoadLibraryA("ole32.dll"),*aut=LoadLibraryA("oleaut32.dll");
    DWORD (CALL *copy)(PROP *,const PROP *)=GetProcAddress(ole,"PropVariantCopy");
    DWORD (CALL *clear)(PROP *)=GetProcAddress(ole,"PropVariantClear");
    WORD *(CALL *bstr_alloc)(const WORD *,DWORD)=GetProcAddress(aut,"SysAllocStringLen");
    void (CALL *bstr_free)(WORD *)=GetProcAddress(aut,"SysFreeString");
    DWORD (CALL *bstr_bytes)(const WORD *)=GetProcAddress(aut,"SysStringByteLen");
    prop_copy_check(copy&&clear&&bstr_alloc&&bstr_free&&bstr_bytes);if(failures)ExitProcess(1);
    const WORD scalars[]={0,1,2,3,4,5,6,7,10,11,14,16,17,18,19,20,21,22,23,64};
    for(unsigned i=0;i<sizeof(scalars)/sizeof(scalars[0]);i++){
        PROP source={0};source.type=scalars[i];source.value.bits=0x1122334455667788ULL;
        struct {UP before;PROP value;UP after;} dest={0x12345678,{0},0x87654321};
        DWORD hr=copy(&dest.value,&source);int equal=prop_copy_equal(&source,&dest.value,sizeof(PROP));
        prop_copy_check(hr==0);prop_copy_check(equal);prop_copy_check(dest.before==0x12345678&&dest.after==0x87654321);
        prop_copy_row(i,source.type,hr,dest.value.type,(DWORD)dest.value.value.bits,(DWORD)(dest.value.value.bits>>32));
        if(!hr)prop_copy_check(clear(&dest.value)==0&&dest.value.type==0);
    }
    const char narrow[]="audio";const WORD wide[]={'a',0xE9,'b',0};
    const unsigned char guid[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const struct {WORD type;const void *data;DWORD bytes;} strings[]={
        {30,narrow,sizeof(narrow)},{31,wide,sizeof(wide)},{72,guid,sizeof(guid)}};
    for(unsigned i=0;i<3;i++){
        PROP source={0},dest={0};source.type=strings[i].type;source.value.pointer=(void *)strings[i].data;
        DWORD hr=copy(&dest,&source);int distinct=!hr&&dest.value.pointer&&dest.value.pointer!=source.value.pointer;
        int equal=!hr&&prop_copy_equal(dest.value.pointer,source.value.pointer,strings[i].bytes);
        prop_copy_check(hr==0);prop_copy_check(distinct&&equal);prop_copy_check(dest.type==source.type);
        prop_copy_row(100+i,source.type,hr,distinct,equal,dest.type);
        if(!hr){prop_copy_check(clear(&dest)==0&&dest.type==0);prop_copy_check(source.value.pointer==strings[i].data);}
    }
    WORD text[]={'a',0,'b',0x100};WORD *original=bstr_alloc(text,4);prop_copy_check(original!=0);
    if(original){
        PROP source={0},dest={0};source.type=8;source.value.pointer=original;
        DWORD hr=copy(&dest,&source);int distinct=!hr&&dest.value.pointer!=original;
        /* PropVariantCopy uses the terminated BSTR text, unlike VariantCopy. */
        DWORD bytes=hr?0:bstr_bytes(dest.value.pointer);int equal=!hr&&prop_copy_equal(dest.value.pointer,original,4);
        prop_copy_check(hr==0);prop_copy_check(distinct&&bytes==2&&equal);
        prop_copy_row(110,8,hr,distinct,bytes,equal);
        if(!hr)prop_copy_check(clear(&dest)==0&&bstr_bytes(original)==8);
        bstr_free(original);
    }
    unsigned char blob[]={1,0,2,3,0,4};
    for(unsigned empty=0;empty<2;empty++){
        PROP source={0},dest={0};source.type=65;source.value.array.count=empty?0:sizeof(blob);source.value.array.data=empty?0:blob;
        DWORD hr=copy(&dest,&source);int equal=!hr&&prop_copy_equal(dest.value.array.data,source.value.array.data,source.value.array.count);
        int independent=empty?1:(dest.value.array.data&&dest.value.array.data!=blob);
        prop_copy_check(hr==0);prop_copy_check(equal&&independent&&dest.value.array.count==source.value.array.count);
        prop_copy_row(120+empty,65,hr,dest.value.array.count,dest.value.array.data!=0,equal);
        if(!hr)prop_copy_check(clear(&dest)==0);
    }
    const WORD vector_types[]={17,3,20,72};const DWORD widths[]={1,4,8,16};
    unsigned char values[32];for(unsigned j=0;j<32;j++)values[j]=(unsigned char)(j+1);
    for(unsigned i=0;i<4;i++){
        PROP source={0},dest={0};source.type=0x1000|vector_types[i];source.value.array.count=2;source.value.array.data=values;
        DWORD hr=copy(&dest,&source);int distinct=!hr&&dest.value.array.data!=values;
        int equal=!hr&&prop_copy_equal(dest.value.array.data,values,2*widths[i]);
        prop_copy_check(hr==0);prop_copy_check(distinct&&equal&&dest.value.array.count==2);
        prop_copy_row(130+i,source.type,hr,dest.value.array.count,distinct,equal);
        if(!hr)prop_copy_check(clear(&dest)==0);
    }
    const WORD *words[]={wide,wide};PROP source={0},dest={0};
    source.type=0x101f;source.value.array.count=2;source.value.array.data=(void *)words;
    DWORD hr=copy(&dest,&source);int good=!hr&&dest.value.array.count==2&&dest.value.array.data!=words;
    if(good){WORD **p=dest.value.array.data;good=p&&p[0]!=wide&&p[1]!=wide&&p[0]!=p[1]&&prop_copy_equal(p[0],wide,sizeof(wide))&&prop_copy_equal(p[1],wide,sizeof(wide));}
    prop_copy_check(hr==0&&good);prop_copy_row(140,source.type,hr,good,dest.type,dest.value.array.count);if(!hr)prop_copy_check(clear(&dest)==0);
    PROP nested[2]={{0},{0}};nested[0].type=31;nested[0].value.pointer=(void *)wide;nested[1].type=3;nested[1].value.bits=42;
    source.type=0x100c;source.value.array.count=2;source.value.array.data=nested;
    hr=copy(&dest,&source);good=!hr&&dest.value.array.data&&dest.value.array.data!=nested&&dest.value.array.count==2;
    if(good){PROP *p=dest.value.array.data;good=p[0].type==31&&p[0].value.pointer!=wide&&prop_copy_equal(p[0].value.pointer,wide,sizeof(wide))&&p[1].type==3&&p[1].value.bits==42;}
    prop_copy_check(hr==0&&good);prop_copy_row(141,source.type,hr,good,dest.type,dest.value.array.count);if(!hr)prop_copy_check(clear(&dest)==0);
    DWORD referenced=77;source.type=0x4003;source.value.pointer=&referenced;
    hr=copy(&dest,&source);good=!hr&&dest.value.pointer==&referenced;
    prop_copy_check(good);prop_copy_row(150,source.type,hr,good,dest.type,referenced);if(!hr)prop_copy_check(clear(&dest)==0&&referenced==77);
    void *vtable[]={0,(void *)prop_copy_addref,(void *)prop_copy_release};
    const WORD interfaces[]={13,9,66,67,68,69};
    for(unsigned i=0;i<6;i++){
        OBJECT object={vtable,1,0,0};PROP src={0},dst={0};src.type=interfaces[i];src.value.pointer=&object;
        hr=copy(&dst,&src);good=!hr&&dst.value.pointer==&object&&object.refs==2&&object.adds==1;
        prop_copy_check(good);prop_copy_row(160+i,src.type,hr,good,object.refs,object.adds);
        if(!hr)prop_copy_check(clear(&dst)==0&&object.refs==1&&object.releases==1);
    }
    PROP self={0};self.type=31;self.value.pointer=(void *)wide;
    hr=copy(&self,&self);good=!hr&&self.type==31&&self.value.pointer!=wide&&prop_copy_equal(self.value.pointer,wide,sizeof(wide));
    prop_copy_check(good);prop_copy_row(170,31,hr,good,self.type,0);
    if(!hr)prop_copy_check(clear(&self)==0);
    PROP invalid={0},target={0};invalid.type=15;
    hr=copy(&target,&invalid);prop_copy_check(hr==0x80020008&&target.type==0);prop_copy_row(180,15,hr,target.type,0,0);
    /* Failure after a completed owned element must not expose a partial copy. */
    nested[1].type=15;source.type=0x100c;source.value.array.count=2;source.value.array.data=nested;
    hr=copy(&dest,&source);prop_copy_check(hr==0x80020008&&dest.type==0);
    prop_copy_row(181,source.type,hr,dest.type,0,0);
    const WORD nullable[]={8,30,31,72};
    for(unsigned i=0;i<4;i++){
        PROP src={0},dst={0};src.type=nullable[i];hr=copy(&dst,&src);
        prop_copy_check(hr==0&&dst.type==src.type&&!dst.value.pointer);
        prop_copy_row(190+i,src.type,hr,dst.type,dst.value.pointer!=0,0);
        if(!hr)prop_copy_check(clear(&dst)==0);
    }
    if(!failures)SAY("PROPCOPY DONE\n");ExitProcess(failures?1:0);
}
