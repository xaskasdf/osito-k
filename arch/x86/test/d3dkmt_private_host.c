#define main adapter_suite_main
#include "d3dkmt_registry_host.c"
#undef main

int main(void)
{
    CHECK(adapter_suite_main()==0);
    unsigned start=checks;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000,.adapter_type=OKMT_ADAPTER_PROTOTYPE};
    OKMT_LUID luid;
    description.private_bytes=D3DKMT_MAX_ADAPTER_PRIVATE_BYTES+1;
    CHECK(d3dkmt_register_adapter(&description,&luid)==STATUS_INVALID_PARAMETER);
    user_bytes=4096; user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    for (unsigned length=1;length<=D3DKMT_MAX_ADAPTER_PRIVATE_BYTES;length++) {
        description.private_bytes=length;
        for (unsigned i=0;i<length;i++) description.private_data[i]=(BYTE)(length+i*13);
        CHECK(d3dkmt_register_adapter(&description,&luid)==0);
        /* Registration snapshots bytes; subsequent source writes are isolated. */
        description.private_data[0]^=0xff;
        OKMT_HANDLE handle=0; CHECK(d3dkmt_open_adapter(caller,luid,&handle)==0);
        for (g_compat32_mode=0;g_compat32_mode<=1;g_compat32_mode++) {
            for (unsigned size=0;size<=D3DKMT_MAX_ADAPTER_PRIVATE_BYTES+1;size++) {
                memset(user+1024,0xa7,256);
                if (g_compat32_mode) {
                    DWORD *q=(DWORD *)user; q[0]=handle; q[1]=OKMT_QUERY_PRIVATE_DRIVER_DATA;
                    q[2]=(DWORD)(uintptr_t)(user+1024); q[3]=size;
                } else *(OKMT_QUERY_ADAPTER_INFO *)user=(OKMT_QUERY_ADAPTER_INFO){handle,OKMT_QUERY_PRIVATE_DRIVER_DATA,user+1024,size};
                CHECK(D3DKMTQueryAdapterInfo(user)==(size==length ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER));
                for (unsigned i=0;i<256;i++) CHECK(user[1024+i]==(size==length && i<length ? (BYTE)(length+i*13) : 0xa7));
            }
            if (g_compat32_mode) ((DWORD *)user)[3]=length;
            else ((OKMT_QUERY_ADAPTER_INFO *)user)->bytes=length;
            memset(user+1024,0xa7,256);
            caller++; CHECK(D3DKMTQueryAdapterInfo(user)==STATUS_INVALID_PARAMETER); caller--;
            denied_first=(uintptr_t)(user+1024); denied_last=denied_first+1;
            CHECK(D3DKMTQueryAdapterInfo(user)==STATUS_INVALID_PARAMETER);
            denied_first=denied_last=0;
            CHECK(user[1024]==0xa7);
        }
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        CHECK(D3DKMTQueryAdapterInfo(user)==D3DKMT_STATUS_DEVICE_REMOVED);
        CHECK(d3dkmt_close_adapter(caller,handle)==0);
    }
    description.private_bytes=0; CHECK(d3dkmt_register_adapter(&description,&luid)==0);
    OKMT_HANDLE handle=0; CHECK(d3dkmt_open_adapter(caller,luid,&handle)==0);
    CHECK(d3dkmt_query_adapter(caller,handle,OKMT_QUERY_PRIVATE_DRIVER_DATA,user,1,0)==STATUS_NOT_SUPPORTED);
    CHECK(d3dkmt_close_adapter(caller,handle)==0 && d3dkmt_unregister_adapter(luid)==0);
    CHECK(munmap(user,user_bytes)==0);
    printf("D3DKMT private adapter: %u checks, failures=%u\n",checks-start,failures);
    return failures ? 1 : 0;
}
