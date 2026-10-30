/* MessageBeep ABI, asynchronous playback and independent PlaySound tests.
 * Native mode only calls the public API. --qemu changes the guest's sound
 * scheme and writes WAV fixtures: use it only on a disposable test image.
 * The runner must also inspect QEMU's captured PCM, not just these returns. */
typedef unsigned int UINT, DWORD;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef int BOOL, LONG;
typedef void *HANDLE;
typedef __UINTPTR_TYPE__ UINT_PTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
void *memset(void *p,int v,__SIZE_TYPE__ n){BYTE *d=p;while(n--)*d++=(BYTE)v;return p;}
void *memcpy(void *p,const void *s,__SIZE_TYPE__ n){BYTE *d=p;const BYTE *a=s;while(n--)*d++=*a++;return p;}

static BOOL (CALL *beep)(UINT);
static BOOL (CALL *play)(const char *,HANDLE,DWORD);
static void (CALL *sleep_ms)(DWORD);
static DWORD (CALL *ticks)(void);
static HANDLE (CALL *create_file)(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
static BOOL (CALL *close_handle)(HANDLE);
static LONG (CALL *reg_create)(HANDLE,const char *,DWORD,char *,DWORD,DWORD,void *,HANDLE *,DWORD *);
static LONG (CALL *reg_set)(HANDLE,const char *,DWORD,DWORD,const BYTE *,DWORD);
static LONG (CALL *reg_close)(HANDLE);
static unsigned checks,failures;
static BYTE wave_data[44+48000*2];
static const char *aliases[]={".Default","SystemHand","SystemQuestion","SystemExclamation","SystemAsterisk"};
static const char *files[]={"alert0.wav","alert1.wav","alert2.wav","alert3.wav","alert4.wav"};
static const UINT types[]={0,0x10,0x20,0x30,0x40};
static const UINT frequencies[]={400,600,800,1200,1600};
#pragma pack(push,1)
typedef struct {WORD tag,channels;DWORD rate,bytes;WORD align,bits,extra;} ALERT_FORMAT;
#pragma pack(pop)
typedef struct ALERT_HEADER {
    BYTE *data;DWORD length,recorded;UINT_PTR user;DWORD flags,loops;
    struct ALERT_HEADER *next;UINT_PTR reserved;
} ALERT_HEADER;
static BYTE silent_data[9600];
static ALERT_HEADER silent_header;

static void alert_report(const char *text)
{
    DWORD size=0,written;while(text[size])size++;
    WriteFile(GetStdHandle((DWORD)-11),text,size,&written,0);
}
static void alert_check(BOOL ok,const char *name)
{
    checks++;if(!ok)failures++;
    alert_report(ok?"ALERT PASS ":"ALERT FAIL ");alert_report(name);alert_report("\n");
}
static void *alert_resolve(HANDLE module,const char *name)
{
    void *p=GetProcAddress(module,name);
    if(!p){alert_report("ALERT FAIL export ");alert_report(name);alert_report("\n");ExitProcess(2);}
    return p;
}
static BOOL alert_has(const char *text,const char *word)
{
    for(;*text;text++){const char *a=text,*b=word;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}
    return 0;
}
static void alert_put16(BYTE *p,WORD v){p[0]=(BYTE)v;p[1]=(BYTE)(v>>8);}
static void alert_put32(BYTE *p,DWORD v){for(UINT i=0;i<4;i++)p[i]=(BYTE)(v>>(i*8));}
static DWORD alert_wave(UINT frequency,UINT duration)
{
    DWORD frames=48*duration,bytes=frames*2;
    memcpy(wave_data,"RIFF",4);alert_put32(wave_data+4,bytes+36);
    memcpy(wave_data+8,"WAVEfmt ",8);alert_put32(wave_data+16,16);
    alert_put16(wave_data+20,1);alert_put16(wave_data+22,1);
    alert_put32(wave_data+24,48000);alert_put32(wave_data+28,96000);
    alert_put16(wave_data+32,2);alert_put16(wave_data+34,16);
    memcpy(wave_data+36,"data",4);alert_put32(wave_data+40,bytes);
    for(UINT i=0;i<frames;i++) {
        int sample=((i*frequency)%48000)<24000?4000:-4000;
        alert_put16(wave_data+44+2*i,(WORD)sample);
    }
    return bytes+44;
}
static BOOL alert_file(const char *name,UINT frequency,UINT duration)
{
    DWORD bytes=alert_wave(frequency,duration),written=0;
    HANDLE file=create_file(name,0x40000000,0,0,2,0,0);
    if(!file||file==(HANDLE)(UINT_PTR)-1)return 0;
    BOOL ok=WriteFile(file,wave_data,bytes,&written,0)&&written==bytes;
    return close_handle(file)&&ok;
}
static BOOL alert_alias(const char *name,const char *value)
{
    const char prefix[]="AppEvents\\Schemes\\Apps\\.Default\\";
    const char suffix[]="\\.Current";
    char path[128];UINT n=0;
    for(UINT i=0;prefix[i];i++)path[n++]=prefix[i];
    for(UINT i=0;name[i];i++)path[n++]=name[i];
    for(UINT i=0;suffix[i];i++)path[n++]=suffix[i];path[n]=0;
    HANDLE key=0;DWORD disposition=0,size=0;while(value[size])size++;
    HANDLE hkcu=(HANDLE)(UINT_PTR)(__INTPTR_TYPE__)(int)0x80000001U;
    if(reg_create(hkcu,path,0,0,0,2,0,&key,&disposition)||!key)return 0;
    BOOL ok=reg_set(key,0,0,1,(const BYTE *)value,size+1)==0;
    return reg_close(key)==0&&ok;
}
static void alert_cue(UINT frequency)
{
    alert_wave(frequency,100);
    if(!play((const char *)wave_data,0,4|2)) {
        alert_report("ALERT FAIL audio cue\n");ExitProcess(2);
    }
    sleep_ms(350);
}
static BOOL alert_continuous_clock(HANDLE winmm)
{
    UINT (CALL *open)(HANDLE *,UINT,const ALERT_FORMAT *,UINT_PTR,UINT_PTR,DWORD)=alert_resolve(winmm,"waveOutOpen");
    UINT (CALL *prepare)(HANDLE,ALERT_HEADER *,UINT)=alert_resolve(winmm,"waveOutPrepareHeader");
    UINT (CALL *write)(HANDLE,ALERT_HEADER *,UINT)=alert_resolve(winmm,"waveOutWrite");
    ALERT_FORMAT format={1,1,48000,96000,2,16,0};HANDLE output=0;
    silent_header.data=silent_data;silent_header.length=sizeof(silent_data);
    silent_header.flags=12;silent_header.loops=0xffffffff;
    return open(&output,0xffffffff,&format,0,0,0)==0&&output&&
        prepare(output,&silent_header,sizeof(silent_header))==0&&
        write(output,&silent_header,sizeof(silent_header))==0;
}
void mainCRTStartup(void)
{
    HANDLE kernel=LoadLibraryA("kernel32.dll"),user=LoadLibraryA("user32.dll");
    beep=alert_resolve(user,"MessageBeep");
    sleep_ms=alert_resolve(kernel,"Sleep");ticks=alert_resolve(kernel,"GetTickCount");
    char *(CALL *command_line)(void)=alert_resolve(kernel,"GetCommandLineA");
    const char *command=command_line();
    if(!alert_has(command,"--qemu")) {
        const UINT values[]={0,0x10,0x20,0x30,0x40,0xffffffff,0x24,0x60,0x12345678};
        for(UINT i=0;i<sizeof(values)/sizeof(values[0]);i++) {
            alert_check(beep(values[i])!=0,"native alert type");sleep_ms(250);
        }
        alert_report(failures?"ALERT FAILED\n":"ALERT DONE\n");ExitProcess(failures?1:0);
    }

    HANDLE winmm=LoadLibraryA("winmm.dll"),registry=LoadLibraryA("advapi32.dll");
    play=alert_resolve(winmm,"PlaySoundA");
    reg_create=alert_resolve(registry,"RegCreateKeyExA");reg_set=alert_resolve(registry,"RegSetValueExA");
    reg_close=alert_resolve(registry,"RegCloseKey");
    create_file=alert_resolve(kernel,"CreateFileA");close_handle=alert_resolve(kernel,"CloseHandle");
    if(alert_has(command,"--noaudio")) {
        UINT (CALL *devices)(void)=alert_resolve(winmm,"waveOutGetNumDevs");
        alert_check(alert_alias("SystemAsterisk",files[4])&&devices()==0,"no output device");
        SetLastError(0x4567);alert_check(!beep(0xffffffff)&&GetLastError()==50,"simple beep reports absent backend");
        SetLastError(0x4567);alert_check(!beep(0x40)&&GetLastError()==50,"alert reports absent backend");
        alert_check(alert_alias("SystemAsterisk",""),"disable alert in guest");
        alert_check(beep(0x40),"disabled alert needs no backend");
        alert_report(failures?"ALERT FAILED\n":"ALERT DONE\n");ExitProcess(failures?1:0);
    }

    if(alert_has(command,"--observe")) {
        if(!alert_continuous_clock(winmm)){alert_report("ALERT FAIL observer\n");ExitProcess(2);}
        sleep_ms(1200);alert_report("ALERT OBSERVER DONE\n");ExitProcess(0);
    }
    alert_check(alert_continuous_clock(winmm),"continuous silent capture clock");
    BOOL prepared=1;
    for(UINT i=0;i<5;i++)prepared=alert_file(files[i],frequencies[i],200)&&alert_alias(aliases[i],files[i])&&prepared;
    alert_check(prepared,"guest sound scheme prepared");
    alert_cue(2400);
    for(UINT i=0;i<5;i++) {
        alert_check(beep(types[i]),aliases[i]);sleep_ms(550);
    }
    alert_check(alert_alias("SystemHand","missing-alert.wav")&&beep(0x10),"missing event uses default");sleep_ms(550);
    alert_check(alert_alias(".Default","missing-default.wav")&&beep(0x10),"missing default uses simple tone");sleep_ms(550);
    alert_check(beep(0xffffffff),"explicit simple tone");sleep_ms(550);
    alert_cue(2400);
    alert_check(alert_alias("SystemExclamation","")&&beep(0x30),"empty event remains silent");sleep_ms(550);
    alert_cue(2600);
    alert_check(alert_alias(".Default","")&&beep(0x10)&&beep(0),"empty default remains silent");sleep_ms(550);
    alert_cue(2800);

    alert_check(alert_alias(".Default",files[0])&&alert_alias("SystemHand",files[1]),"restore default and hand");
    alert_wave(300,100);
    alert_check(play((const char *)wave_data,0,4|1|8|2),"start looping application audio");sleep_ms(250);
    alert_check(beep(0x40),"alert during application audio");sleep_ms(350);
    alert_check(!play((const char *)wave_data,0,4|1|8|2|0x10),"alert does not stop application audio");sleep_ms(100);
    alert_check(play(0,0,0),"stop application audio");sleep_ms(350);

    alert_check(alert_file("long-alert.wav",3200,800)&&alert_alias("SystemAsterisk","long-alert.wav"),"long alert fixture");
    DWORD before=ticks();BOOL started=beep(0x40);DWORD elapsed=ticks()-before;
    alert_check(started&&elapsed<500,"alert returns before 800ms sound completes");sleep_ms(100);
    alert_check(play(0,0,0),"PlaySound stop during alert");sleep_ms(1000);
    alert_cue(3600);

    BOOL burst=1;for(UINT i=0;i<80;i++)burst=beep(0x10)&&burst;
    alert_check(burst,"alert replacement reuses voice capacity");sleep_ms(550);
    alert_cue(3800);

    /* Exit while both kinds of sound are active. The capture must become
     * silent again; application-owned data and the dispatcher cannot leak. */
    alert_check(alert_file("exit-alert.wav",3500,1000)&&alert_alias("SystemAsterisk","exit-alert.wav"),"exit cleanup fixture");
    alert_wave(300,100);
    alert_check(play((const char *)wave_data,0,4|1|8|2)&&beep(0x40),"queue both voices before process exit");
    alert_report(failures?"ALERT FAILED\n":"ALERT DONE\n");ExitProcess(failures?1:0);
}
