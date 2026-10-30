/* Real native memory manager, synthetic DMA; no public KMT claim. */
#define main resource_suite_main
#include "intel_gpu_resource_host.c"
#undef main

static intel_gpu_memory_resource_t *offered_resource(intel_gpu_memory_t *m,uint32_t id)
{for(unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)if(m->resources[i].id==id)return &m->resources[i];return NULL;}
static void offered_close(intel_gpu_memory_t *m,uint32_t device,uint32_t id)
{
    CHECK(intel_gpu_memory_close(m,7,device,id)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_release_resource(m,7,device,id)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(m)==INTEL_GPU_BUFFER_OK);
}
static void offered_cleanup(intel_gpu_memory_t *m,uint32_t device)
{
    CHECK(intel_gpu_memory_close_device(m,7,device)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(m)==INTEL_GPU_BUFFER_OK && !m->pages);
    CHECK(ram.allocated==ram.freed);res_reset();
}
static void offered_fill(intel_gpu_memory_t *m,uint32_t device,uint32_t id,unsigned char value)
{
    intel_gpu_memory_cpu_lease_t cpu={0};
    CHECK(intel_gpu_memory_acquire_cpu(m,7,device,id,&cpu)==INTEL_GPU_BUFFER_OK);
    memset(cpu.cpu,value,(size_t)cpu.bytes);
    CHECK(intel_gpu_memory_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
}
static void offered_check(intel_gpu_memory_t *m,uint32_t device,uint32_t id,unsigned char value)
{
    intel_gpu_memory_cpu_lease_t cpu={0};
    CHECK(intel_gpu_memory_acquire_cpu(m,7,device,id,&cpu)==INTEL_GPU_BUFFER_OK);
    int good=1;for(uint64_t i=0;i<cpu.bytes;i++)if(((unsigned char *)cpu.cpu)[i]!=value)good=0;
    CHECK(good);CHECK(intel_gpu_memory_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
}
static void offered_basic(void)
{
    res_reset();intel_gpu_memory_t m={0};uint32_t device,a,b,c,d;
    CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
    CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,4097,&a)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&b)==INTEL_GPU_BUFFER_OK);
    offered_fill(&m,device,a,0x32);offered_fill(&m,device,b,0x64);
    uint64_t cookies[]={a,b,a},bad[]={a,b,0x100000000ULL|a};
    CHECK(intel_gpu_memory_offer(&m,7,device,bad,3,2)==INTEL_GPU_BUFFER_INVALID);
    CHECK(!offered_resource(&m,a)->offer_priority && !offered_resource(&m,b)->offer_priority);
    CHECK(intel_gpu_memory_offer(&m,8,device,cookies,3,2)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies,3,0)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies,3,5)==INTEL_GPU_BUFFER_INVALID);
    intel_gpu_memory_cpu_lease_t held={0};
    CHECK(intel_gpu_memory_acquire_cpu(&m,7,device,b,&held)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies,3,2)==INTEL_GPU_BUFFER_BUSY);
    CHECK(!offered_resource(&m,a)->offer_priority);
    CHECK(intel_gpu_memory_release_cpu(&held)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies,3,2)==INTEL_GPU_BUFFER_OK);
    CHECK(m.pages==3 && !ram.freed);
    CHECK(intel_gpu_memory_acquire_cpu(&m,7,device,a,&held)==INTEL_GPU_BUFFER_NOT_RESIDENT && !held.self);
    intel_gpu_memory_resident_lease_t lease={0};intel_gpu_memory_lease_t pin={0};
    CHECK(intel_gpu_memory_acquire(&m,7,device,a,&pin)==INTEL_GPU_BUFFER_NOT_RESIDENT && !pin.self);
    CHECK(intel_gpu_memory_acquire_resident(&m,7,device,a,&lease)==INTEL_GPU_BUFFER_NOT_RESIDENT && !lease.self);
    CHECK(intel_gpu_memory_residency(&m,7,device,true,cookies,3)==INTEL_GPU_BUFFER_NOT_RESIDENT);
    intel_gpu_memory_reclaim_work_t work={0};
    CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,3,&work)==INTEL_GPU_BUFFER_OK);
    CHECK(!work.discarded[0] && !work.discarded[1] && !work.discarded[2]);
    intel_gpu_memory_reclaim_work_t moved=work;
    CHECK(intel_gpu_memory_reclaim_release(&moved)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_reclaim_commit(&work)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_reclaim_commit(&work)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK && !work.self);
    offered_check(&m,device,a,0x32);offered_check(&m,device,b,0x64);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies,3,2)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_offer(&m,7,device,cookies+1,1,1)==INTEL_GPU_BUFFER_OK);
    uint32_t freed=0;
    CHECK(intel_gpu_memory_trim(&m,0,1,&freed)==INTEL_GPU_BUFFER_OK && freed==1 && m.pages==2);
    CHECK(offered_resource(&m,b)->discarded && !offered_resource(&m,a)->discarded);
    uint32_t residency[3]={0};
    CHECK(intel_gpu_memory_properties(&m,7,device,INTEL_GPU_PROPERTY_RESIDENCY,cookies,3,residency)==INTEL_GPU_BUFFER_OK);
    CHECK(residency[0]==2 && residency[1]==3 && residency[2]==2);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,8192,&c)==INTEL_GPU_BUFFER_OK && m.pages==4);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&d)==INTEL_GPU_BUFFER_OK && m.pages==3);
    CHECK(offered_resource(&m,a)->discarded); /* Actual pressure reclaimed two pages. */
    CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,3,&work)==INTEL_GPU_BUFFER_NO_MEMORY && !work.self);
    CHECK(offered_resource(&m,a)->offer_priority && offered_resource(&m,b)->offer_priority);
    offered_close(&m,device,c);offered_close(&m,device,d);CHECK(!m.pages);
    CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,3,&work)==INTEL_GPU_BUFFER_OK && m.pages==3);
    CHECK(work.discarded[0]==1 && work.discarded[1]==1 && work.discarded[2]==1);
    CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK); /* Abort publication. */
    CHECK(offered_resource(&m,a)->discarded && offered_resource(&m,b)->discarded);
    CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,3,&work)==INTEL_GPU_BUFFER_OK);
    CHECK(work.discarded[0]==1 && work.discarded[1]==1 && work.discarded[2]==1);
    CHECK(intel_gpu_memory_reclaim_commit(&work)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK);
    offered_check(&m,device,a,0);offered_check(&m,device,b,0);
    offered_close(&m,device,a);offered_close(&m,device,b);offered_cleanup(&m,device);
}

