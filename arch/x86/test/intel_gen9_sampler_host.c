#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_gen9_shader.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL sampler admission %d: %s\n",__LINE__,#x); exit(1); } } while (0)

static unsigned char *load(const char *dir,const char *name,size_t *bytes)
{
    char path[1024]; CHECK(snprintf(path,sizeof(path),"%s/%s.program",dir,name)>0);
    FILE *f=fopen(path,"rb"); CHECK(f); CHECK(!fseek(f,0,SEEK_END));
    long size=ftell(f); CHECK(size>=128 && size<65536); rewind(f);
    unsigned char *data=malloc((size_t)size); CHECK(data);
    CHECK(fread(data,1,(size_t)size,f)==(size_t)size); fclose(f); *bytes=(size_t)size; return data;
}
static void reject_header(unsigned char *data,size_t bytes,unsigned index,uint32_t value)
{
    uint32_t *words=(void *)data,saved=words[index]; words[index]=value;
    struct intel_d3d9_program p; memcpy(&p,data,sizeof(p));
    CHECK(!intel_gen9_resource_shader_validate(&p,data,bytes)); words[index]=saved;
}
static void reject_word(unsigned char *data,size_t bytes,unsigned offset,uint32_t value)
{
    uint32_t *word=(void *)(data+offset),saved=*word; *word=value;
    struct intel_d3d9_program p; memcpy(&p,data,sizeof(p));
    if (intel_gen9_resource_shader_validate(&p,data,bytes))
        fprintf(stderr,"Accepted mutation offset=%u old=%08x new=%08x\n",offset,saved,value);
    CHECK(!intel_gen9_resource_shader_validate(&p,data,bytes)); *word=saved;
}
static void fixture(const char *directory,const char *name)
{
    size_t bytes; unsigned char *data=load(directory,name,&bytes);
    struct intel_d3d9_program p; memcpy(&p,data,sizeof(p));
    if (!intel_gen9_resource_shader_validate(&p,data,bytes)) fprintf(stderr,"Rejected %s\n",name);
    CHECK(intel_gen9_resource_program_validate(&p,bytes));
    CHECK(intel_gen9_resource_shader_validate(&p,data,bytes));
    CHECK(!intel_gen9_shader_validate(&p,data,bytes)); /* unbound route stays closed */
    for (unsigned i=0;i<32;i++) {
        ((uint32_t *)data)[i]^=1;
        CHECK(!intel_gen9_resource_shader_validate(&p,data,bytes));
        ((uint32_t *)data)[i]^=1;
    }
    reject_header(data,bytes,0,2); reject_header(data,bytes,3,0x1234);
    reject_header(data,bytes,9,0); reject_header(data,bytes,9,8);
    reject_header(data,bytes,10,129); reject_header(data,bytes,11,4096);
    reject_header(data,bytes,12,64); reject_header(data,bytes,15,p.dispatch_grf_start[0]+1);
    reject_header(data,bytes,22,10); reject_header(data,bytes,23,p.barycentric_modes^2);
    reject_header(data,bytes,24,513); reject_header(data,bytes,25,9);
    reject_header(data,bytes,27,p.texture_mask^0x10000); reject_header(data,bytes,28,p.sampler_mask^1);
    reject_header(data,bytes,29,p.texcoord_mask^0x100); reject_header(data,bytes,31,1);
    for (unsigned semantic=0;semantic<8;semantic++) {
        unsigned shift=semantic*4,slot=(p.texcoord_map>>shift)&15;
        reject_header(data,bytes,30,(p.texcoord_map&~(15u<<shift))|((slot==15 ? 14u : 15u)<<shift));
    }
    for (unsigned offset=0;offset<p.code_bytes;) {
        uint32_t *w=(void *)(data+p.code_offset+offset);
        bool compact=(w[0]&(1u<<29))!=0; unsigned opcode=w[0]&127;
        unsigned length=compact ? 8 : 16;
        CHECK(length<=p.code_bytes-offset);
        if (w[0] && opcode!=126) {
            reject_word(data,bytes,p.code_offset+offset,w[0]|1u<<(compact ? 7 : 30)); /* debug trap */
            if (!compact) {
                reject_word(data,bytes,p.code_offset+offset,w[0]|1u<<16); /* predicate */
                reject_word(data,bytes,p.code_offset+offset,w[0]|1u<<8); /* Align16 */
                if (opcode>=49 && opcode<=52) {
                    unsigned descriptor=p.code_offset+offset+12;
                    reject_word(data,bytes,descriptor,w[3]^1u<<19); /* undeclared header */
                    reject_word(data,bytes,p.code_offset+offset+8,w[2]|1u<<15); /* indirect source */
                    reject_word(data,bytes,descriptor,w[3]^1); /* wrong binding */
                    if (opcode==49 || opcode==51) {
                        reject_word(data,bytes,descriptor,w[3]^1u<<12); /* other sampler operation */
                        reject_word(data,bytes,descriptor,w[3]|1u<<31); /* sampler EOT */
                        reject_word(data,bytes,descriptor,w[3]^(1u<<8)); /* wrong sampler */
                        reject_word(data,bytes,descriptor,w[3]&~(31u<<20)); /* no returned data */
                        reject_word(data,bytes,p.code_offset+offset+4,(w[1]&~(255u<<21))|(127u<<21));
                    }
                }
            }
        }
        offset+=length;
    }
    CHECK(intel_gen9_resource_shader_validate(&p,data,bytes));
    printf("%s admitted mask=%u textures=%04x inputs=%02x\n",name,p.simd_mask,p.texture_mask,p.texcoord_mask);
    free(data);
}
static void flow_cases(const char *directory)
{
    size_t bytes; unsigned char *data=load(directory,"ps30-dependent",&bytes);
    struct intel_d3d9_program p; memcpy(&p,data,sizeof(p));
    uint32_t *code=(void *)(data+p.code_offset);
    /* SIMD32 has independent logical quarters. Redirect the second sampler
     * into first-half results, while retaining legal register/descriptor bits. */
    unsigned entry=p.kernel_offset[2];
    CHECK(code[(entry+64)/4]==0x02802033);
    reject_word(data,bytes,p.code_offset+entry+64+8,
        (code[(entry+64+8)/4]&~(255u<<5))|(12u<<5));
    /* EOT after only the first half, missing second-half high-RT selection,
     * and mismatched quarter versus payload provenance must all fail. */
    CHECK(code[(entry+112)/4]==0x05800032);
    reject_word(data,bytes,p.code_offset+entry+112+12,code[(entry+112+12)/4]|0x80000000u);
    reject_word(data,bytes,p.code_offset+entry+128+12,code[(entry+128+12)/4]&~0x800u);
    reject_word(data,bytes,p.code_offset+entry+64,code[(entry+64)/4]&~0x2000u);
    /* An otherwise well-formed sampler cannot read the uninitialized thread
     * header, exceed response backing, or operate with undeclared metadata. */
    reject_word(data,bytes,p.code_offset+32+8,0x06000000u|(100u<<5));
    reject_word(data,bytes,p.code_offset+32+12,code[(32+12)/4]|(15u<<20));
    reject_header(data,bytes,27,1); reject_header(data,bytes,28,1);
    /* Every exact-sized native-code prefix has its own allocation, including
     * prefixes ending inside an instruction; ASan checks the read boundary. */
    for (unsigned length=8;length<64;length+=8) {
        struct intel_d3d9_program q=p;
        q.simd_mask=1;q.kernel_offset[1]=q.kernel_offset[2]=0;
        q.dispatch_grf_start[1]=q.dispatch_grf_start[2]=0;
        q.code_bytes=length;q.allocation_bytes=q.code_offset+length;
        unsigned char *shortened=malloc(q.allocation_bytes); CHECK(shortened);
        memcpy(shortened,data,q.allocation_bytes);memcpy(shortened,&q,sizeof(q));
        CHECK(!intel_gen9_resource_shader_validate(&q,shortened,q.allocation_bytes)); free(shortened);
    }
    free(data);

    data=load(directory,"ps30-constant-coordinate",&bytes);memcpy(&p,data,sizeof(p));
    code=(void *)(data+p.code_offset);
    CHECK(code[0]==0x00600001 && code[4]==0x00600001);
    /* Immediate values are not an allowlist. Distinct coordinates are legal. */
    uint32_t original=code[3],seed=0x09358c51;
    for (unsigned i=0;i<4000;i++) {
        seed=seed*1664525u+1013904223u; code[3]=seed;
        CHECK(intel_gen9_resource_shader_validate(&p,data,bytes));
    }
    code[3]=original;
    /* Convert the first MOV into an otherwise valid GRF broadcast from an
     * uninitialized register. No texture coordinate is then defined. */
    uint32_t saved[4];memcpy(saved,code,16);
    code[1]=0x20003ae8;code[2]=120u<<5;code[3]=0;
    CHECK(!intel_gen9_resource_shader_validate(&p,data,bytes));memcpy(code,saved,16);
    /* Removing Gen9's dummy non-header phase must not allow a header-only
     * dispatch (PRM WA 0877), even though this kernel does not read the layer. */
    struct intel_d3d9_program q=p;q.varying_inputs=0;q.layer_input=UINT32_MAX;
    memcpy(data,&q,sizeof(q));CHECK(!intel_gen9_resource_shader_validate(&q,data,bytes));memcpy(data,&p,sizeof(p));
    for (unsigned i=0;i<12000;i++) {
        seed=seed*1664525u+1013904223u; unsigned index=seed%(p.code_bytes/4),value=code[index];
        seed=seed*1664525u+1013904223u;code[index]=seed;
        (void)intel_gen9_resource_shader_validate(&p,data,bytes);code[index]=value;
    }
    CHECK(intel_gen9_resource_shader_validate(&p,data,bytes));free(data);
}

