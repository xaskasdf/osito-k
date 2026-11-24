#include "intel_gen9_shader.h"
#include "intel_gen9_compact_tables.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

static bool gen9_expand_mov(uint64_t raw,uint32_t words[4])
{
    /* Gen8/9 two-source compaction. The expanded instruction is separately
     * checked against the restricted, address-free native ALU contract. */
    unsigned opcode=raw&127;
    if ((opcode!=1 && opcode!=64 && opcode!=65 && opcode!=90) || !(raw&(1ULL<<29)) ||
        (raw&((1ULL<<7)|(1ULL<<28)))) return false;
    uint32_t control=gfx8_control_index_table[(raw>>8)&31];
    uint32_t type=gfx8_datatype_table[(raw>>13)&31];
    uint32_t sub=gfx8_subreg_table[(raw>>18)&31];
    uint32_t region=gfx8_src_index_table[(raw>>30)&31];
    uint64_t lo=opcode | ((uint64_t)(control>>16)<<31) |
        ((uint64_t)((control>>4)&0xfff)<<12) | ((uint64_t)((control>>2)&3)<<9) |
        ((uint64_t)((control>>1)&1)<<34) | ((uint64_t)(control&1)<<8) |
        ((uint64_t)(type>>18)<<61) | ((uint64_t)(type&0xfff)<<35) |
        ((uint64_t)(sub&31)<<48) | ((raw>>40)&255)<<53 |
        (raw&0x0f000000ULL) | ((raw&(1ULL<<23))<<5);
    uint64_t hi=((uint64_t)((type>>12)&63)<<25) | ((sub>>5)&31) |
        ((uint64_t)region<<13) | (((raw>>48)&255)<<5);
    if (((lo>>41)&3)==3 || ((hi>>25)&3)==3) {
        uint32_t immediate=(uint32_t)(((raw>>35)&31)<<8)|(uint32_t)(raw>>56);
        if (immediate&0x1000) immediate|=0xffffe000u;
        hi|=(uint64_t)immediate<<32;
    } else {
        hi|=(uint64_t)((sub>>10)&31)<<32;
        hi|=(uint64_t)gfx8_src_index_table[(raw>>35)&31]<<45;
        hi|=((raw>>56)&255)<<37;
    }
    words[0]=(uint32_t)lo; words[1]=(uint32_t)(lo>>32);
    words[2]=(uint32_t)hi; words[3]=(uint32_t)(hi>>32);
    return true;
}
static bool gen9_payload(const bool initialized[128],unsigned first,unsigned count,unsigned grfs)
{
    if (first<2 || first>=grfs || count>grfs-first) return false;
    for (unsigned i=first;i<first+count;i++) if (!initialized[i]) return false;
    return true;
}
static bool gen9_source(uint32_t bits,unsigned lanes,unsigned grfs,const bool full[128])
{
    /* Only direct scalar broadcast and contiguous float vectors. Inspect all
     * source GRFs, including lanes the hardware execution mask could skip. */
    if (bits&0xfe008000u) return false;
    unsigned sub=bits&31,reg=(bits>>5)&255,h=(bits>>16)&3;
    unsigned w=(bits>>18)&7,v=(bits>>21)&15;
    if ((sub&3) || reg>=grfs) return false;
    unsigned end;
    if (!h && !w && !v) end=reg;
    else if (h==1 && w==3 && v==4 && !sub) end=reg+(lanes-1)/8;
    else return false;
    if (end>=grfs) return false;
    for (unsigned i=reg;i<=end;i++) if (!full[i]) return false;
    return true;
}
static bool gen9_float_alu(const uint32_t w[4],unsigned width,unsigned grfs,
    bool initialized[128],bool full[128])
{
    unsigned opcode=w[0]&127,execution=(w[0]>>21)&7;
    if ((opcode!=1 && opcode!=64 && opcode!=65) || execution<3 || execution>4 ||
        (8u<<(execution-3))>width || (w[0]&~(0x80000000u|7u<<21|127u))) return false;
    unsigned lanes=1u<<execution,reg=(w[1]>>21)&255;
    bool all=(w[1]&4)!=0;
    if (lanes<width && !all) return false;
    uint32_t format=w[1]&~((255u<<21)|4u);
    if (reg>=grfs || lanes/8>grfs-reg) return false;
    if (opcode==1 && ((format==0x20003ee8u && w[2]==0x38000000u) ||
                     (format==0x20002ee8u && w[2]==0))) {
        /* Immediate F/VF, no GRF source. */
    } else {
        if (format!=0x20003ae8u || !gen9_source(w[2]&0x01ffffffu,lanes,grfs,full)) return false;
        if (opcode==1) {
            if ((w[2]&0xfe000000u) || w[3]) return false;
        } else {
            uint32_t type=w[2]&0xfe000000u;
            if (type!=0x3e000000u &&
                (type!=0x3a000000u || !gen9_source(w[3],lanes,grfs,full))) return false;
        }
    }
    for (unsigned i=reg;i<reg+lanes/8;i++) {
        initialized[i]=true;
        if (all) full[i]=true;
    }
    return true;
}
static bool gen9_kernel(const uint8_t *code,unsigned bytes,unsigned width,unsigned grfs,
    unsigned constant_start,unsigned constant_grfs)
{
    bool initialized[128]={0},full[128]={0},ended=false;
    for (unsigned i=constant_start;i<constant_start+constant_grfs;i++) initialized[i]=full[i]=true;
    unsigned registers=width/8,execution=width==8 ? 3 : 4;
    for (unsigned offset=0;offset<bytes;) {
        uint32_t words[4]={0};
        if (bytes-offset<8) return false;
        memcpy(words,code+offset,8);
        bool compact=(words[0]&(1u<<29))!=0;
        unsigned length=compact ? 8 : 16;
        if (bytes-offset<length) return false;
        if (!compact) memcpy(words+2,code+offset+8,8);
        if (ended) {
            /* Alignment after EOT is not executable. Require canonical zero
             * or compact NOP padding so alternate entries cannot hide code. */
            if (words[0]!=0 && words[0]!=0x2000007eu) return false;
            if (words[1] || words[2] || words[3]) return false;
            offset+=length; continue;
        }
        if (compact && !gen9_expand_mov((uint64_t)words[0]|((uint64_t)words[1]<<32),words)) return false;
        unsigned opcode=words[0]&127;
        if (constant_grfs && (opcode==1 || opcode==64 || opcode==65)) {
            if (!gen9_float_alu(words,width,grfs,initialized,full)) return false;
        } else if (opcode==1) {
            unsigned reg=(words[1]>>21)&255;
            /* Align1, no predicates/dependency suppression, full GRF writes,
             * float destination, scalar F or packed VF immediate. Mesa's
             * compactor can express constant zero using the VF encoding. */
            uint32_t format=words[1]&~(255u<<21);
            bool immediate=(format==0x20003ee8u && words[2]==0x38000000u) ||
                (format==0x20002ee8u && words[2]==0);
            if (words[0]!=(execution<<21|1u) ||
                !immediate ||
                reg<2 || reg>=grfs || registers>grfs-reg) return false;
            for (unsigned i=reg;i<reg+registers;i++) initialized[i]=true;
        } else if (opcode==50 || opcode==52) {
            if (compact || words[0]!=(5u<<24|execution<<21|opcode)) return false;
            unsigned first=(words[2]>>5)&255;
            if (first<112) return false; /* Gen9 EOT payload must use g112..g127. */
            uint32_t descriptor=0x80031000u|(width==8 ? 0x400u : 0);
            if (opcode==50) {
                /* SENDC: null result, immediate descriptor, no header,
                 * four float components, BTI0, last RT, end-of-thread. */
                if (words[1]!=0x20000240u || (words[2]&~(255u<<5))!=0x06000000u ||
                    words[3]!=(descriptor|((4*registers)<<25)) ||
                    !gen9_payload(initialized,first,4*registers,grfs)) return false;
            } else {
                /* SENDSC: same RT contract, split 1+3-component payload.
                 * Both descriptors are immediate; ex_desc contains only
                 * the second payload length. No indirect address/register. */
                unsigned second=(words[1]>>12)&255;
                unsigned a=(words[3]>>25)&15,b=4*registers-a;
                if (!a || a>=4*registers || a%registers || (!constant_grfs && a!=registers) ||
                    second<112 || (first<second+b && second<first+a) ||
                    (words[1]&~(255u<<12))!=0x10u ||
                    (words[2]&~(255u<<5))!=b || words[3]!=(descriptor|(a<<25)) ||
                    !gen9_payload(initialized,first,a,grfs) ||
                    !gen9_payload(initialized,second,b,grfs)) return false;
            }
            ended=true;
        } else return false;
        offset+=length;
    }
    return ended;
}
bool intel_gen9_shader_validate(const struct intel_d3d9_program *captured,const void *allocation,size_t bytes)
{
    if (!captured || !allocation || bytes<sizeof(*captured) ||
        bytes>((uintptr_t)-1)-(uintptr_t)allocation ||
        !intel_rcs_pixel_validate(captured,bytes) ||
        memcmp(captured,allocation,sizeof(*captured)) || (captured->simd_mask&~3u)) return false;
    const struct intel_d3d9_program p=*captured;
    const uint8_t *base=allocation;
    for (unsigned i=0;i<p.parameter_count;i++) {
        uint32_t scalar; memcpy(&scalar,base+p.parameter_map_offset+i*4,4);
        if (scalar>=224u*4) return false;
    }
    unsigned first=(p.simd_mask&1) ? 0 : 1;
    if (p.kernel_offset[first]!=0 ||
        (p.simd_mask==3 && p.kernel_offset[1]<=p.kernel_offset[0])) return false;
    const uint8_t *code=(const uint8_t *)allocation+p.code_offset;
    for (unsigned i=first;i<2;i++) if (p.simd_mask&(1u<<i)) {
        unsigned end=i==0 && (p.simd_mask&2) ? p.kernel_offset[1] : p.code_bytes;
        if (end<=p.kernel_offset[i] || !gen9_kernel(code+p.kernel_offset[i],end-p.kernel_offset[i],8u<<i,p.grf_used,
            p.dispatch_grf_start[i],p.curb_read_length)) return false;
    }
    return true;
}

