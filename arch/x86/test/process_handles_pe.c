/* Native Windows/OsitoK oracle; creates only private handles and child processes. */
#define mainCRTStartup unusedJobEntry
#include "job_object_pe.c"
#undef mainCRTStartup

static DWORD (CALL *query_process)(HANDLE,DWORD,void *,DWORD,DWORD *);
static DWORD (CALL *query_object)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *count_handles)(HANDLE,DWORD *);
static HANDLE (CALL *create_thread)(void *,SIZE,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static DWORD table[8192];
static BOOL scratch;
static volatile DWORD churn_failed;

static DWORD snapshot(HANDLE process)
{
    DWORD bytes=0xcccccccc;
    memset(table,0xcc,sizeof(table));
    DWORD status=query_process(process,58,table,sizeof(table),&bytes);
    check(status==0,"ProcessHandleTable succeeds");
    if(status)return 0;
    check(bytes<=sizeof(table)&&!(bytes&3),"table byte count uses DWORD ABI");
    if(bytes>sizeof(table))return 0;
    DWORD count=bytes/4;
    for(DWORD i=0;i<count;i++)
        check(table[i]&&!(table[i]&3)&&(!i||table[i]>table[i-1]),"sorted unique numeric handles");
    if(count<sizeof(table)/4)check(table[count]==0xcccccccc,"snapshot preserves unused output");
    return count;
}

static BOOL has_handle(DWORD count,HANDLE handle)
{
    for(DWORD i=0;i<count;i++)if(table[i]==(DWORD)(SIZE)handle)return 1;
    return 0;
}

static void event_type(HANDLE event)
{
    static SIZE buffer[128];DWORD returned=0;
    check(query_object(event,2,buffer,sizeof(buffer),&returned)==0,"enumerated event is queryable");
    WORD *type=*(WORD **)((BYTE *)buffer+(sizeof(void *)==4?4:8));
    check(*(WORD *)buffer==10&&type&&type[0]=='E'&&type[1]=='v'&&type[2]=='e'&&
          type[3]=='n'&&type[4]=='t',"enumeration refers to real object type");
}

static void self_contract(void)
{
    DWORD before=0,after=0,bytes;
    check(count_handles(SELF,&before),"count current process");
    HANDLE event=create_event(0,0,0,0),alias=0;
    check(event!=0,"create private event");
    check(dup_handle(SELF,event,SELF,&alias,0,0,2),"duplicate private event");
    check(count_handles(SELF,&after)&&after==before+2,"count distinct handles not objects");
    SetLastError(1234);
    check(count_handles(SELF,&after)&&GetLastError()==1234,"count preserves last error");
    DWORD count=snapshot(SELF);
    check(count==after&&has_handle(count,event)&&has_handle(count,alias),"table agrees with owned count");
    event_type(event);event_type(alias);
    DWORD first[4];memcpy(first,table,sizeof(first));
    DWORD sizes[]={0,1,3,4,5,7,8,15,16};
    for(DWORD i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
        DWORD out[5];memset(out,0xcc,sizeof(out));bytes=0xcccccccc;
        DWORD status=query_process(SELF,58,out,sizes[i],&bytes);
        if(sizes[i]<4)check(status==0xc0000004&&bytes==0xcccccccc&&out[0]==0xcccccccc,
                           "short buffer fails without mutation");
        else{
            check(status==0&&bytes==(sizes[i]/4)*4,"small snapshot truncates with success");
            for(DWORD j=0;j<bytes/4&&j<4;j++)check(out[j]==first[j],"truncated prefix is real");
            check(out[sizes[i]/4]==0xcccccccc,"partial DWORD is not written");
        }
    }
    DWORD out[4];bytes=0xcccccccc;
    check(query_process(SELF,58,0,4,&bytes)==0xc0000005,"null table output rejected");
    check(query_process(SELF,58,(BYTE *)out+1,4,&bytes)==0x80000002,"unaligned output rejected");
    check(query_process(SELF,58,(void *)0x8000,4,&bytes)==0xc0000005,"unmapped output rejected");
    check(query_process(SELF,58,out,sizeof(out),(DWORD *)1)==0xc0000005,"invalid length output rejected");
    check(query_process(SELF,58,out,sizeof(out),0)==0,"return length optional");
    void *reserved=alloc(0,4096,0x2000,4);
    check(reserved!=0,"reserve inaccessible output page");
    if(reserved){
        check(query_process(SELF,58,reserved,4,&bytes)==0xc0000005,"reserved page not writable");
        /* Windows' Win32 wrapper can raise on this caller-owned pointer.
         * Check OsitoK's explicit guest validation only in the scratch profile. */
        if(scratch)check(!count_handles(SELF,reserved)&&GetLastError()==998,"counter validates output page");
        free_vm(reserved,0,0x8000);
    }
    DWORD rights[]={0,0x10,0x40,0x400,0x1000,0x1040,0x440,0x1440};
    for(DWORD i=0;i<sizeof(rights)/sizeof(rights[0]);i++){
        HANDLE selected=0;
        check(dup_handle(SELF,SELF,SELF,&selected,rights[i],0,0),"restricted process handle");
        BOOL can_count=(rights[i]&0x1400)!=0,can_table=(rights[i]&0x440)==0x440;
        bytes=0xcccccccc;out[0]=0xcccccccc;
        check(query_process(selected,58,out,4,&bytes)==(can_table?0:0xc0000022),"table enforces query and duplicate rights");
        if(!can_table)check(bytes==0xcccccccc&&out[0]==0xcccccccc,"denied table leaves outputs alone");
        after=0xcccccccc;SetLastError(1234);
        check(count_handles(selected,&after)==can_count,"counter permits query or limited query");
        if(!can_count)check(GetLastError()==5&&after==0xcccccccc,"denied counter does not write");
        check(close_handle(selected),"close restricted process handle");
    }
    check(query_process(event,58,out,4,0)==0xc0000024,"table rejects wrong object type");
    check(query_process(0,58,out,4,0)==0xc0000008,"table rejects null process handle");
    check(query_process(0,58,out,3,0)==0xc0000004,"length validation precedes handle lookup");
    check(close_handle(event),"close original event");
    count=snapshot(SELF);
    check(!has_handle(count,event)&&has_handle(count,alias),"close removes only original handle");
    event_type(alias);
    check(close_handle(alias),"close duplicate event");
    check(count_handles(SELF,&after)&&after==before,"self count returns to baseline");
}

static void child_contract(void)
{
    HANDLE inherited=create_event(0,0,0,0),private_event=create_event(0,0,0,0);
    check(inherited&&private_event&&handle_flags(inherited,1,1),"prepare inherited private event");
    expected_job=inherited; /* spawn helper uses this to enable handle inheritance. */
    PROCESS child;
    BOOL made=spawn("--handle-child",4,&child);expected_job=0;
    check(made,"create suspended handle owner");
    if(made){
        DWORD child_before=0,parent_before=0,child_after=0,parent_after=0;
        check(count_handles(child.process,&child_before)&&count_handles(SELF,&parent_before),"count selected process");
        DWORD count=snapshot(child.process);
        check(count==child_before&&has_handle(count,inherited),"inherited handle belongs to child");
        HANDLE remote=0,back=0;
        check(dup_handle(SELF,private_event,child.process,&remote,0,0,2),"duplicate into selected child");
        check(count_handles(child.process,&child_after)&&child_after==child_before+1,"target count grows");
        check(count_handles(SELF,&parent_after)&&parent_after==parent_before,"foreign handles excluded from parent count");
        count=snapshot(child.process);
        check(count==child_after&&has_handle(count,remote),"foreign snapshot contains target duplicate");
        check(dup_handle(child.process,remote,SELF,&back,0,0,3),"duplicate back and close remote source");
        check(count_handles(child.process,&child_after)&&child_after==child_before,"remote close shrinks target count");
        count=snapshot(child.process);
        check(!has_handle(count,remote),"remote closed handle absent");
        if(back){event_type(back);close_handle(back);}
        check(close_handle(inherited),"release parent inherited owner");inherited=0;
        check(count_handles(child.process,&child_after)&&child_after==child_before,"inherited child owner survives parent close");
        check(resume_thread(child.thread)==1&&wait_object(child.process,30000)==0,"child runs and exits");
        DWORD bytes=0xcccccccc,out=0xcccccccc;
        check(query_process(child.process,58,&out,4,&bytes)==0xc000010a&&bytes==0&&out==0xcccccccc,
              "exited process has no enumerable table");
        check(count_handles(child.process,&child_after)&&child_after==0,"exited process count is zero");
        close_handle(child.thread);close_handle(child.process);
    }
    if(inherited)close_handle(inherited);
    if(private_event)close_handle(private_event);
}

static DWORD CALL churn_handles(void *unused)
{
    (void)unused;
    for(DWORD i=0;i<256;i++){
        HANDLE event=create_event(0,0,0,0),alias=0;
        if(!event||!dup_handle(SELF,event,SELF,&alias,0,0,2))churn_failed=1;
        sleep_ms(0);
        if(alias&&!close_handle(alias))churn_failed=1;
        if(event&&!close_handle(event))churn_failed=1;
    }
    return 0;
}

static void concurrent_contract(void)
{
    DWORD before=0,after=0;
    check(count_handles(SELF,&before),"count before concurrent snapshots");
    HANDLE thread=create_thread(0,0,churn_handles,0,0,0);
    check(thread!=0,"start concurrent duplicate and close");
    if(!thread)return;
    for(DWORD i=0;i<256;i++){
        snapshot(SELF);
        sleep_ms(0);
    }
    check(wait_object(thread,30000)==0&&!churn_failed,"concurrent mutation completed");
    check(close_handle(thread),"close worker handle");
    check(count_handles(SELF,&after)&&before==after,"concurrent handles reclaimed");
}

void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(var,module,name) do{var=GetProcAddress(module,name);if(!var){report("PROCESS-HANDLES MISSING " name "\n");ExitProcess(1);}}while(0)
    LOAD(command_line,k,"GetCommandLineA");
    if(contains(command_line(),"--handle-child"))ExitProcess(0);
    LOAD(query_process,n,"NtQueryInformationProcess");LOAD(query_object,n,"NtQueryObject");
    LOAD(count_handles,k,"GetProcessHandleCount");LOAD(create_event,k,"CreateEventA");
    LOAD(close_handle,k,"CloseHandle");LOAD(dup_handle,k,"DuplicateHandle");
    LOAD(alloc,k,"VirtualAlloc");LOAD(free_vm,k,"VirtualFree");
    LOAD(handle_flags,k,"SetHandleInformation");LOAD(module_name,k,"GetModuleFileNameA");
    LOAD(create_process,k,"CreateProcessA");LOAD(resume_thread,k,"ResumeThread");
    LOAD(wait_object,k,"WaitForSingleObject");LOAD(terminate_process,k,"TerminateProcess");
    LOAD(create_thread,k,"CreateThread");LOAD(sleep_ms,k,"Sleep");
#undef LOAD
    module_name(0,path,sizeof(path));scratch=contains(command_line(),"--scratch");
    self_contract();child_contract();concurrent_contract();
    report(failures?"PROCESS-HANDLES FAIL\n":"PROCESS-HANDLES PASS\n");
    ExitProcess(failures?1:0);
}
