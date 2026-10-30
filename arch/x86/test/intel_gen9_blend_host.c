#define main primitive_regression_main
#include "intel_rcs_primitive_host.c"
#undef main
#include "../drivers/intel_gen9_blend.h"

static void blend_cases(void)
{
    static const unsigned factors[]={1,2,3,4,5,6,7,8,9,10,11,14,15};
    intel_gen9_blend_state_t state,sentinel;
    memset(&sentinel,0xa5,sizeof(sentinel));
    intel_umd_blend_t blend={INTEL_UMD_BLEND_VALID|0xf1,5|(6<<5)|(1<<10),5|(6<<5)|(1<<10),0x80402010};
    CHECK(intel_gen9_blend_pack(&blend,&state));
    CHECK(state.header==0x40000000 && state.entry[0]==0x8e607300 && state.entry[1]==3);
    CHECK(state.ps==0x6398e680 && state.format==192);
    for (unsigned component=0;component<256;component++) {
        float f=(float)component/255.0f;uint32_t bits;memcpy(&bits,&f,4);
        CHECK(intel_gen9_blend_unorm8(component)==bits);
    }
    for (unsigned mask=0;mask<16;mask++) for (unsigned op=1;op<=5;op++)
    for (unsigned i=0;i<13;i++) for (unsigned j=0;j<13;j++) {
        blend.flags=INTEL_UMD_BLEND_VALID|1|(mask<<4);
        blend.rgb=factors[i]|(factors[j]<<5)|(op<<10);
        blend.alpha=2|(1<<5)|(1<<10);
        CHECK(intel_gen9_blend_pack(&blend,&state));
        unsigned disabled=state.entry[0]&15;
        CHECK(((disabled>>2)&1)==!(mask&1) && ((disabled>>1)&1)==!(mask&2));
        CHECK((disabled&1)==!(mask&4) && ((disabled>>3)&1)==!(mask&8));
        CHECK(!!(state.ps&(1u<<30))==!!mask && (state.entry[0]&(1u<<31)));
        CHECK(((state.entry[0]>>18)&7)==op-1);
        CHECK(((state.entry[0]>>5)&7)==0 && ((state.entry[0]>>13)&31)==1 && ((state.entry[0]>>8)&31)==17);
        CHECK(((state.ps>>14)&31)==((state.entry[0]>>26)&31));
        CHECK(((state.ps>>9)&31)==((state.entry[0]>>21)&31));
        if (op>=4) CHECK(((state.entry[0]>>26)&31)==1 && ((state.entry[0]>>21)&31)==1);
    }
    for (unsigned mode=0;mode<15;mode++) {
        blend=(intel_umd_blend_t){INTEL_UMD_BLEND_VALID|0xf1,5|(6<<5)|(1<<10),2|(1<<5)|(1<<10),0};
        switch(mode) {
        case 0:blend.flags=0;break;
        case 1:blend.flags|=4;break;
        case 2:blend.flags|=8;break;
        case 3:blend.rgb|=1<<13;break;
        case 4:blend.alpha|=1u<<31;break;
        case 5:blend.rgb&=~31u;break;
        case 6:blend.rgb=(blend.rgb&~31u)|12;break;
        case 7:blend.alpha=(blend.alpha&~31u)|13;break;
        case 8:blend.rgb=(blend.rgb&~31u)|16;break;
        case 9:blend.alpha=(blend.alpha&~31u)|17;break;
        case 10:blend.rgb&=~(7u<<10);break;
        case 11:blend.alpha=(blend.alpha&~(7u<<10))|(6<<10);break;
        case 12:blend.flags|=1<<8;break;
        case 13:blend.rgb&=~(31u<<5);break;
        case 14:blend.alpha&=~(31u<<5);break;
        }
        state=sentinel;CHECK(!intel_gen9_blend_pack(&blend,&state));CHECK(!memcmp(&state,&sentinel,sizeof(state)));
        intel_rcs_primitive_draw_t d=fixture;d.blend=blend;memset(allocation,0xa5,sizeof(allocation));
        CHECK(!intel_rcs_primitive_build(allocation+2,8192,&d,(void *)blob,sizeof(blob)));
        for(unsigned i=0;i<2051;i++)CHECK(allocation[i]==0xa5a5a5a5);
    }
    for (unsigned mode=0;mode<4;mode++) {
        blend=(intel_umd_blend_t){INTEL_UMD_BLEND_VALID|((mode&1)?0xf0:0x51)|((mode&2)?2:0),
            5|(6<<5)|(1<<10),2|(1<<5)|(1<<10),0x80402010};
        CHECK(intel_gen9_blend_pack(&blend,&state));
        intel_rcs_primitive_draw_t d=fixture;d.blend=blend;
        CHECK(intel_rcs_primitive_build(allocation+2,8192,&d,(void *)blob,sizeof(blob)));
        uint32_t *b=allocation+2;
        CHECK(b[0x1200/4]==state.header && b[0x1204/4]==state.entry[0] && b[0x1208/4]==state.entry[1]);
        CHECK(!memcmp(b+0x11c8/4,state.constant,16));
        CHECK(((b[0x1040/4]>>18)&0x1ff)==((mode&2)?233u:192u));
        unsigned found=0;for(unsigned i=0;i<1024;i++)if(b[i]==0x784d0000){CHECK(b[i+1]==state.ps);found++;}
        CHECK(found==1);packet(b,&d);
    }
}
int main(void)
{
    CHECK(primitive_regression_main()==0);unsigned before=checks;blend_cases();
    printf("Intel Gen9 blend encoding: %u checks passed\n",checks-before);return 0;
}