bool intel_gen9_resource_program_validate(const struct intel_d3d9_program *input,size_t bytes)
{
    if (!input || ((uintptr_t)input&3) || bytes<sizeof(*input) ||
        bytes>((uintptr_t)-1)-(uintptr_t)input) return false;
    const struct intel_d3d9_program p=*input;
    if (p.abi!=INTEL_D3D9_COMPILER_RESOURCE_ABI || p.varying_inputs>9 ||
        (p.texture_mask&~0xffffu) || p.texture_mask!=p.sampler_mask ||
        (p.texcoord_mask&~255u) || p.resource_reserved || (!p.varying_inputs && !p.curb_read_length) ||
        p.barycentric_modes!=(p.texcoord_mask ? 1u : 0u)) return false;
    /* Reuse the common byte/constant/code bounds without admitting resource
     * programs into the legacy, unbound draw path. */
    struct intel_d3d9_program common=p;
    common.abi=INTEL_D3D9_COMPILER_ABI; common.varying_inputs=0;
    common.barycentric_modes=0; common.layer_input=UINT32_MAX;
    memset(common.reserved,0,sizeof(common.reserved));
    if (!intel_rcs_pixel_validate(&common,bytes)) return false;
    unsigned slots=0;
    for (unsigned semantic=0;semantic<8;semantic++) {
        unsigned slot=(p.texcoord_map>>(semantic*4))&15;
        if (!(p.texcoord_mask&(1u<<semantic))) { if (slot!=15) return false; continue; }
        if (slot>=p.varying_inputs || (slots&(1u<<slot))) return false;
        slots|=1u<<slot;
    }
    if (p.layer_input!=UINT32_MAX) {
        if (p.layer_input>=p.varying_inputs || (slots&(1u<<p.layer_input))) return false;
        slots|=1u<<p.layer_input;
    }
    if (slots!=(1u<<p.varying_inputs)-1) return false;
    unsigned previous=0; bool first=true;
    for (unsigned i=0;i<3;i++) {
        if (!(p.simd_mask&(1u<<i))) {
            if (p.kernel_offset[i] || p.dispatch_grf_start[i]) return false;
            continue;
        }
        if (first ? p.kernel_offset[i]!=0 : p.kernel_offset[i]<=previous) return false;
        first=false; previous=p.kernel_offset[i];
        unsigned payload=(i==2 ? 3u : 2u)+(p.barycentric_modes ? 2u*(1u<<i) : 0u);
        if (p.dispatch_grf_start[i]!=payload ||
            payload+p.curb_read_length+2*p.varying_inputs>p.grf_used) return false;
    }
    return true;
}

