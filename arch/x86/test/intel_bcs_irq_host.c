#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_bcs_irq.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"IRQ FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static struct {
    uint32_t master,imr,ier,iir,ring,foreign;
    uint32_t cfg[64];
    unsigned accesses,fail_at,notes,faults,writes;
} hw;
static intel_bcs_irq_t irq;
static bool irq_read(void *ctx,uint32_t reg,uint32_t *value)
{
    CHECK(ctx==&hw);
    if (++hw.accesses==hw.fail_at) return false;
    switch (reg) {
    case 0x44200: *value=hw.master|hw.foreign|((hw.iir&hw.ier) ? 2 : 0); break;
    case 0x44304: *value=hw.imr; break;
    case 0x44308: *value=hw.iir; break;
    case 0x4430c: *value=hw.ier; break;
    case 0x220a8: *value=hw.ring; break;
    default: CHECK(0);
    }
    return true;
}
static bool irq_write(void *ctx,uint32_t reg,uint32_t value)
{
    CHECK(ctx==&hw);
    bool failed=++hw.accesses==hw.fail_at;
    hw.writes++;
    switch (reg) {
    case 0x44200: CHECK(!(value&~0x80000000u)); hw.master=value; break;
    case 0x44304: hw.imr=value; break;
    case 0x44308: CHECK(!(value&~INTEL_BCS_IRQ_BITS)); hw.iir&=~value; break;
    case 0x4430c: CHECK(!(value&~INTEL_BCS_IRQ_BITS)); hw.ier=value; break;
    case 0x220a8: hw.ring=value; break;
    default: CHECK(0);
    }
    return !failed; /* writes may reach hardware despite reported failure */
}
static bool irq_cfg_read(void *ctx,uint16_t off,uint32_t *value)
{
    CHECK(ctx==&hw && off<256 && !(off&3));
    if (++hw.accesses==hw.fail_at) return false;
    *value=hw.cfg[off/4]; return true;
}
static bool irq_cfg_write(void *ctx,uint16_t off,unsigned width,uint32_t value)
{
    CHECK(ctx==&hw && off<256 && (width==2 || width==4) && !(off&(width-1)));
    CHECK((off==4 || off==0x92 || off==0x98 || off==0x9c) ? width==2 || off==0x98 : width==4);
    if (off==4 || off==0x92 || off==0x9c) CHECK(width==2);
    bool failed=++hw.accesses==hw.fail_at;
    hw.writes++;
    if (width==4) hw.cfg[off/4]=value;
    else {
        uint32_t shift=(off&2)*8,mask=0xffffu<<shift;
        hw.cfg[off/4]=(hw.cfg[off/4]&~mask)|((value&0xffff)<<shift);
    }
    return !failed;
}
static void irq_notify(void *ctx,bool fault)
{
    CHECK(ctx==&hw);
    hw.notes++;
    if (fault) { hw.faults++; CHECK(!irq.armed && !hw.master); }
}
static const intel_bcs_irq_io_t io={{irq_read,irq_write,NULL,NULL},irq_cfg_read,irq_cfg_write};
static void irq_reset(bool wide)
{
    memset(&hw,0,sizeof(hw)); memset(&irq,0,sizeof(irq));
    hw.imr=hw.ring=UINT32_MAX;
    hw.cfg[0]=0x3e9b8086;
    hw.cfg[1]=0xa0100002|(wide ? 4 : 0); /* cover BME absent/present; preserve W1C */
    hw.cfg[0x34/4]=0x40;
    hw.cfg[0x40/4]=0x00009001;
    hw.cfg[0x90/4]=(wide ? 0x00800000 : 0)|5;
    hw.cfg[0x94/4]=0xfee07000;
    hw.cfg[0x98/4]=wide ? 0 : 0xbeef1234;
    hw.cfg[0x9c/4]=0xbeef1234;
}
static void irq_restored(bool wide)
{
    CHECK(!hw.master && !hw.ier && hw.imr==UINT32_MAX && hw.ring==UINT32_MAX);
    CHECK(hw.cfg[1]==(0xa0100002u|(wide ? 4 : 0)) && hw.cfg[0x94/4]==0xfee07000);
    CHECK(hw.cfg[0x90/4]==((wide ? 0x00800000u : 0)|5));
    CHECK(hw.cfg[0x98/4]==(wide ? 0 : 0xbeef1234));
    CHECK(hw.cfg[0x9c/4]==0xbeef1234);
}
static void irq_tests(void)
{
    for (unsigned wide=0;wide<2;wide++) {
        irq_reset(wide);
        CHECK(intel_bcs_irq_open(&irq,&io,&hw,7,irq_notify,&hw));
        unsigned opening=hw.accesses;
        CHECK(hw.cfg[0x94/4]==0xfee07000 && hw.cfg[1]==0xa0100406);
        CHECK((hw.cfg[(wide?0x9c:0x98)/4]&0xffff)==43);
        hw.iir=1u<<24;
        intel_bcs_irq_handle(&irq);
        CHECK(hw.notes==1 && irq.events==1 && irq.calls==1 && !hw.iir && !irq.failed);
        intel_bcs_irq_handle(&irq);
        CHECK(irq.spurious==1 && hw.notes==1);
        hw.iir=(1u<<24)|1; /* unrelated identity bits are not acknowledged */
        intel_bcs_irq_handle(&irq);
        CHECK(hw.iir==1 && hw.notes==2);
        CHECK(intel_bcs_irq_quiesce(&irq) && !irq.armed && !irq.closed);
        CHECK((hw.cfg[1]&4) && !hw.master && !hw.ier);
        CHECK(intel_bcs_irq_close(&irq)); irq_restored(wide);
        unsigned before=hw.accesses;
        intel_bcs_irq_handle(&irq);
        CHECK(hw.accesses==before && hw.notes==2);
        CHECK(intel_bcs_irq_close(&irq));
        for (unsigned fail=1;fail<=opening;fail++) {
            irq_reset(wide); hw.fail_at=fail;
            CHECK(!intel_bcs_irq_open(&irq,&io,&hw,7,irq_notify,&hw));
            hw.fail_at=0;
            CHECK(intel_bcs_irq_close(&irq)); irq_restored(wide);
        }
    }
    for (unsigned bad=0;bad<6;bad++) {
        irq_reset(false);
        if (bad==0) hw.cfg[0x40/4]=0x4001; /* cycle */
        if (bad==1) hw.cfg[0x34/4]=0xff;
        if (bad==2) hw.cfg[0x90/4]|=1u<<16; /* existing MSI owner */
        if (bad==3) hw.cfg[0x90/4]|=1u<<24; /* PVM unsupported */
        if (bad==4) hw.master=0x80000000;
        if (bad==5) hw.ier=1;
        CHECK(!intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
        CHECK(!irq.started && !hw.writes);
    }
    for (unsigned cause=0;cause<2;cause++) {
        irq_reset(false); CHECK(intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
        if (cause) hw.foreign=1u<<16; else hw.iir=1u<<19;
        intel_bcs_irq_handle(&irq);
        CHECK(irq.failed && !irq.armed && hw.faults==1 && !hw.master);
        CHECK(intel_bcs_irq_close(&irq)); irq_restored(false);
    }
    irq_reset(false); CHECK(intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
    unsigned start=hw.accesses;
    hw.iir=1u<<24;
    intel_bcs_irq_handle(&irq);
    unsigned handling=hw.accesses-start;
    CHECK(intel_bcs_irq_close(&irq));
    for (unsigned fail=1;fail<=handling;fail++) {
        irq_reset(false); CHECK(intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
        hw.iir=1u<<24; hw.fail_at=hw.accesses+fail;
        intel_bcs_irq_handle(&irq);
        CHECK(irq.failed && !irq.armed && hw.faults==1 && !hw.master);
        hw.fail_at=0;
        CHECK(intel_bcs_irq_close(&irq)); irq_restored(false);
    }
    irq_reset(false); CHECK(intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
    unsigned before=hw.accesses;
    CHECK(intel_bcs_irq_close(&irq));
    unsigned closing=hw.accesses-before;
    for (unsigned fail=1;fail<=closing;fail++) {
        irq_reset(false); CHECK(intel_bcs_irq_open(&irq,&io,&hw,0,irq_notify,&hw));
        hw.fail_at=hw.accesses+fail;
        CHECK(!intel_bcs_irq_close(&irq) && !irq.closed);
        hw.fail_at=0;
        CHECK(intel_bcs_irq_close(&irq)); irq_restored(false);
    }
}
int main(void)
{
    irq_tests();
    printf("Intel BCS IRQ: %u checks passed\n",checks);
    return 0;
}
