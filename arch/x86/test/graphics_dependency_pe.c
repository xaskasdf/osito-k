/* A dependency initializer calls its parent while the parent imports it.
 * The complete parent IAT must exist before that initializer runs. */
typedef unsigned int DWORD;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API DWORD CALL GetCurrentProcessId(void);
API void *CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL LoadLibraryA(const char *);
API int CALL FreeLibrary(void *);
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);

#if defined(CYCLE_PARENT)
API DWORD CALL cycle_peer(void);
__declspec(dllexport) DWORD CALL cycle_value(void)
{
    return GetCurrentProcessId();
}
int CALL cycle_dll_main(void *instance,DWORD reason,void *reserved)
{
    (void)instance;(void)reserved;
    return reason!=1 || cycle_peer()==GetCurrentProcessId();
}
#elif defined(CYCLE_CHILD)
static DWORD observed;
__declspec(dllexport) DWORD CALL cycle_peer(void)
{
    return observed;
}
int CALL cycle_dll_main(void *instance,DWORD reason,void *reserved)
{
    (void)instance;(void)reserved;
    if(reason==1){
        void *parent=GetModuleHandleA("cycle-parent.dll");
        DWORD (CALL *value)(void)=(void *)GetProcAddress(parent,"cycle_value");
        if(!value)return 0;
        observed=value();
    }
    return 1;
}
#else
void cycle_entry(void)
{
    DWORD written;
    void *parent=LoadLibraryA("cycle-parent.dll");
    DWORD (CALL *value)(void)=parent?(void *)GetProcAddress(parent,"cycle_value"):0;
    int passed=value && value()==GetCurrentProcessId();
    if(parent)FreeLibrary(parent);
    const char *text=passed?"CYCLE PASS\n":"CYCLE FAIL\n";
    WriteFile(GetStdHandle((DWORD)-11),text,11,&written,0);
    ExitProcess(passed?0:1);
}
#endif
