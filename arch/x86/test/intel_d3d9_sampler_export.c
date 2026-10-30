/* Compile our input shader and export owned EU code/metadata. No GPU execution. */
#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef SAMPLER_DISASSEMBLE
#include "compiler/brw_compiler.h"
#include "compiler/brw_disasm.h"
#endif
int main(int argc,char **argv)
{
    if (argc!=3) return 2;
    unsigned char bytes[65537]; FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    size_t count=fread(bytes,1,sizeof(bytes),f); int failure=ferror(f); fclose(f);
    if (failure || !count || count>65536) return 2;
    struct intel_d3d9_compiler *c=NULL; const struct intel_d3d9_program *p=(void *)1;
    struct intel_d3d9_compile_error error={0};
    unsigned status=intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_RESOURCE_ABI,0x3e9b,&c);
    if (!status) status=intel_d3d9_compile_pixel(c,bytes,(unsigned)count,&p,&error);
    intel_d3d9_compiler_destroy(c);
    if (status) {
        printf("SAMPLER status=%u word=%u unchanged=%u %s\n",status,error.word,p==(void *)1,error.message);
        return 3;
    }
    printf("SAMPLER status=0 bytes=%u code=%u simd=%u grf=%u scratch=%u textures=%08x samplers=%08x inputs=%08x map=%08x layer=%u bary=%u varying=%u params=%u\n",
        p->allocation_bytes,p->code_bytes,p->simd_mask,p->grf_used,p->scratch_bytes,p->texture_mask,p->sampler_mask,
        p->texcoord_mask,p->texcoord_map,p->layer_input,p->barycentric_modes,p->varying_inputs,p->parameter_count);
#ifdef SAMPLER_DISASSEMBLE
    struct intel_device_info device; struct brw_isa_info isa;
    if (!intel_get_device_info_from_pci_id(0x3e9b,&device)) return 4;
    brw_init_isa_info(&isa,&device);
    for (unsigned i=0;i<3;i++) if (p->simd_mask&(1u<<i)) {
        printf("SIMD%u\n",8u<<i);
        brw_disassemble_with_errors(&isa,(const unsigned char *)p+p->code_offset,p->kernel_offset[i],stdout);
    }
#endif
    f=fopen(argv[2],"wb"); if (!f) return 4;
    failure=fwrite(p,1,p->allocation_bytes,f)!=p->allocation_bytes; failure|=fclose(f)!=0;
    intel_d3d9_program_free(p); return failure ? 4 : 0;
}
