/* Real RIFF traversal through native WINMM and the guest multimedia provider. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct __attribute__((packed)) {
    DWORD flags,fourcc;void *proc;DWORD error;void *task;int capacity;
    char *buffer,*next,*endread,*endwrite;int buffer_offset,disk_offset;
    DWORD data[3],reserved1,reserved2;void *handle;
} INFO;
typedef struct {DWORD id,size,type,offset,flags;} CHUNK;
static void *(CALL *openmm)(char *,INFO *,DWORD);
static DWORD(CALL *closemm)(void *,DWORD);
static int(CALL *readmm)(void *,char *,int), (CALL *seekmm)(void *,int,int);
static DWORD(CALL *descend)(void *,CHUNK *,const CHUNK *,DWORD), (CALL *ascend)(void *,CHUNK *,DWORD);
static DWORD failures;
static const unsigned char riff[]={
    'R','I','F','F',54,0,0,0,'W','A','V','E',
    'J','U','N','K',3,0,0,0,'a','b','c',0,
    'L','I','S','T',18,0,0,0,'I','N','F','O',
    'I','N','A','M',5,0,0,0,'h','e','l','l','o',0,
    'd','a','t','a',4,0,0,0,1,2,3,4
};
static unsigned char memory_riff[sizeof(riff)];
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
static void mmio_probe_check(int ok,const char *name)
{
    if(ok)SAY("MMIO PASS ");else{SAY("MMIO FAIL ");failures++;}
    DWORD n=0,w;while(name[n])n++;WriteFile(GetStdHandle((DWORD)-11),name,n,&w,0);SAY("\n");
}
static DWORD mmio_probe_fourcc(const char *p)
{
    return (DWORD)(unsigned char)p[0]|((DWORD)(unsigned char)p[1]<<8)|((DWORD)(unsigned char)p[2]<<16)|((DWORD)(unsigned char)p[3]<<24);
}
static int mmio_probe_file(const char *name,const void *bytes,DWORD count)
{
    typedef void *(CALL *CREATE)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int(CALL *CLOSE)(void *);
    void *k=LoadLibraryA("kernel32.dll");
    CREATE create=(CREATE)GetProcAddress(k,"CreateFileA");CLOSE close=(CLOSE)GetProcAddress(k,"CloseHandle");
    void *f=create(name,0x40000000,7,0,2,0x80,0);if(f==(void *)(UP)-1)return 0;
    DWORD written=0;int ok=WriteFile(f,bytes,count,&written,0)&&written==count;close(f);return ok;
}
static void mmio_probe_run(void *handle)
{
    CHUNK top={0},chunk={0},list={0};char data[16];
    top.type=mmio_probe_fourcc("WAVE");
    DWORD result=descend(handle,&top,0,32);
    SAY("MMIO RIFF ");HEX(result);HEX(top.id);HEX(top.size);HEX(top.type);HEX(top.offset);HEX(top.flags);SAY("\n");
    mmio_probe_check(!result&&top.id==mmio_probe_fourcc("RIFF")&&top.size==54&&top.offset==8&&seekmm(handle,0,1)==12,"riff-header-and-position");
    result=descend(handle,&chunk,&top,0);
    mmio_probe_check(!result&&chunk.id==mmio_probe_fourcc("JUNK")&&chunk.size==3&&chunk.offset==20&&seekmm(handle,0,1)==20,"next-chunk");
    mmio_probe_check(readmm(handle,data,3)==3&&data[0]=='a'&&data[1]=='b'&&data[2]=='c',"read-chunk-bytes");
    mmio_probe_check(!ascend(handle,&chunk,0)&&seekmm(handle,0,1)==24,"odd-chunk-padding");
    list.type=mmio_probe_fourcc("INFO");
    result=descend(handle,&list,&top,64);
    mmio_probe_check(!result&&list.id==mmio_probe_fourcc("LIST")&&list.size==18&&list.offset==32&&seekmm(handle,0,1)==36,"nested-list");
    chunk.id=mmio_probe_fourcc("INAM");
    mmio_probe_check(!descend(handle,&chunk,&list,16)&&chunk.offset==44&&chunk.size==5,"find-nested-chunk");
    mmio_probe_check(readmm(handle,data,5)==5&&data[0]=='h'&&data[4]=='o',"nested-data");
    mmio_probe_check(!ascend(handle,&chunk,0)&&!ascend(handle,&list,0)&&seekmm(handle,0,1)==50,"ascend-parent");
    chunk.id=mmio_probe_fourcc("data");
    mmio_probe_check(!descend(handle,&chunk,&top,16)&&chunk.offset==58&&chunk.size==4,"find-data-chunk");
    mmio_probe_check(readmm(handle,data,8)==4&&data[0]==1&&data[3]==4&&readmm(handle,data,1)==0,"partial-read-and-eof");
    mmio_probe_check(seekmm(handle,12,0)==12,"seek-absolute");
    chunk.id=mmio_probe_fourcc("MISS");result=descend(handle,&chunk,&top,16);
    SAY("MMIO MISSING ");HEX(result);HEX(seekmm(handle,0,1));SAY("\n");
    mmio_probe_check(result==265,"missing-chunk");
    /* Native MMIO interprets SEEK_END's offset as distance back from EOF. */
    int end_seek=seekmm(handle,4,2),end_read=readmm(handle,data,4);
    SAY("MMIO END SEEK ");HEX(end_seek);HEX(end_read);HEX(data[0]);SAY("\n");
    mmio_probe_check(end_seek==58&&end_read==4&&data[0]==1,"seek-from-end");
    mmio_probe_check(seekmm(handle,-1,0)==-1,"negative-seek-fails");
    mmio_probe_check(closemm(handle,0)==0,"close");
    DWORD closed=closemm(handle,0);int invalid=readmm(handle,data,1);
    SAY("MMIO STALE ");HEX(closed);HEX(invalid);SAY("\n");
    mmio_probe_check(closed!=0&&invalid==-1,"stale-handle");
}
void mmio_probe_entry(void)
{
    typedef char *(CALL *LOWER)(char *);
    LOWER lower=(LOWER)GetProcAddress(LoadLibraryA("user32.dll"),"CharLowerA");
    mmio_probe_check(lower!=0,"ansi-lowercase-export");
    if(lower){
        char text[]={'D','I','R','E','C','T','X',' ',(char)0xc9,0};
        mmio_probe_check(lower(text)==text&&text[0]=='d'&&text[6]=='x'&&(unsigned char)text[8]==0xe9,"ansi-lowercase-in-place");
        mmio_probe_check(lower((char *)(UP)'A')==(char *)(UP)'a'&&lower(0)==0,"ansi-lowercase-character");
    }
    void *w=LoadLibraryA("winmm.dll");
#define LOAD(dst,name) dst=(__typeof__(dst))GetProcAddress(w,name)
    LOAD(openmm,"mmioOpenA");LOAD(closemm,"mmioClose");LOAD(readmm,"mmioRead");
    LOAD(seekmm,"mmioSeek");LOAD(descend,"mmioDescend");LOAD(ascend,"mmioAscend");
    mmio_probe_check(openmm&&closemm&&readmm&&seekmm&&descend&&ascend,"exports");if(failures)ExitProcess(1);
    mmio_probe_check(mmio_probe_file("mmio-fixture.wav",riff,sizeof(riff)),"file-fixture");
    for(unsigned buffered=0;buffered<2;buffered++){
        INFO info={0};void *file=openmm("mmio-fixture.wav",&info,buffered?0x10000:0);
        SAY("MMIO OPEN ");HEX(buffered);HEX(sizeof(INFO));HEX((UP)file);HEX(info.error);SAY("\n");
        mmio_probe_check(file!=0,"open-file");if(file)mmio_probe_run(file);
    }
    for(unsigned i=0;i<sizeof(riff);i++)memory_riff[i]=riff[i];
    INFO mem={0};mem.fourcc=mmio_probe_fourcc("MEM ");mem.buffer=(char *)memory_riff;mem.capacity=sizeof(riff);
    void *file=openmm(0,&mem,0);SAY("MMIO MEMORY ");HEX((UP)file);HEX(mem.error);SAY("\n");
    mmio_probe_check(file!=0,"open-memory");if(file)mmio_probe_run(file);
    INFO missing={0};file=openmm("mmio-absent-file.wav",&missing,0);
    SAY("MMIO ABSENT ");HEX((UP)file);HEX(missing.error);SAY("\n");
    mmio_probe_check(!file&&missing.error!=0,"missing-file-error");
    mmio_probe_check(mmio_probe_file("mmio-short.wav",riff,7),"short-fixture");
    file=openmm("mmio-short.wav",0,0);CHUNK chunk={0};chunk.type=mmio_probe_fourcc("WAVE");
    DWORD result=descend(file,&chunk,0,32);SAY("MMIO SHORT ");HEX(result);SAY("\n");
    mmio_probe_check(result==265,"truncated-header");if(file)closemm(file,0);
    if(!failures)SAY("MMIO DONE\n");ExitProcess(failures?1:0);
}
