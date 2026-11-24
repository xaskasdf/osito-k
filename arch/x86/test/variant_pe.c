/* Native/en-US oracle plus ownership and guest-width Automation ABI checks. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API char *CALL GetCommandLineA(void);
typedef struct {
    WORD type,r1,r2,r3;
    union {U64 bits;void *pointer;struct {void *record,*info;} record;} value;
} VARIANT;
_Static_assert(sizeof(VARIANT)==(sizeof(void *)==4?16:24),"VARIANT layout");
typedef struct {
    DWORD source[4];const WORD *text;DWORD text_length;
    WORD target,flags;DWORD status,output[4];const WORD *output_text;DWORD output_length;
} VARIANT_CASE;
#include "variant_cases.h"
static DWORD checks, failures, case_index;
static DWORD (CALL *change)(VARIANT *,const VARIANT *,WORD,WORD);
static DWORD (CALL *change_ex)(VARIANT *,const VARIANT *,DWORD,WORD,WORD);
static DWORD (CALL *copy)(VARIANT *,const VARIANT *);
static DWORD (CALL *clear)(VARIANT *);
static void (CALL *init)(VARIANT *);
static WORD *(CALL *alloc)(const WORD *,DWORD);
static WORD *(CALL *alloc_bytes)(const char *,DWORD);
static void (CALL *free_string)(WORD *);
static DWORD (CALL *string_bytes)(const WORD *);
static void say(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD v)
{ char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;say(s); }
static void check(int okay,const char *label,DWORD got,DWORD expected)
{
    checks++;
    if(!okay){
        failures++;
        if(failures<100){say("VARIANT FAIL case=");hex(case_index);say(" ");say(label);
            say(" got=");hex(got);say(" expected=");hex(expected);say("\n");}
    }
}
static int equal(const void *a,const void *b,UP bytes)
{ const BYTE *x=a,*y=b;for(UP i=0;i<bytes;i++)if(x[i]!=y[i])return 0;return 1; }
static void move(void *a,const void *b,UP bytes)
{ BYTE *x=a;const BYTE *y=b;for(UP i=0;i<bytes;i++)x[i]=y[i]; }
static DWORD convert(VARIANT *d,const VARIANT *s,WORD flags,WORD type)
{ return change_ex?change_ex(d,s,0x409,flags,type):change(d,s,flags,type); }
static void matrix(void)
{
    for(DWORD i=0;i<sizeof(variant_cases)/sizeof(*variant_cases);i++){
        case_index=i;
        VARIANT_CASE expected=variant_cases[i];
        /* Native x86 Automation uses the legacy nonfinite spelling and does
         * not preserve negative zero in strings. x64 uses the modern CRT. */
        if(sizeof(void *)==4){
            WORD type=(WORD)expected.source[0];
            U64 bits=(U64)expected.source[3]<<32|expected.source[2];
            int floating=type==4||type==5;
            U64 exponent=type==4?0x7f800000ULL:0x7ff0000000000000ULL;
            U64 fraction=type==4?0x007fffffULL:0x000fffffffffffffULL;
            U64 sign=type==4?0x80000000ULL:0x8000000000000000ULL;
            if(floating&&expected.target==6&&(bits&exponent)==exponent&&(bits&fraction))expected.status=0x8002000a;
            if(floating&&expected.target==8){
                static const WORD zero_text[]={'0',0},nan_text[]={'1','.','#','Q','N','A','N',0};
                static const WORD inf_text[]={'1','.','#','I','N','F',0},neg_inf[]={'-','1','.','#','I','N','F',0};
                if(bits==sign){expected.output_text=zero_text;expected.output_length=1;}
                else if((bits&exponent)==exponent){
                    expected.output_text=bits&fraction?nan_text:bits&sign?neg_inf:inf_text;
                    expected.output_length=(bits&fraction)||(bits&sign)?7:6;
                }
            }
        }
        const VARIANT_CASE *r=&expected;
        struct {U64 before;VARIANT v;U64 after;} source={0x76543210,{0},0xfedcba98};
        struct {U64 before;VARIANT v;U64 after;} dest={0x12345678,{0},0x9abcdef0};
        VARIANT original;
        move(&source.v,r->source,16);
        WORD *owner=0;
        if(r->text){owner=alloc(r->text,r->text_length);if(!owner)ExitProcess(2);source.v.value.pointer=owner;}
        dest.v.type=3;dest.v.value.bits=321;original=dest.v;
        SetLastError(0x12345678);
        DWORD hr=convert(&dest.v,&source.v,r->flags,r->target);
        DWORD error=GetLastError();
        check(hr==r->status,"status",hr,r->status);
        if(!change_ex)check(error==0x12345678,"last-error",error,0x12345678);
        check(source.before==0x76543210&&source.after==0xfedcba98,"source guard",(DWORD)source.after,0xfedcba98);
        check(dest.before==0x12345678&&dest.after==0x9abcdef0,"destination guard",(DWORD)dest.after,0x9abcdef0);
        if(hr==r->status){
            if(hr)check(equal(&original,&dest.v,sizeof(original)),"failure preserves destination",dest.v.type,3);
            else if(r->target==8){
                const WORD *text=dest.v.value.pointer;
                check((text!=0)==(r->output_text!=0),"BSTR null",text!=0,r->output_text!=0);
                if(text&&r->output_text){
                    DWORD bytes=string_bytes(text);
                    check(bytes==r->output_length*2,"BSTR bytes",bytes,r->output_length*2);
                    check(bytes==r->output_length*2&&equal(text,r->output_text,bytes),"BSTR text",text[0],r->output_text[0]);
                }
            }else {
                check(dest.v.type==r->target,"type",dest.v.type,r->target);
                DWORD width=r->target<=1?0:r->target==16||r->target==17?1:
                    r->target==2||r->target==11||r->target==18?2:
                    r->target==3||r->target==4||r->target==10||r->target==19||r->target==22||r->target==23?4:
                    r->target==9||r->target==13?(DWORD)sizeof(void *):8;
                if(r->target==14){
                    check(equal((BYTE *)&dest.v+2,(const BYTE *)r->output+2,14),"DECIMAL",((DWORD *)&dest.v)[0],r->output[0]);
                }else if(width){
                    check(equal(&dest.v.value,(const BYTE *)r->output+8,width),"payload low",(DWORD)dest.v.value.bits,r->output[2]);
                    if(width>4)check((DWORD)(dest.v.value.bits>>32)==r->output[3],"payload high",(DWORD)(dest.v.value.bits>>32),r->output[3]);
                }
            }
        }
        if(!hr)clear(&dest.v);
        free_string(owner);
    }
}

