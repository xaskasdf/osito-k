/* Run the production timing reader against read-only Gen9 register vectors. */
#define main memory_probe_suite_main
#include "intel_gfx_probe_host.c"
#undef main
#define REG(address) (*(uint32_t *)(aperture+(address)))
static uint8_t timing_before[0x40000];

static void timing_case(unsigned trans,unsigned pipe,unsigned mode)
{
    probe_reset();igfx_probe_memory();
    unsigned t=trans==3 ? 0xf000 : trans*0x1000,p=pipe*0x1000;
    unsigned port=trans==3 ? 0 : trans+1;
    REG(0x45404)=0x50000155;REG(0x42000)=0x86000000;
    REG(0x70008+t)=0xc0000000;
    REG(0x60400+t)=0x80000000 | mode<<24 | (trans==3 ? (pipe ? pipe+4 : 0)<<12 : port<<28);
    REG(0x64000+port*0x100)=0x80000000;
    REG(0x70180+p)=0x84000000;
    REG(0x60000+t)=0x0897077f;REG(0x6000c+t)=REG(0x60010+t)=0x04640437;
    REG(0x6001c+p)=0x077f0437;REG(0x70188+p)=120;REG(0x70190+p)=0x0437077f;
    REG(0x70040+p)=17;REG(0x70048+p)=300;
    CHECK(igfx_find_active_scanout()==0);
    igfx.ready=igfx.gen9=igfx.gop_scanout_retained=true;
}

int main(void)
{
    unsigned cases=0;
    for (unsigned t=0;t<4;t++) for (unsigned p=0;p<3;p++) {
        if (t!=3 && t!=p) continue;
        for (unsigned mode=0;mode<3;mode++) {
            if (t==3 && mode!=2) continue;
            timing_case(t,p,mode);
            for (unsigned raw=0;raw<1125;raw++) {
                REG(0x70000+p*0x1000)=raw;
                memcpy(timing_before,aperture+0x40000,sizeof(timing_before));
                intel_gfx_timing_t s={0};CHECK(intel_gfx_read_timing(&s)==0);
                unsigned line=(raw+(mode==0?2:1))%1125;
                CHECK(s.frame_count==17 && s.frame_timestamp==300 && s.raw_scanline==raw);
                CHECK(s.active_lines==1080 && s.total_lines==1125 && s.scanline==line);
                CHECK(s.in_vertical_blank==(line>=1080));
                CHECK(!memcmp(timing_before,aperture+0x40000,sizeof(timing_before)));
                cases++;
            }
        }
    }
    for (unsigned i=0;i<12;i++) {
        timing_case(3,0,2);
        switch (i) {
        case 0:igfx.ready=false;break;
        case 1:igfx.gen9=false;break;
        case 2:igfx.gop_scanout_retained=false;break;
        case 3:REG(0x45404)=0;break;
        case 4:REG(0x7f008)&=~(1u<<30);break;
        case 5:REG(0x70000)=1125;break;
        case 6:REG(0x70000)=UINT32_MAX;break;
        case 7:REG(0x70000)=1u<<31;break;
        case 8:REG(0x6f010)^=1;break;
        case 9:igfx.trans_vtotal^=1u<<16;break;
        case 10:igfx.active_pipe=1;break;
        case 11:igfx.pipeconf^=1;break;
        }
        intel_gfx_timing_t s,saved;memset(&s,0x5a,sizeof(s));memcpy(&saved,&s,sizeof(s));
        CHECK(intel_gfx_read_timing(&s)==1 && !memcmp(&s,&saved,sizeof(s)));
    }
    timing_case(3,0,2);CHECK(intel_gfx_read_timing(NULL)==1);
    REG(0x70040)=UINT32_MAX;REG(0x70048)=UINT32_MAX;
    intel_gfx_timing_t s={0};CHECK(intel_gfx_read_timing(&s)==0);
    CHECK(s.frame_count==UINT32_MAX && s.frame_timestamp==UINT32_MAX);
    /* A frame rollover between timestamp/line reads retries as one capture. */
    change_register=0x70040;change_mask=UINT32_MAX;change_on_read=2;
    CHECK(intel_gfx_read_timing(&s)==0 && s.frame_count==0 && change_reads==4);
    const uint32_t changed[]={0x45404,0x42000,0x7f008,0x6f400,0x64000,
        0x70180,0x6f000,0x6f00c,0x6001c,0x70188,0x70190,0x7019c,0x701ac,0x701a4,0x7018c,0x6f010};
    for (unsigned i=0;i<sizeof(changed)/sizeof(changed[0]);i++) {
        timing_case(3,0,2);change_register=changed[i];change_mask=1;change_on_read=2;
        intel_gfx_timing_t saved;memset(&s,0xa5,sizeof(s));memcpy(&saved,&s,sizeof(s));
        CHECK(intel_gfx_read_timing(&s)==2 && !memcmp(&s,&saved,sizeof(s)));
    }
    printf("Intel timing: %u checks, %u line vectors, 0 failures (no MMIO writes)\n",checks,cases);
    return 0;
}
