/* Repeated sub-MiB read-only maps, lifetime, revision and pagefile contracts. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE_T;
typedef __INTPTR_TYPE__ IPTR;
typedef long long COUNTER;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API void CALL ExitProcess(DWORD);
#define SAY(text) do { DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), text, sizeof(text)-1, &written, 0); } while (0)
#define CHECK(condition, text) do { if (condition) { SAY("SMALL-SECTION PASS " text "\n"); } \
    else { SAY("SMALL-SECTION FAIL " text "\n"); ExitProcess(1); } } while (0)
#define LOAD(variable, name) do { *(void **)&variable = GetProcAddress(kernel, name); \
    if (!variable) { SAY("SMALL-SECTION MISSING " name "\n"); ExitProcess(2); } } while (0)
#define HEX(value) do { unsigned long long number = (value); char hex[17]; DWORD written; \
    for (unsigned digit=0; digit<16; digit++) \
        hex[digit] = "0123456789abcdef"[(number >> ((15-digit)*4)) & 15]; \
    hex[16]='\n'; WriteFile(GetStdHandle((DWORD)-11),hex,sizeof(hex),&written,0); } while (0)
#define BYTE_AT(i) ((unsigned char)(((i)*13u + ((i)>>8)) ^ 0xa7u))
#ifndef FILE_BYTES
#define FILE_BYTES 959752u
#endif
#define ROUNDS 32u
static unsigned char buffer[65536];

void small_section_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
    HANDLE (CALL *open_file)(const char*,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
    HANDLE (CALL *create_map)(HANDLE,void*,DWORD,DWORD,DWORD,const unsigned short*);
    void *(CALL *map_view)(HANDLE,DWORD,DWORD,DWORD,SIZE_T);
    BOOL (CALL *unmap)(const void*), (CALL *close_handle)(HANDLE), (CALL *flush)(HANDLE);
    BOOL (CALL *truncate_file)(HANDLE), (CALL *delete_file)(const char*);
    DWORD (CALL *seek)(HANDLE,int,int*,DWORD);
    BOOL (CALL *counter)(COUNTER*), (CALL *frequency)(COUNTER*);
    LOAD(open_file,"CreateFileA"); LOAD(create_map,"CreateFileMappingW");
    LOAD(map_view,"MapViewOfFile"); LOAD(unmap,"UnmapViewOfFile");
    LOAD(close_handle,"CloseHandle"); LOAD(flush,"FlushFileBuffers");
    LOAD(seek,"SetFilePointer"); LOAD(truncate_file,"SetEndOfFile");
    LOAD(delete_file,"DeleteFileA"); LOAD(counter,"QueryPerformanceCounter");
    LOAD(frequency,"QueryPerformanceFrequency");
    const char *path = sizeof(void*) == 4 ? "ssect32.dat" : "ssect64.dat";
    HANDLE file = open_file(path,0xc0000000u,7,0,2,0x80,0);
    CHECK(file && file != (HANDLE)(IPTR)-1,"create fixture file");
    DWORD expected_sum=0;
    BOOL prepared=1;
    for (DWORD offset=0; offset<FILE_BYTES; offset+=sizeof(buffer)) {
        DWORD length=FILE_BYTES-offset, written=0;
        if (length>sizeof(buffer)) length=sizeof(buffer);
        for (DWORD i=0;i<length;i++) { buffer[i]=BYTE_AT(offset+i); expected_sum+=buffer[i]; }
        if (!WriteFile(file,buffer,length,&written,0) || written!=length) prepared=0;
    }
    CHECK(prepared && flush(file) && close_handle(file),"prepare non-page-aligned file");
    COUNTER hz=0, begin=0, end=0;
    CHECK(frequency(&hz) && hz>0 && counter(&begin),"performance clock");
    BOOL intact=1, released=1;
    for (DWORD round=0;round<ROUNDS;round++) {
        file=open_file(path,0x80000000u,7,0,3,0x80,0);
        HANDLE section=create_map(file,0,2,0,0,0);
        const volatile unsigned char *view=map_view(section,4,0,0,0);
        if (!section || !view) { SAY("SMALL-SECTION FAIL repeated map\n"); ExitProcess(1); }
        if (!close_handle(file) || !close_handle(section)) released=0;
        DWORD sum=0;
        for (DWORD i=0;i<FILE_BYTES;i++) sum+=view[i];
        if (sum!=expected_sum || view[0]!=BYTE_AT(0) || view[FILE_BYTES-1]!=BYTE_AT(FILE_BYTES-1)) {
            if (intact) {
                SAY("SMALL-SECTION MISMATCH_ROUND "); HEX(round);
                SAY("SMALL-SECTION MISMATCH_SUM "); HEX(sum);
                SAY("SMALL-SECTION EXPECTED_SUM "); HEX(expected_sum);
                for (DWORD i=0;i<FILE_BYTES;i++) {
                    if (view[i]==BYTE_AT(i)) continue;
                    SAY("SMALL-SECTION MISMATCH_OFFSET "); HEX(i);
                    SAY("SMALL-SECTION MISMATCH_BYTE "); HEX(view[i]);
                    SAY("SMALL-SECTION EXPECTED_BYTE "); HEX(BYTE_AT(i));
                    break;
                }
            }
            intact=0;
        }
        if (!unmap((const void*)view)) released=0;
    }
    CHECK(counter(&end),"finish performance clock");
    SAY("SMALL-SECTION CLOCK_HZ "); HEX((unsigned long long)hz);
    SAY("SMALL-SECTION MAP32_TICKS "); HEX((unsigned long long)(end-begin));
    CHECK(intact,"32 full checksums after file and section close");
    CHECK(released,"32 view and handle lifetimes");
    file=open_file(path,0xc0000000u,7,0,3,0x80,0);
    DWORD written=0; buffer[0]=0x5a;
    CHECK(seek(file,17,0,0)==17 && WriteFile(file,buffer,1,&written,0) && written==1 && flush(file),
          "write after all views close");
    HANDLE section=create_map(file,0,2,0,0,0);
    const unsigned char *view=map_view(section,4,0,0,0);
#if FILE_BYTES < 1048576u
    /* Preserve a live small mapping across disk revision changes, before
     * touching it. An identical write avoids assuming mapped/I/O coherence. */
    CHECK(seek(file,17,0,0)==17 && WriteFile(file,buffer,1,&written,0) &&
          written==1 && flush(file) && view && view[17]==0x5a && view[18]==BYTE_AT(18),
          "untouched small view survives disk revision change");
