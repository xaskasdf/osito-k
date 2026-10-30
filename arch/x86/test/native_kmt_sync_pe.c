/* Independent ABI/lifetime oracle, safe on Windows or an empty guest. */
#define native_d3dkmt_entry unused_sync_discovery_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry
#define ZERO(object) do { for (unsigned z=0;z<sizeof(object);z++) ((volatile unsigned char *)&(object))[z]=0; } while(0)
void native_kmt_sync_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll");
#define LOAD(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    LOAD(CreateSynchronizationObject,OKMT_CREATE_SYNC);
    LOAD(CreateSynchronizationObject2,OKMT_CREATE_SYNC2);
    LOAD(DestroySynchronizationObject,OKMT_DESTROY_SYNC);
    LOAD(WaitForSynchronizationObject,OKMT_WAIT_SYNC);
    LOAD(WaitForSynchronizationObject2,OKMT_WAIT_SYNC2);
    LOAD(SignalSynchronizationObject,OKMT_SIGNAL_SYNC);
    LOAD(SignalSynchronizationObject2,OKMT_SIGNAL_SYNC2);
    LOAD(EnumAdapters2,OKMT_ENUM_ADAPTERS2);
    LOAD(CloseAdapter,OKMT_CLOSE_ADAPTER);
    LOAD(CreateDevice,OKMT_CREATE_DEVICE);
    LOAD(DestroyDevice,OKMT_DESTROY_DEVICE);
#undef LOAD
    if (!CreateSynchronizationObject || !CreateSynchronizationObject2 || !DestroySynchronizationObject ||
        !WaitForSynchronizationObject || !WaitForSynchronizationObject2 || !SignalSynchronizationObject ||
        !SignalSynchronizationObject2 || !EnumAdapters2 || !CloseAdapter || !CreateDevice || !DestroyDevice) {
        REPORT("SYNC UNAVAILABLE\n"); ExitProcess(2); return;
    }
    OKMT_CREATE_SYNC old; OKMT_CREATE_SYNC2 modern; OKMT_WAIT_SYNC2 wait; OKMT_SIGNAL_SYNC2 signal;
    ZERO(old); ZERO(modern); ZERO(wait); ZERO(signal);
    old.device=modern.device=wait.context=signal.context=0xffffffffu;
    old.info.type=modern.info.type=OKMT_SYNC_MUTEX; old.object=modern.object=0xfeed;
    wait.count=signal.count=1; wait.objects[0]=signal.objects[0]=0xffffffffu;
    CHECK(CreateSynchronizationObject(&old)<0 && old.object==0xfeed,"invalid-create-preserves-output");
    CHECK(CreateSynchronizationObject2(&modern)<0 && modern.object==0xfeed,"invalid-create2-preserves-output");
    OKMT_DESTROY_SYNC destroy={0xffffffffu};
    CHECK(DestroySynchronizationObject(&destroy)<0,"invalid-destroy");
    CHECK(WaitForSynchronizationObject((OKMT_WAIT_SYNC *)&wait)<0,"invalid-wait");
    CHECK(WaitForSynchronizationObject2(&wait)<0,"invalid-wait2");
    CHECK(SignalSynchronizationObject((OKMT_SIGNAL_SYNC *)&signal)<0,"invalid-signal");
    CHECK(SignalSynchronizationObject2(&signal)<0,"invalid-signal2");
    OKMT_ADAPTER_INFO adapters[16]; OKMT_ENUM_ADAPTERS2 list={16,adapters};
    CHECK(EnumAdapters2(&list)==0 && list.count<=16,"enumerate");
#ifdef KMT_SYNC_EMPTY
    CHECK(list.count==0,"no-invented-adapter");
#endif
    for (unsigned i=0;i<list.count && i<16;i++) {
        OKMT_CREATE_DEVICE device; ZERO(device); device.object.adapter=adapters[i].adapter;
        OKMT_S32 status=CreateDevice(&device); VALUE("SYNC create-device=",status);
        CHECK(status==0 && device.device,"create-device");
        if (status==0) {
            for (unsigned type=1;type<=3;type++) {
                ZERO(modern); modern.device=device.device; modern.info.type=type;
                if (type==2) { modern.info.data.semaphore.maximum=4; modern.info.data.semaphore.initial=2; }
                if (type==3) modern.info.data.value=0x100000003ULL;
                status=CreateSynchronizationObject2(&modern); VALUE("SYNC type=",type); VALUE("SYNC create2=",status);
                CHECK(status==0 && modern.object,"create-typed-object");
                if (status==0) {
                    destroy.object=modern.object; CHECK(DestroySynchronizationObject(&destroy)==0,"destroy-typed-object");
                    CHECK(DestroySynchronizationObject(&destroy)<0,"double-destroy-rejected");
                }
                if (type==3) continue;
                ZERO(old); old.device=device.device; old.info.type=type;
                if (type==2) { old.info.data.semaphore.maximum=4; old.info.data.semaphore.initial=2; }
                status=CreateSynchronizationObject(&old); VALUE("SYNC create1=",status);
                CHECK(status==0 && old.object,"create-legacy-object");
                if (status==0) { destroy.object=old.object; CHECK(DestroySynchronizationObject(&destroy)==0,"destroy-legacy-object"); }
            }
            OKMT_DESTROY_DEVICE close={device.device}; CHECK(DestroyDevice(&close)==0,"destroy-device");
        }
        OKMT_CLOSE_ADAPTER close={adapters[i].adapter}; CHECK(CloseAdapter(&close)==0,"close-adapter");
    }
    VALUE("SYNC checks=",checks); VALUE("SYNC failures=",failures);
    FreeLibrary(gdi); ExitProcess(failures ? 1 : 0);
}
