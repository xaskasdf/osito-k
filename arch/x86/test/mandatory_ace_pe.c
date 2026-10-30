/* Caller-owned ACL construction, unchanged on Windows and OsitoK. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API BOOL CALL AddMandatoryAce(void *,DWORD,DWORD,DWORD,void *);

void *memcpy(void *out,const void *in,__SIZE_TYPE__ n)
{ for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out; }
void *memset(void *out,int value,__SIZE_TYPE__ n)
{ for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)value;return out; }

typedef struct { BYTE revision,reserved;WORD size,count,reserved2; } ACL;
typedef struct { BYTE revision,count,authority[6];DWORD sub[16]; } SID;
static DWORD storage[256],before[256],expected[256];
static SID sid;
static ACL *acl=(ACL *)((BYTE *)storage+4);
static DWORD failures,cases;
static BOOL (CALL *init_acl)(void *,DWORD,DWORD);
static BOOL (CALL *add)(void *,DWORD,DWORD,DWORD,void *);
static BOOL (CALL *get_ace)(void *,DWORD,void **);
static BOOL (CALL *get_info)(void *,void *,DWORD,DWORD);

static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char b[9];for(DWORD i=0;i<8;i++)b[i]="0123456789abcdef"[(n>>(28-4*i))&15];b[8]=0;report(b); }
static void check(BOOL ok,const char *label)
{
    if(ok)return;
    failures++;report("MANDATORY-ACE FAIL: ");report(label);
    report(" case=");hex(cases);report(" error=");hex(GetLastError());report("\n");
}
static BOOL same(const void *a,const void *b,DWORD n)
{ for(DWORD i=0;i<n;i++)if(((const BYTE *)a)[i]!=((const BYTE *)b)[i])return 0;return 1; }
static void setup(DWORD size,DWORD revision)
{
    memset(storage,0xcc,sizeof(storage));memset(&sid,0,sizeof(sid));
    sid.revision=1;sid.count=1;sid.authority[5]=16;sid.sub[0]=0x2000;
    check(init_acl(acl,512,2),"InitializeAcl");
    acl->size=(WORD)size;acl->revision=(BYTE)revision;
}
static void run(const char *name,DWORD revision,DWORD flags,DWORD policy,
                void *label,DWORD error,DWORD used)
{
    SID captured;
    memcpy(&captured,label,8);
    DWORD sid_length=8+4*captured.count;
    if(sid_length<=sizeof(captured))memcpy(&captured,label,sid_length);
    memcpy(before,storage,sizeof(storage));memcpy(expected,storage,sizeof(storage));
    if(!error) {
        ACL *want=(ACL *)((BYTE *)expected+4);
        BYTE *ace=(BYTE *)want+used;
        ace[0]=17;ace[1]=(BYTE)flags;*(WORD *)(ace+2)=(WORD)(8+sid_length);
        *(DWORD *)(ace+4)=policy;memcpy(ace+8,&captured,sid_length);
        want->count++;if(revision>want->revision)want->revision=(BYTE)revision;
    }
    SetLastError(0x1234);
    BOOL ok=(cases++&1)?add(acl,revision,flags,policy,label):
                       AddMandatoryAce(acl,revision,flags,policy,label);
    DWORD got=GetLastError();
    if(ok!=(error==0)||got!=(error?error:0x1234)) {
        report("case: ");report(name);report(" expected error=");hex(error);report("\n");
        check(0,"result and LastError");
    }
    check(same(storage,expected,sizeof(storage)),"exact ACL, existing data and guards");
    if(!error&&ok) {
        DWORD info[3]={0},out_revision=0;
        check(get_info(acl,info,sizeof(info),2)&&info[0]==acl->count&&
              info[1]==used+8+sid_length&&info[2]==acl->size-info[1],"ACL size information");
        check(get_info(acl,&out_revision,4,1)&&out_revision==acl->revision,"ACL revision information");
        void *last=0;
        check(get_ace(acl,acl->count-1,&last)&&last==(BYTE *)acl+used,"GetAce append address");
    }
}
static void existing(BYTE type,DWORD size,DWORD revision,DWORD sid_offset,DWORD count)
{
    setup(512,revision);acl->count=1;
    BYTE *ace=(BYTE *)acl+8;
    memset(ace,0,size);ace[0]=type;*(WORD *)(ace+2)=(WORD)size;
    ace[sid_offset]=1;ace[sid_offset+1]=(BYTE)count;ace[sid_offset+7]=5;
}
void mainCRTStartup(void)
{
    HANDLE lib=LoadLibraryA("advapi32.dll");
    init_acl=(void *)GetProcAddress(lib,"InitializeAcl");
    add=(void *)GetProcAddress(lib,"AddMandatoryAce");
    get_ace=(void *)GetProcAddress(lib,"GetAce");
    get_info=(void *)GetProcAddress(lib,"GetAclInformation");
    if(!init_acl||!add||!get_ace||!get_info) {
        report("MANDATORY-ACE FAIL: missing export\n");ExitProcess(1);
    }
    for(DWORD size=0;size<=32;size++) {
        setup(size,2);run("buffer size",2,0,1,&sid,size<8?1336:size<28?1344:0,8);
    }
    for(DWORD rev=0;rev<=6;rev++) {
        setup(512,2);run("requested revision",rev,0,1,&sid,rev>4?1306:0,8);
        setup(512,rev);run("ACL revision",2,0,1,&sid,rev>4?1306:rev<2?1336:0,8);
    }
    for(DWORD policy=0;policy<16;policy++) {
        setup(512,2);run("policy bits",2,0,policy,&sid,policy>7?87:0,8);
    }
    for(DWORD bit=0;bit<32;bit++) {
        setup(512,2);run("flag bits",2,1U<<bit,1,&sid,bit>=5?87:0,8);
    }
    setup(512,2);run("all supported bits",4,31,7,&sid,0,8);
    for(DWORD rev=0;rev<3;rev++) {
        setup(512,2);sid.revision=(BYTE)rev;
        run("SID revision",2,0,1,&sid,rev==1?0:1337,8);
    }
    for(DWORD count=0;count<=16;count++) {
        setup(512,2);sid.count=(BYTE)count;
        for(DWORD i=0;i<count;i++)sid.sub[i]=0x1000+i;
        run("SID count",2,0,1,&sid,count>15?1337:0,8);
    }
    const DWORD rids[]={0,0x1000,0x2000,0x2100,0x3000,0x4000,0xffffffff};
    for(DWORD i=0;i<sizeof(rids)/sizeof(rids[0]);i++) {
        setup(512,2);sid.sub[0]=rids[i];run("integrity RID",2,0,1,&sid,0,8);
    }
    for(DWORD i=0;i<6;i++) {
        setup(512,2);sid.authority[i]=5;run("label authority",2,0,1,&sid,87,8);
    }
    setup(0,0);sid.revision=0;run("SID error precedes others",99,0x80,8,&sid,1337,8);
    setup(0,0);run("requested revision precedes flags",99,0x80,8,&sid,1306,8);
    setup(0,5);run("ACL revision precedes flags",2,0x80,8,&sid,1306,8);
    setup(0,0);run("flags precede ACL",2,0x80,8,&sid,87,8);
    setup(512,2);sid.authority[5]=5;
    run("authority precedes revision",99,0,1,&sid,87,8);

    const BYTE basic[]={0,1,2,3,9,10,13,14,17,19,20};
    for(DWORD i=0;i<sizeof(basic);i++) {
        existing(basic[i],20,2,8,1);
        run("preserve basic ACE",2,16,2,&sid,0,28);
        existing(basic[i],16,2,8,0);
        run("zero-subauthority ACE",2,0,0,&sid,0,24);
        existing(basic[i],20,2,8,1);((BYTE *)acl)[16]=0;
        run("invalid existing SID",2,0,1,&sid,1336,28);
        existing(basic[i],20,2,8,16);
        run("oversized existing SID",2,0,1,&sid,1336,28);
    }
    for(DWORD size=0;size<20;size++) {
        existing(17,20,2,8,1);*(WORD *)((BYTE *)acl+10)=(WORD)size;
        run("truncated ACE",2,0,1,&sid,1336,8+size);
    }
    existing(17,20,2,8,1);*(WORD *)((BYTE *)acl+10)=21;
    run("unaligned ACE",2,0,1,&sid,1336,29);
    existing(17,20,2,8,1);acl->size=27;
    run("ACE beyond ACL",2,0,1,&sid,1336,28);
    setup(512,2);acl->count=65535;
    run("invalid ACE count",2,0,1,&sid,1336,8);
    const BYTE objects[]={5,6,7,8,11,12,15,16};
    for(DWORD i=0;i<sizeof(objects);i++)for(DWORD flags=0;flags<4;flags++) {
        DWORD offset=12+16*((flags&1)!=0)+16*((flags&2)!=0);
        existing(objects[i],offset+12,4,offset,1);
        *(DWORD *)((BYTE *)acl+16)=flags;
        run("object ACE with optional GUIDs",2,0,1,&sid,0,20+offset);
        existing(objects[i],24,3,12,1);
        run("object ACE requires DS revision",2,0,1,&sid,1336,32);
    }
    existing(4,36,3,12,1);
    BYTE *compound=(BYTE *)acl+8;
    *(WORD *)(compound+8)=1;compound[24]=1;compound[25]=1;compound[31]=5;
    run("compound ACE preserved",2,0,1,&sid,0,44);
    existing(21,28,2,8,1);
    *(DWORD *)((BYTE *)acl+28)=0x78747261;
    run("conditional filter preserved",2,0,1,&sid,0,36);
    setup(512,2);acl->count=1;
    BYTE *opaque=(BYTE *)acl+8;
    opaque[0]=22;opaque[1]=0;*(WORD *)(opaque+2)=4;
    run("opaque future ACE preserved",2,0,1,&sid,0,12);

    setup(512,2);run("first label",2,0,0,&sid,0,8);
    run("SID aliases existing ACL",2,1,1,(BYTE *)acl+16,0,28);
    for(DWORD i=2;i<25;i++)run("repeated append",2,i&31,i&7,&sid,0,8+20*i);
    run("full ACL is unchanged",2,0,1,&sid,1344,508);
    for(DWORD i=0;i<256;i++) {
        setup(28,2);run("repeated exact allocation",2,0,0,&sid,0,8);
        run("repeated full ACL",2,0,0,&sid,1344,28);
    }
    report(failures?"MANDATORY-ACE FAILED\n":"MANDATORY-ACE PASS\n");
    ExitProcess(failures?1:0);
}
