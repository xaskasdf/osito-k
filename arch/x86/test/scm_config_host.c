/* Test the actual SCM1 encoder/decoder, including the oldest full field.
 * Unused kernel entry points are discarded; no privileged code executes. */
#include "../win32/scm.c"
extern int printf(const char *, ...);
extern void exit(int);

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){printf("SCM HOST FAIL %d\n",__LINE__);exit(1);}} while(0)

int main(void)
{
    SCM_SERVICE original={0},restored={0};
    SCM_STORE_RECORD record={0};
    scm_copy(original.name,sizeof(original.name),"probe");
    scm_copy(original.binary_path,sizeof(original.binary_path),"C:\\inert.exe");
    original.tag_id=0x12345678;
    WCHAR dependencies[]={ 'a',0,'+', 'b',0,0 };
    CHECK(scm_wide_multisz(original.dependencies,sizeof(original.dependencies),dependencies));
    CHECK(scm_multisz_bytes(original.dependencies,sizeof(original.dependencies))==6);
    scm_store_record_from_service(&record,&original);
    CHECK(scm_store_record_valid(&record));
    scm_store_apply_config(&restored,&record);
    for(unsigned i=0;i<6;i++)CHECK(restored.dependencies[i]==(char)dependencies[i]);
    CHECK(restored.tag_id==0x12345678);
    BYTE encoded[520];
    DWORD offset=0;
    CHECK(scm_config_write_string(encoded,&offset,restored.dependencies,TRUE,FALSE)==encoded);
    CHECK(offset==6);
    for(unsigned i=0;i<6;i++)CHECK(encoded[i]==dependencies[i]);
    offset=0;scm_config_write_string(encoded,&offset,restored.dependencies,TRUE,TRUE);
    CHECK(offset==12);
    for(unsigned i=0;i<6;i++)CHECK(((WCHAR *)(void *)encoded)[i]==dependencies[i]);

    /* Legacy format: all 255 data bytes followed by a single NUL. */
    for(unsigned i=0;i<255;i++)record.dependencies[i]='x';
    record.dependencies[255]=0;
    CHECK(scm_store_record_valid(&record));
    scm_store_apply_config(&restored,&record);
    CHECK(scm_multisz_bytes(restored.dependencies,256)==257);
    offset=0;scm_config_write_string(encoded,&offset,restored.dependencies,TRUE,TRUE);
    CHECK(offset==514);
    for(unsigned i=0;i<255;i++)CHECK(((WCHAR *)(void *)encoded)[i]=='x');
    CHECK(((WCHAR *)(void *)encoded)[255]==0&&((WCHAR *)(void *)encoded)[256]==0);
    scm_store_record_from_service(&record,&restored);
    CHECK(scm_store_record_valid(&record)&&record.dependencies[254]=='x'&&!record.dependencies[255]);
    record.dependencies[255]='x';
    CHECK(!scm_store_record_valid(&record));
    record.dependencies[0]=0;
    CHECK(!scm_store_record_valid(&record));
    record.dependencies[1]=0;
    CHECK(scm_store_record_valid(&record));

    WCHAR full[256];for(unsigned i=0;i<256;i++)full[i]='x';
    CHECK(!scm_wide_multisz(restored.dependencies,256,full));
    full[254]=full[255]=0;
    CHECK(scm_wide_multisz(restored.dependencies,256,full));
    CHECK(scm_multisz_bytes(restored.dependencies,256)==256);
    CHECK(scm_wide_multisz(restored.dependencies,256,(WCHAR[]){0}));
    for(unsigned i=0;i<256;i++)CHECK(!restored.dependencies[i]);
    printf("SCM HOST checks=%u failures=0\n",checks);
    return 0;
}
