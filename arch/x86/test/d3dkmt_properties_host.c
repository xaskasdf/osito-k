#define main properties_adapter_regression
#include "d3dkmt_registry_host.c"
#undef main

static struct { unsigned serial,live,calls,closed,hook; DWORD states[64],priority[64];
    OKMT_HANDLE device,resource; } property;
static NTSTATUS property_open(void *c,DWORD pid,uint64_t *cookie)
{ CHECK(c==&property && pid==100);*cookie=11;return 0; }
static NTSTATUS property_close(void *c,DWORD pid,uint64_t cookie)
{ CHECK(c==&property && pid==100 && cookie==11 && !property.live);return 0; }
static NTSTATUS property_allocate(void *c,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *d,uint64_t *cookie)
{
    CHECK(c==&property && pid==100 && device==11 && d->bytes);
    *cookie=++property.serial;property.live++;property.states[*cookie]=2;
    property.priority[*cookie]=0x78000000;return 0;
}
static NTSTATUS property_free(void *c,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(c==&property && pid==100 && device==11 && property.states[cookie]);
    property.states[cookie]=0;property.live--;property.closed++;return 0;
}
static NTSTATUS property_transaction(void *c,DWORD pid,uint64_t device,DWORD operation,
    const uint64_t *cookies,DWORD count,DWORD *values)
{
    CHECK(c==&property && pid==100 && device==11);property.calls++;
    unsigned hook=property.hook;property.hook=0;
    if (hook==1) {
        unsigned before=property.closed;
        CHECK(d3dkmt_destroy_resource(pid,property.device,property.resource)==0);
        CHECK(property.closed==before); /* all backing is retained */
    } else if (hook==2) {denied_first=(uintptr_t)(user+8192);denied_last=denied_first+4096;}
    else if (hook==3) {*(DWORD *)(user+512)=0;*(DWORD *)(user+8192)=0;}
    else if (hook==4) return STATUS_UNSUCCESSFUL;
    else if (hook==5) {values[0]=99;return 0;}
    else if (hook==6) {
        D3DKMT_ALLOCATION_DESCRIPTION d={4096};OKMT_HANDLE output=0,group=0;
        CHECK(d3dkmt_create_resource_allocations(pid,property.device,property.resource,FALSE,
            1,&d,&group,&output)==D3DKMT_STATUS_DEVICE_BUSY && !output);
    } else if (hook==7) {
        d3dkmt_release_process(pid);CHECK(property.live>0);
    }
    for (DWORD i=0;i<count;i++) {
        CHECK(cookies[i]>0 && cookies[i]<=property.serial && property.states[cookies[i]]);
        if (operation==D3DKMT_PROPERTY_RESIDENCY) values[i]=property.states[cookies[i]];
        else property.priority[cookies[i]]=values[i];
    }
    return 0;
}
typedef struct { DWORD device,resource,allocations,count,values; } PROP32;
static void property_header(BOOL compat,OKMT_HANDLE resource,DWORD count)
{
    if (compat) *(PROP32 *)user=(PROP32){property.device,resource,
        resource?0:(DWORD)(uintptr_t)(user+512),count,(DWORD)(uintptr_t)(user+8192)};
    else *(OKMT_ALLOCATION_PROPERTIES *)user=(OKMT_ALLOCATION_PROPERTIES){property.device,
        resource,resource?NULL:(void *)(user+512),count,(void *)(user+8192)};
}
static void property_cases(BOOL compat,unsigned phase)
{
    memset(&property,0,sizeof(property));caller=100;g_compat32_mode=compat;
    denied_first=denied_last=0;cancellation_requested=FALSE;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000};OKMT_LUID luid;
    D3DKMT_DEVICE_PROVIDER d={property_open,property_close,&property,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_PROVIDER a={property_allocate,property_free,NULL,phase==1?NULL:property_transaction,NULL,NULL};
    OKMT_HANDLE adapter=0,handles[3];D3DKMT_ALLOCATION_DESCRIPTION input[3]={{4096},{5000},{3}};
    CHECK(d3dkmt_register_memory_driver(&description,&d,&a,&luid)==0);
    CHECK(d3dkmt_open_adapter(caller,luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&property.device)==0);
    CHECK(d3dkmt_create_resource_allocations(caller,property.device,0,TRUE,3,input,&property.resource,handles)==0);
    DWORD *values=(DWORD *)(user+8192);OKMT_HANDLE *list=(void *)(user+512);
    memcpy(list,handles,sizeof(handles));values[0]=values[1]=values[2]=0xfeed;
    property_header(compat,0,3);
    if (phase==1) {
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_NOT_SUPPORTED && values[0]==0xfeed);
        CHECK(D3DKMTSetAllocationPriority(user)==STATUS_NOT_SUPPORTED);
    } else if (phase==2) {
        property_header(compat,property.resource,0);property.hook=1;
        CHECK(D3DKMTQueryAllocationResidency(user)==0 && values[0]==2 && !property.live);
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER);
    } else if (phase==3) {
        property_header(compat,property.resource,0);property.hook=7;
        CHECK(D3DKMTQueryAllocationResidency(user)==0 && values[0]==2 && !property.live);
    } else {
        CHECK(D3DKMTQueryAllocationResidency(user)==0 && values[0]==2 && values[1]==2 && values[2]==2);
        property.states[2]=3;property_header(compat,property.resource,0);property.hook=6;
        CHECK(D3DKMTQueryAllocationResidency(user)==0 && values[0]==3);
        property.states[2]=1;
        CHECK(D3DKMTQueryAllocationResidency(user)==0 && values[0]==2);
        values[0]=0xa0000000;CHECK(D3DKMTSetAllocationPriority(user)==0);
        CHECK(property.priority[1]==0xa0000000 && property.priority[2]==0xa0000000 && property.priority[3]==0xa0000000);
        property_header(compat,0,3);values[0]=0;values[1]=0x28000000;values[2]=0xffffffff;property.hook=3;
        CHECK(D3DKMTSetAllocationPriority(user)==0);
        CHECK(property.priority[1]==0 && property.priority[2]==0x28000000 && property.priority[3]==0xffffffff);
        memcpy(list,handles,sizeof(handles));values[0]=values[1]=values[2]=0xfeed;
        property.hook=2;CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER && values[0]==0xfeed);
        denied_first=denied_last=0;property.hook=4;
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_UNSUCCESSFUL && values[0]==0xfeed);
        property.hook=5;CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_UNSUCCESSFUL && values[0]==0xfeed);
        unsigned called=property.calls;list[2]=0;
        CHECK(D3DKMTSetAllocationPriority(user)==STATUS_INVALID_PARAMETER && property.calls==called);
        CHECK(property.priority[1]==0 && property.priority[2]==0x28000000);
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER && values[0]==0xfeed);
        list[2]=handles[2];caller=101;
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER);caller=100;
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) list[i]=handles[0];
        property_header(compat,0,D3DKMT_MAX_HANDLES);
        CHECK(D3DKMTQueryAllocationResidency(user)==0);
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) CHECK(values[i]==2);
        property_header(compat,0,D3DKMT_MAX_HANDLES+1);
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER);
        property_header(compat,property.resource,1);
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_INVALID_PARAMETER);
        property_header(compat,0,0);CHECK(D3DKMTSetAllocationPriority(user)==STATUS_INVALID_PARAMETER);
        property_header(compat,0,1);cancellation_requested=TRUE;
        CHECK(D3DKMTQueryAllocationResidency(user)==STATUS_PROCESS_IS_TERMINATING);
        cancellation_requested=FALSE;
    }
    d3dkmt_release_process(caller);
    for (unsigned i=0;i<16;i++) {(void)d3dkmt_collect_allocations();(void)d3dkmt_collect_devices();}
    CHECK(!property.live && property.closed==3 && d3dkmt_unregister_adapter(luid)==0);
}
int main(void)
{
    user_bytes=16384;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    for (unsigned compat=0;compat<2;compat++)for (unsigned phase=0;phase<4;phase++) property_cases(compat,phase);
    CHECK(munmap(user,user_bytes)==0);
    printf("D3DKMT allocation properties: checks=%u failures=%u PE32/PE64\n",checks,failures);
    return failures?1:0;
}
