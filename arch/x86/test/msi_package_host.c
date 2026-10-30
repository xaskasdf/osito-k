/* Reuse the inert filesystem/registry and allocation adapters. */
#define main msi_database_oracle_main
#include "msi_database_host.c"
#undef main
#include "../win32/msi_package.h"
#include "../win32/kernel32_shim.h"

static uint16_t cp1252(unsigned c)
{
    static const uint16_t special[32] = {
        0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
        0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,
        0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178
    };
    return c >= 128 && c < 160 ? special[c - 128] : (uint16_t)c;
}
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, PCSTR input, int bytes, PWSTR output, int units)
{
    CHECK((cp == 0 || cp == 1252) && !flags && input && bytes == -1);
    int needed = (int)strlen(input) + 1;
    if (!output) return needed;
    if (units < needed) return 0;
    for (int i = 0; i < needed; i++) output[i] = cp1252((unsigned char)input[i]);
    return needed;
}
int WINAPI CompareStringOrdinal(PCWSTR a, int na, PCWSTR b, int nb, BOOL fold)
{
    if (na < 0) { na = 0; while (a[na]) na++; }
    if (nb < 0) { nb = 0; while (b[nb]) nb++; }
    for (int i = 0; i < na && i < nb; i++) {
        unsigned x = a[i], y = b[i];
        if (fold) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x < y) return 1;
        if (x > y) return 3;
    }
    return na < nb ? 1 : na > nb ? 3 : 2;
}
int WINAPI CompareStringW(DWORD locale, DWORD flags, PCWSTR a, int na, PCWSTR b, int nb)
{
    (void)locale; (void)flags; (void)a; (void)na; (void)b; (void)nb;
    CHECK(0); return 0; /* The real VC2013 package conditions require no NLS ordering. */
}
static uint32_t get_number(FILE *f)
{
    uint8_t b[4]; CHECK(fread(b, 1, 4, f) == 4);
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint16_t *get_text(FILE *f)
{
    uint32_t size = get_number(f); CHECK(size < 1048576);
    uint16_t *p = calloc(size + 1, 2); CHECK(p != NULL);
    CHECK(fread(p, 2, size, f) == size); return p;
}
static int equal_wide(const uint16_t *a, const uint16_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(!fseek(f, 0, SEEK_END)); long length = ftell(f); rewind(f);
    CHECK(length > 0 && length < 32 * 1024 * 1024);
    uint8_t *data = malloc((size_t)length); CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)length, f) == (size_t)length); fclose(f);
    *size = (uint32_t)length; return data;
}
#ifndef MSI_PACKAGE_ADAPTERS_ONLY
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    uint32_t size; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    uint32_t error = 999; msi_package_t *package = msi_package_create(db, &error);
    CHECK(package != NULL && !error);
    FILE *f = fopen(argv[2], "rb"); CHECK(f != NULL);
    CHECK(get_number(f) == 0x314b504dU);
    unsigned properties_checked = 0, directories_checked = 0, conditions_checked = 0;
    for (int group = 0; group < 4; group++) {
        uint32_t count = get_number(f); CHECK(count < 1000);
        for (uint32_t i = 0; i < count; i++) {
            uint16_t *name = get_text(f);
            if (group == 3) {
                int result = (int)get_number(f);
                CHECK(msi_package_condition(package, name) == result); conditions_checked++;
            } else {
                uint16_t *expected = get_text(f);
                if (!group) CHECK(msi_package_set(package, name, expected) == 0);
                else if (group == 1) { CHECK(equal_wide(msi_package_get(package, name), expected)); properties_checked++; }
                else {
                    const uint16_t *actual = NULL;
                    CHECK(msi_package_directory(package, name, &actual) == 0 && actual != NULL);
                    CHECK(equal_wide(actual, expected)); directories_checked++;
                }
                free(expected);
            }
            free(name);
        }
    }
    CHECK(fgetc(f) == EOF); fclose(f);
    /* Native restricted costing selects ROOTDRIVE; identifiers are case-sensitive. */
    static const uint16_t target_key[] = {'T','A','R','G','E','T','D','I','R',0};
    static const uint16_t root_key[] = {'R','O','O','T','D','R','I','V','E',0};
    static const uint16_t wrong_root[] = {'R','o','o','t','D','r','i','v','e',0};
    static const uint16_t correct_drive[] = {'C',':','\\',0}, wrong_drive[] = {'D',':','\\',0};
    const uint16_t *root_path = NULL;
    CHECK(msi_package_set(package, target_key, NULL) == 0);
    CHECK(msi_package_set(package, root_key, correct_drive) == 0);
    CHECK(msi_package_set(package, wrong_root, wrong_drive) == 0);
    CHECK(msi_package_directory(package, target_key, &root_path) == 0 && equal_wide(root_path, correct_drive));
    CHECK(msi_package_set(package, root_key, NULL) == 0);
    CHECK(msi_package_directory(package, target_key, &root_path) == 1606 && root_path == NULL);
    static const uint16_t folder[] = {'S','y','s','t','e','m','F','o','l','d','e','r',0};
    static const uint16_t child[] = {'S','y','s','t','e','m','F','o','l','d','e','r','_','x','8','6','_','V','C',0};
    static const uint16_t changed[] = {'Z',':','\\','C','h','a','n','g','e','d',0};
    static const uint16_t changed_path[] = {'Z',':','\\','C','h','a','n','g','e','d','\\',0};
    const uint16_t *path;
    CHECK(msi_package_set(package, folder, changed) == 0);
    CHECK(msi_package_directory(package, child, &path) == 0 && equal_wide(path, changed_path));
    CHECK(msi_package_set(package, folder, msi_package_get(package, folder)) == 0);
    CHECK(msi_package_directory(package, child, &path) == 0 && equal_wide(path, changed_path));
    CHECK(msi_package_set(package, folder, (const uint16_t[]){'r','e','l',0}) == 0);
    CHECK(msi_package_directory(package, child, &path) == 1606 && path == NULL);
    msi_package_close(package);
    unsigned baseline = allocations;
    for (int i = 0; i < 600; i++) {
        fail_after = i; error = 999; package = msi_package_create(db, &error);
        if (package) {
            (void)msi_package_set(package, folder, changed);
            (void)msi_package_directory(package, child, &path);
            msi_package_close(package);
        } else CHECK(error != 0);
        CHECK(allocations == baseline);
    }
    fail_after = -1;
    msi_close(db); free(data); CHECK(allocations == 0);
    printf("MSI PACKAGE PASS properties=%u directories=%u conditions=%u allocation_cases=600 checks=%u\n",
           properties_checked, directories_checked, conditions_checked, checks);
    return 0;
}
#endif
