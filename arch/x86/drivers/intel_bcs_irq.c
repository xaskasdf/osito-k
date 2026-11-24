#include "intel_bcs_irq.h"
#define MASTER 0x44200u
#define ENABLE 0x80000000u
#define IMR 0x44304u
#define IIR 0x44308u
#define IER 0x4430cu
#define RING_IMR 0x220a8u

static bool bcs_irq_write(intel_bcs_irq_t *s,uint32_t reg,uint32_t value,uint32_t mask)
{
    uint32_t readback=0;
    s->fault_reg=reg;
    if (!s->io.mmio.write(s->context,reg,value) ||
        !s->io.mmio.read(s->context,reg,&readback)) return false;
    s->last_value=readback;
    return (readback&mask)==(value&mask);
}
static bool bcs_irq_config(intel_bcs_irq_t *s,uint16_t off,unsigned width,uint32_t value)
{
    uint32_t readback;
    s->fault_reg=0x10000000u|off;
    if (!s->io.cfg_write(s->context,off,width,value) ||
        !s->io.cfg_read(s->context,off&~3u,&readback)) return false;
    readback>>=(off&3)*8; s->last_value=readback;
    uint32_t mask=width==2 ? 0xffffu : UINT32_MAX;
    return (readback&mask)==(value&mask);
}
static void bcs_irq_fail(intel_bcs_irq_t *s)
{
    s->failed=true;
    s->armed=false;
    /* Best effort containment, independent of which access failed. Close
     * must still prove readback before permitting the owner to release state. */
    s->io.mmio.write(s->context,MASTER,0);
    uint32_t ignored;
    s->io.mmio.read(s->context,MASTER,&ignored);
    s->io.mmio.write(s->context,IER,0);
    s->io.mmio.read(s->context,IER,&ignored);
    s->wake(s->wake_context,true);
}
bool intel_bcs_irq_open(intel_bcs_irq_t *s,const intel_bcs_irq_io_t *io,void *ctx,
    uint32_t apic,void (*wake)(void *,bool),void *wake_context)
{
    if (!s || s->self || !io || !io->mmio.read || !io->mmio.write ||
        !io->cfg_read || !io->cfg_write || !wake || apic>255) return false;
    s->open_stage=1;
    uint32_t id,cmd,header,ptr,seen[2]={0};
    if (!io->cfg_read(ctx,0,&id) || id!=0x3e9b8086u ||
        !io->cfg_read(ctx,4,&cmd) || !(cmd&(1u<<20)) ||
        !io->cfg_read(ctx,0x34,&ptr)) return false;
    ptr&=0xff;
    s->open_stage=2;
    for (unsigned n=0;;n++) {
        if (n==48 || ptr<0x40 || ptr>0xfc || (ptr&3) ||
            (seen[ptr/128]&(1u<<((ptr%128)/4)))) return false;
        seen[ptr/128]|=1u<<((ptr%128)/4);
        if (!io->cfg_read(ctx,(uint16_t)ptr,&header)) return false;
        if ((header&0xff)==5) break;
        ptr=(header>>8)&0xff;
    }
    uint16_t control=header>>16;
    s->cap=ptr; s->control=control; s->open_stage=3;
    unsigned data_offset=(control&0x80) ? 12 : 8;
    if ((control&0x101) || ptr+data_offset+2>256) return false;
    uint32_t master,data;
    s->open_stage=4;
    if (!io->mmio.read(ctx,MASTER,&master)) return false;
    s->last_value=master; s->fault_reg=MASTER;
    if (master) return false;
    s->open_stage=5;
    if (!io->mmio.read(ctx,IER,&s->ier) || s->ier) return false;
    s->open_stage=6;
    if (!io->mmio.read(ctx,IMR,&s->imr) ||
        !io->mmio.read(ctx,RING_IMR,&s->ring) ||
        (s->ring&INTEL_BCS_IRQ_BITS)!=INTEL_BCS_IRQ_BITS ||
        !io->cfg_read(ctx,ptr+4,&s->addr_lo) ||
        ((control&0x80) && !io->cfg_read(ctx,ptr+8,&s->addr_hi)) ||
        !io->cfg_read(ctx,ptr+data_offset,&data)) return false;
    s->self=s; s->io=*io; s->context=ctx; s->wake=wake; s->wake_context=wake_context;
    s->cap=ptr; s->control=control; s->command=(uint16_t)cmd;
    s->data_offset=data_offset; s->data=data;
    s->started=true; /* every original captured before the first write */
    s->open_stage=7;
    if (!bcs_irq_write(s,MASTER,0,ENABLE) ||
        !bcs_irq_config(s,4,2,s->command|0x404) ||
        !bcs_irq_config(s,ptr+4,4,0xfee00000u|(apic<<12)) ||
        ((control&0x80) && !bcs_irq_config(s,ptr+8,4,0)) ||
        !bcs_irq_config(s,ptr+data_offset,2,INTEL_BCS_IRQ_VECTOR) ||
        !bcs_irq_config(s,ptr+2,2,(control&~0x71u)|1) ||
        !bcs_irq_write(s,IIR,INTEL_BCS_IRQ_BITS,0) ||
        !bcs_irq_write(s,IIR,INTEL_BCS_IRQ_BITS,0) ||
        !bcs_irq_write(s,RING_IMR,s->ring&~INTEL_BCS_IRQ_BITS,INTEL_BCS_IRQ_BITS) ||
        !bcs_irq_write(s,IMR,s->imr&~INTEL_BCS_IRQ_BITS,INTEL_BCS_IRQ_BITS) ||
        !bcs_irq_write(s,IER,INTEL_BCS_IRQ_BITS,UINT32_MAX)) goto fail;
    s->armed=true;
    if (!bcs_irq_write(s,MASTER,ENABLE,ENABLE)) goto fail;
    s->open_stage=8;
    return true;
fail:
    bcs_irq_fail(s);
    return false;
}
bool intel_bcs_irq_quiesce(intel_bcs_irq_t *s)
{
    if (!s) return false;
    if (!s->started || s->closed) return true;
    if (s->self!=s) return false;
    s->armed=false;
    /* Contain before restoring the PCI address or dropping sink lifetime. */
    return bcs_irq_write(s,MASTER,0,ENABLE) &&
        bcs_irq_write(s,IER,0,UINT32_MAX) &&
        bcs_irq_config(s,s->cap+2,2,s->control&~1u);
}
bool intel_bcs_irq_close(intel_bcs_irq_t *s)
{
    if (!s) return false;
    if (!s->started || s->closed) return true;
    if (!intel_bcs_irq_quiesce(s)) return false;
    bool ok=true;
    ok=bcs_irq_write(s,IMR,s->imr,INTEL_BCS_IRQ_BITS)&&ok;
    ok=bcs_irq_write(s,RING_IMR,s->ring,INTEL_BCS_IRQ_BITS)&&ok;
    ok=bcs_irq_write(s,IIR,INTEL_BCS_IRQ_BITS,0)&&ok;
    ok=bcs_irq_write(s,IIR,INTEL_BCS_IRQ_BITS,0)&&ok;
    ok=bcs_irq_config(s,s->cap+4,4,s->addr_lo)&&ok;
    if (s->control&0x80) ok=bcs_irq_config(s,s->cap+8,4,s->addr_hi)&&ok;
    ok=bcs_irq_config(s,s->cap+s->data_offset,2,s->data)&&ok;
    ok=bcs_irq_config(s,4,2,s->command)&&ok;
    s->closed=ok;
    return ok;
}
void intel_bcs_irq_handle(intel_bcs_irq_t *s)
{
    if (!s || s->self!=s || !s->armed) return;
    s->calls++;
    uint32_t master,bits;
    if (!bcs_irq_write(s,MASTER,0,ENABLE) ||
        !s->io.mmio.read(s->context,MASTER,&master) || (master&~2u) ||
        !s->io.mmio.read(s->context,IIR,&bits)) goto fail;
    bits&=INTEL_BCS_IRQ_BITS;
    if (bits) {
        s->events++;
        if (!bcs_irq_write(s,IIR,bits,0)) goto fail;
    } else s->spurious++;
    if (bits&(1u<<19)) goto fail;
    if (!bcs_irq_write(s,MASTER,ENABLE,ENABLE)) goto fail;
    if (bits) s->wake(s->wake_context,false);
    return;
fail:
    bcs_irq_fail(s);
}
