#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <string.h>
struct vertex_fixture {
    const char *name;
    const unsigned char *input;
    unsigned bytes, status;
    const unsigned char *reference;
    unsigned reference_bytes;
};
#include "vertex-fixtures.h"
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
int main(void)
{
    unsigned positive=0, rejected=0;
    for (unsigned round=0; round<2; round++) {
        struct intel_d3d9_compiler *c=NULL;
        CHECK(!intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_VERTEX_ABI, 0x3e9b, &c));
        if (!c) return 1;
        const struct intel_d3d9_vertex_program *held=NULL;
        const struct vertex_fixture *last=NULL;
        for (unsigned n=0; n<sizeof(vertex_fixtures)/sizeof(vertex_fixtures[0]); n++) {
            const struct vertex_fixture *f=&vertex_fixtures[n];
            const struct intel_d3d9_vertex_program *p=(void *)1;
            struct intel_d3d9_compile_error error;
            unsigned status=intel_d3d9_compile_vertex(c,f->input,f->bytes,&p,&error);
            CHECK(status==f->status && error.status==status);
            if (status!=f->status) printf("VERTEX fixture %s got=%u expected=%u word=%u %s\n",f->name,status,f->status,error.word,error.message);
            if (!status) {
                positive++;
                CHECK(p->base.allocation_bytes==f->reference_bytes);
                CHECK(p->base.allocation_bytes==f->reference_bytes && !memcmp(p,f->reference,f->reference_bytes));
                if (held) intel_d3d9_program_free(&held->base);
                held=p;last=f;
            } else { rejected++;CHECK(p==(void *)1); }
        }
        intel_d3d9_compiler_destroy(c);
        CHECK(held && last);
        if (held && last) {
            CHECK(!memcmp(held,last->reference,last->reference_bytes));
            intel_d3d9_program_free(&held->base);
        }
    }
    printf("VERTEX API checks=%u positive=%u rejected=%u failures=%u\n",checks,positive,rejected,failures);
    return failures ? 1 : 0;
}