static void offered_capture(void)
{
    for(unsigned queued=0;queued<2;queued++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t device,id;uint64_t cookie;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,1,1));
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&id)==INTEL_GPU_BUFFER_OK);cookie=id;
        intel_gpu_memory_resident_lease_t lease={0};
        CHECK(intel_gpu_memory_acquire_resident(&m,7,device,id,&lease)==INTEL_GPU_BUFFER_OK);
        if(queued)CHECK(intel_gpu_memory_reserve_work(&lease)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,2)==INTEL_GPU_BUFFER_OK);
        if(!queued)CHECK(intel_gpu_memory_reserve_work(&lease)==INTEL_GPU_BUFFER_NOT_RESIDENT);
        uint32_t freed=99;CHECK(intel_gpu_memory_trim(&m,0,1,&freed)==INTEL_GPU_BUFFER_OK && !freed && m.pages==1);
        CHECK(intel_gpu_memory_release_resident(&lease)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_trim(&m,0,1,&freed)==INTEL_GPU_BUFFER_OK && freed==1 && !m.pages);
        offered_close(&m,device,id);offered_cleanup(&m,device);
    }
}

static void offered_quota(void)
{
    res_reset();intel_gpu_memory_t m={0};uint32_t device,other,a,b,c;
    CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,2));
    CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_open_device(&m,8,&other)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,8192,&a)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create(&m,8,other,8192,&b)==INTEL_GPU_BUFFER_OK);
    uint64_t ca=a,cb=b;
    CHECK(intel_gpu_memory_offer(&m,8,other,&cb,1,1)==INTEL_GPU_BUFFER_OK);
    c=0xeeee;CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&c)==INTEL_GPU_BUFFER_NO_MEMORY && c==0xeeee);
    CHECK(m.pages==4 && !offered_resource(&m,b)->discarded); /* Another PID cannot fix our quota. */
    CHECK(intel_gpu_memory_offer(&m,7,device,&ca,1,3)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&c)==INTEL_GPU_BUFFER_OK);
    CHECK(m.pages==3 && offered_resource(&m,a)->discarded && !offered_resource(&m,b)->discarded);
    offered_close(&m,device,c);offered_close(&m,device,a);
    CHECK(intel_gpu_memory_close_device(&m,8,other)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);offered_cleanup(&m,device);
}

typedef struct {intel_gpu_memory_t *m;uint32_t device;uint64_t cookie;
    intel_gpu_memory_reclaim_work_t work;enum intel_gpu_buffer_status status;} OFFER_RACE;
