#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../win32/osversion.h"
#define GUID10 "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"
#define GUID81 "{1f676c76-80e1-4239-95bb-83d0f6d0da78}"
#define OPEN "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'><compatibility xmlns='urn:schemas-microsoft-com:compatibility.v1'><application>"
#define CLOSE "</application></compatibility></assembly>"
#define MODERN OPEN "<supportedOS Id='" GUID10 "'/>" CLOSE
int main(void)
{
    const struct { const char *text; unsigned expected; } cases[] = {
        {"", 62}, {"<assembly/>", 62}, {MODERN, 100},
        {OPEN "<supportedOS Id='" GUID81 "'/>" CLOSE, 63},
        {OPEN "<supportedOS Id='" GUID10 "'/><supportedOS Id='" GUID81 "'/>" CLOSE, 100},
        {OPEN "<!-- <supportedOS Id='" GUID10 "'/> -->" CLOSE, 62},
        {OPEN "<![CDATA[<supportedOS Id='" GUID10 "'/>]]>" CLOSE, 62},
        {OPEN "<supportedOS xmlns='wrong' Id='" GUID10 "'/>" CLOSE, 62},
        {OPEN "<unknown><supportedOS Id='" GUID10 "'/></unknown>" CLOSE, 62},
        {OPEN "<supportedOS other='" GUID10 "'/>" CLOSE, 62},
        {OPEN "<supportedOS Id='" GUID10 "'/></bad></compatibility></assembly>", 62},
        {OPEN "<supportedOS Id='" GUID10 "'/>" CLOSE "<second/>", 62},
        {"<!DOCTYPE assembly>" MODERN, 62},
        {"<?xml version='1.0'?>\n" MODERN, 100},
        {"<a:assembly xmlns:a='urn:schemas-microsoft-com:asm.v1' xmlns:c='urn:schemas-microsoft-com:compatibility.v1'><c:compatibility><c:application><c:supportedOS Id='" GUID81 "'/></c:application></c:compatibility></a:assembly>", 63},
        {OPEN "<supportedOS Id='{8E0F7A12-BFB3-4FE8-B9A5-48FD50A15A9A}'/>" CLOSE, 100},
        {OPEN "<unknown xmlns='wrong'/><supportedOS Id='" GUID10 "'/>" CLOSE, 100},
        {OPEN "<c:supportedOS xmlns:c='wrong' Id='" GUID10 "'/>" CLOSE, 62}
    };
    unsigned checks = 0, failures = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %u case %u\n", __LINE__, index); } } while (0)
    unsigned index = 0;
    for (; index < sizeof(cases)/sizeof(cases[0]); index++) {
        unsigned len = (unsigned)strlen(cases[index].text);
        unsigned char *exact = malloc(len ? len : 1);
        memcpy(exact, cases[index].text, len);
        CHECK(osversion_manifest_level(exact, len) == cases[index].expected);
        free(exact);
        for (unsigned be = 0; be < 2; be++) {
            unsigned char *wide = malloc(len*2+2);
            wide[0] = be ? 0xfe : 0xff; wide[1] = be ? 0xff : 0xfe;
            for (unsigned i = 0; i < len; i++) { wide[2+i*2+be] = (unsigned char)cases[index].text[i]; wide[2+i*2+!be] = 0; }
            CHECK(osversion_manifest_level(wide, len*2+2) == cases[index].expected);
            free(wide);
        }
    }
    const char *valid = MODERN;
    for (unsigned cut = 0; cut < strlen(valid); cut++) {
        unsigned char *short_xml = malloc(cut ? cut : 1);
        memcpy(short_xml, valid, cut);
        CHECK(osversion_manifest_level(short_xml, cut) == 62);
        free(short_xml);
    }
    unsigned seed = 0x10203040;
    for (unsigned trial = 0; trial < 20000; trial++) {
        seed = seed * 1664525 + 1013904223;
        unsigned len = seed % 1024;
        unsigned char *random = malloc(len ? len : 1);
        for (unsigned i = 0; i < len; i++) { seed = seed * 1664525 + 1013904223; random[i] = seed >> 24; }
        unsigned result = osversion_manifest_level(random, len);
        CHECK(result == 62 || result == 63 || result == 100);
        free(random);
    }
    printf("OSVERSION manifest checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
