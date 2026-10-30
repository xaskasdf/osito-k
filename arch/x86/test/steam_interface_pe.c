/* Diagnostic for the original 32-bit Steam client used by Braid.
 * SteamClient014 slots 0/1 are the pipe create/release calls used by the
 * game's original steam_api.dll. No user/session/ticket API is requested. */
typedef unsigned int DWORD;
typedef unsigned char BYTE;
#define CALL __attribute__((stdcall))
#define THIS __attribute__((thiscall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API int CALL FreeLibrary(void *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
#ifdef STEAMIFACE_START_CLIENT
API int CALL CreateProcessA(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,void *);
API void *CALL OpenEventA(DWORD,int,const char *);
API int CALL CloseHandle(void *);
API int CALL GetExitCodeProcess(void *,DWORD *);
API void CALL Sleep(DWORD);
#endif
static DWORD checks,failures;
static void say(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v){char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;say(s);}
static void check(int ok,const char *label){checks++;if(!ok){failures++;say("STEAMIFACE FAIL ");say(label);say("\n");}}
void steam_interface_entry(void)
{
#ifdef STEAMIFACE_START_CLIENT
    /* Only the disposable QEMU fixture builds this variant. Its Steam tree
     * contains runtime files, with no copied account/profile credentials. */
    DWORD startup[17]={68};
    struct {void *process,*thread;DWORD pid,tid;} child={0};
    say("STEAMIFACE START_CLIENT\n");
    int started=CreateProcessA("C:\\Steam\\steam.exe",0,0,0,0,0,0,"C:\\Steam",startup,&child);
    check(started,"create original Steam client");
    say("STEAMIFACE CLIENT_PID=");hex(child.pid);say(" error=");hex(GetLastError());say("\n");
    void *master=0;
    if(started){
        for(unsigned attempt=0;attempt<240;attempt++){
            master=OpenEventA(0x00100000,0,"steam3master_sharedmemlock");
            if(master)break;
            DWORD code=259;
            if(GetExitCodeProcess(child.process,&code)&&code!=259){
                say("STEAMIFACE CLIENT_EXIT=");hex(code);say("\n");break;
            }
            Sleep(500);
        }
        CloseHandle(child.thread);CloseHandle(child.process);
    }
    check(master!=0,"Steam master event available");
    if(master){say("STEAMIFACE MASTER_EVENT_AVAILABLE\n");CloseHandle(master);}
#endif
    const char *names[]={"tier0_s.dll","vstdlib_s.dll","steamclient.dll"};
    void *modules[3]={0,0,0};
    for(unsigned i=0;i<3;i++){
        say("STEAMIFACE LOADING ");say(names[i]);say("\n");
        modules[i]=LoadLibraryA(names[i]);
        check(modules[i]!=0,"original DLL load");
        if(!modules[i]){say("STEAMIFACE LOAD_ERROR=");hex(GetLastError());say("\n");break;}
    }
    if(modules[2]){
        void *(*factory)(const char *,int *)=GetProcAddress(modules[2],"CreateInterface");
        check(factory!=0,"CreateInterface export");
        if(factory){
            int result=0x12345678;
            void *missing=factory("OsitoMissingInterface001",&result);
            check(missing==0,"unknown interface rejected");check(result==1,"unknown interface status");
            result=0x12345678;
            say("STEAMIFACE CREATING SteamClient014\n");
            void *client=factory("SteamClient014",&result);
            say("STEAMIFACE CLIENT=");hex((DWORD)(__UINTPTR_TYPE__)client);say(" status=");hex((DWORD)result);say("\n");
            check(client!=0,"SteamClient014 available");check(result==0,"SteamClient014 status");
            if(client){
                void **table=*(void ***)client;
                check(table!=0&&table[0]!=0&&table[1]!=0,"client pipe methods present");
                if(table&&table[0]&&table[1]){
                    int (THIS *create_pipe)(void *)=(void *)table[0];
                    BYTE (THIS *release_pipe)(void *,int)=(void *)table[1];
                    say("STEAMIFACE CREATE_PIPE\n");
                    int pipe=create_pipe(client);
                    say("STEAMIFACE PIPE=");hex((DWORD)pipe);say("\n");
#ifdef STEAMIFACE_START_CLIENT
                    check(pipe!=0,"connect to original Steam client");
#endif
                    /* No running Steam client is installed in the QEMU fixture;
                     * a zero pipe is an availability result, not proof of IPC. */
                    if(pipe){
                        say("STEAMIFACE RELEASE_PIPE\n");
                        check(release_pipe(client,pipe)!=0,"release created pipe");
                    }
                }
            }
        }
    }
    for(unsigned i=3;i>0;i--)if(modules[i-1]){
        say("STEAMIFACE UNLOAD ");say(names[i-1]);say("\n");
        check(FreeLibrary(modules[i-1])!=0,"unload original DLL");
    }
    say("STEAMIFACE DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