/* Each GRF records which logical eight-lane quarter may consume its values.
 * 0x10 additionally proves initialization independent of the execution mask
 * (constants/hardware inputs/NoMask writes), required for scalar broadcasts.
 * Interpolation operands also retain provenance; ordinary writes consume it. */
typedef struct { uint8_t valid[128],coefficient[128],barycentric[128]; } gen9_resource_flow_t;

static bool gen9_resource_read(const gen9_resource_flow_t *f,unsigned reg,
    unsigned count,unsigned registers,unsigned quarter,unsigned grfs)
{
    if (!count || reg>=grfs || count>grfs-reg) return false;
    for (unsigned i=0;i<count;i++)
        if (!(f->valid[reg+i]&(1u<<(quarter+i%registers)))) return false;
    return true;
}

static void gen9_resource_write(gen9_resource_flow_t *f,unsigned reg,
    unsigned count,unsigned registers,unsigned quarter,bool all)
{
    for (unsigned i=0;i<count;i++) {
        f->valid[reg+i]=all ? 0x1f : (uint8_t)(1u<<(quarter+i%registers));
        f->coefficient[reg+i]=f->barycentric[reg+i]=0;
    }
}

static bool gen9_resource_source(const gen9_resource_flow_t *f,uint32_t bits,
    unsigned registers,unsigned quarter,unsigned grfs,bool all)
{
    if (bits&0xfe008000u) return false;
    unsigned sub=bits&31,reg=(bits>>5)&255;
    if (reg>=grfs || (sub&3)) return false;
    uint32_t region=bits&~(255u<<5|31u|3u<<13); /* float abs/negate preserve provenance */
    if (!region) return (f->valid[reg]&0x10)!=0;
    if (region!=0x008d0000u || sub || registers>grfs-reg) return false;
    if (all) {
        for (unsigned i=0;i<registers;i++) if (!(f->valid[reg+i]&0x10)) return false;
        return true;
    }
    return gen9_resource_read(f,reg,registers,registers,quarter,grfs);
}

