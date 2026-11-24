#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct sampler_fixture {
    const char *name; const unsigned char *input;
    unsigned bytes,status,samplers,coords,parameters;
    const unsigned char *reference; unsigned reference_bytes;
};
#include "sampler-fixtures.h"
static unsigned checks,failures;
#define CHECK(x) do {checks++;if (!(x)) {failures++;printf("SAMPLER FAIL %u %s\n",__LINE__,#x);}} while(0)
int main(void)
{
    struct intel_d3d9_compiler *c=NULL,*legacy=NULL;
    CHECK(intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_RESOURCE_ABI,0x3e9b,&c)==0 && c);
    CHECK(intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_ABI,0x3e9b,&legacy)==0 && legacy);
    if (!c || !legacy) return 1;
    unsigned positive=0,rejected=0;
    const struct intel_d3d9_program *held[64]={0};
    unsigned total=sizeof(sampler_fixtures)/sizeof(sampler_fixtures[0]); CHECK(total<=64);
    for (unsigned i=0;i<total;i++) {
        const struct sampler_fixture *f=&sampler_fixtures[i];
        unsigned char input[4096]; CHECK(f->bytes<=sizeof(input)); memcpy(input,f->input,f->bytes);
        const struct intel_d3d9_program *p=(void *)1;
        struct intel_d3d9_compile_error error;
        unsigned status=intel_d3d9_compile_pixel(c,input,f->bytes,&p,&error);
        CHECK(status==f->status && error.status==status);
        if (status!=f->status) printf("SAMPLER case=%s got=%u expected=%u message=%s\n",f->name,status,f->status,error.message);
        if (status) {rejected++;CHECK(p==(void *)1);continue;}
        positive++; memset(input,0xa5,f->bytes);
        CHECK(p->abi==3 && p->header_bytes==128 && p->allocation_bytes==f->reference_bytes);
        CHECK(p->texture_mask==f->samplers && p->sampler_mask==f->samplers && p->texcoord_mask==f->coords);
        CHECK(p->parameter_count==f->parameters && !p->resource_reserved && !p->scratch_bytes);
        CHECK(p->bytecode_bytes==f->bytes && !memcmp((const unsigned char *)p+p->bytecode_offset,f->input,f->bytes));
        CHECK(p->allocation_bytes==f->reference_bytes && !memcmp(p,f->reference,f->reference_bytes));
        /* Opt-in ABI is a real boundary: the production ABI2 UMD cannot
         * accidentally receive a texture program it does not submit yet. */
        const struct intel_d3d9_program *old=(void *)1;
        CHECK(intel_d3d9_compile_pixel(legacy,f->input,f->bytes,&old,NULL)==INTEL_D3D9_COMPILE_UNSUPPORTED && old==(void *)1);
        held[i]=p;
    }
    intel_d3d9_compiler_destroy(c);intel_d3d9_compiler_destroy(legacy);
    for (unsigned i=0;i<total;i++) if (held[i]) {
        CHECK(!memcmp(held[i],sampler_fixtures[i].reference,sampler_fixtures[i].reference_bytes));
        intel_d3d9_program_free(held[i]);
    }
    CHECK(positive==13 && rejected==16);
    printf("SAMPLER API checks=%u positive=%u rejected=%u failures=%u\n",checks,positive,rejected,failures);
    return failures ? 1 : 0;
}
