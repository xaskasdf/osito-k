/* Static IAT resolution must use the same system directory as LoadLibrary. */
typedef unsigned int DWORD;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD system_path_probe_value(void);
void system_path_static_entry(void)
{
    DWORD written;
    int pass = system_path_probe_value() == sizeof(void *) * 8;
    static const char ok[] = "SYSPATH STATIC PASS\n";
    static const char bad[] = "SYSPATH STATIC FAIL\n";
    WriteFile(GetStdHandle((DWORD)-11), pass ? ok : bad, sizeof(ok)-1, &written, 0);
    ExitProcess(pass ? 0 : 1);
}