static bool gen9_resource_kernel(const uint8_t *code,unsigned bytes,unsigned width,
    const struct intel_d3d9_program *p,unsigned start)
{
    gen9_resource_flow_t flow={0};
    unsigned grfs=p->grf_used,seen_samplers=0,written=0;
    bool ended=false;
    for (unsigned i=start;i<start+p->curb_read_length+2*p->varying_inputs;i++) flow.valid[i]=0x1f;
    if (p->barycentric_modes) {
        unsigned bary=width==32 ? 3 : 2;
        for (unsigned i=0;i<2*(width/8);i++) {
            flow.valid[bary+i]=0x1f; flow.barycentric[bary+i]=1;
        }
        for (unsigned semantic=0;semantic<8;semantic++) if (p->texcoord_mask&(1u<<semantic)) {
            unsigned slot=(p->texcoord_map>>(semantic*4))&15;
            unsigned reg=start+p->curb_read_length+2*slot;
            flow.coefficient[reg]=flow.coefficient[reg+1]=1;
        }
    }
    for (unsigned offset=0;offset<bytes;) {
        uint32_t w[4]={0};
        if (bytes-offset<8) return false;
        memcpy(w,code+offset,8);
        bool compact=(w[0]&(1u<<29))!=0;
        unsigned length=compact ? 8 : 16;
        if (bytes-offset<length) return false;
        if (!compact) memcpy(w+2,code+offset+8,8);
        offset+=length;
        if (ended) {
            if ((w[0] && w[0]!=0x2000007e) || w[1] || w[2] || w[3]) return false;
            continue;
        }
        if (compact && !gen9_expand_mov((uint64_t)w[0]|((uint64_t)w[1]<<32),w)) return false;
        unsigned opcode=w[0]&127,execution=(w[0]>>21)&7,quarter=(w[0]>>12)&3;
        if (execution<3 || execution>4) return false;
        unsigned registers=1u<<(execution-3),lanes=registers*8;
        if (quarter%registers || quarter+registers>width/8) return false;
        unsigned dst=(w[1]>>21)&255;
        if (opcode==1 || opcode==64 || opcode==65 || opcode==90) {
            if (w[0]&~(0x80000000u|7u<<21|3u<<12|127u) || dst>=grfs || registers>grfs-dst) return false;
            bool all=(w[1]&4)!=0;
            uint32_t format=w[1]&~(255u<<21|4u);
            if (opcode==90) {
                unsigned coeff=(w[2]>>5)&255,bary=width==32 ? 3+2*quarter : 2;
                if (all || (w[0]&0x80000000u) || !p->barycentric_modes || lanes!=(width==8 ? 8u : 16u) ||
                    format!=0x20003ae8u || (w[2]&~(255u<<5|16u))!=0x3a000000u ||
                    coeff>=grfs || !flow.coefficient[coeff] ||
                    w[3]!=(0x008d0000u|bary<<5) || bary+2*registers>grfs) return false;
                for (unsigned i=0;i<2*registers;i++) if (!flow.barycentric[bary+i]) return false;
            } else if (opcode==1 && ((format==0x20003ee8u && w[2]==0x38000000u) ||
                                    (format==0x20002ee8u && !w[2]))) {
                /* Immediate binary32 / packed float, no memory access. */
            } else {
                if (format!=0x20003ae8u || !gen9_resource_source(&flow,w[2]&0x01ffffffu,registers,quarter,grfs,all)) return false;
                if (opcode==1) { if ((w[2]&0xfe000000u) || w[3]) return false; }
                else {
                    uint32_t type=w[2]&0xfe000000u;
                    if (type!=0x3e000000u && (type!=0x3a000000u ||
                        !gen9_resource_source(&flow,w[3],registers,quarter,grfs,all))) return false;
                }
            }
            gen9_resource_write(&flow,dst,registers,registers,quarter,all);
        } else if (opcode==49 || opcode==51 || opcode==50 || opcode==52) {
            bool sampler=opcode==49 || opcode==51,split=opcode==51 || opcode==52;
            if (compact || w[0]!=((sampler ? 2u : 5u)<<24|execution<<21|quarter<<12|opcode)) return false;
            unsigned first=(w[2]>>5)&255,second=(w[1]>>12)&255;
            unsigned a=(w[3]>>25)&15,b=split ? w[2]&31 : 0;
            unsigned response=(w[3]>>20)&31;
            if (!a || a%registers || b%registers || (split && !b)) return false;
            if (split) {
                uint32_t expected=sampler ? 0x18 : 0x10;
                if ((w[1]&~(255u<<12|(sampler ? 255u<<21 : 0)))!=expected ||
                    (w[2]&~(255u<<5))!=b) return false;
            } else {
                if ((w[1]&~(sampler ? 255u<<21 : 0))!=(sampler ? 0x20000248u : 0x20000240u) ||
                    (w[2]&~(255u<<5))!=0x06000000u) return false;
            }
            if (!gen9_resource_read(&flow,first,a,registers,quarter,grfs) ||
                (split && !gen9_resource_read(&flow,second,b,registers,quarter,grfs))) return false;
            if (split && first<second+b && second<first+a) return false;
            if (sampler) {
                unsigned index=(w[3]>>8)&15;
                uint32_t descriptor=(a<<25)|(response<<20)|((lanes==8 ? 1u : 2u)<<17)|(index<<8)|(index+1);
                if (w[3]!=descriptor || !(p->sampler_mask&(1u<<index)) ||
                    a+b!=2*registers || !response || response>4*registers || response%registers ||
                    dst>=grfs || response>grfs-dst ||
                    (!split && dst+response>127 && first+a>dst)) return false;
                seen_samplers|=1u<<index;
                gen9_resource_write(&flow,dst,response,registers,quarter,false);
            } else {
                unsigned mask=((1u<<registers)-1)<<quarter;
                bool eot=(w[3]>>31)!=0;
                if (eot && (first<112 || (split && second<112))) return false;
                uint32_t descriptor=(eot ? 0x80000000u : 0)|0x31000u|
                    (lanes==8 ? 0x400u : 0)|(quarter ? 0x800u : 0)|(a<<25);
                if (w[3]!=descriptor || a+b!=4*registers || (written&mask) ||
                    (width==32 && (lanes!=16 || quarter%2)) || (width!=32 && lanes!=width)) return false;
                written|=mask;
                if (eot) {
                    if (written!=(1u<<(width/8))-1) return false;
                    ended=true;
                }
            }
        } else return false;
    }
    return ended && seen_samplers==p->sampler_mask;
}