static void *offered_worker(void *argument)
{
    OFFER_RACE *r=argument;r->status=intel_gpu_memory_reclaim_prepare(r->m,7,r->device,&r->cookie,1,&r->work);
    return NULL;
}
static void offered_exit(void)
{
    for(unsigned action=0;action<4;action++)for(unsigned round=0;round<16;round++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t device,id;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&id)==INTEL_GPU_BUFFER_OK);
        uint64_t cookie=id;uint32_t freed=0;
        CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,1)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_trim(&m,0,1,&freed)==INTEL_GPU_BUFFER_OK && freed==1);
        OFFER_RACE race={.m=&m,.device=device,.cookie=id};pthread_t thread;
        ram.hold=1;CHECK(!pthread_create(&thread,NULL,offered_worker,&race));
        while(!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE))sched_yield();
        if(action==0)CHECK(intel_gpu_memory_close(&m,7,device,id)==INTEL_GPU_BUFFER_OK);
        if(action==1)CHECK(intel_gpu_memory_close_device(&m,7,device)==INTEL_GPU_BUFFER_OK);
        if(action==2)intel_gpu_memory_release_process(7);
        if(action==3)intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && ram.freed==1);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);CHECK(!pthread_join(thread,NULL));ram.hold=0;
        CHECK(race.status==INTEL_GPU_BUFFER_INVALID && !race.work.self && m.pages==1);
        CHECK(!offered_resource(&m,id)->busy && !offered_resource(&m,id)->reclaim_owner);
        CHECK(intel_gpu_memory_release_resource(&m,7,device,id)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages);
        intel_gpu_memory_shutdown(&m);CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(ram.allocated==ram.freed);res_reset();
    }
}

static void offered_partial_failure(void)
{
    for(unsigned mode=0;mode<3;mode++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t device,a,b,freed;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,2,2));
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&a)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&b)==INTEL_GPU_BUFFER_OK);
        offered_fill(&m,device,a,0x31);offered_fill(&m,device,b,0x32);
        uint64_t cookies[]={a,b};
        CHECK(intel_gpu_memory_offer(&m,7,device,cookies,2,2)==INTEL_GPU_BUFFER_OK);
        if(mode==2)ram.fail_release=ram.releases+1;
        CHECK(intel_gpu_memory_trim(&m,0,2,&freed)==INTEL_GPU_BUFFER_OK);
        CHECK(freed==(mode==2?1u:2u) && m.pages==(mode==2?1u:0u));
        if(mode==0)ram.fail_allocate=ram.allocated+2;
        if(mode==1)ram.fail_sync=ram.syncs+2;
        intel_gpu_memory_reclaim_work_t work={0};
        enum intel_gpu_buffer_status status=intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,2,&work);
        if(mode<2) {
            CHECK(status==(mode==0?INTEL_GPU_BUFFER_NO_MEMORY:INTEL_GPU_BUFFER_IO) && !work.self);
            CHECK(offered_resource(&m,a)->offer_priority && offered_resource(&m,b)->offer_priority);
            CHECK(offered_resource(&m,a)->discarded && offered_resource(&m,b)->discarded);
            CHECK(!offered_resource(&m,a)->busy && !offered_resource(&m,b)->busy);
            CHECK(m.pages==(mode==0?1u:2u));
            unsigned allocated=ram.allocated;
            CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,cookies,2,&work)==INTEL_GPU_BUFFER_OK);
            CHECK(ram.allocated==allocated+(mode==0));
            CHECK(work.discarded[0]==1 && work.discarded[1]==1);
        } else CHECK(status==INTEL_GPU_BUFFER_OK && !work.discarded[0] && work.discarded[1]);
        CHECK(intel_gpu_memory_reclaim_commit(&work)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK);
        offered_check(&m,device,a,mode==2?0x31:0);offered_check(&m,device,b,0);
        offered_close(&m,device,a);offered_close(&m,device,b);
        CHECK(intel_gpu_memory_close_device(&m,7,device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages);
        CHECK(ram.allocated==ram.freed+(mode==0));res_reset();
    }
}

static void offered_commit_close(void)
{
    for(unsigned action=0;action<4;action++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t device,id,freed;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,2,2));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&id)==INTEL_GPU_BUFFER_OK);
        uint64_t cookie=id;intel_gpu_memory_reclaim_work_t work={0};
        CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,2)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_trim(&m,0,1,&freed)==INTEL_GPU_BUFFER_OK && freed==1);
        CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,&cookie,1,&work)==INTEL_GPU_BUFFER_OK);
        if(action==0)CHECK(intel_gpu_memory_close(&m,7,device,id)==INTEL_GPU_BUFFER_OK);
        if(action==1)CHECK(intel_gpu_memory_close_device(&m,7,device)==INTEL_GPU_BUFFER_OK);
        if(action==2)intel_gpu_memory_release_process(7);
        if(action==3)intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_reclaim_commit(&work)==INTEL_GPU_BUFFER_INVALID);
        CHECK(work.attempted && work.discarded[0] && offered_resource(&m,id)->discarded);
        CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_resource(&m,7,device,id)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages);
        intel_gpu_memory_shutdown(&m);CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK && ram.allocated==ram.freed);
        res_reset();
    }
}

