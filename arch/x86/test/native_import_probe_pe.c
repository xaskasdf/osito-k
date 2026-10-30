/* Load one supplied import-only fixture without invoking its imports. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void native_import_probe_entry(void)
{
    const char *command=GetCommandLineA();
    if (*command=='"') { command++; while (*command && *command!='"') command++; if (*command) command++; }
    else while (*command && *command!=' ') command++;
    while (*command==' ') command++;
    if (!*command || *command=='"') ExitProcess(1);
    HMODULE module=LoadLibraryA(command);
    DWORD status=module ? 0 : GetLastError(),written;
    char text[]="IMPORTPROBE error=00000000\n";
    for (unsigned i=0;i<8;i++) text[18+i]="0123456789abcdef"[(status>>(28-4*i))&15];
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,sizeof(text)-1,&written,NULL);
    if (module) FreeLibrary(module);
    ExitProcess(module ? 0 : 2);
}
