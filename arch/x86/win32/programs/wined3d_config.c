/* External WineD3D configuration utility; no renderer or Microsoft payloads.
 * The setting takes effect when a new process loads WineD3D. */
typedef unsigned int DWORD;
typedef int LONG;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
#define SAY(text) do { static const char message[]=text; DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11),message,sizeof(message)-1,&written,0); } while (0)
#define VALUE(value) do { DWORD number=(DWORD)(value),written; char text[9]; \
    for (DWORD i=0;i<8;++i) text[i]="0123456789abcdef"[(number>>((7-i)*4))&15]; \
    text[8]='\n'; WriteFile(GetStdHandle((DWORD)-11),text,9,&written,0); } while (0)

void wined3d_config_entry(void)
{
    const char *(CALL *get_command)(void) =
        (void *)GetProcAddress(LoadLibraryA("kernel32.dll"),"GetCommandLineA");
    if (!get_command) { SAY("WINED3D-CONFIG missing command line\n"); ExitProcess(1); }
    const char *p=get_command();
    while (*p==' ' || *p=='\t') ++p;
    if (*p=='"') { ++p; while (*p && *p!='"') ++p; if (*p) ++p; }
    else while (*p && *p!=' ' && *p!='\t') ++p;
    while (*p==' ' || *p=='\t') ++p;
    char command[32]; DWORD length=0;
    while (*p && length<sizeof(command)-1) command[length++]=*p++;
    while (length && (command[length-1]==' ' || command[length-1]=='\t')) --length;
    command[length]=0;
    const char *commands[]={"csmt","csmt 0","csmt 1","csmt default"};
    int mode=-1;
    if (!*p) for (int n=0;n<4;++n) {
        DWORD i=0;
        while (command[i] && command[i]==commands[n][i]) ++i;
        if (!command[i] && !commands[n][i]) mode=n;
    }
    if (mode<0) { SAY("Usage: wined3d-config.exe csmt [0|1|default]\n"); ExitProcess(2); }
    HANDLE library=LoadLibraryA("advapi32.dll"),key=0;
    LONG (CALL *open_key)(HANDLE,const char *,DWORD,DWORD,HANDLE *) =
        (void *)GetProcAddress(library,"RegOpenKeyExA");
    LONG (CALL *create_key)(HANDLE,const char *,DWORD,char *,DWORD,DWORD,void *,HANDLE *,DWORD *) =
        (void *)GetProcAddress(library,"RegCreateKeyExA");
    LONG (CALL *query)(HANDLE,const char *,DWORD *,DWORD *,void *,DWORD *) =
        (void *)GetProcAddress(library,"RegQueryValueExA");
    LONG (CALL *set)(HANDLE,const char *,DWORD,DWORD,const void *,DWORD) =
        (void *)GetProcAddress(library,"RegSetValueExA");
    LONG (CALL *remove)(HANDLE,const char *) = (void *)GetProcAddress(library,"RegDeleteValueA");
    LONG (CALL *close_key)(HANDLE) = (void *)GetProcAddress(library,"RegCloseKey");
    LONG (CALL *flush)(HANDLE) = (void *)GetProcAddress(library,"RegFlushKey");
    if (!open_key || !create_key || !query || !set || !remove || !close_key || !flush) {
        SAY("WINED3D-CONFIG missing registry exports\n"); ExitProcess(1);
    }
    HANDLE hkcu=(HANDLE)(__INTPTR_TYPE__)(int)0x80000001;
    const char *path="Software\\Wine\\Direct3D";
    LONG status;
    if (mode==1 || mode==2)
        status=create_key(hkcu,path,0,0,0,0x20003,0,&key,0);
    else
        status=open_key(hkcu,path,0,mode==0 ? 0x20019 : 0x20003,&key);
    if (status==2 && (mode==0 || mode==3)) {
        SAY("WINED3D-CONFIG csmt=default (WineD3D 3.8: 1)\n"); ExitProcess(0);
    }
    if (status) goto error;
    if (mode==1 || mode==2) {
        DWORD value=(DWORD)(mode-1);
        status=set(key,"csmt",0,4,&value,sizeof(value));
    } else if (mode==3) {
        status=remove(key,"csmt"); if (status==2) status=0;
    }
    if (status) goto error;
    if (mode!=0) { status=flush(key); if (status) goto error; }
    DWORD type=0,value=0,size=sizeof(value);
    status=query(key,"csmt",0,&type,&value,&size);
    if (status==2) {
        if (mode==1 || mode==2) goto error;
        SAY("WINED3D-CONFIG csmt=default (WineD3D 3.8: 1)\n");
    } else {
        if (status) goto error;
        if (type!=4 || size!=4 || mode==3 || ((mode==1 || mode==2) && value!=(DWORD)(mode-1))) {
            status=13; goto error;
        }
        SAY("WINED3D-CONFIG csmt="); VALUE(value);
    }
    close_key(key);
    ExitProcess(0);
error:
    SAY("WINED3D-CONFIG error="); VALUE(status);
    if (key) close_key(key);
    ExitProcess(1);
}
