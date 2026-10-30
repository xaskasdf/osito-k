#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_gen9_shader.h"
#include "native_umd_shader_reference.h"
static unsigned checks;
static void check(bool value,unsigned line)
{ checks++; if (!value) { fprintf(stderr,"FAIL line %u\n",line); exit(1); } }
#define CHECK(v) check((v),__LINE__)
static void program_case(const void *bytes,size_t count)
{
    struct intel_d3d9_program p; memcpy(&p,bytes,sizeof(p));
    bool valid=intel_gen9_shader_validate(&p,bytes,count);
    if (!valid) {
        fprintf(stderr,"Rejected fixture grfs=%u code=%u offsets=%u/%u mask=%u\n",p.grf_used,p.code_bytes,p.kernel_offset[0],p.kernel_offset[1],p.simd_mask);
        for (unsigned i=0;i<p.code_bytes;i+=4) fprintf(stderr,"%08x%s",((const uint32_t *)((const uint8_t *)bytes+p.code_offset))[i/4],i%16==12 ? "\n" : " ");
    }
    CHECK(valid);
    uint8_t *copy=malloc(count); CHECK(copy!=NULL); memcpy(copy,bytes,count);
    for (unsigned field=0;field<32;field++) {
        ((uint32_t *)copy)[field]^=1;
        CHECK(!intel_gen9_shader_validate(&p,copy,count));
        ((uint32_t *)copy)[field]^=1;
    }
    CHECK(!memcmp(copy,bytes,count)); free(copy);
}
static void fixture_cases(const char *path)
{
    FILE *f=fopen(path,"rb"); CHECK(f!=NULL); CHECK(!fseek(f,0,SEEK_END));
    long length=ftell(f); CHECK(length>16); rewind(f);
    uint8_t *data=malloc((size_t)length); CHECK(data!=NULL);
    CHECK(fread(data,1,(size_t)length,f)==(size_t)length); fclose(f);
    uint32_t *header=(void *)data; CHECK(header[1]==1 || header[1]==2);
    size_t offset=16;
    for (unsigned i=0;i<header[2];i++) {
        CHECK((size_t)length-offset>=16); uint32_t *entry=(void *)(data+offset); offset+=16;
        CHECK(entry[0]<=(size_t)length-offset);
        program_case(data+offset,entry[0]); offset+=entry[0];
        if (header[1]==2) { CHECK(entry[3]<=(size_t)length-offset); offset+=entry[3]; }
    }
    CHECK(offset==(size_t)length); free(data);
}
static void mutation_cases(void)
{
    uint32_t words[320/4]={0}; uint8_t *data=(void *)words;
    memcpy(data,reference_header,sizeof(reference_header));
    memcpy(data+192,reference_native,sizeof(reference_native));
    struct intel_d3d9_program p; memcpy(&p,data,sizeof(p));
    program_case(data,sizeof(words));
    /* EOT payloads require high GRFs even when lower registers were fully
     * initialized. Relocate all four outputs together to preserve dataflow. */
    CHECK(((words[49]>>21)&255)==126 && ((words[57]>>21)&255)==125);
    CHECK(((words[53]>>8)&255)==123 && ((words[55]>>8)&255)==124);
    words[49]=(words[49]&~(255u<<21))|(26u<<21);
    words[57]=(words[57]&~(255u<<21))|(25u<<21);
    words[53]=(words[53]&~(255u<<8))|(23u<<8);
    words[55]=(words[55]&~(255u<<8))|(24u<<8);
    words[61]=(words[61]&~(255u<<12))|(23u<<12);
    words[62]=(words[62]&~(255u<<5))|(26u<<5);
    CHECK(!intel_gen9_shader_validate(&p,data,sizeof(words)));
    memcpy(data+192,reference_native,sizeof(reference_native));
    words[62]=(words[62]&~(255u<<5))|(125u<<5); /* overlapping, initialized high payloads */
    CHECK(!intel_gen9_shader_validate(&p,data,sizeof(words)));
    memcpy(data+192,reference_native,sizeof(reference_native));
    /* Alter instruction control, destination, region or message fields.
     * Each changes a semantic restriction, not the expected color. */
    const unsigned mutations[][2]={
        {48,0x20},{48,1u<<16},{48,1u<<28},{48,1u<<30},{48,1u<<31},
        {49,1u<<3},{49,1u<<9},{49,1u<<16},{49,1u<<31},
        {50,1u<<15},{50,1u<<30},
        {52,1u<<7},{52,1u<<28},{52,1u<<23},
        {60,1},{60,1u<<24},{61,1u<<29},{62,1u<<13},{63,1u<<31},
        {63,1},{63,1u<<19},{63,1u<<20},{63,1u<<25}};
    for (unsigned i=0;i<sizeof(mutations)/sizeof(*mutations);i++) {
        words[mutations[i][0]]^=mutations[i][1];
        CHECK(!intel_gen9_shader_validate(&p,data,sizeof(words)));
        words[mutations[i][0]]^=mutations[i][1];
    }
    /* Distinct immediate colors remain legal: validation is not a table of
     * fixture binaries and does not evaluate the shader on the CPU. */
    uint32_t seed=0x928fed13;
    for (unsigned i=0;i<10000;i++) {
        seed=seed*1664525u+1013904223u; words[51]=seed;
        CHECK(intel_gen9_shader_validate(&p,data,sizeof(words)));
    }
    memcpy(data+192,reference_native,sizeof(reference_native));
    /* Exact-sized truncated inputs, with matching header length, exercise
     * every code boundary under ASan rather than only a size mismatch. */
    for (unsigned bytes=8;bytes<64;bytes+=8) {
        struct intel_d3d9_program short_program=p;
        short_program.simd_mask=1; short_program.kernel_offset[1]=0;
        short_program.code_bytes=bytes; short_program.allocation_bytes=192+bytes;
        uint8_t *short_data=malloc(short_program.allocation_bytes); CHECK(short_data!=NULL);
        memcpy(short_data,data,short_program.allocation_bytes); memcpy(short_data,&short_program,sizeof(short_program));
        CHECK(!intel_gen9_shader_validate(&short_program,short_data,short_program.allocation_bytes)); free(short_data);
    }
    /* Missing payload initialization is rejected even with a legal SEND. */
    memcpy(data+192,reference_native+48,16);
    CHECK(!intel_gen9_shader_validate(&p,data,sizeof(words)));
    memcpy(data+192,reference_native,sizeof(reference_native));
    for (unsigned i=0;i<12000;i++) {
        seed=seed*1664525u+1013904223u;
        unsigned index=48+(seed%32); uint32_t saved=words[index];
        seed=seed*1664525u+1013904223u; words[index]=seed;
        (void)intel_gen9_shader_validate(&p,data,sizeof(words));
        words[index]=saved;
    }
    CHECK(intel_gen9_shader_validate(&p,data,sizeof(words)));
    CHECK(!intel_gen9_shader_validate(NULL,data,sizeof(words)));
    CHECK(!intel_gen9_shader_validate(&p,NULL,sizeof(words)));
    CHECK(!intel_gen9_shader_validate(&p,data,127));
}
int main(int argc,char **argv)
{
    CHECK(argc==3); fixture_cases(argv[1]); fixture_cases(argv[2]); mutation_cases();
    printf("Intel Gen9 native program admission: %u checks passed\n",checks); return 0;
}