static void authored_flow_cases(void)
{
    uint32_t storage[272/4]={0};
    struct intel_d3d9_program *p=(void *)storage;
    *p=(struct intel_d3d9_program){.abi=3,.header_bytes=128,.allocation_bytes=sizeof(storage),
        .pci_id=0x3e9b,.bytecode_offset=128,.bytecode_bytes=4,.code_offset=192,.code_bytes=80,
        .stage=1,.simd_mask=1,.grf_used=127,.dispatch_grf_start={2,0,0},.varying_inputs=1,
        .layer_input=0,.texture_mask=1,.sampler_mask=1,.texcoord_map=UINT32_MAX};
    uint32_t *code=storage+48;
    const uint32_t native[]={
        0x00600001,0x20003ee8,0x38000000,0x3f000000, /* masked U */
        0x00600001,0x20203ee8,0x38000000,0x3f000000, /* masked V */
        0x02600033,0x0f601018,0x00000001,0x02420001, /* g123..126 = sample(g0,g1) */
        0x00600001,0x2f603ae8,0x008d0f60,0,          /* vector move of sampled R */
        0x05600032,0x20000240,0x06000f60,0x88031400  /* four-channel RT */
    };
    memcpy(code,native,sizeof(native));
    CHECK(intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    /* Scalar broadcast cannot consume a masked sampler result: channel zero
     * need not have been active even when another channel is. */
    code[14]=0x00000f60;
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    code[14]=native[14];code[13]|=4; /* NoMask cannot consume masked vector data */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    code[13]=native[13];
    /* A declared push constant is initialized independently of the mask. */
    p->parameter_count=4;p->curb_read_length=1;p->parameter_map_offset=132;
    code[14]=0x00000040;
    CHECK(intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    code[13]|=4;
    CHECK(intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    storage[33]=896;
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));storage[33]=0;
    p->parameter_count=p->curb_read_length=p->parameter_map_offset=0;
    memcpy(code,native,sizeof(native));
    code[9]=0x0f600018; /* overlapping split sampler payloads, both from g0 */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    memcpy(code,native,sizeof(native));
    code[9]=0x01401018;code[13]=0x21403ae8;code[14]=0x008d0140;code[18]=0x06000140;
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage))); /* initialized, but low EOT GRFs */
    memcpy(code,native,sizeof(native));
    code[1]=0x21003ee8; /* leave g0 as an unreadable thread header */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    memcpy(code,native,sizeof(native));
    /* Authored interpolation operands use hardware barycentrics g2/g3 and
     * the declared TEXCOORD coefficient pair g4/g5. */
    p->barycentric_modes=1;p->texcoord_mask=1;p->texcoord_map=0xfffffff0;
    p->layer_input=UINT32_MAX;p->dispatch_grf_start[0]=4;
    const uint32_t pln[]={0x0060005a,0x20003ae8,0x3a000080,0x008d0040,
                          0x0060005a,0x20203ae8,0x3a000090,0x008d0040};
    memcpy(code,pln,sizeof(pln));
    CHECK(intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    code[1]=0x20803ae8; /* clobber coefficient provenance before the second PLN */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    code[1]=0x20403ae8; /* clobber barycentric provenance */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
    memcpy(code,pln,sizeof(pln));code[2]=0x3a0000c0; /* undeclared coefficients */
    CHECK(!intel_gen9_resource_shader_validate(p,storage,sizeof(storage)));
}

int main(int argc,char **argv)
{
    CHECK(argc==2);
    const char *names[]={"ps20-sample","ps30-sample","ps30-sparse","ps30-pair","ps30-dependent",
        "ps30-tint","ps30-coordinate-swizzle","ps30-constant-coordinate","ps30-input-color",
        "ps30-unused-declaration","ps30-dead-sample","ps20-hlsl-sample","ps30-hlsl-pair"};
    for (unsigned i=0;i<sizeof(names)/sizeof(*names);i++) fixture(argv[1],names[i]);
    flow_cases(argv[1]);
    authored_flow_cases();
    CHECK(!intel_gen9_resource_shader_validate(NULL,NULL,0));
    printf("Intel Gen9 resource admission: %u checks passed\n",checks);
    return 0;
}
