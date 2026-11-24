/* A second product may share the registered VC2013 servicing component only
 * with the same key path. The expected fields below come from the independently
 * captured original MSI Registry/Component tables, not from the engine plan. */
#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_engine.c"
static int mode;
static const uint16_t product[] = u"{00000000-0000-0000-0000-000000000001}";
static const uint16_t component[] = u"Components\\{02A33D46-BBAA-3AE1-B18F-390B34F69D25}";
static const uint16_t path[] = u"SOFTWARE\\Microsoft\\DevDiv\\vc\\Servicing\\12.0";
LONG WINAPI RegOpenKeyExW(HKEY root, PCWSTR name, DWORD options, DWORD access, PHKEY out)
{
    CHECK(!options && access == (KEY_READ | KEY_WOW64_64KEY)); *out = NULL;
    if (root == HKEY_LOCAL_MACHINE && equal_wide(name,MSI_PRODUCT_STORE)) *out = (HKEY)1;
    else if (root == (HKEY)1 && equal_wide(name,product)) *out = (HKEY)2;
    else if (root == (HKEY)2 && equal_wide(name,component)) *out = (HKEY)3;
    return *out ? 0 : 2;
}
LONG WINAPI RegEnumKeyExW(HKEY key, DWORD index, PWSTR name, DWORD *length, DWORD *reserved, PWSTR cls, DWORD *clen, PVOID stamp)
{
    CHECK(key == (HKEY)1 && !reserved && !cls && !clen && !stamp && *length >= 39);
    if (index) return 259;
    memcpy(name,product,sizeof(product)); *length = 38; return 0;
}
LONG WINAPI RegQueryValueExW(HKEY key, PCWSTR name, DWORD *reserved, DWORD *type, BYTE *out, DWORD *size)
{
    CHECK(key == (HKEY)3 && !reserved);
    uint32_t number = 0, count = 4; const void *data = &number; *type = REG_DWORD;
    if (equal_wide(name,EW("Kind"))) number = 2;
    else if (equal_wide(name,EW("Root"))) number = mode == 6 ? 1 : 2;
    else if (equal_wide(name,EW("View"))) number = mode == 7 ? 256 : 512;
    else if (equal_wide(name,EW("State"))) number = mode == 5 ? 2 : 3;
    else if (equal_wide(name,EW("KeyOnly"))) number = 0;
    else if (equal_wide(name,EW("Name"))) { data = u"SP"; count = 6; *type = REG_SZ; }
    else if (equal_wide(name,EW("Path"))) {
        if (mode == 4) return 2;
        data = path; count = sizeof(path); *type = mode == 2 ? REG_BINARY : REG_SZ;
        if (mode == 8 && out) { *size = count + 2; return 234; }
    } else { CHECK(0); return 2; }
    if (out && *size < count) { *size = count; return 234; }
    if (out) { memcpy(out,data,count); if (mode == 3 && equal_wide(name,EW("Path"))) out[0] = 'X'; }
    *size = count; return 0;
}
LONG WINAPI RegCloseKey(HKEY key)
{ CHECK(key == (HKEY)1 || key == (HKEY)2 || key == (HKEY)3); return 0; }
int main(int argc,char **argv)
{
    CHECK(argc == 3);
    uint32_t size,error;uint8_t *data=read_file(argv[1],&size);
    msi_db_t *db=msi_open(data,size); CHECK(db != NULL);
    msi_package_t *package=msi_package_create(db,&error);CHECK(package && !error);
    FILE *file=fopen(argv[2],"rb");CHECK(file && get_number(file)==0x314b504dU);
    uint32_t properties=get_number(file);
    for(uint32_t i=0;i<properties;i++){
        uint16_t *name=get_text(file),*value=get_text(file);
        CHECK(!msi_package_set(package,name,value));free(name);free(value);
    }
    fclose(file);
    msi_plan_t *plan=msi_plan_create_fresh(db,package,NULL,NULL,&error);CHECK(plan && !error);
    msi_registry_t *authored=msi_registry_prepare(plan,&error);CHECK(authored && !error);
    msi_summary_t *summary=msi_summary_read(db,&error);CHECK(summary && !error);
    msi_registry_t *registration=msi_registration_prepare(plan,summary,authored,EW("C:\\source\\"),EW("C:\\Windows\\Installer\\cache.msi"),&error);
    CHECK(registration && !error);
    unsigned baseline=allocations;
    for(mode=1;mode<=8;mode++){
        error=engine_shared_components(registration);
        CHECK(error==(mode==1?0:mode==4?2:mode==8?234:1638));CHECK(allocations==baseline);
    }
    mode=1;
    for(int i=0;i<12;i++){
        fail_after=i;error=engine_shared_components(registration);fail_after=-1;
        CHECK(error==0 || error==8);CHECK(allocations==baseline);
    }
    msi_registry_close(registration);msi_summary_close(summary);msi_registry_close(authored);
    msi_plan_close(plan);msi_package_close(package);msi_close(db);free(data);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI SHARED COMPONENT PASS cases=20 checks=%u\n",checks);return 0;
}