typedef struct {intel_gpu_memory_t *m;uint32_t device,id;
    intel_gpu_memory_cpu_lease_t lease;enum intel_gpu_buffer_status status;} OFFER_CPU_RACE;
static void *offered_cpu_worker(void *argument)
{
    OFFER_CPU_RACE *r=argument;
    r->status=intel_gpu_memory_acquire_cpu(r->m,7,r->device,r->id,&r->lease);
    return NULL;
}
static void offered_property_snapshot(void)
{
    res_reset();intel_gpu_memory_t m={0};uint32_t device,id;
    CHECK(intel_gpu_memory_init(&m,&allocator,&ram,2,2));
    CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&id)==INTEL_GPU_BUFFER_OK);
    uint64_t cookie=id;uint32_t value=0xeeee;OFFER_CPU_RACE race={.m=&m,.device=device,.id=id};
    ram.hold=1;pthread_t thread;CHECK(!pthread_create(&thread,NULL,offered_cpu_worker,&race));
    while(!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE))sched_yield();
    CHECK(intel_gpu_memory_properties(&m,7,device,INTEL_GPU_PROPERTY_RESIDENCY,&cookie,1,&value)==INTEL_GPU_BUFFER_OK);
    CHECK(value==2);value=0x77;
    CHECK(intel_gpu_memory_properties(&m,7,device,INTEL_GPU_PROPERTY_SET_PRIORITY,&cookie,1,&value)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,1)==INTEL_GPU_BUFFER_BUSY);
    __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);CHECK(!pthread_join(thread,NULL));ram.hold=0;
    CHECK(race.status==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_release_cpu(&race.lease)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,1)==INTEL_GPU_BUFFER_OK);
    intel_gpu_memory_reclaim_work_t work={0};
    CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,&cookie,1,&work)==INTEL_GPU_BUFFER_OK);
    value=0xeeee;
    CHECK(intel_gpu_memory_properties(&m,7,device,INTEL_GPU_PROPERTY_RESIDENCY,&cookie,1,&value)==INTEL_GPU_BUFFER_BUSY);
    CHECK(value==0xeeee);
    CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK);
    offered_close(&m,device,id);offered_cleanup(&m,device);
}

typedef struct {intel_gpu_memory_reclaim_work_t *work;enum intel_gpu_buffer_status result;} COMMIT_RACE;
static void *offered_commit_worker(void *argument)
{COMMIT_RACE *r=argument;r->result=intel_gpu_memory_reclaim_commit(r->work);return NULL;}
static void offered_commit_races(void)
{
    for(unsigned round=0;round<32;round++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t device,id;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,2,2));
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,device,4096,&id)==INTEL_GPU_BUFFER_OK);
        uint64_t cookie=id;intel_gpu_memory_reclaim_work_t work={0};
        CHECK(intel_gpu_memory_offer(&m,7,device,&cookie,1,2)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_reclaim_prepare(&m,7,device,&cookie,1,&work)==INTEL_GPU_BUFFER_OK);
        COMMIT_RACE races[2]={{&work,INTEL_GPU_BUFFER_BUSY},{&work,INTEL_GPU_BUFFER_BUSY}};pthread_t threads[2];
        for(unsigned i=0;i<2;i++)CHECK(!pthread_create(&threads[i],NULL,offered_commit_worker,&races[i]));
        for(unsigned i=0;i<2;i++)CHECK(!pthread_join(threads[i],NULL));
        CHECK((races[0].result==INTEL_GPU_BUFFER_OK && races[1].result==INTEL_GPU_BUFFER_INVALID) ||
              (races[1].result==INTEL_GPU_BUFFER_OK && races[0].result==INTEL_GPU_BUFFER_INVALID));
        CHECK(intel_gpu_memory_reclaim_release(&work)==INTEL_GPU_BUFFER_OK);
        offered_close(&m,device,id);offered_cleanup(&m,device);
    }
}

int main(void)
{
    resource_suite_main();unsigned before=checks;
    offered_basic();offered_capture();offered_quota();offered_exit();
    offered_partial_failure();offered_commit_close();
    offered_property_snapshot();
    offered_commit_races();
    printf("Intel GPU memory offer: %u checks passed, 64 forced exit races, 32 concurrent commit groups\n",checks-before);
    return 0;
}
