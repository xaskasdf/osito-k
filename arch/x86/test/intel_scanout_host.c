/* Exercise production read-only scanout discovery with explicit register
 * vectors from the Gen9/KBL PRM. No privileged init or test-only decoder. */
#define main memory_probe_suite_main
#include "intel_gfx_probe_host.c"
#undef main

#define REG(address) (*(uint32_t *)(aperture+(address)))
static uint8_t before[0x40000];

static void scanout_case(unsigned transcoder,unsigned pipe)
{
    probe_reset(); igfx_probe_memory();
    REG(0x45404)=0x50000155; /* PG1/PG2, all DDI IO state bits, no requests. */
    REG(0x42000)=0x86000000; /* Fuse download and PG1/PG2 distributed. */
    unsigned trans_offset=transcoder==3 ? 0xf000 : transcoder*0x1000;
    unsigned pipe_offset=pipe*0x1000;
    unsigned port=transcoder==3 ? 0 : transcoder+1;
    REG(0x70008+trans_offset)=0xc0000000; /* requested AND actually enabled */
    REG(0x60400+trans_offset)=0x82000006 | (transcoder==3 ? (pipe ? pipe+4 : 0)<<12 : port<<28);
    REG(0x64000+port*0x100)=0x80000006;
    REG(0x70180+pipe_offset)=0x84000000;
    REG(0x60000+trans_offset)=0x0897077f; /* 2200 total, 1920 active */
    REG(0x6000c+trans_offset)=0x04640437; /* 1125 total, 1080 active */
    REG(0x6001c+pipe_offset)=0x077f0437;
    REG(0x70188+pipe_offset)=120; /* units are 64 bytes */
    REG(0x70190+pipe_offset)=0x0437077f;
    REG(0x7019c+pipe_offset)=REG(0x701ac+pipe_offset)=0;
    REG(0x701a4+pipe_offset)=REG(0x7018c+pipe_offset)=0;
}