bool intel_gen9_resource_shader_validate(const struct intel_d3d9_program *captured,
    const void *allocation,size_t bytes)
{
    if (!captured || !allocation || bytes>((uintptr_t)-1)-(uintptr_t)allocation ||
        !intel_gen9_resource_program_validate(captured,bytes) || memcmp(captured,allocation,sizeof(*captured))) return false;
    const struct intel_d3d9_program p=*captured;
    const uint8_t *base=allocation;
    for (unsigned i=0;i<p.parameter_count;i++) {
        uint32_t scalar; memcpy(&scalar,base+p.parameter_map_offset+i*4,4);
        if (scalar>=224u*4) return false;
    }
    for (unsigned i=0;i<3;i++) if (p.simd_mask&(1u<<i)) {
        unsigned end=p.code_bytes;
        for (unsigned j=i+1;j<3;j++) if (p.simd_mask&(1u<<j)) { end=p.kernel_offset[j]; break; }
        if (!gen9_resource_kernel(base+p.code_offset+p.kernel_offset[i],end-p.kernel_offset[i],
            8u<<i,&p,p.dispatch_grf_start[i])) return false;
    }
    return true;
}

bool intel_gen9_vertex_program_validate(const struct intel_d3d9_vertex_program *v,size_t bytes)
{
    if (!v || ((uintptr_t)v&3) || bytes<sizeof(*v) || bytes>((uintptr_t)-1)-(uintptr_t)v) return false;
    const struct intel_d3d9_program *p=&v->base;
    if (p->abi!=4 || p->header_bytes!=sizeof(*v) || p->allocation_bytes!=bytes || p->pci_id!=0x3e9b ||
        p->bytecode_offset!=sizeof(*v) || !p->bytecode_bytes || (p->bytecode_bytes&3) || p->bytecode_bytes>65536 ||
        p->code_offset<sizeof(*v)+p->bytecode_bytes || (p->code_offset&63) || p->code_offset>bytes ||
        !p->code_bytes || (p->code_bytes&7) || p->code_bytes>INTEL_RCS_PIXEL_MAX_CODE || p->code_bytes!=bytes-p->code_offset ||
        p->stage || p->simd_mask!=1 || !p->grf_used || p->grf_used>128 || p->scratch_bytes ||
        p->constant_data_offset || p->constant_data_bytes || (p->flags&~1u) || p->barycentric_modes ||
        p->layer_input!=UINT32_MAX || p->parameter_count>512 || p->curb_read_length!=(p->parameter_count+7)/8 ||
        p->dispatch_grf_start[0]!=2 || p->dispatch_grf_start[1] || p->dispatch_grf_start[2] ||
        (v->input_mask&~65535u) || !(v->output_mask&1) || (v->output_mask&~2047u)) return false;
    for (unsigned i=0;i<3;i++) if (p->kernel_offset[i]) return false;
    for (unsigned i=0;i<5;i++) if (p->reserved[i]) return false;
    if (p->parameter_count) {
        if ((p->parameter_map_offset&3) || p->parameter_map_offset<sizeof(*v)+p->bytecode_bytes ||
            p->parameter_map_offset>p->code_offset || p->parameter_count*4>p->code_offset-p->parameter_map_offset) return false;
    } else if (p->parameter_map_offset) return false;
    unsigned inputs=0,outputs=0;
    for (unsigned i=0;i<16;i++) {
        unsigned semantic=v->input_semantics[i];
        if (!(v->input_mask&(1u<<i))) {if (semantic!=UINT32_MAX) return false;continue;}
        if ((semantic&~0x000f000fu) || (semantic&15)>13 || (semantic&15)==9) return false;
        for (unsigned j=0;j<i;j++) if (v->input_semantics[j]==semantic) return false;
        inputs++;
    }
    for (unsigned i=0;i<12;i++) {
        if (!(v->output_mask&(1u<<i))) {if (v->output_slots[i]!=UINT32_MAX) return false;continue;}
        if (v->output_slots[i]!=++outputs) return false;
    }
    unsigned size=inputs>outputs+1 ? inputs : outputs+1;
    return p->varying_inputs==inputs && v->urb_read_length==(inputs+1)/2 &&
        v->urb_entry_size==(size+3)/4 && 2+p->curb_read_length+8*v->urb_read_length<=p->grf_used;
}

