/* Extend the adapter suite using its process/user-memory test environment. */
#define main adapter_suite_main
#include "d3dkmt_registry_host.c"
#undef main
#include <sched.h>

typedef struct {
    pthread_mutex_t lock;
    struct { uint64_t cookie; DWORD pid; } slots[D3DKMT_MAX_HANDLES];
    uint64_t serial;
    unsigned opens,closes,live;
    int fail_open,fail_close,reenter,gate,entered,proceed;
    DWORD fail_close_pid;
    OKMT_LUID luid;
} DEVICE_FIXTURE;
static DEVICE_FIXTURE fixture={.lock=PTHREAD_MUTEX_INITIALIZER};

static NTSTATUS device_open(void *context,DWORD pid,uint64_t *cookie)
{
    DEVICE_FIXTURE *f=context;
    DWORD count=0;
    /* Callback reentry must not deadlock on a held registry lock. */
    CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    pthread_mutex_lock(&f->lock);
    f->opens++;
    NTSTATUS status=STATUS_NO_MEMORY;
    if (!f->fail_open) for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) {
        if (f->slots[i].pid) continue;
        f->slots[i].pid=pid;
        *cookie=f->slots[i].cookie=++f->serial;
        f->live++;
        status=STATUS_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&f->lock);
    if (f->reenter==1) d3dkmt_release_process(pid);
    if (f->reenter==2) CHECK(d3dkmt_unregister_adapter(f->luid)==D3DKMT_STATUS_DEVICE_BUSY);
    if (f->gate) {
        __atomic_store_n(&f->entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->proceed,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}

static NTSTATUS device_close(void *context,DWORD pid,uint64_t cookie)
{
    DEVICE_FIXTURE *f=context;
    DWORD count=0;
    CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    pthread_mutex_lock(&f->lock);
    f->closes++;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) {
        if (f->slots[i].pid!=pid || f->slots[i].cookie!=cookie) continue;
        int fail=f->fail_close || f->fail_close_pid==pid;
        status=fail ? STATUS_NO_MEMORY : STATUS_SUCCESS;
        if (!fail) { f->slots[i].pid=0; f->live--; }
        break;
    }
    pthread_mutex_unlock(&f->lock);
    CHECK(status!=STATUS_INVALID_PARAMETER);
    return status;
}

static void *device_worker(void *argument)
{
    DWORD pid=(DWORD)(uintptr_t)argument;
    for (unsigned i=0;i<1000;i++) {
        OKMT_HANDLE adapter=0,device=0;
        CHECK(d3dkmt_open_adapter(pid,fixture.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(pid,adapter,0,&device)==0 && device!=0);
        CHECK(d3dkmt_close_adapter(pid,adapter)==0);
        CHECK(d3dkmt_destroy_device(pid+100,device)==STATUS_INVALID_PARAMETER);
        if (i&1) { d3dkmt_release_process(pid); }
        else CHECK(d3dkmt_destroy_device(pid,device)==0);
        CHECK(d3dkmt_destroy_device(pid,device)==STATUS_INVALID_PARAMETER);
    }
    return NULL;
}

typedef struct { DWORD pid; OKMT_HANDLE adapter,device; NTSTATUS result; } OPEN_RACE;
static void *release_during_open(void *argument)
{
    OPEN_RACE *race=argument;
    race->result=d3dkmt_create_device(race->pid,race->adapter,0,&race->device);
    return NULL;
}

typedef struct { OKMT_HANDLE device; unsigned entered,resume; } LEASE_RACE;
static void *device_lease_worker(void *argument)
{
    LEASE_RACE *race=argument;
    D3DKMT_DEVICE_LEASE lease={0},denied={0};
    CHECK(d3dkmt_acquire_device(40,race->device,&lease)==0);
    uint64_t cookie=lease.cookie;
    __atomic_add_fetch(&race->entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&race->resume,__ATOMIC_ACQUIRE)) sched_yield();
    CHECK(d3dkmt_acquire_device(40,race->device,&denied)==STATUS_INVALID_PARAMETER && !denied.self);
    CHECK(lease.cookie==cookie && lease.provider.context==&fixture);
    CHECK(d3dkmt_release_device(&lease)==0 && !lease.self);
    return NULL;
}

