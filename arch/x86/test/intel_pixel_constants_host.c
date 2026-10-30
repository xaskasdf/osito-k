#include "../drivers/intel_gen9_shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static const char *names[]={"c0","c223","sparse","local","repeat","swizzle","mask","128"};
static const unsigned maps[][8]={{0,1,2,3},{892,893,894,895},{20,21,22,23,892,893,894,895},
    {4,5,6,7},{28,29,30,31},{40,41,42,43},{20,21,22,23,8,9,10,11}};
int main(int argc,char **argv)
{
    CHECK(argc==2);
    for (unsigned fixture=0;fixture<8;fixture++) {
        char path[1024]; snprintf(path,sizeof(path),"%s/uniform-%s.program",argv[1],names[fixture]);
        FILE *file=fopen(path,"rb"); CHECK(file); CHECK(!fseek(file,0,SEEK_END)); long size=ftell(file);
        CHECK(size>=128 && size<131072); rewind(file); unsigned char *bytes=malloc((size_t)size); CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size); fclose(file);
        struct intel_d3d9_program *p=(void *)bytes;
        CHECK(p->abi==2 && p->parameter_count==(fixture==7 ? 512u : fixture==2 || fixture==6 ? 8u : 4u));
        for (unsigned i=0;i<p->parameter_count;i++) {
            unsigned value; memcpy(&value,bytes+p->parameter_map_offset+4*i,4);
            CHECK(value==(fixture==7 ? i : maps[fixture][i]));
        }
        CHECK(intel_rcs_pixel_validate(p,(size_t)size));
        /* SIMD32 admission remains unavailable; compilation can still produce
         * that native variant without claiming the current KMD supports it. */
        bool supported=!(p->simd_mask&4);
        bool accepted=intel_gen9_shader_validate(p,bytes,(size_t)size);
        if (accepted!=supported) fprintf(stderr,"fixture %s rejected\n",names[fixture]);
        CHECK(accepted==supported);
        uint32_t batch[INTEL_RCS_DRAW_BYTES/4]; memset(batch,0xcc,sizeof(batch));
        intel_rcs_pixel_draw_t draw={.batch_gpu=0x10000,.program_gpu=0x20000,.destination_gpu=0x40000,
            .destination_bytes=4096,.destination_width=16,.destination_height=16,.destination_pitch=64,
            .width=16,.height=16,.dispatch_mask=p->simd_mask&3};
        draw.constants.count=p->parameter_count;
        for (unsigned i=0;i<p->parameter_count;i++) draw.constants.values[i]=0x3f000000+i;
        CHECK(intel_rcs_pixel_build(batch,sizeof(batch),&draw,p,(size_t)size));
        CHECK(!memcmp(batch+0x1800/4,draw.constants.values,p->parameter_count*4));
        for (unsigned i=p->parameter_count;i<512;i++) CHECK(batch[0x1800/4+i]==0);
        unsigned ps=0,constant=0,alloc=0;
        for (unsigned i=0;i<512;i++) {
            if (batch[i]==0x7820000a) { CHECK(batch[i+6]&(1u<<11)); ps++; }
            if (batch[i]==0x78170209) {
                CHECK(batch[i+1]==0 && batch[i+2]==p->curb_read_length<<16);
                for (unsigned j=3;j<9;j++) CHECK(batch[i+j]==0);
                CHECK(batch[i+9]==0x11800 && batch[i+10]==0); constant++;
            }
            if (batch[i]==0x79160000) { CHECK(batch[i+1]==(p->curb_read_length+31)/32); alloc++; }
        }
        CHECK(ps==1 && constant==1 && alloc==1);
        draw.constants.count++; memset(batch,0x5a,sizeof(batch));
        CHECK(!intel_rcs_pixel_build(batch,sizeof(batch),&draw,p,(size_t)size)); CHECK(batch[0]==0x5a5a5a5a);
        unsigned save=p->parameter_map_offset;
        const unsigned bad_offsets[]={0,4,129,0xfffffffcu,p->code_offset};
        for (unsigned i=0;i<sizeof(bad_offsets)/sizeof(*bad_offsets);i++) {
            p->parameter_map_offset=bad_offsets[i]; CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size));
        }
        p->parameter_map_offset=save;
        unsigned value; memcpy(&value,bytes+save,4); unsigned bad=896; memcpy(bytes+save,&bad,4);
        CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size)); memcpy(bytes+save,&value,4);
        p->abi=1; CHECK(!intel_rcs_pixel_validate(p,(size_t)size)); p->abi=2;
        p->curb_read_length++; CHECK(!intel_rcs_pixel_validate(p,(size_t)size)); p->curb_read_length--;
        if (supported) {
            /* Invalid controls/types and arbitrary SEND opcodes must not
             * enter through the new register ALU/compact expansion path. */
            uint32_t *code=(void *)(bytes+p->code_offset),original=*code;
            const unsigned bits[]={1u<<7,1u<<16,1u<<28};
            for (unsigned i=0;i<sizeof(bits)/sizeof(*bits);i++) {
                *code=original^bits[i]; CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size));
            }
            *code=(original&~127u)|49; CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size)); *code=original;
            if (fixture==3) {
                /* The first MUL is uncompacted: exercise actual native bit
                 * fields, uninitialized GRFs, addresses and vector bounds. */
                CHECK(!(code[0]&(1u<<29)) && (code[0]&127)==65);
                const unsigned mutations[][2]={{0,1u<<16},{0,1u<<11},{0,1u<<24},
                    {1,1u<<31},{1,1u<<16},{1,1u<<29},{2,1u<<15},{2,1u<<31},
                    {2,1u},{2,1u<<16},{2,1u<<27}};
                for (unsigned i=0;i<sizeof(mutations)/sizeof(*mutations);i++) {
                    unsigned index=mutations[i][0],bit=mutations[i][1];
                    code[index]^=bit; CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size)); code[index]^=bit;
                }
                unsigned source=code[2]; code[2]&=~(255u<<5); /* g0 payload is not a float constant. */
                CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size)); code[2]=source;
                unsigned *simd16=(void *)(bytes+p->code_offset+p->kernel_offset[1]);
                unsigned control=simd16[1]; simd16[1]&=~4u; /* Scalar temp requires WE_all. */
                CHECK(!intel_gen9_shader_validate(p,bytes,(size_t)size)); simd16[1]=control;
            }
        }
        free(bytes);
    }
    printf("Native pixel constants: %u checks passed\n",checks); return 0;
}