int main(void)
{
    if (memory_probe_suite_main()) return 1;
    for (unsigned t=0;t<4;t++) for (unsigned p=0;p<3;p++) {
        if (t!=3 && p!=t) continue;
        scanout_case(t,p);
        memcpy(before,aperture+0x40000,sizeof(before));
        CHECK(igfx_find_active_scanout()==0);
        CHECK(igfx.active_pipe==(int)p && igfx.active_transcoder==(int)t && igfx.active_plane==0);
        CHECK(igfx.active_port==(t==3 ? 0 : (int)t+1));
        CHECK(igfx.pipeconf==0xc0000000 && igfx.plane_ctl==0x84000000 && igfx.plane_stride==120);
        CHECK(igfx.trans_htotal==0x0897077f && igfx.trans_vtotal==0x04640437 && igfx.pipe_src==0x077f0437);
        CHECK(!memcmp(before,aperture+0x40000,sizeof(before)));
        /* Discovering the active buffer does not take ownership or ready it. */
        CHECK(!igfx.ready && !igfx.gop_scanout_retained);
    }
    scanout_case(3,0);
    REG(0x45404)&=~(1u<<30); REG(0x42000)&=~(1u<<25);
    capture_reads=true;
    CHECK(igfx_find_active_scanout()==0 && igfx.active_transcoder==3);
    for (unsigned i=0;i<captured_count;i++) {
        uint32_t r=captured_reads[i];
        CHECK(r!=0x70008 && r!=0x71008 && r!=0x72008); /* stopped A/B/C transcoders */
        CHECK(r<0x71000 || r>=0x73000); /* no PG2 plane B/C */
    }
    for (unsigned input=0;input<8;input++) {
        scanout_case(3,0); REG(0x6f400)=(REG(0x6f400)&~(7u<<12)) | (input<<12);
        if (input==5 || input==6) { scanout_case(3,input-4); }
        CHECK((igfx_find_active_scanout()==0)==(input==0 || input==5 || input==6));
    }
    /* Captured P1 UHD 630 eDP registers. SURFLIVE has reserved bit 5 set;
     * its address field still names the GOP surface at GGTT offset zero. */
    scanout_case(3,0);
    REG(0x45404)=0x50000005; REG(0x42000)=0x8e000000;
    REG(0x6f400)=0x82010002; REG(0x64000)=0x87000003;
    REG(0x6f000)=0x084b077f; REG(0x6f00c)=0x04570437;
    REG(0x701ac)=0x20;
    CHECK(igfx_find_active_scanout()==0);
    CHECK(igfx.active_transcoder==3 && igfx.active_pipe==0 && igfx.active_port==0);
    CHECK(igfx.plane_surf==0 && igfx.plane_surf_live==0x20); /* keep raw evidence */
    for (unsigned bit=0;bit<32;bit++) {
        scanout_case(3,0); REG(0x701ac)=1u<<bit;
        CHECK((igfx_find_active_scanout()==0)==(bit<12));
        scanout_case(3,0); REG(0x7019c)=1u<<bit;
        CHECK((igfx_find_active_scanout()==0)==(bit<12));
    }
    /* Each rejection changes an independent property of the firmware plane.
     * Unknown/all-ones registers, pending changes and non-GOP surfaces cannot
     * be used to assign the desktop to a native driver. */
    for (unsigned c=0;c<37;c++) {
        scanout_case(3,0);
        switch (c) {
        case 0: REG(0x45404)=0; break;
        case 1: REG(0x45404)=UINT32_MAX; break;
        case 2: REG(0x42000)=UINT32_MAX; break;
        case 3: REG(0x42000)&=~(1u<<26); break;
        case 4: REG(0x7f008)=1u<<31; break; /* requested but not enabled */
        case 5: REG(0x7f008)=1u<<30; break; /* disabling */
        case 6: REG(0x7f008)=UINT32_MAX; break;
        case 7: REG(0x6f400)&=~(1u<<31); break;
        case 8: REG(0x6f400)=UINT32_MAX; break;
        case 9: REG(0x6f400)=(REG(0x6f400)&~(7u<<24))|(3u<<24); break; /* eDP cannot MST */
        case 10: REG(0x45404)&=~4u; break;
        case 11: REG(0x64000)|=1u<<7; break; /* idle DDI */
        case 12: REG(0x64000)&=~(1u<<31); break;
        case 13: REG(0x70180)&=~(1u<<31); break;
        case 14: REG(0x70180)=UINT32_MAX; break;
        case 15: REG(0x70180)^=1u<<24; break; /* wrong format */
        case 16: REG(0x70180)|=1u<<10; break; /* tiled, no linear match */
        case 17: REG(0x70180)|=1u<<15; break; /* compression */
        case 18: REG(0x70180)|=1u<<20; break; /* RGBX order */
        case 19: REG(0x70180)|=1u<<8; break; /* reflected */
        case 20: REG(0x70180)|=1; break; /* rotated */
        case 21: REG(0x70188)=7680; break; /* bytes are not stride units */
        case 22: REG(0x70188)=119; break;
        case 23: REG(0x70190)^=1; break;
        case 24: REG(0x6001c)^=1; break;
        case 25: REG(0x6f000)^=1; break;
        case 26: REG(0x6f00c)^=1; break;
        case 27: REG(0x7019c)=0x1000; break; /* queued/live differ */
        case 28: REG(0x7019c)=REG(0x701ac)=0x1000; break; /* not GOP */
        case 29: REG(0x701a4)=1; break;
        case 30: REG(0x7018c)=1; break;
        case 31: REG(0x7f008)|=1u<<21; break; /* interlaced */
        case 32: igfx.gop_in_aperture=false; break;
        case 33: igfx.gop_sample_present=false; break;
        case 34: igfx.memory_valid=false; break;
        case 35: REG(0x6f000)=0x077f077f; break; /* no horizontal blank */
        case 36: REG(0x6f00c)=0x04370437; break;
        }
        CHECK(igfx_find_active_scanout()==-1);
        CHECK(igfx.active_pipe==-1 && igfx.active_transcoder==-1);
    }
    for (unsigned pipe=1;pipe<3;pipe++) {
        scanout_case(3,pipe); REG(0x45404)&=~(1u<<30);
        CHECK(igfx_find_active_scanout()==-1); /* eDP B/C still needs PG2 */
    }
    for (unsigned t=0;t<3;t++) {
        scanout_case(t,t); REG(0x45404)&=~(1u<<30);
        CHECK(igfx_find_active_scanout()==-1);
        scanout_case(t,t); REG(0x60400+t*0x1000)&=~(7u<<28);
        CHECK(igfx_find_active_scanout()==-1); /* no connected DDI */
    }
    const uint32_t changed[]={0x45404,0x42000,0x7f008,0x6f400,0x64000,
        0x70180,0x6f000,0x6f00c,0x6001c,0x70188,0x70190,0x7019c,0x701ac,0x701a4,0x7018c};
    for (unsigned i=0;i<sizeof(changed)/sizeof(changed[0]);i++) {
        scanout_case(3,0); change_register=changed[i]; change_mask=1;
        change_on_read=i<2 ? 5 : 2; /* first 3 gated transcoders read PG0 only */
        CHECK(igfx_find_active_scanout()==-1);
    }
    scanout_case(3,0);
    REG(0x70008)=0xc0000000; REG(0x60400)=0x92000006; REG(0x64100)=0x80000006;
    REG(0x60000)=REG(0x6f000); REG(0x6000c)=REG(0x6f00c);
    CHECK(igfx_find_active_scanout()==-1); /* ambiguous duplicate source */
    scanout_case(3,0); igfx.gop_ggtt_offset=0x40000;
    REG(0x7019c)=0x40008; REG(0x701ac)=0x40020;
    CHECK(igfx_find_active_scanout()==0 && igfx.plane_surf_live==0x40020);
    printf("Intel scanout: %u checks, 0 failures (read-only register vectors)\n",checks);
    return 0;
}
