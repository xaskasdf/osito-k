/* GetDeviceID by name and ordinal, with hardware-dependent endpoint checks. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
void *memset(void *out,int value,UP bytes)
{unsigned char *p=out;for(UP i=0;i<bytes;i++)p[i]=(unsigned char)value;return out;}
void *memcpy(void *out,const void *in,UP bytes)
{unsigned char *p=out;const unsigned char *q=in;for(UP i=0;i<bytes;i++)p[i]=q[i];return out;}
typedef struct {DWORD a;WORD b,c;unsigned char d[8];} GUID;
typedef struct {GUID ids[32];DWORD count,calls,stop;} ENUM;
static unsigned failures;
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
static void dsound_id_check(int ok)
{if(ok)SAY("DSOUNDID PASS\n");else{failures++;SAY("DSOUNDID FAIL\n");}}
static int dsound_id_equal(const GUID *a,const GUID *b)
{const unsigned char *x=(const unsigned char *)a,*y=(const unsigned char *)b;for(unsigned i=0;i<16;i++)if(x[i]!=y[i])return 0;return 1;}
static void dsound_id_row(DWORD id,DWORD hr,DWORD a,DWORD b)
{
    char row[64]="DSOUNDID ROW ";DWORD v[]={id,hr,a,b},at=13,wr;
    for(unsigned i=0;i<4;i++){for(int j=7;j>=0;j--)row[at++]="0123456789abcdef"[(v[i]>>(j*4))&15];row[at++]=' ';}
    row[at++]='\n';WriteFile(GetStdHandle((DWORD)-11),row,at,&wr,0);
}
static int CALL dsound_id_enumerate(GUID *id,const void *description,const void *module,ENUM *state)
{
    (void)description;(void)module;state->calls++;
    if(id&&state->count<32)state->ids[state->count++]=*id;
    return !state->stop;
}
void dsound_id_entry(void)
{
    void *dll=LoadLibraryA("dsound.dll");
    DWORD (CALL *getid)(const GUID *,GUID *)=GetProcAddress(dll,"GetDeviceID");
    DWORD (CALL *ordinal)(const GUID *,GUID *)=GetProcAddress(dll,(const char *)(UP)9);
    DWORD (CALL *enumerate)(void *,void *)=GetProcAddress(dll,"DirectSoundEnumerateA");
    DWORD (CALL *enumerate_w)(void *,void *)=GetProcAddress(dll,"DirectSoundEnumerateW");
    DWORD (CALL *create)(const GUID *,void **,void *)=GetProcAddress(dll,"DirectSoundCreate");
    dsound_id_check(getid&&ordinal&&enumerate&&enumerate_w&&create);if(failures)ExitProcess(1);
    const GUID inputs[]={{0},{0x12345678,0x3456,0x5678,{1,2,3,4,5,6,7,8}}};
    for(unsigned i=0;i<2;i++){
        struct {DWORD before;GUID value;DWORD after;} out={0x11223344,{0},0x55667788};
        memset(&out.value,0xa5,sizeof(out.value));GUID initial=out.value;
        DWORD hr=getid(&inputs[i],&out.value);int equal=dsound_id_equal(&initial,&out.value);
        dsound_id_check(hr==0x88780078&&equal);dsound_id_check(out.before==0x11223344&&out.after==0x55667788);
        GUID alias=inputs[i];DWORD other=ordinal(&alias,&alias);int same=dsound_id_equal(&alias,&inputs[i]);
        dsound_id_check(other==hr&&same);dsound_id_row(i,hr,equal,same);
    }
    GUID target=inputs[1];DWORD hr=getid(0,&target);dsound_id_check(hr==0x80070057&&dsound_id_equal(&target,&inputs[1]));dsound_id_row(10,hr,dsound_id_equal(&target,&inputs[1]),0);
    hr=getid(&inputs[1],0);dsound_id_check(hr==0x80070057);dsound_id_row(11,hr,0,0);
    hr=getid(0,0);dsound_id_check(hr==0x80070057);dsound_id_row(12,hr,0,0);
    ENUM devices={0},wide={0},stopped={0};stopped.stop=1;
    dsound_id_check(enumerate(dsound_id_enumerate,&devices)==0);
    dsound_id_check(enumerate_w(dsound_id_enumerate,&wide)==0&&wide.calls==devices.calls&&wide.count==devices.count);
    dsound_id_check(enumerate(dsound_id_enumerate,&stopped)==0&&stopped.calls==(devices.calls?1:0));
    for(unsigned i=0;i<4;i++){
        GUID special={0xdef00000+i,0x9c6d,0x47ed,{0xaa,0xf1,0x4d,0xda,0x8f,0x2b,0x5c,3}};
        GUID out=inputs[1],other=inputs[1];hr=getid(&special,&out);DWORD hr2=ordinal(&special,&other);
        int valid=hr==0||hr==0x88780078;
        dsound_id_check(valid);dsound_id_check(hr==hr2&&dsound_id_equal(&out,&other));
        int mapped=hr?1:!dsound_id_equal(&out,&special)&&!dsound_id_equal(&out,&inputs[0]);
        dsound_id_check(mapped);dsound_id_row(20+i,0,valid,mapped);
        SAY("DSOUNDID DEVICE ");dsound_id_row(i,hr,out.a,0);
        GUID resolved=out;DWORD repeat=hr?hr:getid(&resolved,&resolved);
        dsound_id_check(hr?dsound_id_equal(&out,&inputs[1]):repeat==0&&dsound_id_equal(&resolved,&out));
        if(!(i&1)){
            int found=hr?devices.calls==0:0;
            for(unsigned j=0;j<devices.count;j++)if(dsound_id_equal(&out,&devices.ids[j]))found=1;
            dsound_id_check(found);
            void *object=0;DWORD created=hr?hr:create(&out,&object,0);
            dsound_id_check(hr?created==0x88780078:created==0&&object);
            if(!created&&object){void **vtable=*(void ***)object;((DWORD(CALL *)(void *))vtable[2])(object);}
        }
    }
    if(!failures)SAY("DSOUNDID DONE\n");ExitProcess(failures?1:0);
}
