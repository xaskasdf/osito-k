/* Persisted v1 hives written before HKCU software views were shared. */
#define main registry_store_tests_main
#include "registry_store_host.c"
#undef main
void serial_puthex(uint64_t value, int digits) { (void)value; (void)digits; }

static void legacy_value(const char *path, const char *name, BYTE view,
                         DWORD number, BYTE encoding)
{
    CHECK(reg_ensure_key(path, view) != NULL);
    for (unsigned i = 0; i < MAX_REG_VALUES; i++) if (!reg_values[i].used) {
        REG_VALUE *value = &reg_values[i];
        memset(value, 0, sizeof(*value));
        reg_strcpy(value->key_path, path); reg_strcpy(value->name, name);
        value->view = view; value->encoding = encoding; value->type = REG_DWORD;
        value->data_len = 4; memcpy(value->data, &number, 4); value->used = 1;
        return;
    }
    CHECK(0);
}

static void shared_value(const char *path, const char *name, DWORD expected)
{
    for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
        REG_VALUE *value = reg_find_value(path, name, view); DWORD number = 0;
        if (value) memcpy(&number, value->data, 4);
        CHECK(value && value->view == REG_VIEW_SHARED && number == expected);
    }
    unsigned copies = 0;
    for (unsigned i = 0; i < MAX_REG_VALUES; i++)
        copies += reg_values[i].used && !reg_stricmp(reg_values[i].key_path,path) &&
                  !reg_stricmp(reg_values[i].name,name);
    CHECK(copies == 1);
}

int main(int argc, char **argv)
{
    if (registry_store_tests_main(argc, argv)) return 1;
    stored_size = 0; reg_init_state = 0; reg_init();
    const char *path = "hkcu\\software\\osito-shared-test";
    const char *com = "hkcu\\software\\classes\\clsid\\osito-shared-test";
    legacy_value(path,"Only64",REG_VIEW_64,64,REG_DATA_LEGACY);
    legacy_value(path,"Only32",REG_VIEW_32,32,REG_DATA_LEGACY);
    legacy_value(path,"Conflict",REG_VIEW_32,32,REG_DATA_LEGACY);
    legacy_value(path,"Conflict",REG_VIEW_64,64,REG_DATA_LEGACY);
    legacy_value(path,"SharedWins",REG_VIEW_64,64,REG_DATA_LEGACY);
    legacy_value(path,"SharedWins",REG_VIEW_SHARED,0,REG_DATA_UTF16);
    legacy_value(com,"Separate",REG_VIEW_32,32,REG_DATA_LEGACY);
    legacy_value(com,"Separate",REG_VIEW_64,64,REG_DATA_LEGACY);
    reg_mark_dirty_locked(); CHECK(reg_store_save());
    unsigned before_writes = writes; reg_init_state = 0; reg_init();
    CHECK(writes == before_writes); /* Read-only compatibility during load. */
    shared_value(path,"Only64",64); shared_value(path,"Only32",32);
    shared_value(path,"Conflict",64); shared_value(path,"SharedWins",0);
    REG_VALUE *encoded = reg_find_value(path,"SharedWins",REG_VIEW_32);
    CHECK(encoded && encoded->encoding == REG_DATA_UTF16);
    for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
        REG_VALUE *value = reg_find_value(com,"Separate",view); DWORD number = 0;
        if (value) memcpy(&number,value->data,4);
        CHECK(value && value->view == view && number == (view == REG_VIEW_32 ? 32U : 64U));
    }
    DWORD update = 99;
    CHECK(reg_set_value_view(path,"Only64",REG_DWORD,&update,4,REG_VIEW_32)==0);
    shared_value(path,"Only64",99);
    HKEY key = NULL;
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER,"Software\\Osito-Shared-Test",0,KEY_READ|KEY_WOW64_64KEY,&key)==0);
    if (key) CHECK(RegDeleteValueA(key,"Only32")==0);
    CHECK(!reg_find_value(path,"Only32",REG_VIEW_32) && !reg_find_value(path,"Only32",REG_VIEW_64));
    CHECK(reg_store_save()); reg_init_state = 0; reg_init();
    shared_value(path,"Only64",99); shared_value(path,"Conflict",64);
    CHECK(!reg_find_value(path,"Only32",REG_VIEW_64)); CHECK(!allocations);
    printf("registry-shared: %u checks, %u failures\n",checks,failures);
    return failures != 0;
}
