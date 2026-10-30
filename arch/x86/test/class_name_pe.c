/* Hidden custom windows exercise ANSI class-name queries on both PE ABIs. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API WORD CALL RegisterClassA(const void *);
API int CALL UnregisterClassA(const char *,void *);
API void *CALL CreateWindowExA(DWORD,const char *,const char *,DWORD,int,int,int,int,void *,void *,void *,void *);
API int CALL DestroyWindow(void *);
API IPTR CALL DefWindowProcA(void *,DWORD,UPTR,IPTR);
typedef struct {DWORD style;void *proc;int cls_extra,wnd_extra;
    void *instance,*icon,*cursor,*background;const char *menu,*name;} CLASS;
typedef int (CALL *QUERY)(void *,char *,int);
#define SAY(s) do{DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
static unsigned failures;
static void class_name_check(int ok)
{if(ok)SAY("CLASSNAME PASS\n");else{failures++;SAY("CLASSNAME FAIL\n");}}
static void class_name_row(DWORD id,int result,DWORD error,const unsigned char *data)
{
    char out[128];static const char hex[]="0123456789abcdef";
    DWORD values[11]={id,(DWORD)result,error};
    for(unsigned i=0;i<8;i++)values[i+3]=data[i*4]|((DWORD)data[i*4+1]<<8)|
        ((DWORD)data[i*4+2]<<16)|((DWORD)data[i*4+3]<<24);
    unsigned n=0;const char *prefix="CLASSNAME ROW ";while(*prefix)out[n++]=*prefix++;
    for(unsigned j=0;j<11;j++){for(unsigned i=0;i<8;i++)out[n++]=hex[(values[j]>>(28-i*4))&15];out[n++]=' ';}
    out[n++]='\n';DWORD wr;WriteFile(GetStdHandle((DWORD)-11),out,n,&wr,0);
}
void class_name_entry(void)
{
    QUERY query=(QUERY)GetProcAddress(GetModuleHandleA("user32.dll"),"GetClassNameA");
    class_name_check(query!=0);if(!query)ExitProcess(1);
    void *instance=GetModuleHandleA(0);
    static const char name[]="OsitoClassProbe";
    CLASS cls;for(unsigned i=0;i<sizeof(cls);i++)((unsigned char *)&cls)[i]=0;
    cls.proc=(void *)DefWindowProcA;cls.instance=instance;cls.name=name;
    WORD atom=RegisterClassA(&cls);class_name_check(atom!=0);if(!atom)ExitProcess(1);
    void *window=CreateWindowExA(0,(const char *)(UPTR)atom,"",0,0,0,20,20,0,0,instance,0);
    class_name_check(window!=0);if(!window)ExitProcess(1);
    static const int sizes[]={32,16,15,4,1,0,2};
    for(unsigned id=0;id<11;id++) {
        unsigned char buffer[36];for(unsigned i=0;i<sizeof(buffer);i++)buffer[i]=0xa5;
        int size=id<7?sizes[id]:id==10?0:32;
        void *handle=id==7||id==10?0:id==8?(void *)(UPTR)0x12345678:window;
        if(id==9)class_name_check(DestroyWindow(window));
        SetLastError(0x12345678);
        int result=query(handle,(char *)buffer,size);DWORD error=GetLastError();
        class_name_row(id,result,error,buffer);
        int safe=1;unsigned used=id<7&&size>0?(unsigned)(size<16?size:16):0;
        for(unsigned i=used;i<sizeof(buffer);i++)if(buffer[i]!=0xa5)safe=0;
        if(id<7&&size>0){unsigned length=used-1;safe&=result==(int)length;
            for(unsigned i=0;i<length;i++)safe&=buffer[i]==(unsigned char)name[i];
            safe&=buffer[length]==0;
        }else safe&=result==0;
        class_name_check(safe);
    }
    class_name_check(UnregisterClassA(name,instance));
    if(!failures)SAY("CLASSNAME DONE\n");ExitProcess(failures?1:0);
}
