typedef unsigned int UINT;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API UINT CALL GetLastError(void);
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("SDLRUN PASS " name "\n");else{SAY("SDLRUN FAIL " name "\n");failures++;}}while(0)
typedef void (*version_fn)(unsigned char *);
typedef int (*count_fn)(void);
typedef const char *(*name_fn)(int);
typedef int (*init_fn)(UINT);
typedef UINT (*was_fn)(UINT);
typedef void (*quit_fn)(void);
typedef const char *(*error_fn)(void);
void CALL sdl_runtime_entry(void)
{
    SAY("SDLRUN START\n");
    void *sdl=LoadLibraryA("SDL2.dll");
    CHECK(sdl!=0,"load-original-sdl2");
    if(!sdl)ExitProcess(1);
    version_fn version=(version_fn)GetProcAddress(sdl,"SDL_GetVersion");
    count_fn videos=(count_fn)GetProcAddress(sdl,"SDL_GetNumVideoDrivers");
    name_fn video=(name_fn)GetProcAddress(sdl,"SDL_GetVideoDriver");
    count_fn audios=(count_fn)GetProcAddress(sdl,"SDL_GetNumAudioDrivers");
    init_fn init=(init_fn)GetProcAddress(sdl,"SDL_Init");
    was_fn was=(was_fn)GetProcAddress(sdl,"SDL_WasInit");
    quit_fn quit=(quit_fn)GetProcAddress(sdl,"SDL_Quit");
    error_fn error=(error_fn)GetProcAddress(sdl,"SDL_GetError");
    CHECK(version&&videos&&video&&audios&&init&&was&&quit&&error,"runtime-exports");
    if(failures)ExitProcess(1);
    struct{unsigned char v[3];volatile unsigned char guard;}ver={{0,0,0},0x65};
    version(ver.v);CHECK(ver.v[0]==2&&ver.guard==0x65,"version-and-abi");
    CHECK(videos()>0&&video(0)&&video(0)[0],"video-driver-list");
    CHECK(audios()>0,"audio-driver-list");
    int result=init(1); /* SDL_INIT_TIMER, no hidden graphics success. */
    if(result<0){const char *s=error();UINT length=0,n;while(s&&s[length])length++;if(length)WriteFile(GetStdHandle((UINT)-11),s,length,&n,0);SAY("\n");}
    CHECK(result==0&&(was(1)&1),"initialize-timer");
    quit();CHECK(was(0)==0,"quit-subsystems");
    if(!failures)SAY("SDLRUN DONE\n");
    ExitProcess(failures?1:0);
}
