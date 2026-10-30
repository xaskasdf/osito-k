/* Native/guest SetupIterateCabinet oracle. Extracted payloads are never run. */
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
#if __SIZEOF_POINTER__ == 4
#pragma pack(push,1)
#endif
typedef struct {const void *name;DWORD size,error;WORD date,time,attributes;char target[260];} INFOA;
typedef struct {const void *name;DWORD size,error;WORD date,time,attributes;WORD target[260];} INFOW;
typedef struct {const void *path,*file,*disk;WORD set,number;} CABINFO;
typedef struct {const void *target,*source;DWORD error,flags;} PATHS;
#if __SIZEOF_POINTER__ == 4
#pragma pack(pop)
#endif
static int (CALL *iterateA)(const char *,DWORD,void *,void *);
static int (CALL *iterateW)(const WORD *,DWORD,void *,void *);
static DWORD (CALL *error)(void), (CALL *attributes)(const char *);
static void (CALL *seterror)(DWORD);
static int (CALL *deletefile)(const char *), (CALL *setattr)(const char *,DWORD);
static void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static void *(CALL *createW)(const WORD *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static int (CALL *closefile)(void *), (CALL *readfile)(void *,void *,DWORD,DWORD *,void *);
static unsigned failures,wide,mode,incount,donecount,cabcount,caseid;
#define SAY(s) do{DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do{char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
#define TEXT(s,w) do{const void *str=(s);if(str){for(unsigned z=0;z<1024;z++){WORD ch=(w)?((const WORD *)str)[z]:((const unsigned char *)str)[z];if(!ch)break;char c=(char)ch;DWORD wr;WriteFile(GetStdHandle((DWORD)-11),&c,1,&wr,0);}}}while(0)
static void cabinet_probe_check(int ok,const char *name)
{
    if(ok)SAY("CABINET PASS ");else{failures++;SAY("CABINET FAIL ");}
    TEXT(name,0);SAY("\n");
}
static DWORD CALL cabinet_probe_callback(void *context,DWORD notify,UP param1,UP param2)
{
    cabinet_probe_check(context==(void *)(UP)0xabc123,"callback-context");
    SAY("CABINET NOTIFY ");HEX(wide);HEX(caseid);HEX(notify);
    if(notify==16){
        CABINFO *c=(void *)param1;cabcount++;HEX(c->set);HEX(c->number);
        TEXT(c->path,wide);SAY("|");TEXT(c->file,wide);SAY("|");TEXT(c->disk,wide);SAY("\n");
        return mode==8?123:0;
    }
    if(notify==17){
        INFOA *a=(void *)param1;INFOW *w=(void *)param1;incount++;
        HEX(a->size);HEX(a->error);HEX(a->date);HEX(a->time);HEX(a->attributes);
        TEXT(a->name,wide);SAY("|");TEXT((void *)param2,wide);SAY("\n");
        if(incount==1)cabinet_probe_check(wide?!w->target[0]:!a->target[0],"first-target-initially-empty");
        if(mode==0||mode==8)return 2;
        if(mode==2)return 0;
        if(mode==3){a->error=123;return 0;}
        if(mode==4)return 7;
        if(mode==5)return 1;
        char target[]="cab-out0.bin";target[7]=(char)('0'+incount-1);
        const char *name=mode==6?"cab-absent-directory\\out.bin":target;
        for(unsigned i=0;;i++){if(wide)w->target[i]=(unsigned char)name[i];else a->target[i]=name[i];if(!name[i])break;}
        return 1;
    }
    if(notify==19){
        PATHS *p=(void *)param1;donecount++;HEX(p->error);HEX(p->flags);HEX(param2);
        TEXT(p->target,wide);SAY("|");TEXT(p->source,wide);SAY("\n");
        return mode==7?123:0;
    }
    HEX(param1);HEX(param2);SAY("\n");return 0;
}
static void cabinet_probe_case(const char *file,unsigned which,unsigned action,DWORD reserved)
{
    caseid=which;mode=action;incount=donecount=cabcount=0;
    for(unsigned i=0;i<3;i++){char name[]="cab-out0.bin";name[7]=(char)('0'+i);setattr(name,0x80);deletefile(name);}
    WORD path[260];unsigned n=0;for(;file[n];n++)path[n]=(unsigned char)file[n];path[n]=0;
    seterror(0x13579);
    int rc=wide?iterateW(path,reserved,(void *)cabinet_probe_callback,(void *)(UP)0xabc123):
                iterateA(file,reserved,(void *)cabinet_probe_callback,(void *)(UP)0xabc123);
    DWORD last=error();
    static const DWORD expected_error[16]={0,0,0,123,995,3,3,123,123,87,2,0,0,0,13,0};
    static const unsigned expected_files[16]={3,3,1,1,1,1,1,1,0,0,0,3,3,3,1,3};
    static const unsigned expected_done[16]={0,3,0,0,0,0,0,1,0,0,0,0,3,0,1,3};
    cabinet_probe_check((rc!=0)==(expected_error[which]==0)&&last==expected_error[which],"result-and-error");
    cabinet_probe_check(incount==expected_files[which]&&donecount==expected_done[which]&&cabcount==(which==9||which==10?0U:1U),"notification-counts");
    SAY("CABINET RESULT ");HEX(wide);HEX(which);HEX(rc);HEX(last);HEX(cabcount);HEX(incount);HEX(donecount);SAY("\n");
    for(unsigned i=0;i<3;i++){
        char name[]="cab-out0.bin";name[7]=(char)('0'+i);DWORD attr=attributes(name);
        if(which==1||which==12||which==15)cabinet_probe_check(attr!=0xffffffffU,"extracted-file-exists");
        if(which==14)cabinet_probe_check(attr==0xffffffffU,"failed-extraction-removes-target");
        if(attr==0xffffffffU)continue;
        cabinet_probe_check(attr==0x20,"extracted-archive-attribute");
        void *f=create(name,0x80000000,7,0,3,0x80,0);DWORD size=0,hash=2166136261U;
        if(f!=(void *)(UP)-1){unsigned char buf[1024];DWORD got=0;while(readfile(f,buf,sizeof(buf),&got,0)&&got){size+=got;for(DWORD j=0;j<got;j++)hash=(hash^buf[j])*16777619U;}closefile(f);}
        if((which==1||which==12)&&i<3){
            static const DWORD expected_size[3]={14,0,70000};
            static const DWORD expected_hash[3]={0x23e1760f,0x811c9dc5,0xe32e51f5};
            cabinet_probe_check(size==expected_size[i]&&hash==expected_hash[i],"stored-mszip-content");
        }
        if(which==15){
            static const DWORD expected_size[3]={177152,12848,66865};
            static const DWORD expected_hash[3]={0xe2f6f3f8,0x1a1edede,0x140baaaf};
            cabinet_probe_check(size==expected_size[i]&&hash==expected_hash[i],"original-cab-content");
        }
        SAY("CABINET FILE ");HEX(wide);HEX(which);HEX(i);HEX(size);HEX(hash);HEX(attr);SAY("\n");
    }
}
void cabinet_probe_entry(void)
{
    void *s=LoadLibraryA("setupapi.dll"),*k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,mod,name) dst=(__typeof__(dst))GetProcAddress(mod,name)
    LOAD(iterateA,s,"SetupIterateCabinetA");LOAD(iterateW,s,"SetupIterateCabinetW");
    LOAD(error,k,"GetLastError");LOAD(seterror,k,"SetLastError");LOAD(attributes,k,"GetFileAttributesA");
    LOAD(deletefile,k,"DeleteFileA");LOAD(setattr,k,"SetFileAttributesA");LOAD(create,k,"CreateFileA");
    LOAD(createW,k,"CreateFileW");
    LOAD(closefile,k,"CloseHandle");LOAD(readfile,k,"ReadFile");
    cabinet_probe_check(iterateA&&iterateW&&error&&seterror&&attributes&&deletefile&&setattr&&create&&createW&&closefile&&readfile,"exports");
    if(failures)ExitProcess(1);
    for(wide=0;wide<2;wide++){
        for(unsigned missing_parent=0;missing_parent<2;missing_parent++){
            const char *name=missing_parent?"cab-absent-directory\\missing.tmp":"cab-absent-file.tmp";
            WORD path[260];unsigned n=0;while(name[n]){path[n]=(WORD)name[n];n++;}path[n]=0;
            for(DWORD disposition=1;disposition<=5;disposition++){
                if(!missing_parent&&disposition!=3&&disposition!=5)continue;
                void *handle=wide?createW(path,0xc0000000U,7,0,disposition,0x80,0):
                                  create(name,0xc0000000U,7,0,disposition,0x80,0);
                DWORD last=error();
                cabinet_probe_check(handle==(void *)(UP)-1&&last==(missing_parent?3U:2U),"file-versus-path-not-found");
                if(handle!=(void *)(UP)-1)closefile(handle);
            }
        }
        for(unsigned i=0;i<9;i++)cabinet_probe_case("stored.cab",i,i,0);
        cabinet_probe_case("stored.cab",9,0,0x12345678);
        cabinet_probe_case("missing-cab.cab",10,0,0);
        cabinet_probe_case("truncated.cab",11,0,0);
        cabinet_probe_case("mszip.cab",12,1,0);
        cabinet_probe_case("bad-checksum.cab",13,0,0);
        cabinet_probe_case("bad-checksum.cab",14,1,0);
        cabinet_probe_case("dxupdate.cab",15,1,0);
    }
    if(!failures)SAY("CABINET DONE\n");ExitProcess(failures?1:0);
}
