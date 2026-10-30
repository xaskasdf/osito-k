/* Exercise the runtime lookup/call used by FLTK, on Windows and in the guest. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UINT_PTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,HANDLE);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API void CALL ExitProcess(DWORD);
#define SAY(s) do {DWORD written_;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&written_,0);}while(0)
#define CHECK(c,s) do{if(c)SAY("IMMLOCALE PASS " s "\n");else{SAY("IMMLOCALE FAIL " s "\n");failures++;}}while(0)

void imm_entry(void)
{
    DWORD failures=0;
    HANDLE imm=LoadLibraryA("imm32.dll"), user=LoadLibraryA("user32.dll");
    int(CALL *is_ime)(HANDLE)=(void *)GetProcAddress(imm,"ImmIsIME");
    HANDLE(CALL *create_context)(void)=(void *)GetProcAddress(imm,"ImmCreateContext");
    int(CALL *destroy_context)(HANDLE)=(void *)GetProcAddress(imm,"ImmDestroyContext");
    HANDLE(CALL *get_layout)(DWORD)=(void *)GetProcAddress(user,"GetKeyboardLayout");
    CHECK(is_ime&&create_context&&destroy_context&&get_layout,"runtime-imports");
    if(failures)ExitProcess(1);
    const UINT_PTR locales[]={0,0x409,0x04090409,0x08090809,(UINT_PTR)-1};
    for(unsigned i=0;i<sizeof(locales)/sizeof(locales[0]);i++){
        SetLastError(0x1234ABCD);
        DWORD result=is_ime((HANDLE)locales[i]),error=GetLastError();
        DWORD values[]={i,result,error};char row[29];DWORD used=0,written;
        for(unsigned j=0;j<3;j++){
            for(unsigned k=0;k<8;k++)row[used++]="0123456789ABCDEF"[(values[j]>>(28-4*k))&15];
            row[used++]=' ';
        }
        row[used++]='\n';SAY("IMMLOCALE RESULT ");WriteFile(GetStdHandle((DWORD)-11),row,used,&written,0);
        CHECK(!result,"non-ime-layout");
    }
    HANDLE layout=get_layout(0);
    int before=is_ime(layout);
    HANDLE context=create_context();CHECK(context!=0,"create-context");
    CHECK(is_ime(layout)==before,"context-does-not-install-ime");
    if(context)CHECK(destroy_context(context),"destroy-context");
    CHECK(is_ime(layout)==before,"layout-survives-context");
    if(!failures)SAY("IMMLOCALE DONE\n");ExitProcess(failures?1:0);
}