int main(void)
{
    if (adapter_suite_main()) return 1;
    checks=failures=0;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    D3DKMT_DEVICE_PROVIDER provider={device_open,device_close,&fixture,NULL,NULL,NULL};
    OKMT_HANDLE adapter=0,device=0xfeed,second=0;
    DWORD value=0,count=0;
    CHECK(d3dkmt_register_adapter(&description,&fixture.luid)==0);
    CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==STATUS_NOT_SUPPORTED && device==0xfeed);
    d3dkmt_release_process(10);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    CHECK(d3dkmt_register_driver(&description,NULL,&fixture.luid)==STATUS_INVALID_PARAMETER);
    D3DKMT_DEVICE_PROVIDER incomplete=provider; incomplete.close=NULL;
    CHECK(d3dkmt_register_driver(&description,&incomplete,&fixture.luid)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
    CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(10,adapter,16,&device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_device(10,adapter,1,&device)==STATUS_NOT_SUPPORTED);
    CHECK(d3dkmt_create_device(11,adapter,0,&device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_device(0,adapter,0,&device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_device(10,adapter,0,NULL)==STATUS_INVALID_PARAMETER);
    CHECK(device==0xfeed && fixture.opens==0);
    fixture.fail_open=1;
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES+8;i++)
        CHECK(d3dkmt_create_device(10,adapter,0,&device)==STATUS_NO_MEMORY && device==0xfeed);
    fixture.fail_open=0;
    CHECK(fixture.live==0 && fixture.closes==0);
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==0 && device!=adapter);
    CHECK(d3dkmt_destroy_device(10,adapter)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_close_adapter(10,device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_query_adapter(10,device,OKMT_QUERY_DRIVER_VERSION,&value,4,FALSE)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_device(10,device,0,&second)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_close_adapter(10,adapter)==0 && fixture.live==1);
    CHECK(d3dkmt_destroy_device(11,device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_device(10,device)==0 && fixture.live==0);
    CHECK(d3dkmt_destroy_device(10,device)==STATUS_INVALID_PARAMETER);

    /* Removal hides discovery immediately but retains the provider until the
     * last logical close succeeds and unregister is retried. */
    CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==0);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_enumerate_adapters(10,0,NULL,&count)==0 && count==0);
    CHECK(d3dkmt_create_device(10,adapter,0,&second)==D3DKMT_STATUS_DEVICE_REMOVED);
    fixture.fail_close=1;
    CHECK(d3dkmt_destroy_device(10,device)==0 && fixture.live==1);
    CHECK(d3dkmt_destroy_device(10,device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_collect_devices()==STATUS_NO_MEMORY);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    fixture.fail_close=0;
    CHECK(d3dkmt_collect_devices()==0 && fixture.live==0);
    CHECK(d3dkmt_destroy_device(10,device)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    CHECK(d3dkmt_close_adapter(10,adapter)==0);

    /* Native lifetime outlasts user admission and adapter removal. */
    for (unsigned phase=0;phase<4;phase++) {
        CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
        CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(10,adapter,0,&device)==0);
        D3DKMT_DEVICE_LEASE first={0},last={0},denied={0};
        CHECK(d3dkmt_acquire_device(11,device,&denied)==STATUS_INVALID_PARAMETER && !denied.self);
        CHECK(d3dkmt_acquire_device(10,adapter,&denied)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_device(10,device,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_device(10,device,&first)==0 && first.self==&first && first.cookie);
        CHECK(d3dkmt_acquire_device(10,device,&first)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_device(10,device,&last)==0 && last.cookie==first.cookie);
        D3DKMT_DEVICE_LEASE moved=first;
        CHECK(d3dkmt_release_device(&moved)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_close_adapter(10,adapter)==0);
        unsigned closed=fixture.closes;
        if (phase==2) {
            CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(d3dkmt_acquire_device(10,device,&denied)==D3DKMT_STATUS_DEVICE_REMOVED);
        }
        if (phase==1) d3dkmt_release_process(10);
        else CHECK(d3dkmt_destroy_device(10,device)==0);
        CHECK(d3dkmt_destroy_device(10,device)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_device(10,device,&denied)==STATUS_INVALID_PARAMETER && !denied.self);
        CHECK(d3dkmt_collect_devices()==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(fixture.live==1 && fixture.closes==closed);
        CHECK(d3dkmt_release_device(&first)==0 && !first.self);
        CHECK(d3dkmt_release_device(&first)==STATUS_INVALID_PARAMETER);
        CHECK(fixture.closes==closed && fixture.live==1);
        fixture.fail_close=phase==3;
        CHECK(d3dkmt_release_device(&last)==0 && !last.self);
        CHECK(fixture.closes==closed+1 && fixture.live==(phase==3));
        if (phase==3) {
            CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(d3dkmt_collect_devices()==STATUS_NO_MEMORY);
            fixture.fail_close=0;
            CHECK(d3dkmt_collect_devices()==0 && !fixture.live);
        }
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    }
    CHECK(d3dkmt_release_device(NULL)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
    for (unsigned round=0;round<32;round++) {
        CHECK(d3dkmt_open_adapter(40,fixture.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(40,adapter,0,&device)==0);
        LEASE_RACE race={.device=device}; pthread_t threads[4];
        for (unsigned i=0;i<4;i++) CHECK(pthread_create(&threads[i],NULL,device_lease_worker,&race)==0);
        while (__atomic_load_n(&race.entered,__ATOMIC_ACQUIRE)!=4) sched_yield();
        unsigned closed=fixture.closes;
        CHECK(d3dkmt_destroy_device(40,device)==0 && fixture.closes==closed && fixture.live==1);
        __atomic_store_n(&race.resume,1,__ATOMIC_RELEASE);
        for (unsigned i=0;i<4;i++) CHECK(pthread_join(threads[i],NULL)==0);
        CHECK(fixture.closes==closed+1 && fixture.live==0);
        CHECK(d3dkmt_close_adapter(40,adapter)==0);
    }
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);

    /* Callback reentry: cancellation rolls back without publishing a device. */
    for (int mode=1;mode<=2;mode++) {
        CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
        CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
        fixture.reenter=mode; fixture.fail_close=1; device=0xfeed;
        NTSTATUS expected=mode==1 ? STATUS_PROCESS_IS_TERMINATING : D3DKMT_STATUS_DEVICE_REMOVED;
        CHECK(d3dkmt_create_device(10,adapter,0,&device)==expected && device==0xfeed);
        CHECK(fixture.live==1);
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        fixture.reenter=0; fixture.fail_close=0;
        CHECK(d3dkmt_collect_devices()==0 && fixture.live==0);
        d3dkmt_release_process(10);
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    }

    /* Force each removal/exit interleaving while open is outside the lock. */
    for (int mode=0;mode<3;mode++) {
        CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
        OPEN_RACE race={.pid=10,.device=0xfeed};
        CHECK(d3dkmt_open_adapter(race.pid,fixture.luid,&race.adapter)==0);
        fixture.gate=1; fixture.entered=fixture.proceed=0;
        fixture.fail_open=mode==2;
        pthread_t thread;
        CHECK(pthread_create(&thread,NULL,release_during_open,&race)==0);
        while (!__atomic_load_n(&fixture.entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (mode==0 || mode==2) d3dkmt_release_process(race.pid);
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&fixture.proceed,1,__ATOMIC_RELEASE);
        CHECK(pthread_join(thread,NULL)==0);
        NTSTATUS expected=mode==0 ? STATUS_PROCESS_IS_TERMINATING :
            mode==1 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_NO_MEMORY;
        CHECK(race.result==expected && race.device==0xfeed && fixture.live==0);
        d3dkmt_release_process(race.pid);
        CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
        fixture.gate=fixture.fail_open=0;
    }

    CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);
    pthread_t workers[4];
    for (unsigned i=0;i<4;i++) CHECK(pthread_create(&workers[i],NULL,device_worker,(void *)(uintptr_t)(20+i))==0);
    for (unsigned i=0;i<4;i++) CHECK(pthread_join(workers[i],NULL)==0);
    CHECK(fixture.live==0);
    CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
    unsigned previous=fixture.opens;
    for (unsigned i=1;i<D3DKMT_MAX_HANDLES;i++) CHECK(d3dkmt_open_adapter(11,fixture.luid,&second)==0);
    device=0xfeed;
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==STATUS_NO_MEMORY && device==0xfeed);
    CHECK(fixture.opens==previous);
    d3dkmt_release_process(11);
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==0);
    d3dkmt_release_process(10);
    CHECK(fixture.live==0 && d3dkmt_destroy_device(10,device)==STATUS_INVALID_PARAMETER);

    /* One failing close must not prevent collecting another process's device. */
    CHECK(d3dkmt_open_adapter(10,fixture.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(10,adapter,0,&device)==0);
    CHECK(d3dkmt_open_adapter(11,fixture.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(11,adapter,0,&second)==0);
    fixture.fail_close=1;
    d3dkmt_release_process(10); d3dkmt_release_process(11);
    CHECK(fixture.live==2);
    fixture.fail_close=0; fixture.fail_close_pid=10;
    (void)d3dkmt_collect_devices();
    (void)d3dkmt_collect_devices();
    CHECK(fixture.live==1);
    CHECK(d3dkmt_destroy_device(11,second)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    fixture.fail_close_pid=0;
    CHECK(d3dkmt_collect_devices()==0 && fixture.live==0);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    CHECK(d3dkmt_register_driver(&description,&provider,&fixture.luid)==0);

    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user!=MAP_FAILED) {
        for (int compat=0;compat<=1;compat++) {
            g_compat32_mode=compat;
            CHECK(d3dkmt_open_adapter(caller,fixture.luid,&adapter)==0);
            size_t bytes=compat ? 36 : sizeof(OKMT_CREATE_DEVICE);
            unsigned char expected[sizeof(OKMT_CREATE_DEVICE)];
            memset(user,0xa5,4096);
            if (compat) { ((DWORD *)user)[0]=adapter; ((DWORD *)user)[1]=0; }
            else { ((OKMT_CREATE_DEVICE *)user)->object.adapter=adapter; ((OKMT_CREATE_DEVICE *)user)->flags=0; }
            memcpy(expected,user,bytes);
            previous=fixture.opens;
            denied_first=(uintptr_t)(user+bytes-1); denied_last=denied_first+1;
            CHECK(D3DKMTCreateDevice(user)==STATUS_INVALID_PARAMETER);
            CHECK(fixture.opens==previous && !memcmp(user,expected,bytes));
            denied_first=denied_last=0;
            CHECK(D3DKMTCreateDevice(user+4096-bytes+1)==STATUS_INVALID_PARAMETER);
            fixture.fail_open=1;
            CHECK(D3DKMTCreateDevice(user)==STATUS_NO_MEMORY && !memcmp(user,expected,bytes));
            fixture.fail_open=0;
            CHECK(D3DKMTCreateDevice(user)==0);
            if (compat) {
                DWORD *p=(DWORD *)user; device=p[2];
                CHECK(p[0]==adapter && p[1]==0);
                for (unsigned i=3;i<9;i++) CHECK(p[i]==0);
            } else {
                OKMT_CREATE_DEVICE *p=(OKMT_CREATE_DEVICE *)user; device=p->device;
                CHECK(p->object.adapter==adapter && p->flags==0);
                CHECK(!p->command_buffer && !p->command_bytes && !p->allocation_list &&
                    !p->allocation_count && !p->patch_list && !p->patch_count);
            }
            CHECK(device && user[bytes]==0xa5);
            *(OKMT_DESTROY_DEVICE *)(user+128)=(OKMT_DESTROY_DEVICE){device};
            caller++;
            CHECK(D3DKMTDestroyDevice(user+128)==STATUS_INVALID_PARAMETER);
            caller--;
            CHECK(D3DKMTDestroyDevice(user+128)==0);
            CHECK(D3DKMTDestroyDevice(user+128)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTDestroyDevice(user+4093)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTCreateDevice(NULL)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTDestroyDevice(NULL)==STATUS_INVALID_PARAMETER);
            d3dkmt_release_process(caller);
        }
        CHECK(munmap(user,4096)==0);
    }
    CHECK(fixture.live==0);
    CHECK(d3dkmt_collect_devices()==0);
    CHECK(d3dkmt_unregister_adapter(fixture.luid)==0);
    CHECK(d3dkmt_enumerate_adapters(10,0,NULL,&count)==0 && count==0);
    printf("D3DKMT device checks=%u failures=%u opens=%u closes=%u live=%u\n",
        checks,failures,fixture.opens,fixture.closes,fixture.live);
    return failures ? 1 : 0;
}