typedef struct {void **vtable;DWORD refs,adds,releases,invokes,invoke_flags;DWORD returned_type;} OBJECT;
static DWORD CALL addref(OBJECT *object) { object->adds++;return ++object->refs; }
static DWORD CALL release(OBJECT *object) { object->releases++;return --object->refs; }
static DWORD CALL query(OBJECT *object,const DWORD *iid,void **result)
{
    if(iid[0]!=0&&iid[0]!=0x20400){*result=0;return 0x80004002;}
    *result=object;addref(object);return 0;
}
static DWORD CALL invoke(OBJECT *object,DWORD dispid,const DWORD *iid,DWORD locale,WORD flags,
                         const void *parameters,VARIANT *result,void *exception,DWORD *argument)
{
    (void)exception;(void)argument;
    object->invokes++;object->invoke_flags=flags;
    check(dispid==0&&iid&&!iid[0]&&!iid[1]&&!iid[2]&&!iid[3],"DISPID_VALUE",dispid,0);
    check(locale==0x409||locale==0x400,"Invoke locale",locale,0x409);
    const BYTE *p=parameters;
    DWORD offset=(DWORD)sizeof(void *)*2;
    check(p&&*(const DWORD *)(p+offset)==0&&*(const DWORD *)(p+offset+4)==0,"empty DISPPARAMS",0,0);
    if(!result)return 0x80070057;
    init(result);result->type=(WORD)object->returned_type;
    if(result->type==8){const WORD text[]={'4','2',0};result->value.pointer=alloc(text,2);}
    else result->value.bits=42;
    return 0;
}
static void ownership(void)
{
    case_index=0x10000;
    struct {U64 before;VARIANT v;U64 after;} guarded;
    BYTE *raw=(BYTE *)&guarded;
    for(UP i=0;i<sizeof(guarded);i++)raw[i]=0xa5;
    init(&guarded.v);
    VARIANT zero={0};
    check(change_ex?guarded.v.type==0:equal(&guarded.v,&zero,sizeof(zero)),"VariantInit",guarded.v.type,0);
    check(guarded.before==0xa5a5a5a5a5a5a5a5ULL&&guarded.after==guarded.before,"VariantInit guard",0,0);
    guarded.v.type=3;guarded.v.r1=0x1122;guarded.v.value.bits=0x123456789abcdef0ULL;
    VARIANT before=guarded.v;before.type=0;
    check(clear(&guarded.v)==0&&equal(&guarded.v,&before,sizeof(before)),"VariantClear preserves payload",0,0);
    const char bytes[5]={'a',0,0,'b',1};
    VARIANT source={0},dest={0};source.type=8;source.value.pointer=alloc_bytes(bytes,5);
    check(copy(&dest,&source)==0,"copy BSTR",0,0);
    check(dest.type==8&&dest.value.pointer!=source.value.pointer&&string_bytes(dest.value.pointer)==5&&
        equal(dest.value.pointer,bytes,5),"copy odd byte BSTR",0,0);
    check(copy(&dest,&dest)==0&&string_bytes(dest.value.pointer)==5,"self copy BSTR",0,0);
    clear(&dest);clear(&source);
    source.type=8;source.value.pointer=0;
    check(copy(&dest,&source)==0&&dest.value.pointer&&string_bytes(dest.value.pointer)==0,"copy NULL BSTR",0,0);clear(&dest);
    const WORD text[]={'1','2','3',0};
    source.type=8;source.value.pointer=alloc(text,3);
    check(convert(&source,&source,0,3)==0&&source.type==3&&(DWORD)source.value.bits==123,"in-place BSTR to integer",0,0);
    clear(&source);
    source.type=8;source.value.pointer=alloc(text,3);before=source;
    check(convert(&source,&source,0,15)==0x80020008&&equal(&source,&before,sizeof(source)),"in-place failure preserves BSTR",0,0);
    clear(&source);
    source.type=0x8003;dest.type=3;dest.value.bits=321;before=dest;
    check(copy(&dest,&source)==0x80020008&&equal(&before,&dest,sizeof(dest)),"copy invalid source",0,0);
    check(clear(&source)==0x80020008&&source.type==0x8003,"clear invalid type",0,0);
    check(copy(&source,&source)==0x80020008,"self copy invalid type",0,0);
    source.type=0;
    DWORD value=123;
    source.type=0x4003;source.value.pointer=&value;dest.type=0;
    check(convert(&dest,&source,0,8)==0&&dest.type==8&&string_bytes(dest.value.pointer)==6&&
        equal(dest.value.pointer,text,6),"byref I4 to BSTR",0,0);clear(&dest);
    check(clear(&source)==0&&value==123,"byref clear borrowed",value,123);
    VARIANT inner={0};inner.type=3;inner.value.bits=42;
    source.type=0x400c;source.value.pointer=&inner;
    check(convert(&dest,&source,0,20)==0&&dest.value.bits==42,"byref VARIANT",0,0);clear(&dest);
    inner.type=0x400c;inner.value.pointer=&source;
    check(convert(&dest,&source,0,3)==0x80070057,"nested byref VARIANT",0,0);
    source.type=0x4003;source.value.pointer=0;
    check(convert(&dest,&source,0,3)==0x80020005,"NULL byref",0,0);
    source.type=0;
    check(change(0,&source,0,3)==0x80070057,"NULL destination",0,0);
    check(change(&dest,0,0,3)==0x80070057,"NULL source",0,0);
    void *vtable[7]={(void *)query,(void *)addref,(void *)release,0,0,0,(void *)invoke};
    OBJECT object={vtable,1,0,0,0,0,3};
    source.type=9;source.value.pointer=&object;
    check(copy(&dest,&source)==0&&object.refs==2,"copy dispatch AddRef",object.refs,2);
    check(clear(&dest)==0&&object.refs==1,"clear dispatch Release",object.refs,1);
    check(convert(&dest,&source,0,13)==0&&dest.value.pointer==&object&&object.refs==2,"QI unknown",object.refs,2);
    clear(&dest);
    DWORD hr=convert(&dest,&source,0,3);
    check(hr==0&&dest.type==3&&(DWORD)dest.value.bits==42&&object.invokes==1,"default value I4",hr,0);
    check(object.invoke_flags==2,"PROPERTYGET",object.invoke_flags,2);clear(&dest);
    check(convert(&dest,&source,1,3)==0x80020005&&object.invokes==1,"NOVALUEPROP",object.invokes,1);
    object.returned_type=8;
    check(convert(&dest,&source,0,20)==0&&dest.type==20&&dest.value.bits==42&&object.invokes==2,"default BSTR coercion",object.invokes,2);
    clear(&dest);
    source.type=13;
    check(convert(&dest,&source,0,9)==0&&dest.type==9&&object.refs==2,"QI dispatch",object.refs,2);clear(&dest);
    source.type=0x400d;source.value.pointer=&dest.value.pointer;dest.value.pointer=&object;
    check(clear(&source)==0&&object.refs==1,"borrowed interface clear",object.refs,1);
    dest.value.pointer=0;
    check(object.adds==object.releases,"COM reference balance",object.adds,object.releases);
}

void variant_entry(void)
{
    void *module=LoadLibraryA("oleaut32.dll");
    change=GetProcAddress(module,"VariantChangeType");copy=GetProcAddress(module,"VariantCopy");
    clear=GetProcAddress(module,"VariantClear");init=GetProcAddress(module,"VariantInit");
    alloc=GetProcAddress(module,"SysAllocStringLen");alloc_bytes=GetProcAddress(module,"SysAllocStringByteLen");
    free_string=GetProcAddress(module,"SysFreeString");string_bytes=GetProcAddress(module,"SysStringByteLen");
    check(change&&copy&&clear&&init&&alloc&&alloc_bytes&&free_string&&string_bytes,"exports",0,0);
    check(GetProcAddress(module,(const char *)(UP)12)==(void *)change,"ordinal 12",0,0);
    if(failures)ExitProcess(2);
    const char *cmd=GetCommandLineA(),*flag="--native";
    for(DWORD i=0;cmd[i];i++){
        DWORD j=0;while(flag[j]&&cmd[i+j]==flag[j])j++;
        if(!flag[j])change_ex=GetProcAddress(module,"VariantChangeTypeEx");
    }
    matrix();ownership();
    say("VARIANT DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