#else
    CHECK(view && view[17]==0x5a && view[18]==BYTE_AT(18),"new map sees file revision");
#endif
    CHECK(unmap(view) && close_handle(section),"release revised map");
    CHECK(seek(file,513,0,0)==513 && truncate_file(file),"truncate file to partial page");
    section=create_map(file,0,2,0,0,0); view=map_view(section,4,0,0,0);
    CHECK(view && view[17]==0x5a && view[512]==BYTE_AT(512),"partial-page map contents");
    CHECK(unmap(view) && close_handle(section) && close_handle(file),"release partial-page map");
    section=create_map((HANDLE)(IPTR)-1,0,4,0,4096,0);
    volatile DWORD *first=map_view(section,2,0,0,4096), *second=map_view(section,2,0,0,4096);
    CHECK(first && second && first[0]==0 && second[1023]==0,"small pagefile section starts zeroed");
    first[0]=0x12345678; second[1023]=0xabcdef01;
    CHECK(second[0]==0x12345678 && first[1023]==0xabcdef01,"pagefile views share writes");
    CHECK(close_handle(section) && unmap((const void*)first) && unmap((const void*)second),"release pagefile maps");
    CHECK(delete_file(path),"remove fixture file");
    SAY("SMALL-SECTION DONE\n");
    ExitProcess(0);
}
