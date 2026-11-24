/* SHFILEOPSTRUCT is byte-packed on x86, naturally aligned on x64. */
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
#if defined(__i386__)
#pragma pack(push,1)
#endif
typedef struct {void *window;DWORD operation;const char *from,*to;WORD flags;int aborted;void *mappings;const char *title;} FILEOP;
#if defined(__i386__)
#pragma pack(pop)
#endif
_Static_assert(sizeof(FILEOP)==(sizeof(UP)==4?30:56),"SHFILEOPSTRUCT ABI");
static DWORD failures;
static char paths[12][520],sources[1040];
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("FILEOP PASS " name "\n");else{SAY("FILEOP FAIL " name "\n");failures++;}}while(0)
#define GET(type,name,module) type name=(type)GetProcAddress(module,#name)
void fileop_test_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll"),*s=LoadLibraryA("shell32.dll");
    typedef int (CALL *opfn)(FILEOP *);
    typedef DWORD (CALL *fullfn)(const char *,DWORD,char *,char **);
    typedef int (CALL *mkdirfn)(const char *,void *);
    typedef DWORD (CALL *attrfn)(const char *);
    typedef void *(CALL *createfn)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *closefn)(void *);
    typedef int (CALL *readfn)(void *,void *,DWORD,DWORD *,void *);
    GET(opfn,SHFileOperationA,s);GET(fullfn,GetFullPathNameA,k);GET(mkdirfn,CreateDirectoryA,k);
    GET(attrfn,GetFileAttributesA,k);GET(createfn,CreateFileA,k);GET(closefn,CloseHandle,k);GET(readfn,ReadFile,k);
    CHECK(SHFileOperationA&&GetFullPathNameA&&CreateDirectoryA&&CreateFileA,"exports");
    if(failures)ExitProcess(1);
    const char *names[]={"dxop_src","dxop_src\\nested","dxop_src\\a.dat","dxop_src\\nested\\b.dat",
        "dxop_copy","dxop_copy\\nested\\b.dat","dxop_single.dat","dxop_renamed.dat",
        "dxop_out","dxop_out\\dxop_renamed.dat","dxop_out\\*.dat","dxop_missing.dat"};
    for(unsigned i=0;i<12;i++){DWORD n=GetFullPathNameA(names[i],519,paths[i],0);CHECK(n&&n<519,"absolute-private-path");paths[i][n+1]=0;}
    struct {UP before;FILEOP value;UP after;} guarded={(UP)0x11223344,{0,3,0,0,0x614,0,0,0},(UP)0x55667788};
    FILEOP *op=&guarded.value;
    /* All destructive probes are confined to the fixture working directory. */
    unsigned cleanup[]={0,4,6,7,8};
    for(unsigned i=0;i<5;i++){op->from=paths[cleanup[i]];SHFileOperationA(op);}
    CHECK(CreateDirectoryA(paths[0],0)&&CreateDirectoryA(paths[1],0)&&CreateDirectoryA(paths[8],0),"create-private-directories");
    for(unsigned i=2;i<=3;i++){
        void *file=CreateFileA(paths[i],0x40000000,3,0,1,0x80,0);DWORD written=0;
        CHECK(file!=(void *)(UP)-1&&WriteFile(file,"directx",7,&written,0)&&written==7,"create-source-file");
        if(file!=(void *)(UP)-1)CloseHandle(file);
    }
    op->operation=2;op->from=paths[2];op->to=paths[6];op->aborted=0x12345678;
    CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(paths[6])!=(DWORD)-1,"copy-file");
    CHECK(guarded.before==0x11223344&&guarded.after==0x55667788,"structure-canaries");
    op->operation=4;op->from=paths[6];op->to=paths[7];
    CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(paths[6])==(DWORD)-1&&GetFileAttributesA(paths[7])!=(DWORD)-1,"rename-file");
    op->operation=1;op->from=paths[7];op->to=paths[8];
    CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(paths[7])==(DWORD)-1&&GetFileAttributesA(paths[9])!=(DWORD)-1,"move-into-directory");
    op->operation=2;op->from=paths[0];op->to=paths[4];
    CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(paths[5])!=(DWORD)-1,"recursive-copy");
    void *file=CreateFileA(paths[5],0x80000000,3,0,3,0x80,0);char data[8]={0};DWORD count=0;
    CHECK(file!=(void *)(UP)-1&&ReadFile(file,data,7,&count,0)&&count==7&&data[0]=='d'&&data[6]=='x',"recursive-copy-payload");
    if(file!=(void *)(UP)-1)CloseHandle(file);
    unsigned at=0;for(unsigned i=2;i<=3;i++){unsigned j=0;do{sources[at++]=paths[i][j];}while(paths[i][j++]);}sources[at]=0;
    op->from=sources;op->to=paths[8];
    CHECK(SHFileOperationA(op)==0&&!op->aborted,"multi-source-copy");
    op->operation=3;op->from=paths[10];op->to=0;
    CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(paths[9])==(DWORD)-1&&(GetFileAttributesA(paths[8])&16),"wildcard-file-delete-keeps-directory");
    op->operation=2;op->from=paths[11];op->to=paths[6];
    CHECK(SHFileOperationA(op)!=0&&GetFileAttributesA(paths[6])==(DWORD)-1,"missing-source-fails");
    op->operation=3;op->to=0;
    for(unsigned i=0;i<3;i++){op->from=paths[i==0?0:i==1?4:8];CHECK(SHFileOperationA(op)==0&&!op->aborted&&GetFileAttributesA(op->from)==(DWORD)-1,"recursive-delete");}
    static const char empty[]={0,0};op->from=empty;
    CHECK(SHFileOperationA(op)==0&&!op->aborted,"empty-operation");
    if(!failures)SAY("FILEOP DONE\n");
    ExitProcess(failures?1:0);
}
