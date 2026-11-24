/* Create only a new private test root; remove that root on completion. */
typedef unsigned int DWORD;
typedef int LONG;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetCurrentProcessId(void);
API LONG CALL RegCreateKeyExA(void *,const char *,DWORD,char *,DWORD,DWORD,void *,void **,DWORD *);
API LONG CALL RegOpenKeyExA(void *,const char *,DWORD,DWORD,void **);
API LONG CALL RegOpenKeyExW(void *,const WCHAR *,DWORD,DWORD,void **);
API LONG CALL RegEnumKeyExA(void *,DWORD,char *,DWORD *,void *,void *,void *,void *);
API LONG CALL RegQueryInfoKeyA(void *,void *,void *,void *,DWORD *,void *,void *,void *,void *,void *,void *,void *);
API LONG CALL RegDeleteTreeA(void *,const char *);
API LONG CALL RegDeleteKeyExA(void *,const char *,DWORD,DWORD);
API LONG CALL RegCloseKey(void *);
static unsigned failures;
void *memcpy(void *out,const void *in,__SIZE_TYPE__ size)
{unsigned char *d=out;const unsigned char *s=in;for(__SIZE_TYPE__ i=0;i<size;i++)d[i]=s[i];return out;}
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define CHECK(c) do {if(c)SAY("REGPARENT PASS\n");else{SAY("REGPARENT FAIL\n");failures++;}}while(0)
static int equal_fold(const char *a,const char *b)
{
    for (;;) {char x=*a++,y=*b++;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return 0;if(!x)return 1;}
}
void registry_parent_entry(void)
{
    char path[]="Software\\OsitoK\\Tests\\MsiParent20261007-00000000";
    DWORD pid=GetCurrentProcessId(),pos=sizeof(path)-2;
    for(unsigned i=0;i<8;i++){path[pos-i]="0123456789abcdef"[pid&15];pid>>=4;}
    void *hkcu=(void *)(__INTPTR_TYPE__)(int)0x80000001U,*root=0,*child=0;DWORD disposition=0;
    LONG error=RegCreateKeyExA(hkcu,path,0,0,0,0xf003f|0x100,0,&root,&disposition);
    CHECK(error==0&&disposition==1);if(failures){if(root)RegCloseKey(root);ExitProcess(1);}
    CHECK(RegCreateKeyExA(root,"Products\\Product",0,0,0,0xf003f|0x100,0,&child,&disposition)==0);
    if(child)RegCloseKey(child);
    static const char *const names[]={"Products","Products\\","Products\\\\"};
    for(unsigned wide=0;wide<2;wide++)for(unsigned i=0;i<3;i++){
        WCHAR text[32];unsigned n=0;do{text[n]=(unsigned char)names[i][n];}while(names[i][n++]);
        void *parent=0;
        error=wide?RegOpenKeyExW(root,text,0,0x20019|0x100,&parent):RegOpenKeyExA(root,names[i],0,0x20019|0x100,&parent);
        CHECK(error==0&&parent!=0);
        if(parent){
            char name[64];DWORD length=64,count=99;
            CHECK(RegEnumKeyExA(parent,0,name,&length,0,0,0,0)==0&&length==7&&equal_fold(name,"Product"));
            CHECK(RegQueryInfoKeyA(parent,0,0,0,&count,0,0,0,0,0,0,0)==0&&count==1);
            RegCloseKey(parent);
        }
    }
    void *missing=0;CHECK(RegOpenKeyExA(root,"Product",0,0x20019|0x100,&missing)==2&&missing==0);
    CHECK(RegDeleteTreeA(root,"Products")==0);CHECK(RegCloseKey(root)==0);
    CHECK(RegDeleteKeyExA(hkcu,path,0x100,0)==0);
    if(!failures)SAY("REGPARENT DONE\n");ExitProcess(failures?1:0);
}