static bool gen9_vertex_kernel(const uint8_t *code,const struct intel_d3d9_vertex_program *v)
{
    const struct intel_d3d9_program *p=&v->base;
    gen9_resource_flow_t f={0};
    bool handle[128]={0},ended=false;
    unsigned written=0,wanted=0,grfs=p->grf_used;
    handle[1]=true;
    for (unsigned i=2;i<2+p->curb_read_length+4*p->varying_inputs;i++) f.valid[i]=0x1f;
    for (unsigned i=0;i<11;i++) if (v->output_mask&(1u<<i)) wanted|=1u<<v->output_slots[i];
    for (unsigned offset=0;offset<p->code_bytes;) {
        uint32_t w[4]={0};
        if (p->code_bytes-offset<8) return false;
        memcpy(w,code+offset,8);
        bool compact=(w[0]&(1u<<29))!=0;
        unsigned length=compact ? 8 : 16;
        if (p->code_bytes-offset<length) return false;
        if (!compact) memcpy(w+2,code+offset+8,8);
        offset+=length;
        if (ended) {
            if ((w[0] && w[0]!=0x2000007e) || w[1] || w[2] || w[3]) return false;
            continue;
        }
        if (compact && !gen9_expand_mov((uint64_t)w[0]|((uint64_t)w[1]<<32),w)) return false;
        unsigned opcode=w[0]&127,dst=(w[1]>>21)&255;
        if (opcode==1 || opcode==64 || opcode==65) {
            if ((w[0]&~0x80000000u)!=(3u<<21|opcode) || dst>=grfs) return false;
            bool all=(w[1]&4)!=0;
            uint32_t format=w[1]&~(255u<<21|4u);
            if (opcode==1 && format==0x20000208u) {
                /* Only an exact NoMask UD copy preserves URB handle provenance. */
                unsigned source=(w[2]>>5)&255;
                if (!all || (w[0]&0x80000000u) || (w[2]&~(255u<<5))!=0x008d0000 ||
                    w[3] || source>=grfs || !handle[source]) return false;
                handle[dst]=true;f.valid[dst]=0;continue;
            }
            if (opcode==1 && ((format==0x20003ee8u && w[2]==0x38000000u) ||
                             (format==0x20002ee8u && !w[2]))) {
                /* Immediate F/VF value, no memory access. */
            } else {
                if (format!=0x20003ae8u || !gen9_resource_source(&f,w[2]&0x01ffffffu,1,0,grfs,all)) return false;
                if (opcode==1) {if ((w[2]&0xfe000000u) || w[3]) return false;}
                else {
                    unsigned type=w[2]&0xfe000000u;
                    if (type!=0x3e000000u && (type!=0x3a000000u || !gen9_resource_source(&f,w[3],1,0,grfs,all))) return false;
                }
            }
            f.valid[dst]=all ? 0x1f : 1;handle[dst]=false;
        } else if (opcode==91 || opcode==92) {
            /* Gen9 align16 three-source floating MAD/LRP, direct GRFs only.
             * Vector sources use xyzw; scalar replication uses xxxx and a
             * DWORD-aligned subregister. The high subregister bit encodes
             * halfword addressing and is deliberately rejected. */
            dst=w[1]>>24;bool all=(w[1]&4)!=0;
            if (compact || (w[0]&~0x80000000u)!=(0x00600100u|opcode) || dst>=grfs ||
                (w[1]&~(0xff000000u|0x7e0u|4u))!=0x001e0000u || (w[3]&0x80000000u)) return false;
            uint64_t sources=(uint64_t)w[2]|((uint64_t)w[3]<<32);
            for (unsigned i=0;i<3;i++) {
                unsigned source=(sources>>(21*i))&0x1fffff,reg=(source>>12)&255;
                bool scalar=(source&1)!=0;
                if (reg>=grfs || (source&0x100000u) ||
                    (scalar ? ((source>>1)&255)!=0 : (source&4095)!=0x1c8) ||
                    !(f.valid[reg]&(scalar || all ? 0x10 : 1))) return false;
            }
            f.valid[dst]=all ? 0x1f : 1;handle[dst]=false;
        } else if (opcode==51) {
            /* SIMD8 URB write, one immutable handle GRF plus 1/2 float4s.
             * No per-slot offset or channel-mask payload can redirect it. */
            unsigned first=(w[2]>>5)&255,second=(w[1]>>12)&255,count=w[2]&31;
            unsigned slot=(w[3]>>4)&0x7ff;
            bool eot=(w[3]>>31)!=0;
            if (compact || w[0]!=0x06600033u || (w[1]&~(255u<<12))!=0x10 ||
                (w[2]&~(255u<<5|31u)) || first>=grfs || !handle[first] ||
                (count!=4 && count!=8) || !slot || slot>=16 || count/4>16-slot ||
                w[3]!=(0x02080007u|(slot<<4)|(eot ? 0x80000000u : 0)) ||
                !gen9_resource_read(&f,second,count,1,0,grfs) ||
                (first>=second && first<second+count) || (eot && (first<112 || second<112))) return false;
            unsigned mask=((1u<<(count/4))-1)<<slot;
            if ((mask&~wanted) || (mask&written)) return false;
            written|=mask;
            if (eot) {if (written!=wanted) return false;ended=true;}
        } else return false;
    }
    return ended && written==wanted;
}

bool intel_gen9_vertex_shader_validate(const struct intel_d3d9_vertex_program *captured,
    const void *allocation,size_t bytes)
{
    if (!allocation || bytes>((uintptr_t)-1)-(uintptr_t)allocation ||
        !intel_gen9_vertex_program_validate(captured,bytes) || memcmp(captured,allocation,sizeof(*captured))) return false;
    const uint8_t *base=allocation;
    for (unsigned i=0;i<captured->base.parameter_count;i++) {
        uint32_t scalar;memcpy(&scalar,base+captured->base.parameter_map_offset+i*4,4);
        if (scalar>=1024) return false;
    }
    return gen9_vertex_kernel(base+captured->base.code_offset,captured);
}
