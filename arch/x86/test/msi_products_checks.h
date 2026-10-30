#define MW(s) ((const WCHAR *)u##s)
#define PRODUCT_CHECK(c) product_check((c), __LINE__)
#include "msi_products_cases.h"
static const WCHAR *const product_minimum=MW("{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}");
static int product_text_equal(const WCHAR *a,const WCHAR *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static unsigned product_text_length(const WCHAR *text)
{unsigned n=0;while(text[n])n++;return n;}
static void product_negative_checks(void)
{
    for(unsigned i=0;i<sizeof(product_error_cases)/sizeof(*product_error_cases);i++){
        const PRODUCT_ERROR_CASE *test=&product_error_cases[i];
        WCHAR text[32];for(unsigned j=0;j<32;j++)text[j]='Z';DWORD size=32;
        PRODUCT_CHECK(MsiGetProductInfoW(test->product,test->attribute,text,&size)==test->result);
        PRODUCT_CHECK(size==32);
        for(unsigned j=0;j<32;j++)PRODUCT_CHECK(text[j]=='Z');
    }
}
static void product_expect(const WCHAR *name,const WCHAR *expected)
{
    WCHAR text[512];DWORD size=512;
    PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,name,text,&size)==0);
    PRODUCT_CHECK(size==product_text_length(expected));
    PRODUCT_CHECK(product_text_equal(text,expected));
}
static void product_installed_checks(void)
{
    product_expect(MW("VersionString"),MW("12.0.21005"));
    product_expect(MW("Version"),MW("201347597"));
    product_expect(MW("VersionMajor"),MW("12"));product_expect(MW("VersionMinor"),MW("0"));
    product_expect(MW("Language"),MW("1033"));product_expect(MW("InstalledLanguage"),MW("1033"));
    product_expect(MW("Publisher"),MW("Microsoft Corporation"));
    product_expect(MW("PackageCode"),MW("{E9934153-EAB1-4DA6-AA72-86C8BB1EDF2C}"));
    product_expect(MW("LocalPackage"),MW("C:\\Windows\\Installer\\{E9934153-EAB1-4DA6-AA72-86C8BB1EDF2C}.msi"));
    product_expect(MW("InstallSource"),MW("C:\\minimum\\"));
    product_expect(MW("AssignmentType"),MW("1"));product_expect(MW("Transforms"),MW(""));
    product_expect(MW("InstanceType"),MW("0"));
    for(unsigned empty=0;empty<2;empty++)for(unsigned null=0;null<2;null++)for(DWORD capacity=0;capacity<16;capacity++){
        WCHAR text[32];for(unsigned j=0;j<32;j++)text[j]='Z';DWORD size=capacity;
        const WCHAR *expected=empty?MW(""):MW("12.0.21005");DWORD length=empty?0:10;
        DWORD rc=MsiGetProductInfoW(product_minimum,empty?MW("Transforms"):MW("VersionString"),null?NULL:text,&size);
        PRODUCT_CHECK(rc==(null||empty||capacity>length?0U:234U));PRODUCT_CHECK(size==length);
        for(unsigned j=0;j<32;j++){
            WCHAR want=(!null&&(empty||capacity>length)&&j<=length)?expected[j]:'Z';
            PRODUCT_CHECK(text[j]==want);
        }
    }
    PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("VersionString"),NULL,NULL)==0);
    WCHAR text[32];PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("VersionString"),text,NULL)==87);
    DWORD size=32;text[0]='Z';
    PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("ProductVersion"),text,&size)==1608&&text[0]=='Z'&&size==32);
    PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("InstallDate"),text,&size)==120&&text[0]=='Z'&&size==32);
}
