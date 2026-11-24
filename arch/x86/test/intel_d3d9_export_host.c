/* Diagnostic transport of an owned native compiler result, not a runtime DLL. */
#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv)
{
    if (argc!=3) return 2;
    unsigned char input[65537];
    FILE *file=fopen(argv[1],"rb");
    if (!file) return 2;
    size_t bytes=fread(input,1,sizeof(input),file);
    int failed=ferror(file); fclose(file);
    if (failed || !bytes || bytes>65536) return 2;
    struct intel_d3d9_compiler *compiler=NULL;
    const struct intel_d3d9_program *program=NULL;
    struct intel_d3d9_compile_error error={0};
    unsigned status=intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_ABI,0x3e9b,&compiler);
    if (!status) status=intel_d3d9_compile_pixel(compiler,input,(unsigned)bytes,&program,&error);
    intel_d3d9_compiler_destroy(compiler);
    if (status) { fprintf(stderr,"compile status=%u word=%u %s\n",status,error.word,error.message); return 3; }
    file=fopen(argv[2],"wb");
    if (!file) { intel_d3d9_program_free(program); return 4; }
    failed=fwrite(program,1,program->allocation_bytes,file)!=program->allocation_bytes;
    failed|=fclose(file)!=0;
    printf("NATIVE PROGRAM bytes=%u code=%u simd=%u grf=%u layer=%u\n",program->allocation_bytes,
        program->code_bytes,program->simd_mask,program->grf_used,program->layer_input);
    intel_d3d9_program_free(program);
    return failed ? 4 : 0;
}
