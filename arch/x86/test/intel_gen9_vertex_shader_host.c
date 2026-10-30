#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_gen9_shader.h"
static unsigned checks,accepted,rejected;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);exit(1);}} while (0)
int main(int argc,char **argv)
{
    CHECK(argc>1);
    for (int file=1;file<argc;file++) {
        FILE *f=fopen(argv[file],"rb");CHECK(f);
        CHECK(!fseek(f,0,SEEK_END));long bytes=ftell(f);CHECK(bytes>256);rewind(f);
        unsigned char *data=malloc(bytes+64),*copy=malloc(bytes+64);CHECK(data && copy);
        CHECK(fread(data,1,bytes,f)==(size_t)bytes);fclose(f);
        struct intel_d3d9_vertex_program *v=(void *)data;
        CHECK(intel_gen9_vertex_program_validate(v,bytes));
        bool valid=intel_gen9_vertex_shader_validate(v,data,bytes);
        bool supported=!strstr(argv[file],"-min.") && !strstr(argv[file],"-max.") &&
            !strstr(argv[file],"-frc.") && !strstr(argv[file],"-slt.") && !strstr(argv[file],"-sge.");
        printf("VS admit=%u expected=%u %s\n",valid,supported,argv[file]);
        CHECK(valid==supported);
        if (valid) accepted++; else rejected++;
        if (valid) {
            memcpy(copy,data,bytes);struct intel_d3d9_vertex_program *bad=(void *)copy;
            const unsigned fields[]={0,1,2,3,4,8,9,11,12,13,14,15,16,17,19,20,22,23,25,27,28,29,30,31,34,35,63};
            for (unsigned n=0;n<sizeof(fields)/sizeof(fields[0]);n++) {
                uint32_t *word=(uint32_t *)copy+fields[n],old=*word;
                *word=0xdeadbeef;CHECK(!intel_gen9_vertex_shader_validate(bad,copy,bytes));*word=old;
            }
            if (v->base.parameter_count) {
                uint32_t *word=(void *)(copy+v->base.parameter_map_offset),old=*word;
                *word=1024;CHECK(!intel_gen9_vertex_shader_validate(bad,copy,bytes));*word=old;
            }
            unsigned end=v->base.code_offset+v->base.code_bytes;
            for (unsigned off=v->base.code_offset;off<end;) {
                uint32_t *word=(void *)(copy+off),old=*word;
                unsigned length=old&(1u<<29) ? 8 : 16;
                if ((old&127)==51) {
                    for (unsigned n=0;n<32;n++) {
                        uint32_t original=word[3];word[3]^=1u<<n;
                        CHECK(!intel_gen9_vertex_shader_validate(bad,copy,bytes));word[3]=original;
                    }
                    uint32_t original=word[2];word[2]&=~(255u<<5);
                    CHECK(!intel_gen9_vertex_shader_validate(bad,copy,bytes));word[2]=original;
                }
                if (old && old!=0x2000007e) {
                    *word=(old&~127u)|32u;CHECK(!intel_gen9_vertex_shader_validate(bad,copy,bytes));*word=old;
                }
                off+=length;
            }
            CHECK(!intel_gen9_vertex_shader_validate(v,copy,bytes-1));
            bad->base.flags^=1;CHECK(!intel_gen9_vertex_shader_validate(v,copy,bytes));bad->base.flags^=1;
        }
        free(copy);free(data);
    }
    printf("VERTEX ADMISSION checks=%u accepted=%u unsupported=%u failures=0\n",checks,accepted,rejected);
    return 0;
}
