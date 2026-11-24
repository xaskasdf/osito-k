/* Native GGTT port and opt-in physical GPU qualification. */
#include "../include/types.h"
#include "../include/paging.h"
#include "../include/pci.h"
#include "../include/dma_diag.h"
#include "intel_gfx.h"
#include "intel_gt.h"
#include "intel_ggtt_space.h"
#include "intel_bcs.h"
#include "intel_bcs_queue.h"
#include "intel_bcs_worker.h"
#include "intel_bcs_irq.h"
#include "intel_bcs_context.h"
#include "intel_rcs.h"
#include "intel_rcs_draw.h"
#include "intel_rcs_program_test.h"
#include "intel_rcs_sampled_test.h"
#include "intel_rcs_vertex_test.h"
#include "intel_gpu_memory.h"
#include "intel_gpu_fences.h"
#include "intel_gpu_executor.h"
#include "intel_gpu_bcs_owner.h"
#include "intel_gfx_capture.h"
#include "intel_ppgtt.h"
#include "../kernel/smp_work.h"
#include "../win32/ntsyscall.h"
#include "../win32/ntprocess.h"

extern int paging_map_mmio(uint64_t, uint64_t);
extern void *mem_alloc_pages(uint64_t);
extern void mem_free_pages(void *, uint64_t);
extern uint64_t idt_get_monotonic_ns(void);
extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);
extern void serial_puthex(uint64_t, int);

static struct {
    const intel_gfx_state_t *device;
    volatile uint8_t *mmio;
    intel_gt_state_t gt;
    intel_ggtt_space_t space;
    intel_ggtt_binding_t binding;
    uint8_t bitmap[64];
    uint64_t original, ram_phys;
    uint32_t first, last_write;
    bool wrote, attempted;
} native_memory;

static bool gt_hw_read(void *ctx, uint32_t reg, uint32_t *value)
{
    (void)ctx;
    if ((reg & 3) || reg >= 0x200000) return false;
    *value = mmio_read32((volatile void *)(native_memory.mmio + reg));
    /* Some writable masks legitimately read as all ones. Register-specific
     * validity checks belong to the GT/BCS state machine. */
    return true;
}

static bool gt_hw_write(void *ctx, uint32_t reg, uint32_t value)
{
    (void)ctx;
    if ((reg & 3) || reg >= 0x200000) return false;
    mmio_write32((volatile void *)(native_memory.mmio + reg), value);
    return true;
}

static uint64_t gt_hw_now(void *ctx)
{
    (void)ctx;
    return idt_get_monotonic_ns();
}

static void gt_hw_relax(void *ctx)
{
    (void)ctx;
    __asm__ volatile ("pause" ::: "memory");
}

static bool ggtt_hw_read(void *ctx, uint32_t page, uint64_t *pte)
{
    (void)ctx;
    if (page < native_memory.first || page - native_memory.first >= 512)
        return false;
    uint32_t reg;
    if (!intel_ggtt_pte_offset(&native_memory.device->memory,
                              (uint64_t)page << 12, &reg)) return false;
    *pte = mmio_read64((volatile void *)(native_memory.mmio + reg));
    return *pte != UINT64_MAX;
}

static bool ggtt_hw_write(void *ctx, uint32_t page, uint64_t pte)
{
    (void)ctx;
    if (!native_memory.gt.ready || page < native_memory.first ||
        page - native_memory.first >= 512) return false;
    uint32_t reg;
    if (!intel_ggtt_pte_offset(&native_memory.device->memory,
                              (uint64_t)page << 12, &reg)) return false;
    /* UC MMIO table aperture, never the stolen backing RAM. One aligned
     * qword store; keep the page alive until restore/readback completes. */
    mmio_write64((volatile void *)(native_memory.mmio + reg), pte);
    native_memory.last_write = page;
    native_memory.wrote = true;
    return true;
}

static bool ggtt_hw_sync(void *ctx)
{
    if (!native_memory.gt.ready) return false;
    __asm__ volatile ("mfence" ::: "memory");
    uint64_t value;
    /* Gen9 snoops UC writes to GTTMMADR to invalidate translations (PRM
     * GTTMMADR). A posting read completes these writes. Bind only with GT
     * stopped; the BCS diagnostic resets/retires its engine before unbind.
     * This is not a live-engine remapping or GuC/WC-table contract. */
    return !native_memory.wrote ||
        ggtt_hw_read(ctx, native_memory.last_write, &value);
}

static bool ggtt_mapping_uc(uint64_t phys, uint64_t bytes)
{
    uint64_t cr3 = paging_get_kernel_cr3();
    for (uint64_t off = 0; off < bytes; off += 4096) {
        for (unsigned alias = 0; alias < 2; alias++) {
            uint64_t va = phys + off + (alias ? KERNEL_VBASE : 0);
            uint64_t flags, size;
            if (paging_translate_in_cr3(cr3, va) != phys + off ||
                paging_query_mapping_in_cr3(cr3, va, &flags, &size) ||
                (flags & 0x1b) != 0x1b || (flags & 4) ||
                (flags & (size == 4096 ? 0x80 : 0x1000))) return false;
            __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
        }
    }
    return true;
}

static void ggtt_test_cache_flush(void *ram, unsigned pages)
{
    for (unsigned off = 0; off < pages * 4096; off += 64)
        __asm__ volatile ("clflush (%0)" :: "r"((uint8_t *)ram + off) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

#if INTEL_BCS_DIAGNOSTIC
static bool ggtt_test_ram_valid(uint64_t phys, unsigned pages)
{
    const intel_ggtt_layout_t *m = &native_memory.device->memory;
    uint64_t bytes = (uint64_t)pages * 4096;
    if (!phys || !pages || (phys & 4095) || bytes >= INTEL_GGTT_PHYS_LIMIT ||
        phys > INTEL_GGTT_PHYS_LIMIT - bytes ||
        (phys < m->stolen_base + m->stolen_bytes && phys + bytes > m->table_stolen_base) ||
        (phys < m->mmio_base + INTEL_GGTT_BAR_BYTES && phys + bytes > m->mmio_base) ||
        (phys < m->aperture_base + m->aperture_bytes && phys + bytes > m->aperture_base))
        return false;
    uint64_t cr3 = paging_get_kernel_cr3();
    for (unsigned i = 0; i < pages; i++) {
        uint64_t va = (uint64_t)PHYS_TO_VIRT(phys + 4096*i), flags, size;
        if (paging_translate_in_cr3(cr3, va) != phys + 4096*i ||
            paging_query_mapping_in_cr3(cr3, va, &flags, &size) ||
            (flags & 0x1b) != 3 || (flags & 4) ||
            (flags & (size == 4096 ? 0x80 : 0x1000))) return false;
    }
    return true;
}

typedef struct {
    unsigned allocated_pages,freed_pages,guard_failures;
} gpu_ram_accounting_t;
typedef struct {
    intel_bcs_context_t native;
    intel_gpu_buffer_t *source, *destination;
    intel_gpu_cpu_access_t cpu_probe[2];
    uint32_t device, resource[2];
    uint64_t table_hash, ring_hash, token;
    uint32_t seed, checked, pixels, saved_head, saved_tail, saved_ctl;
} bcs_test_context_t;
static struct {
    bcs_test_context_t context[2];
    intel_bcs_state_t engine;
    intel_ppgtt_pat_t pat;
    intel_bcs_queue_t queue;
    unsigned admission[6]; /* persistent across diagnostic error cleanup */
    intel_bcs_worker_t worker;
    intel_bcs_irq_t irq;
    sched_signal_t client_signal;
    unsigned service_idle_checks, service_reclaims;
    unsigned cancelled, deferred, remote_steps, local_steps;
    unsigned wake_calls, wake_ap, wake_results, wake_errors;
    intel_gpu_memory_t resources;
    intel_gpu_buffer_t hwsp;
    intel_ggtt_binding_t hwsp_binding;
    uint64_t hwsp_original;
    bool hwsp_pinned;
    gpu_ram_accounting_t ram;
    unsigned resource_deferred;
    unsigned cpu_handoffs;
} bcs_memory;
static struct {
    intel_cs_state_t engine;
    intel_bcs_queue_t queue;
    intel_bcs_context_t native;
    intel_gpu_buffer_t buffers[9]; /* context, ring, HWSP, tables, batch, src, dst, program, vertices */
    intel_gpu_buffer_t alternate[3]; /* replacement target, program, vertices */
    intel_ppgtt_pat_t pat;
    intel_rcs_cache_t cache;
    intel_ggtt_binding_t bindings[3];
    uint64_t originals[24];
    gpu_ram_accounting_t ram;
    uint64_t descriptor,token;
    uint32_t pins,buffer_pins,completed,draws,pixels,tail,cpu,program_count,program_version;
    const uint8_t *program_cases;
    bool passed;
} rcs_memory;
static unsigned bcs_service_busy;
static struct {
    intel_gpu_buffer_t buffers[4]; /* context, ring, HWSP, PPGTT */
    intel_ggtt_binding_t bindings[3];
    uint64_t originals[4],descriptor,expected,initial;
    intel_ppgtt_pat_t pat;
    intel_cs_state_t engine;
    gpu_ram_accounting_t ram;
    uint32_t pins,buffer_pins,completed,copied,observed,torn,cpu;
    intel_gpu_memory_t memory;
    intel_gpu_fences_t fences;
    intel_gpu_fence_lease_t leases[2],*access;
    uint64_t cookies[2],addresses[2];
    uint32_t pid,device,quota_checks;
    uint32_t mapped_views,retired_views,readonly_views,view_matches,lease_checks;
    bool passed;
} fence_memory;
static intel_gpu_bcs_owner_t capture_owner;
static gpu_ram_accounting_t capture_ram;
static const intel_bcs_copy_t bcs_copy[2] = {
    {0x1235ff000ULL,8192,0x1403ff000ULL,8192,256,256,37,23,0,0,0,0},
    {0x1235ff000ULL,21105,0x1403ff000ULL,31565,512,768,113,41,0,0,0,0}
};

static bool gpu_ram_allocate(void *ctx, uint32_t pages, uint64_t *dma, void **cpu)
{
    gpu_ram_accounting_t *ram=ctx ? ctx : &bcs_memory.ram;
    uint64_t phys=(uint64_t)mem_alloc_pages((uint64_t)pages+2);
    if (!phys) return false;
    ram->allocated_pages+=pages+2;
    *dma=phys+4096; *cpu=PHYS_TO_VIRT(*dma);
    return true;
}

static bool gpu_ram_validate(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    (void)ctx;
    if (dma<4096 || cpu!=PHYS_TO_VIRT(dma) || !ggtt_test_ram_valid(dma-4096,pages+2)) return false;
    uint32_t *ram=PHYS_TO_VIRT(dma-4096);
    for (unsigned i=0;i<1024;i++) ram[i]=ram[(pages+1)*1024+i]=0xa5c39000u^i;
    return true;
}

static bool gpu_ram_sync(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    gpu_ram_accounting_t *counts=ctx ? ctx : &bcs_memory.ram;
    (void)cpu;
    uint32_t *ram=PHYS_TO_VIRT(dma-4096);
    ggtt_test_cache_flush(ram,pages+2);
    for (unsigned i=0;i<1024;i++)
        if (ram[i]!=(0xa5c39000u^i) || ram[(pages+1)*1024+i]!=(0xa5c39000u^i)) {
            counts->guard_failures++; return false;
        }
    return true;
}

static bool gpu_ram_release(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    if (!gpu_ram_sync(ctx,dma,cpu,pages)) return false;
    mem_free_pages((void *)(dma-4096),(uint64_t)pages+2);
    gpu_ram_accounting_t *ram=ctx ? ctx : &bcs_memory.ram;
    ram->freed_pages+=pages+2;
    return true;
}
static bool gpu_ram_cpu_physical(void *ctx,uint64_t dma,void *cpu,uint32_t pages,
    uint64_t *physical)
{
    (void)ctx;
    /* This allocator admits only identity-DMA PMM RAM. Recheck the actual CPU
     * mapping and physical exclusions; do not extend that identity assumption
     * to another allocator or to future IOMMU-translated DMA allocations. */
    if (!physical || dma<4096 || cpu!=PHYS_TO_VIRT(dma) ||
        !ggtt_test_ram_valid(dma-4096,pages+2)) return false;
    *physical=dma;
    return true;
}
static const intel_gpu_allocator_t gpu_ram={gpu_ram_allocate,gpu_ram_validate,
    gpu_ram_release,gpu_ram_sync,gpu_ram_cpu_physical};

static void fence_test_wake(void *context)
{
    /* Diagnostic owns the idle GT synchronously; it collects explicitly. */
    (void)context;
}
static bool fence_test_token(void *ctx,uint64_t *value)
{
    (void)ctx;
    /* Ordinary WB CPU loads, deliberately no CLFLUSH or aperture alias for
     * the fence. The separate HWSP token still proves command retirement. */
    uint64_t sample=__atomic_load_n((uint64_t *)fence_memory.access->cpu,__ATOMIC_ACQUIRE);
    if (sample==fence_memory.expected) fence_memory.observed++;
    else if (sample!=fence_memory.initial) fence_memory.torn++;
    uint64_t aperture=native_memory.device->bar2_phys+fence_memory.bindings[0].gpu_address+INTEL_BCS_TOKEN_OFFSET;
    *value=mmio_read64(PHYS_TO_VIRT(aperture)); return true;
}
static void fence_test_work(void *argument,void *result)
{
    (void)argument; (void)result;
    extern uint32_t smp_current_cpu(void);
    fence_memory.cpu=smp_current_cpu();
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    for (unsigned object=0;object<2;object++) {
    fence_memory.access=&fence_memory.leases[object];
    uint64_t *fence=fence_memory.access->cpu;
    uint64_t gpu=0x400000000ULL+(uint64_t)object*4096;
    for (unsigned round=0;round<32;round++) {
        if (fence_memory.engine.referenced && !fence_memory.engine.retired) return;
        fence_memory.engine=(intel_cs_state_t){0};
        fence_memory.initial=0x1122334400000000ULL|round;
        fence_memory.expected=0xa5c3900000000000ULL|((uint64_t)round<<32)|(0xfffffff0u-round);
        __atomic_store_n(fence,fence_memory.initial,__ATOMIC_RELEASE);
        if (__atomic_load_n(fence,__ATOMIC_ACQUIRE)!=fence_memory.initial) return;
        uint64_t token=0x66656e6300000000ULL+round+1;
        if (!intel_bcs_build_fence(fence_memory.buffers[0].cpu,8192,fence_memory.buffers[1].cpu,4096,
            fence_memory.bindings[0].gpu_address,fence_memory.bindings[1].gpu_address,
            fence_memory.buffers[3].dma,0x7100+round,token,gpu,
            fence_memory.expected,&fence_memory.descriptor)) return;
        memset((uint8_t *)fence_memory.buffers[2].cpu+0x370,0xff,48);
        for (unsigned i=0;i<4;i++)
            if (intel_gpu_buffer_sync(&fence_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) return;
        /* Fence cachelines remain dirty/loaded in CPU caches at submission. */
        if (intel_bcs_run(&io,NULL,fence_test_token,fence_memory.descriptor,
            fence_memory.bindings[2].gpu_address,token,INTEL_BCS_FENCE_TAIL,&fence_memory.engine)!=INTEL_BCS_OK)
            return;
        uint64_t deadline=idt_get_monotonic_ns()+100000000ULL;
        while (__atomic_load_n(fence,__ATOMIC_ACQUIRE)!=fence_memory.expected) {
            if (idt_get_monotonic_ns()>=deadline) return;
            __asm__ volatile("pause" ::: "memory");
        }
        fence_memory.completed++;
        /* Reverse direction: leave the source dirty in CPU WB cache, then
         * copy two pixels on BCS to another aligned region in the same owned
         * coherent page. No overlapping bytes and no CPU cache handoff. */
        __atomic_store_n(fence+24,fence_memory.initial,__ATOMIC_RELEASE);
        __atomic_store_n(fence+32,~fence_memory.initial,__ATOMIC_RELEASE);
        intel_bcs_copy_t copy={.source=gpu+0xc0,.source_bytes=8,
            .destination=gpu+0x100,.destination_bytes=8,
            .source_pitch=8,.destination_pitch=8,.width=2,.height=1};
        fence_memory.engine=(intel_cs_state_t){0};token+=0x10000;
        if (!intel_bcs_build_copy(fence_memory.buffers[0].cpu,8192,fence_memory.buffers[1].cpu,4096,
            fence_memory.bindings[0].gpu_address,fence_memory.bindings[1].gpu_address,
            fence_memory.buffers[3].dma,0x7300+round,token,&copy,&fence_memory.descriptor)) return;
        memset((uint8_t *)fence_memory.buffers[2].cpu+0x370,0xff,48);
        for (unsigned i=0;i<4;i++)
            if (intel_gpu_buffer_sync(&fence_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) return;
        if (intel_bcs_run(&io,NULL,fence_test_token,fence_memory.descriptor,
            fence_memory.bindings[2].gpu_address,token,INTEL_BCS_COPY_TAIL,&fence_memory.engine)!=INTEL_BCS_OK)
            return;
        if (__atomic_load_n(fence+32,__ATOMIC_ACQUIRE)!=fence_memory.initial ||
            __atomic_load_n(fence+24,__ATOMIC_ACQUIRE)!=fence_memory.initial ||
            __atomic_load_n(fence,__ATOMIC_ACQUIRE)!=fence_memory.expected) return;
        for (unsigned i=0;i<512;i++) if (i!=0 && i!=24 && i!=32 && fence[i]!=(0x13579bdf24680000ULL^i)) return;
        fence_memory.copied++;
    }
    }
    fence_memory.passed=true;
}
static struct {
    intel_bcs_context_t context;
    intel_bcs_queue_t queue;
    intel_bcs_state_t engine;
    intel_gpu_buffer_t surfaces[2];
    intel_ppgtt_pat_t pat;
    intel_gpu_fence_mapping_t mappings[2];
    uint64_t source,destination;
    unsigned copied;
} fence_context;

static bool fence_context_token(void *opaque,uint64_t *token)
{
    intel_bcs_context_t *c=opaque;
    if (c!=&fence_context.context || !c->ggtt_pinned) return false;
    *token=mmio_read64(PHYS_TO_VIRT(native_memory.device->bar2_phys+
        c->binding.gpu_address+INTEL_BCS_TOKEN_OFFSET));
    return true;
}
static bool fence_context_prepare(void *opaque,const intel_bcs_copy_t *copy,
    uint64_t token,uint64_t *descriptor,uint32_t *tail)
{
    intel_bcs_context_t *c=opaque;
    /* The diagnostic retains both coherent mappings for each whole queued
     * copy. These bytes intentionally have simultaneous WB CPU/GPU access. */
    if (c!=&fence_context.context || !c->ready ||
        copy->source!=fence_context.source || copy->destination!=fence_context.destination ||
        copy->source_bytes!=8 || copy->destination_bytes!=8) return false;
    unsigned mappings=0;
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        intel_bcs_coherent_range_t *m=&c->coherent[i];
        if (m->mapped && m->resident && m->reference.self==&m->reference &&
            (m->gpu==fence_memory.leases[0].gpu || m->gpu==fence_memory.leases[1].gpu)) mappings++;
    }
    if (mappings!=2) return false;
    uint32_t *state=c->state.cpu,*ring=state+2048;
    if (c->prepared) {
        if (intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK ||
            !intel_bcs_append_copy(state,8192,ring,4096,c->tail,token,copy,&c->tail)) return false;
    } else {
        if (!intel_bcs_build_copy(state,8192,ring,4096,c->binding.gpu_address,
            c->binding.gpu_address+8192,c->tables.dma,c->hardware_id,token,copy,&c->descriptor)) return false;
        c->tail=INTEL_BCS_COPY_TAIL;c->prepared=true;
    }
    if (intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK) return false;
    *descriptor=c->descriptor;*tail=c->tail;return true;
}
static bool fence_context_test(void)
{
    /* Original fence leases and PAT4 remain owned by the outer diagnostic.
     * This exercises production context mapping/retirement with real DMA. */
    if (fence_context.context.state.live || fence_context.context.tables.live ||
        fence_context.surfaces[0].live || fence_context.surfaces[1].live ||
        fence_context.mappings[0].self || fence_context.mappings[1].self ||
        (fence_context.engine.started && !fence_context.engine.retired) || fence_context.pat.changed)
        return false;
    memset(&fence_context,0,sizeof(fence_context));
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    const intel_bcs_queue_ops_t ops={.prepare=fence_context_prepare,.token_read=fence_context_token};
    intel_bcs_context_t *c=&fence_context.context;
    intel_bcs_queue_t *q=&fence_context.queue;
    bool passed=false;
    for (unsigned i=0;i<2;i++) if (intel_gpu_buffer_create(&fence_context.surfaces[i],&gpu_ram,
        &fence_memory.ram,fence_memory.pid,4096)!=INTEL_GPU_BUFFER_OK) goto cleanup;
    if (!intel_bcs_context_reserve(c,&native_memory.space,&gpu_ram,&fence_memory.ram,1,0x7500,
        0x10000000,0x20000000,8,native_memory.device->memory.aperture_bytes) ||
        intel_bcs_context_activate(c,fence_memory.pid,&fence_context.surfaces[0],&fence_context.surfaces[1])!=INTEL_BCS_OK ||
        !intel_ppgtt_pat_acquire(&io,NULL,&fence_context.pat)) goto cleanup;
    native_memory.gt.ready=false;
    if (intel_bcs_enable(&io,NULL,fence_memory.bindings[2].gpu_address,&fence_context.engine)!=INTEL_BCS_OK ||
        intel_bcs_queue_init(q,&fence_context.engine,&io,NULL,&ops,0x636f686500000001ULL)!=INTEL_BCS_OK ||
        !intel_bcs_context_attach(c,q,c) || intel_bcs_queue_publish(q)!=INTEL_BCS_OK) goto cleanup;
    c->epoch=fence_memory.leases[0].epoch;
    for (unsigned i=0;i<2;i++) if (intel_gpu_fence_map(&fence_memory.leases[i],c,
        &fence_memory.memory,fence_memory.device,&fence_memory.pat,&fence_context.mappings[i])!=INTEL_BCS_OK) goto cleanup;
    for (unsigned round=0;round<3;round++) {
        if (round==1 && intel_bcs_context_rebind(c,&fence_context.surfaces[1],&fence_context.surfaces[0])!=INTEL_BCS_OK)
            goto cleanup;
        if (round==2) {
            for (unsigned i=0;i<2;i++) if (intel_gpu_fence_unmap(&fence_context.mappings[i])!=INTEL_BCS_OK)
                goto cleanup;
            for (unsigned i=0;i<2;i++) if (intel_gpu_fence_map(&fence_memory.leases[i],c,
                &fence_memory.memory,fence_memory.device,&fence_memory.pat,&fence_context.mappings[i])!=INTEL_BCS_OK)
                goto cleanup;
        }
        uint64_t *source=(uint64_t *)fence_memory.leases[round==2?1:0].cpu+64;
        uint64_t *destination=(uint64_t *)fence_memory.leases[round==2?0:1].cpu+96;
        uint64_t value=0x11223344abc00000ULL+round,job=0;
        __atomic_store_n(source,value,__ATOMIC_RELEASE);
        __atomic_store_n(destination,~value,__ATOMIC_RELEASE);
        fence_context.source=fence_memory.leases[round==2?1:0].gpu+0x200;
        fence_context.destination=fence_memory.leases[round==2?0:1].gpu+0x300;
        intel_bcs_copy_t copy={.source=fence_context.source,.destination=fence_context.destination,
            .source_bytes=8,.destination_bytes=8,.source_pitch=8,.destination_pitch=8,.width=2,.height=1};
        if (intel_bcs_queue_enqueue(q,c->owner,c->queue_id,&copy,&job)!=INTEL_BCS_OK) goto cleanup;
        uint64_t deadline=idt_get_monotonic_ns()+1000000000ULL;
        intel_bcs_job_result_t result={0};
        for (;;) {
            enum intel_bcs_status status=intel_bcs_queue_step(q);
            if (status!=INTEL_BCS_OK && status!=INTEL_BCS_PENDING && status!=INTEL_BCS_BUSY) goto cleanup;
            if (intel_bcs_queue_result(q,c->owner,job,false,&result)!=INTEL_BCS_OK) goto cleanup;
            if (result.phase>=INTEL_BCS_JOB_COMPLETE) break;
            if (idt_get_monotonic_ns()>=deadline) goto cleanup;
        }
        if (result.phase!=INTEL_BCS_JOB_COMPLETE || result.error ||
            intel_bcs_queue_result(q,c->owner,job,true,&result)!=INTEL_BCS_OK ||
            __atomic_load_n(destination,__ATOMIC_ACQUIRE)!=value ||
            __atomic_load_n(source,__ATOMIC_ACQUIRE)!=value) goto cleanup;
        fence_context.copied++;
    }
    for (unsigned i=0;i<2;i++) if (intel_gpu_fence_unmap(&fence_context.mappings[i])!=INTEL_BCS_OK)
        goto cleanup;
    passed=c->rebindings==9 && fence_context.engine.tlb_invalidations==9;
cleanup:
    if (q->initialized) {
        if (intel_bcs_queue_shutdown(q)!=INTEL_BCS_OK) return false;
    } else if (fence_context.engine.started && intel_bcs_close(&io,NULL,&fence_context.engine)!=INTEL_BCS_OK)
        return false;
    native_memory.gt.ready=true;
    if (!intel_bcs_context_destroy(c) ||
        (q->registered && intel_bcs_queue_unpublish(q)!=INTEL_BCS_OK)) return false;
    for (unsigned i=0;i<2;i++) if (intel_gpu_fence_unmap(&fence_context.mappings[i])!=INTEL_BCS_OK) return false;
    for (unsigned i=0;i<2;i++) if (fence_context.surfaces[i].started &&
        intel_gpu_buffer_close(&fence_context.surfaces[i],fence_memory.pid)!=INTEL_GPU_BUFFER_OK) return false;
    if (fence_context.pat.started && !intel_ppgtt_pat_restore(&io,NULL,&fence_context.pat)) {
        native_memory.gt.ready=false;return false;
    }
    serial_puts("[IGFX] context coherent passed=");serial_putdec(passed);
    serial_puts(" copies=");serial_putdec(fence_context.copied);
    serial_puts(" updates=");serial_putdec(c->rebindings);
    serial_puts(" tlb=");serial_putdec(fence_context.engine.tlb_invalidations);
    serial_puts(" retired=");serial_putdec(fence_context.engine.retired);
    serial_puts(" managed=1");
    serial_puts("\n");return passed;
}

static bool intel_gfx_fence_test(void)
{
    /* Called by the BSP diagnostic while bcs_service_busy owns the idle GT. */
    if (!native_memory.gt.ready || fence_memory.pins || fence_memory.buffer_pins || fence_memory.leases[0].self || fence_memory.leases[1].self ||
        (fence_memory.fences.self && !intel_gpu_fences_empty(&fence_memory.fences)) ||
        fence_memory.memory.pages || fence_memory.memory.registered ||
        (fence_memory.engine.referenced && !fence_memory.engine.retired) || fence_memory.pat.changed)
        return false;
    for (unsigned i=0;i<4;i++) if (fence_memory.buffers[i].live) return false;
    memset(&fence_memory,0,sizeof(fence_memory));
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    const uint32_t pages[4]={2,1,1,4};
    const char *stage="owned memory";
    bool passed=false;uint64_t before=14695981039346656037ULL,after=before;
    for (uint64_t off=0;off<native_memory.device->memory.ggtt_bytes;off+=8)
        before=(before^mmio_read64((volatile void *)(native_memory.mmio+INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
    for (unsigned i=0;i<4;i++) {
        if (intel_gpu_buffer_create(&fence_memory.buffers[i],&gpu_ram,&fence_memory.ram,1,(uint64_t)pages[i]*4096)!=INTEL_GPU_BUFFER_OK)
            goto release;
        if (intel_gpu_buffer_pin(&fence_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto release;
        fence_memory.buffer_pins|=1u<<i;
    }
    stage="owned coherent fence objects";
    if (!intel_gpu_memory_init(&fence_memory.memory,&gpu_ram,&fence_memory.ram,2,2) ||
        intel_gpu_memory_publish(&fence_memory.memory)!=INTEL_GPU_BUFFER_OK ||
        !intel_gpu_fences_init(&fence_memory.fences,&fence_memory.memory,fence_test_wake,NULL) ||
        nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&fence_memory.pid)!=STATUS_SUCCESS ||
        nt_process_open_gpu_device_retained(fence_memory.pid,&fence_memory.memory,&fence_memory.device)!=STATUS_SUCCESS)
        goto release;
    intel_ppgtt_range_t ranges[2];
    for (unsigned i=0;i<2;i++) {
        if (intel_gpu_fence_create(&fence_memory.fences,fence_memory.pid,fence_memory.device,7+i,i==0,
            &fence_memory.cookies[i],&fence_memory.addresses[i])!=STATUS_SUCCESS) goto release;
        fence_memory.mapped_views++;
        intel_gpu_fence_lease_t *lease=&fence_memory.leases[i];
        if (intel_gpu_fence_acquire(&fence_memory.fences,fence_memory.pid,fence_memory.device,
            fence_memory.cookies[i],lease)!=STATUS_SUCCESS || *(uint64_t *)lease->cpu!=7+i) goto release;
        ranges[i]=(intel_ppgtt_range_t){0x400000000ULL+(uint64_t)i*4096,lease->dma,4096,true};
        for (unsigned qword=1;qword<512;qword++) ((uint64_t *)lease->cpu)[qword]=0x13579bdf24680000ULL^qword;
    }
    uint64_t extra=0xcafef00d,extra_address=0xcafef00d;
    if (fence_memory.memory.pages!=2 ||
        intel_gpu_fence_create(&fence_memory.fences,fence_memory.pid,fence_memory.device,0,TRUE,
            &extra,&extra_address)!=STATUS_NO_MEMORY || extra!=0xcafef00d || extra_address!=0xcafef00d) goto release;
    fence_memory.quota_checks++;
    uint32_t used=0;
    if (!intel_ppgtt_build_ranges_coherent(fence_memory.buffers[3].cpu,16384,fence_memory.buffers[3].dma,
        ranges,2,3,&used)) goto release;
    stage="GGTT bindings";
    for (unsigned i=0,original=0;i<3;i++) {
        uint64_t addresses[2]={fence_memory.buffers[i].dma,fence_memory.buffers[i].dma+4096};
        if (intel_ggtt_bind(&native_memory.space,&fence_memory.bindings[i],addresses,pages[i],1,
            native_memory.device->memory.aperture_bytes,fence_memory.originals+original)!=INTEL_GGTT_BIND_OK ||
            intel_ggtt_pin(&native_memory.space,&fence_memory.bindings[i],fence_memory.bindings[i].cookie)!=INTEL_GGTT_BIND_OK)
            goto release;
        original+=pages[i];fence_memory.pins|=1u<<i;
    }
    stage="coherent cache policy";
    if (!intel_ppgtt_coherent_pat_acquire(&io,NULL,&fence_memory.pat)) goto release;
    stage="read-only coherent CPU views";
    for (unsigned i=0;i<2;i++) {
        NT_VM_DRIVER_VIEW *view=&fence_memory.fences.fences[i].vm;
        uint64_t flags=0,size=0;
        if (view->address!=fence_memory.addresses[i] ||
            paging_query_mapping_in_cr3(view->cr3,view->address,&flags,&size) || size!=4096 ||
            (flags&((1ULL<<63)|0x9b))!=((1ULL<<63)|1) ||
            paging_translate_in_cr3(view->cr3,view->address)!=fence_memory.leases[i].physical ||
            !nt_vm_user_range_accessible(view->address,4096,FALSE) ||
            nt_vm_user_range_accessible(view->address,4096,TRUE)) goto release;
        fence_memory.readonly_views++;
    }
    stage="GPU stores with dirty CPU cachelines"; native_memory.gt.ready=false;
    int task=smp_submit_any(fence_test_work,NULL,NULL);
    if (task>=0) smp_wait(task); else fence_test_work(NULL,NULL);
    if (fence_memory.engine.referenced && !fence_memory.engine.retired) goto done;
    native_memory.gt.ready=true;
    for (unsigned i=0;i<2;i++) {
        uint64_t *view=(uint64_t *)(uintptr_t)fence_memory.addresses[i];
        if (__atomic_load_n(view,__ATOMIC_ACQUIRE)==fence_memory.expected &&
            __atomic_load_n(view+32,__ATOMIC_ACQUIRE)==fence_memory.initial) fence_memory.view_matches++;
    }
    passed=fence_memory.passed && fence_memory.completed==64 && fence_memory.copied==64 && fence_memory.observed &&
        fence_memory.readonly_views==2 && fence_memory.view_matches==2;
    if (passed) {
        stage="context coherent mapping transactions";
        passed=fence_context_test();
        if (!native_memory.gt.ready) goto done;
    }
release:
    if (fence_memory.fences.self) {
        intel_gpu_fences_shutdown(&fence_memory.fences);
        for (unsigned i=0;i<2;i++) if (fence_memory.cookies[i]) {
            if (intel_gpu_fence_close(&fence_memory.fences,fence_memory.pid,fence_memory.device,
                fence_memory.cookies[i])!=STATUS_SUCCESS) goto done;
            fence_memory.cookies[i]=0;
        }
        for (unsigned i=0;i<INTEL_GPU_FENCES;i++) (void)intel_gpu_fences_collect(&fence_memory.fences);
        for (unsigned i=0;i<2;i++) if (fence_memory.leases[i].self) {
            intel_gpu_fence_t *object=&fence_memory.fences.fences[i];
            if (!object->vm_done || object->quarantined || object->vm_reference.self ||
                nt_vm_user_range_accessible(fence_memory.addresses[i],4096,FALSE)) goto done;
            fence_memory.retired_views++;
            fence_memory.lease_checks++;
        }
        if (passed && (fence_memory.memory.pages!=2 || intel_gpu_memory_collect_budget(&fence_memory.memory)))
            goto done;
        /* All DMA is physically retired before releasing either mapping lease.
         * This diagnostic owns its private PPGTT/context, never re-submits it. */
        for (unsigned i=0;i<2;i++) if (fence_memory.leases[i].self &&
            intel_gpu_fence_release(&fence_memory.leases[i])!=STATUS_SUCCESS) goto done;
        fence_memory.access=NULL;
        for (unsigned i=0;i<INTEL_GPU_FENCES;i++) (void)intel_gpu_fences_collect(&fence_memory.fences);
        if (!intel_gpu_fences_empty(&fence_memory.fences)) goto done;
        if (passed && (fence_memory.memory.pages!=2 || intel_gpu_memory_collect_budget(&fence_memory.memory)!=2))
            goto done;
        fence_memory.quota_checks++;
    }
    if (fence_memory.memory.self) {
        intel_gpu_memory_shutdown(&fence_memory.memory);
        if (fence_memory.device) {
            if (intel_gpu_memory_release_device(&fence_memory.memory,fence_memory.pid,
                fence_memory.device)!=INTEL_GPU_BUFFER_OK) goto done;
            fence_memory.device=0;
        }
    }
    if (fence_memory.pat.started && !intel_ppgtt_coherent_pat_restore(&io,NULL,&fence_memory.pat)) {
        native_memory.gt.ready=false;goto done;
    }
    for (unsigned i=3;i--;) {
        intel_ggtt_binding_t *binding=&fence_memory.bindings[i];
        if (fence_memory.pins&(1u<<i)) {
            if (intel_ggtt_unpin(&native_memory.space,binding,binding->cookie)!=INTEL_GGTT_BIND_OK) goto done;
            fence_memory.pins&=~(1u<<i);
        }
        if (binding->phase!=INTEL_GGTT_EMPTY && intel_ggtt_unbind(&native_memory.space,binding,binding->cookie)!=INTEL_GGTT_BIND_OK)
            goto done;
    }
    for (unsigned i=0;i<4;i++) {
        if (fence_memory.buffer_pins&(1u<<i)) {
            if (intel_gpu_buffer_unpin(&fence_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto done;
            fence_memory.buffer_pins&=~(1u<<i);
        }
        if (fence_memory.buffers[i].started && intel_gpu_buffer_close(&fence_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto done;
    }
    if (fence_memory.memory.self) {
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) (void)intel_gpu_memory_collect(&fence_memory.memory);
        if (fence_memory.memory.pages) goto done;
        if (fence_memory.memory.registered && intel_gpu_memory_unpublish(&fence_memory.memory)!=INTEL_GPU_BUFFER_OK) goto done;
        fence_memory.quota_checks++;
    }
    for (uint64_t off=0;off<native_memory.device->memory.ggtt_bytes;off+=8)
        after=(after^mmio_read64((volatile void *)(native_memory.mmio+INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
done:
    passed=passed && before==after && !fence_memory.pins && !fence_memory.buffer_pins && !fence_memory.pat.changed &&
        intel_gpu_fences_empty(&fence_memory.fences) && !fence_memory.memory.pages && !fence_memory.memory.registered &&
        fence_memory.quota_checks==3 && fence_memory.retired_views==2 && fence_memory.lease_checks==2 &&
        !fence_memory.ram.guard_failures && fence_memory.ram.allocated_pages==fence_memory.ram.freed_pages;
    serial_puts("[IGFX] coherent fence passed=");serial_putdec(passed);
    serial_puts(" stage=");serial_puts(stage);serial_puts(" stores=");serial_putdec(fence_memory.completed);
    serial_puts(" copies=");serial_putdec(fence_memory.copied);
    serial_puts(" observed=");serial_putdec(fence_memory.observed);serial_puts(" torn=");serial_putdec(fence_memory.torn);
    serial_puts(" retired=");serial_putdec(fence_memory.engine.retired);serial_puts(" restored=");serial_putdec(before==after);
    serial_puts(" pat_restored=");serial_putdec(!fence_memory.pat.changed);
    serial_puts(" pages=");serial_putdec(fence_memory.ram.freed_pages);serial_puts("/");serial_putdec(fence_memory.ram.allocated_pages);
    serial_puts(" cpu=");serial_putdec(fence_memory.cpu);serial_puts(" status=");serial_putdec(fence_memory.engine.status);
    serial_puts("\n[IGFX] coherent ownership mapped=");serial_putdec(fence_memory.mapped_views);
    serial_puts(" retired=");serial_putdec(fence_memory.retired_views);
    serial_puts(" readonly=");serial_putdec(fence_memory.readonly_views);
    serial_puts(" matches=");serial_putdec(fence_memory.view_matches);
    serial_puts(" lease_checks=");serial_putdec(fence_memory.lease_checks);
    serial_puts(" retained=");serial_putdec(!intel_gpu_fences_empty(&fence_memory.fences));
    serial_puts("\n[IGFX] coherent managed pages=");serial_putdec(fence_memory.memory.pages);
    serial_puts(" registered=");serial_putdec(fence_memory.memory.registered);
    serial_puts(" quota_checks=");serial_putdec(fence_memory.quota_checks);
    serial_puts("\n");return passed;
}

static bool ggtt_bcs_token(void *ctx, uint64_t *value)
{
    bcs_test_context_t *c = ctx;
    if (!c || c->native.binding.phase != INTEL_GGTT_BOUND || !c->native.binding.pins) return false;
    uint64_t phys = native_memory.device->bar2_phys + c->native.binding.gpu_address +
                    INTEL_BCS_TOKEN_OFFSET;
    *value = mmio_read64(PHYS_TO_VIRT(phys));
    return true;
}

static bool bcs_test_prepare(bcs_test_context_t *c, unsigned index, uint64_t token)
{
    (void)token;
    uint32_t owner=100+index;
    const intel_bcs_copy_t *copy=&bcs_copy[index];
    intel_gpu_memory_t *m=&bcs_memory.resources;
    intel_gpu_memory_lease_t source={0},destination={0};
    bool ready=false;
    if (intel_gpu_memory_open_device(m,owner,&c->device)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_create(m,owner,c->device,copy->source_bytes,&c->resource[0])!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_create(m,owner,c->device,copy->destination_bytes,&c->resource[1])!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_acquire(m,owner,c->device,c->resource[0],&source)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_acquire(m,owner,c->device,c->resource[1],&destination)!=INTEL_GPU_BUFFER_OK)
        goto release;
    c->source=source.buffer; c->destination=destination.buffer;
    ready=intel_bcs_context_create(&c->native,&native_memory.space,&gpu_ram,NULL,owner,
        0x1001+index,c->source,copy->source,c->destination,copy->destination,
        native_memory.device->memory.aperture_bytes);
release:
    if (source.self && intel_gpu_memory_release(&source)!=INTEL_GPU_BUFFER_OK) return false;
    if (destination.self && intel_gpu_memory_release(&destination)!=INTEL_GPU_BUFFER_OK) return false;
    if (!ready) return false;
    c->table_hash=14695981039346656037ULL;
    for(unsigned i=0;i<c->native.tables.pages*512;i++)
        c->table_hash=(c->table_hash^((uint64_t *)c->native.tables.cpu)[i])*1099511628211ULL;
    return true;
}

static bool bcs_test_verify(bcs_test_context_t *c)
{
    intel_bcs_context_t *n=&c->native;
    unsigned index=(unsigned)(c-bcs_memory.context);
    const intel_bcs_copy_t *copy=&bcs_copy[index];
    if (intel_gpu_buffer_sync(&n->state,n->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(&n->tables,n->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(c->source,n->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(c->destination,n->owner)!=INTEL_GPU_BUFFER_OK) return false;
    uint32_t *ram=n->state.cpu, *saved=ram+1024;
    c->saved_head=saved[5]; c->saved_tail=saved[7]; c->saved_ctl=saved[3];
    if ((saved[5]&0x1ffffc)!=n->tail || (saved[7]&0x1ffff8)!=n->tail ||
        saved[9]!=n->binding.gpu_address+8192 ||
        (((uint64_t)saved[49]<<32)|saved[51])!=n->tables.dma) return false;
    uint64_t tables=14695981039346656037ULL,ring=14695981039346656037ULL;
    for(unsigned i=0;i<n->tables.pages*512;i++)
        tables=(tables^((uint64_t *)n->tables.cpu)[i])*1099511628211ULL;
    for(unsigned i=0;i<512;i++) ring=(ring^((uint64_t *)(ram+2048))[i])*1099511628211ULL;
    if (tables!=c->table_hash || ring!=c->ring_hash) return false;
    for(unsigned i=0;i<c->source->pages*1024;i++)
        if (((uint32_t *)c->source->cpu)[i]!=(c->seed^(i*0x10101u))) return false;
    unsigned src_stride=copy->source_pitch/4,dst_stride=copy->destination_pitch/4;
    for(unsigned i=0;i<c->destination->pages*1024;i++) {
        bool copied=i/dst_stride<copy->height && i%dst_stride<copy->width;
        unsigned source_index=(i/dst_stride)*src_stride+i%dst_stride;
        uint32_t want=copied ? c->seed^(source_index*0x10101u) : 0xe4823100u^c->seed^i;
        if (((uint32_t *)c->destination->cpu)[i]!=want) return false;
    }
    if (mmio_read64((uint8_t *)ram+INTEL_BCS_TOKEN_OFFSET)!=c->token) return false;
    c->checked++; c->pixels+=copy->width*copy->height;
    return true;
}

static bool bcs_test_queue_prepare(void *backing, const intel_bcs_copy_t *copy,
    uint64_t token, uint64_t *descriptor, uint32_t *tail)
{
    bcs_test_context_t *c=backing;
    unsigned index=(unsigned)(c-bcs_memory.context);
    if (index>=2 || memcmp(copy,&bcs_copy[index],sizeof(*copy))) return false;
    c->token=token;
    c->seed=0x673acd00u^(index*0x1000000u)^(c->checked*0x1000u);
    for (unsigned i=0;i<c->source->pages*1024;i++) ((uint32_t *)c->source->cpu)[i]=c->seed^(i*0x10101u);
    for (unsigned i=0;i<c->destination->pages*1024;i++) ((uint32_t *)c->destination->cpu)[i]=0xe4823100u^c->seed^i;
    if (!intel_bcs_context_prepare(&c->native,copy,token,descriptor,tail)) return false;
    uint32_t *ram=c->native.state.cpu;
    c->ring_hash=14695981039346656037ULL;
    for (unsigned i=0;i<512;i++)
        c->ring_hash=(c->ring_hash^((uint64_t *)(ram+2048))[i])*1099511628211ULL;
    return true;
}

static enum intel_bcs_status bcs_test_queue_acquire(void *backing,uint64_t token)
{
    bcs_test_context_t *c=backing;
    return intel_bcs_context_acquire(&c->native,token);
}
static enum intel_bcs_status bcs_test_queue_retire(void *backing,uint64_t token)
{
    bcs_test_context_t *c=backing;
    return intel_bcs_context_retire(&c->native,token);
}
static bool bcs_test_cpu_handoff(bcs_test_context_t *c,bool available)
{
    intel_gpu_buffer_t *buffers[2]={c->source,c->destination};
    for (unsigned i=0;i<2;i++) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_acquire_cpu(buffers[i],
            c->native.owner,&c->cpu_probe[i]);
        if (!available) {
            if (r!=INTEL_GPU_BUFFER_BUSY || c->cpu_probe[i].self) return false;
        } else {
            if (r!=INTEL_GPU_BUFFER_OK) return false;
            /* Actual GPU-written first destination pixel, through the CPU
             * acquire cache handoff while its context remains resident. */
            bool data=*(volatile uint32_t *)c->cpu_probe[i].cpu==c->seed;
            if (intel_gpu_buffer_release_cpu(&c->cpu_probe[i])!=INTEL_GPU_BUFFER_OK || !data)
                return false;
        }
        bcs_memory.cpu_handoffs++;
    }
    return true;
}

typedef struct {
    intel_bcs_queue_t *queue;
    enum intel_bcs_status status;
    unsigned caller_cpu, done;
    bool poll;
} bcs_test_work_t;

static void bcs_test_queue_wakeup(void *context, uint32_t events)
{
    extern uint32_t smp_current_cpu(void);
    intel_bcs_queue_snapshot_t snapshot;
    if (intel_bcs_queue_snapshot(context,&snapshot)!=INTEL_BCS_OK ||
        snapshot.queued+snapshot.active+snapshot.terminal>INTEL_BCS_QUEUE_JOBS) {
        __atomic_add_fetch(&bcs_memory.wake_errors,1,__ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&bcs_memory.wake_calls,1,__ATOMIC_RELAXED);
    if (smp_current_cpu()!=0)
        __atomic_add_fetch(&bcs_memory.wake_ap,1,__ATOMIC_RELAXED);
    if ((events&INTEL_BCS_EVENT_RESULT) && snapshot.terminal)
        __atomic_add_fetch(&bcs_memory.wake_results,1,__ATOMIC_RELAXED);
}

static void bcs_test_queue_step(void *argument, void *result)
{
    (void)result;
    extern uint32_t smp_current_cpu(void);
    bcs_test_work_t *work=argument;
    if (smp_current_cpu()!=work->caller_cpu) bcs_memory.remote_steps++;
    else bcs_memory.local_steps++;
    work->status=intel_bcs_queue_step(work->queue);
    for (unsigned poll=0;work->poll && poll<200000 && work->status==INTEL_BCS_PENDING;poll++) {
        gt_hw_relax(NULL);
        work->status=intel_bcs_queue_step(work->queue);
    }
    __atomic_store_n(&work->done,1,__ATOMIC_RELEASE);
}

static enum intel_bcs_status bcs_test_worker_call(intel_bcs_queue_t *q, bool poll)
{
    extern uint32_t smp_current_cpu(void);
    bcs_test_work_t work={.queue=q,.caller_cpu=smp_current_cpu(),.poll=poll};
    int task=smp_submit_any(bcs_test_queue_step,&work,NULL);
    if (task<0) return INTEL_BCS_BUSY;
    for (unsigned wait=0;wait<1000000;wait++) {
        if (__atomic_load_n(&work.done,__ATOMIC_ACQUIRE)) break;
        __asm__ volatile("pause");
    }
    smp_wait(task); /* join before releasing stack-backed callback state */
    return work.status;
}

static bool bcs_test_queue_run(const intel_gt_io_t *io, uint64_t token_base,
    unsigned *completed)
{
    intel_bcs_queue_t *q=&bcs_memory.queue;
    const intel_bcs_queue_ops_t ops={bcs_test_queue_prepare,ggtt_bcs_token,
        bcs_test_queue_acquire,bcs_test_queue_retire};
    if (intel_bcs_queue_init(q,&bcs_memory.engine,io,NULL,&ops,token_base+1)!=INTEL_BCS_OK)
        return false;
    for (unsigned i=0;i<2;i++)
        if (!intel_bcs_context_attach(&bcs_memory.context[i].native,q,&bcs_memory.context[i]))
            return false;
    if (intel_bcs_queue_publish(q)!=INTEL_BCS_OK) return false;
    if (intel_bcs_queue_watch(q,bcs_test_queue_wakeup,q)!=INTEL_BCS_OK) return false;
    /* Four actual copies per batch; cancel two intervening jobs before
     * dispatch. Reuse freed result slots without reordering the FIFO. */
    for (unsigned batch=0;batch<35;batch++) {
        uint64_t fence[6];
        unsigned *admission=bcs_memory.admission;
        memset(admission,0,sizeof(bcs_memory.admission));
        intel_bcs_job_result_t result;
        for (unsigned i=0;i<6;i++) {
            const intel_bcs_queue_gate_t gate={&admission[i],0,1,2};
            if (intel_bcs_queue_stage(q,100+i%2,bcs_memory.context[i%2].native.queue_id,
                &bcs_copy[i%2],&gate,&fence[i])!=INTEL_BCS_OK) return false;
        }
        if (!batch && (bcs_test_worker_call(q,false)!=INTEL_BCS_OK || bcs_memory.engine.submitted_jobs))
            return false; /* staged commands cannot touch the engine */
        for (unsigned i=0;i<6;i++) {
            __atomic_store_n(&admission[i],i==2 || i==3 ? 2 : 1,__ATOMIC_RELEASE);
            if (intel_bcs_queue_finish_stage(q,100+i%2,fence[i],&admission[i])!=INTEL_BCS_OK) return false;
        }
        for (unsigned i=2;i<4;i++) {
            if (intel_bcs_queue_cancel(q,100+i%2,fence[i])!=INTEL_BCS_OK ||
                intel_bcs_queue_result(q,100+i%2,fence[i],true,&result)!=INTEL_BCS_OK ||
                result.phase!=INTEL_BCS_JOB_CANCELLED) return false;
            bcs_memory.cancelled++;
        }
        for (unsigned i=0;i<6;i++) {
            if (i==2 || i==3) continue;
            unsigned owner=100+i%2;
            bcs_test_context_t *c=&bcs_memory.context[i%2];
            if (!batch && !i) {
                if (intel_gpu_buffer_acquire_cpu(c->source,owner,&c->cpu_probe[0])!=INTEL_GPU_BUFFER_OK)
                    return false;
                *(volatile uint32_t *)c->cpu_probe[0].cpu=0x76543210;
                if (bcs_test_worker_call(q,false)!=INTEL_BCS_BUSY || bcs_memory.engine.submitted_jobs ||
                    *(volatile uint32_t *)c->cpu_probe[0].cpu!=0x76543210) return false;
                if (intel_gpu_buffer_release_cpu(&c->cpu_probe[0])!=INTEL_GPU_BUFFER_OK) return false;
                bcs_memory.cpu_handoffs++;
            }
            if (bcs_test_worker_call(q,false)!=INTEL_BCS_PENDING) return false;
            if (!bcs_test_cpu_handoff(c,false)) return false;
            if (batch==34 && i>=4) {
                uint64_t cancelled;
                if (intel_bcs_queue_enqueue(q,owner,c->native.queue_id,&bcs_copy[i%2],&cancelled)!=INTEL_BCS_OK ||
                    intel_bcs_queue_cancel(q,owner,fence[i])!=INTEL_BCS_BUSY) return false;
                /* Exercise the same exit notification reached from GDI/VM
                 * teardown, using diagnostic owners, with a submitted job. */
                extern void d3dkmt_release_process(uint32_t);
                d3dkmt_release_process(owner);
                if (
                    intel_bcs_queue_result(q,owner,cancelled,true,&result)!=INTEL_BCS_OK ||
                    result.phase!=INTEL_BCS_JOB_CANCELLED ||
                    intel_bcs_context_destroy(&c->native))
                    return false;
                intel_gpu_memory_lease_t denied={0};
                if (intel_gpu_memory_acquire(&bcs_memory.resources,owner,c->device,
                        c->resource[0],&denied)!=INTEL_GPU_BUFFER_INVALID ||
                    intel_gpu_memory_collect(&bcs_memory.resources)!=INTEL_GPU_BUFFER_BUSY ||
                    intel_gpu_memory_collect(&bcs_memory.resources)!=INTEL_GPU_BUFFER_BUSY ||
                    !c->source->live || !c->destination->live) return false;
                bcs_memory.resource_deferred+=2;
                bcs_memory.cancelled++; bcs_memory.deferred++;
            }
            enum intel_bcs_status status=bcs_test_worker_call(q,true);
            if (status!=INTEL_BCS_OK ||
                intel_bcs_queue_result(q,owner,fence[i],true,&result)!=INTEL_BCS_OK ||
                result.phase!=INTEL_BCS_JOB_COMPLETE) return false;
            if (!c->source->closing && !bcs_test_cpu_handoff(c,true)) return false;
            if (!bcs_test_verify(c)) return false;
            (*completed)++;
        }
    }
    for (unsigned i=0;i<2;i++) {
        if (intel_bcs_context_destroy(&bcs_memory.context[i].native))
            return false; /* completion still retains the saved DMA context */
        bcs_memory.deferred++;
    }
    return bcs_memory.cancelled==72 && bcs_memory.deferred==4 &&
        bcs_memory.resource_deferred==4 && bcs_memory.wake_results==212 &&
        !bcs_memory.wake_errors;
}

static void bcs_test_service_notify(void *context, uint32_t events)
{
    bcs_test_queue_wakeup(context,events);
    sched_signal_set(&bcs_memory.client_signal,events);
}

static bool gpu_irq_cfg_read(void *ctx,uint16_t offset,uint32_t *value)
{
    (void)ctx;
    return pci_diag_read32(0,2,0,offset,value);
}
static bool gpu_irq_cfg_write(void *ctx,uint16_t offset,unsigned width,uint32_t value)
{
    (void)ctx;
    if (width==2) pci_cfg_write16(0,2,0,offset,(uint16_t)value);
    else if (width==4) pci_cfg_write32(0,2,0,offset,value);
    else return false;
    return true; /* core verifies readback */
}
static void gpu_irq_wake(void *ctx,bool fault)
{
    intel_bcs_worker_interrupt(ctx,fault);
}
static bool ggtt_test_preflight(const intel_gfx_state_t *);
static bool gpu_irq_change(int action)
{
    extern uint32_t idt_get_bsp_apic_id(void);
    const intel_bcs_irq_io_t io={{gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax},
        gpu_irq_cfg_read,gpu_irq_cfg_write};
    if (action>0 && !ggtt_test_preflight(native_memory.device)) return false;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    bool ok=action>0 ? intel_bcs_irq_open(&bcs_memory.irq,&io,NULL,idt_get_bsp_apic_id(),
        gpu_irq_wake,&bcs_memory.worker) : action==0 ? intel_bcs_irq_quiesce(&bcs_memory.irq) :
        intel_bcs_irq_close(&bcs_memory.irq);
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
    return ok;
}

static bool bcs_test_rebind(unsigned index,unsigned batch)
{
    bcs_test_context_t *c=&bcs_memory.context[index];
    intel_gpu_memory_t *m=&bcs_memory.resources;
    uint32_t owner=100+index,resource[2]={0},old[2]={c->resource[0],c->resource[1]};
    intel_gpu_memory_lease_t source={0},destination={0};
    bool changed=false;
    uint32_t old_pages=c->source->pages+c->destination->pages;
    uint64_t source_dma=c->source->dma,destination_dma=c->destination->dma;
    const intel_bcs_copy_t *copy=&bcs_copy[index];
    uint64_t extra=(batch&1)*4096;
    if (intel_gpu_memory_create(m,owner,c->device,copy->source_bytes+extra,&resource[0])!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_create(m,owner,c->device,copy->destination_bytes+extra,&resource[1])!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_acquire(m,owner,c->device,resource[0],&source)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_acquire(m,owner,c->device,resource[1],&destination)!=INTEL_GPU_BUFFER_OK)
        goto release;
    if (source.buffer->dma==source_dma || destination.buffer->dma==destination_dma ||
        intel_bcs_context_rebind(&c->native,source.buffer,destination.buffer)!=INTEL_BCS_OK)
        goto release;
    c->source=source.buffer; c->destination=destination.buffer;
    c->resource[0]=resource[0]; c->resource[1]=resource[1];
    changed=true;
release:
    if (source.self && intel_gpu_memory_release(&source)!=INTEL_GPU_BUFFER_OK) return false;
    if (destination.self && intel_gpu_memory_release(&destination)!=INTEL_GPU_BUFFER_OK) return false;
    if (!changed) return false; /* shutdown collects even partially created replacements */
    uint32_t charged=m->pages;
    for (unsigned i=0;i<2;i++) {
        if (intel_gpu_memory_close(m,owner,c->device,old[i])!=INTEL_GPU_BUFFER_OK ||
            intel_gpu_memory_collect(m)!=INTEL_GPU_BUFFER_OK) return false;
        intel_gpu_memory_lease_t stale={0};
        if (intel_gpu_memory_acquire(m,owner,c->device,old[i],&stale)!=INTEL_GPU_BUFFER_INVALID)
            return false;
        bcs_memory.service_reclaims++;
    }
    if (m->pages!=charged-old_pages || bcs_memory.engine.retired || !bcs_memory.worker.started)
        return false;
    c->table_hash=14695981039346656037ULL;
    for(unsigned i=0;i<c->native.tables.pages*512;i++)
        c->table_hash=(c->table_hash^((uint64_t *)c->native.tables.cpu)[i])*1099511628211ULL;
    return true;
}

static bool bcs_test_service_run(const intel_gt_io_t *io, uint64_t token_base,
    unsigned *completed)
{
    extern void sched_yield(void);
    intel_bcs_queue_t *q=&bcs_memory.queue;
    intel_bcs_worker_t *w=&bcs_memory.worker;
    const intel_bcs_queue_ops_t ops={bcs_test_queue_prepare,ggtt_bcs_token,
        bcs_test_queue_acquire,bcs_test_queue_retire};
    bool passed=false;
    if (intel_bcs_queue_init(q,&bcs_memory.engine,io,NULL,&ops,token_base+1)!=INTEL_BCS_OK)
        return false;
    for (unsigned i=0;i<2;i++)
        if (!intel_bcs_context_attach(&bcs_memory.context[i].native,q,&bcs_memory.context[i]))
            return false;
    if (intel_bcs_queue_publish(q)!=INTEL_BCS_OK) return false;
    if (intel_bcs_worker_start(w,q,bcs_test_service_notify,q,true)!=INTEL_BCS_OK) goto stop;
    if (!gpu_irq_change(true)) goto stop;
    for (unsigned batch=0;batch<16;batch++) {
        uint64_t until=idt_get_monotonic_ns()+3000000000ULL;
        while (!intel_bcs_worker_idle(w)) {
            if (idt_get_monotonic_ns()>=until) goto stop;
            sched_yield();
        }
        /* Even before the first DMA submission, a parked task retains queue,
         * callback and bundle pointers that a hotreload must not invalidate. */
        if (intel_gfx_kexec_ready()) goto stop;
        bcs_memory.service_idle_checks++;
        if (batch) for (unsigned i=0;i<2;i++)
            if (!bcs_test_rebind(i,batch)) goto stop;
        uint64_t fence[2];
        for (unsigned i=0;i<2;i++)
            if (intel_bcs_queue_enqueue(q,100+i,bcs_memory.context[i].native.queue_id,
                &bcs_copy[i],&fence[i])!=INTEL_BCS_OK) goto stop;
        for (unsigned i=0;i<2;i++) {
            intel_bcs_job_result_t result;
            for (;;) {
                /* Clear the hint BEFORE inspecting the fence. A completion
                 * between the peek and wait remains latched by the signal. */
                sched_signal_take(&bcs_memory.client_signal);
                if (intel_bcs_queue_result(q,100+i,fence[i],false,&result)!=INTEL_BCS_OK)
                    goto stop;
                if (result.phase==INTEL_BCS_JOB_COMPLETE) break;
                if (result.phase>=INTEL_BCS_JOB_COMPLETE || idt_get_monotonic_ns()>=until)
                    goto stop;
                if (sched_signal_wait(&bcs_memory.client_signal,100)!=SCHED_SIGNAL_READY)
                    sched_yield();
            }
            if (intel_bcs_queue_result(q,100+i,fence[i],true,&result)!=INTEL_BCS_OK ||
                result.error!=INTEL_BCS_OK) goto stop;
            if (!bcs_test_cpu_handoff(&bcs_memory.context[i],true)) goto stop;
            if (!bcs_test_verify(&bcs_memory.context[i])) goto stop;
            (*completed)++;
        }
    }
    passed=!bcs_memory.wake_errors && *completed==32 && bcs_memory.irq.events &&
        !bcs_memory.irq.failed && !w->watchdog_wakes &&
        bcs_memory.engine.tlb_invalidations==30 && bcs_memory.service_reclaims==60;
stop:
    if (!gpu_irq_change(false)) return false;
    if (w->started) {
        intel_bcs_worker_stop(w);
        uint64_t until=idt_get_monotonic_ns()+3000000000ULL;
        while (intel_bcs_worker_reap(w)!=INTEL_BCS_OK) {
            if (idt_get_monotonic_ns()>=until) return false; /* retain all DMA/task state */
            sched_yield();
        }
    }
    if (!gpu_irq_change(-1)) return false;
    return passed;
}

static bool intel_gfx_bcs_test(bool service)
{
    const intel_gfx_state_t *d=native_memory.device;
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    bool passed=false,restored=false,pat_restored=true;
    const char *stage="prepare contexts";
    unsigned job=0;
    uint64_t token_base=0x6f73690000000000ULL | (idt_get_monotonic_ns()&0xffffffff);
    uint64_t before_hash=14695981039346656037ULL,after_hash=before_hash;
    if (!native_memory.gt.ready || !native_memory.space.ready) return false;
    if (bcs_memory.hwsp.started || bcs_memory.resources.initialized) return false;
    for(unsigned i=0;i<2;i++) if (bcs_memory.context[i].native.started) return false;
    if (!intel_gpu_memory_init(&bcs_memory.resources,&gpu_ram,NULL,64,32) ||
        intel_gpu_memory_publish(&bcs_memory.resources)!=INTEL_GPU_BUFFER_OK) goto done;
    for(uint64_t off=0;off<d->memory.ggtt_bytes;off+=8)
        before_hash=(before_hash^mmio_read64((volatile void *)(native_memory.mmio+
            INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
    for(unsigned i=0;i<2;i++)
        if (!bcs_test_prepare(&bcs_memory.context[i],i,token_base+i+1)) goto release;
    stage="separate global HWSP";
    if (intel_gpu_buffer_create(&bcs_memory.hwsp,&gpu_ram,NULL,1,4096)!=INTEL_GPU_BUFFER_OK)
        goto release;
    memset((uint8_t *)bcs_memory.hwsp.cpu+0x370,0xff,6*8);
    if (intel_gpu_buffer_sync(&bcs_memory.hwsp,1)!=INTEL_GPU_BUFFER_OK ||
        intel_ggtt_bind(&native_memory.space,&bcs_memory.hwsp_binding,&bcs_memory.hwsp.dma,
            1,1,d->memory.aperture_bytes,&bcs_memory.hwsp_original)!=INTEL_GGTT_BIND_OK)
        goto release;
    if (intel_ggtt_pin(&native_memory.space,&bcs_memory.hwsp_binding,
        bcs_memory.hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK) goto release;
    bcs_memory.hwsp_pinned=true;
    stage="private PAT acquire";
    if (!intel_ppgtt_pat_acquire(&io,NULL,&bcs_memory.pat)) goto release;
    pat_restored=false;
    native_memory.gt.ready=false; /* GGTT remains fixed; PPGTT updates use the queue gate */
    stage="enable engine";
    if (intel_bcs_enable(&io,NULL,bcs_memory.hwsp_binding.gpu_address,
        &bcs_memory.engine)!=INTEL_BCS_OK) goto retire;
    stage=service ? "persistent post-boot service" : "device resources, process exit and deferred reclaim";
    passed=service ? bcs_test_service_run(&io,token_base,&job) : bcs_test_queue_run(&io,token_base,&job);
retire:
    if (bcs_memory.worker.started || (bcs_memory.irq.started && !bcs_memory.irq.closed)) goto done;
    if (bcs_memory.queue.initialized) {
        if (intel_bcs_queue_shutdown(&bcs_memory.queue)!=INTEL_BCS_OK) goto done;
        if (intel_bcs_queue_unwatch(&bcs_memory.queue)!=INTEL_BCS_OK) goto done;
        if (bcs_memory.queue.registered &&
            intel_bcs_queue_unpublish(&bcs_memory.queue)!=INTEL_BCS_OK) goto done;
    } else if (bcs_memory.engine.started &&
        intel_bcs_close(&io,NULL,&bcs_memory.engine)!=INTEL_BCS_OK) goto done;
    if (bcs_memory.engine.referenced && !bcs_memory.engine.retired) goto done;
    native_memory.gt.ready=true;
release:
    if (bcs_memory.pat.started)
        pat_restored=intel_ppgtt_pat_restore(&io,NULL,&bcs_memory.pat);
    for(unsigned i=0;i<2;i++) {
        bcs_test_context_t *c=&bcs_memory.context[i];
        if (!intel_bcs_context_destroy(&c->native)) goto done;
    }
    intel_gpu_memory_shutdown(&bcs_memory.resources);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (intel_gpu_memory_collect(&bcs_memory.resources)!=INTEL_GPU_BUFFER_OK) goto done;
    if (bcs_memory.resources.pages ||
        intel_gpu_memory_unpublish(&bcs_memory.resources)!=INTEL_GPU_BUFFER_OK) goto done;
    if (bcs_memory.hwsp_pinned) {
        if (intel_ggtt_unpin(&native_memory.space,&bcs_memory.hwsp_binding,
            bcs_memory.hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK) goto done;
        bcs_memory.hwsp_pinned=false;
    }
    if (bcs_memory.hwsp_binding.phase!=INTEL_GGTT_EMPTY &&
        intel_ggtt_unbind(&native_memory.space,&bcs_memory.hwsp_binding,
            bcs_memory.hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK) goto done;
    if (bcs_memory.hwsp.started && intel_gpu_buffer_close(&bcs_memory.hwsp,1)!=INTEL_GPU_BUFFER_OK)
        goto done;
    for(uint64_t off=0;off<d->memory.ggtt_bytes;off+=8)
        after_hash=(after_hash^mmio_read64((volatile void *)(native_memory.mmio+
            INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
    restored=before_hash==after_hash;
done:
    passed=passed && restored && pat_restored && !bcs_memory.ram.guard_failures && !bcs_memory.worker.started &&
        bcs_memory.ram.allocated_pages==bcs_memory.ram.freed_pages;
    serial_puts("[IGFX] native BCS passed="); serial_putdec(passed);
    serial_puts(" stage="); serial_puts(stage);
    serial_puts(" job="); serial_putdec(job);
    serial_puts(" status="); serial_putdec(bcs_memory.engine.status);
    serial_puts(" jobs="); serial_putdec(bcs_memory.engine.completed_jobs);
    serial_puts("/"); serial_putdec(bcs_memory.engine.submitted_jobs);
    serial_puts(" cpu_handoffs="); serial_putdec(bcs_memory.cpu_handoffs);
    serial_puts(" retired="); serial_putdec(bcs_memory.engine.retired);
    serial_puts(" restored="); serial_putdec(restored);
    serial_puts(" pat_restored="); serial_putdec(pat_restored);
    serial_puts(" pat_last="); serial_puthex(bcs_memory.pat.last_value,8);
    serial_puts(" cancelled="); serial_putdec(bcs_memory.cancelled);
    serial_puts(" deferred="); serial_putdec(bcs_memory.deferred);
    serial_puts(" ap_steps="); serial_putdec(bcs_memory.remote_steps);
    serial_puts(" bsp_steps="); serial_putdec(bcs_memory.local_steps);
    serial_puts(" resource_deferred="); serial_putdec(bcs_memory.resource_deferred);
    serial_puts(" pages="); serial_putdec(bcs_memory.ram.freed_pages);
    serial_puts("/"); serial_putdec(bcs_memory.ram.allocated_pages);
    serial_puts(" guards="); serial_putdec(bcs_memory.ram.guard_failures);
    serial_puts(" charged_pages="); serial_putdec(bcs_memory.resources.pages);
    serial_puts(" memory_registered="); serial_putdec(bcs_memory.resources.registered);
    serial_puts(" wake_calls="); serial_putdec(bcs_memory.wake_calls);
    serial_puts(" wake_ap="); serial_putdec(bcs_memory.wake_ap);
    serial_puts(" wake_results="); serial_putdec(bcs_memory.wake_results);
    serial_puts(" wake_errors="); serial_putdec(bcs_memory.wake_errors);
    serial_puts(" service="); serial_putdec(service);
    serial_puts(" idle_checks="); serial_putdec(bcs_memory.service_idle_checks);
    serial_puts(" live_reclaims="); serial_putdec(bcs_memory.service_reclaims);
    serial_puts(" tlb_invalidations="); serial_putdec(bcs_memory.engine.tlb_invalidations);
    serial_puts(" worker_pid="); serial_putdec(bcs_memory.worker.pid);
    serial_puts(" worker_live="); serial_putdec(bcs_memory.worker.started);
    serial_puts(" worker_sleeps="); serial_putdec(bcs_memory.worker.sleeps);
    serial_puts(" worker_ap="); serial_putdec(bcs_memory.worker.ap_operations);
    serial_puts(" worker_bsp="); serial_putdec(bcs_memory.worker.bsp_operations);
    serial_puts(" irq_calls="); serial_putdec(bcs_memory.irq.calls);
    serial_puts(" irq_events="); serial_putdec(bcs_memory.irq.events);
    serial_puts(" irq_failed="); serial_putdec(bcs_memory.irq.failed);
    serial_puts(" irq_closed="); serial_putdec(bcs_memory.irq.closed);
    serial_puts(" irq_stage="); serial_putdec(bcs_memory.irq.open_stage);
    serial_puts(" msi_cap="); serial_puthex(bcs_memory.irq.cap,4);
    serial_puts(" msi_ctrl="); serial_puthex(bcs_memory.irq.control,4);
    serial_puts(" irq_waits="); serial_putdec(bcs_memory.worker.irq_waits);
    serial_puts(" watchdog="); serial_putdec(bcs_memory.worker.watchdog_wakes);
    serial_puts(" irq_reg="); serial_puthex(bcs_memory.irq.fault_reg,8);
    serial_puts(" irq_value="); serial_puthex(bcs_memory.irq.last_value,8);
    serial_puts("\n[IGFX] BCS token="); serial_puthex(bcs_memory.engine.observed_token,16);
    serial_puts(" expected="); serial_puthex(bcs_memory.engine.target_token,16);
    serial_puts(" head/tail="); serial_puthex(bcs_memory.engine.head,8);
    serial_puts("/"); serial_puthex(bcs_memory.engine.tail,8);
    serial_puts(" csb="); serial_puthex(bcs_memory.engine.csb_status,8);
    serial_puts("/"); serial_puthex(bcs_memory.engine.csb_id,8);
    serial_puts(" el="); serial_puthex(bcs_memory.engine.execlist_status,8);
    serial_puts(" error="); serial_puthex(bcs_memory.engine.engine_error,8);
    serial_puts(" reg="); serial_puthex(bcs_memory.engine.fault_reg,8);
    serial_puts(" value="); serial_puthex(bcs_memory.engine.last_value,8);
    serial_puts("\n");
    for(unsigned i=0;i<2;i++) {
        bcs_test_context_t *c=&bcs_memory.context[i];
        serial_puts("[IGFX] BCS context="); serial_putdec(i);
        serial_puts(" checked="); serial_putdec(c->checked);
        serial_puts(" pixels="); serial_putdec(c->pixels);
        serial_puts(" tables="); serial_putdec(c->native.table_pages);
        serial_puts(" rebindings="); serial_putdec(c->native.rebindings);
        serial_puts(" saved_head/tail/ctl="); serial_puthex(c->saved_head,8);
        serial_puts("/"); serial_puthex(c->saved_tail,8);
        serial_puts("/"); serial_puthex(c->saved_ctl,8);
        serial_puts(" retained_ram="); serial_puthex(
            (uint64_t)((c->source && c->source->live ? c->source->pages : 0)+
            (c->destination && c->destination->live ? c->destination->pages : 0)+
            c->native.state.live*c->native.state.pages+c->native.tables.live*c->native.tables.pages)*4096,16);
        serial_puts("\n");
    }
    return passed;
}

static bool rcs_test_token(void *ctx,uint64_t *value)
{
    (void)ctx;
    bool shared=rcs_memory.program_version==2;
    if (shared ? !rcs_memory.native.ggtt_pinned : !(rcs_memory.pins&1)) return false;
    uint64_t address=native_memory.device->bar2_phys+(shared ?
        rcs_memory.native.binding.gpu_address : rcs_memory.bindings[0].gpu_address)+INTEL_RCS_TOKEN_OFFSET;
    *value=mmio_read64(PHYS_TO_VIRT(address));
    return true;
}

static enum intel_bcs_status rcs_test_queue_acquire(void *opaque,uint64_t token)
{
    return intel_bcs_context_acquire(opaque,token);
}
static enum intel_bcs_status rcs_test_queue_retire(void *opaque,uint64_t token)
{
    return intel_bcs_context_retire(opaque,token);
}
static bool rcs_test_queue_prepare(void *opaque,const intel_rcs_queue_draw_t *work,uint64_t token,
    uint64_t *descriptor,uint32_t *tail)
{
    if (!intel_bcs_context_prepare_draw(opaque,work,token,&rcs_memory.descriptor,&rcs_memory.tail)) return false;
    *descriptor=rcs_memory.descriptor; *tail=rcs_memory.tail;
    return true;
}

static void rcs_sampled_test_work(void *argument,void *result)
{
    (void)argument;(void)result;
    extern uint32_t smp_current_cpu(void);
    rcs_memory.cpu=smp_current_cpu();
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    intel_cs_state_t *engine=&rcs_memory.engine;
    uint32_t *context=rcs_memory.buffers[0].cpu,*ring=rcs_memory.buffers[1].cpu;
    const uint8_t *entry=rcs_memory.program_cases;
    uint64_t base=0x73616d0000000000ULL|(idt_get_monotonic_ns()&0xffffffff);
    if (intel_cs_enable(INTEL_CS_RENDER,&io,NULL,rcs_memory.bindings[2].gpu_address,engine)!=INTEL_CS_OK)
        goto close;
    for (unsigned job=0;job<=rcs_memory.program_count;job++) {
        const intel_rcs_program_test_case_t *test=(const void *)entry;
        const struct intel_d3d9_program *program=(const void *)(test+1);
        const intel_rcs_sampled_test_t *fixture=(const void *)((const uint8_t *)program+test->program_bytes);
        const intel_rcs_vertex_test_t *vertex=rcs_memory.program_version==4 ? (const void *)fixture : NULL;
        const intel_rcs_indexed_test_t *indexed=rcs_memory.program_version==5 ? (const void *)fixture : NULL;
        const struct intel_d3d9_vertex_program *vs=vertex ? (const void *)(vertex+1) :
            indexed && indexed->vertex_bytes ? (const void *)(indexed+1) : NULL;
        rcs_memory.token=base+job+1;
        if (!job) {
            if (!intel_rcs_build(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
                rcs_memory.bindings[0].gpu_address,rcs_memory.bindings[1].gpu_address,
                rcs_memory.buffers[3].dma,0x2001,rcs_memory.token,&rcs_memory.descriptor)) goto close;
            rcs_memory.tail=INTEL_RCS_FLUSH_BYTES;
        } else {
            memset(rcs_memory.buffers[7].cpu,0,rcs_memory.buffers[7].bytes);
            memcpy(rcs_memory.buffers[7].cpu,(const uint8_t *)program+program->code_offset,program->code_bytes);
            if (vs) memcpy((uint8_t *)rcs_memory.buffers[7].cpu+65536,
                (const uint8_t *)vs+vs->base.code_offset,vs->base.code_bytes);
            memcpy(rcs_memory.buffers[5].cpu,fixture->source,sizeof(fixture->source));
            memcpy(rcs_memory.buffers[8].cpu,fixture->vertices,sizeof(fixture->vertices));
            memset(rcs_memory.buffers[6].cpu,0x5a,rcs_memory.buffers[6].bytes);
            bool built=indexed ? intel_rcs_indexed_build(rcs_memory.buffers[4].cpu,rcs_memory.buffers[4].bytes,
                &fixture->draw,program,test->program_bytes,vs ? &indexed->vertex : NULL,vs,indexed->vertex_bytes,
                &indexed->indices,(const uint8_t *)rcs_memory.buffers[8].cpu+4096,4096,NULL,0) :
                vs ? intel_rcs_vertex_build(rcs_memory.buffers[4].cpu,rcs_memory.buffers[4].bytes,
                &fixture->draw,program,test->program_bytes,&vertex->vertex,vs,vertex->vertex_bytes,NULL,0) :
                intel_rcs_sampled_build(rcs_memory.buffers[4].cpu,rcs_memory.buffers[4].bytes,
                    &fixture->draw,program,test->program_bytes,NULL,0);
            if (!built ||
                !intel_rcs_append_batch(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
                    rcs_memory.tail,0x200000,rcs_memory.token,&rcs_memory.tail)) goto close;
        }
        for (unsigned i=0;i<9;i++)
            if (intel_gpu_buffer_sync(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto close;
        if (intel_cs_submit(&io,NULL,rcs_test_token,rcs_memory.descriptor,rcs_memory.token,
            rcs_memory.tail,engine)!=INTEL_CS_OK || intel_cs_wait(&io,NULL,rcs_test_token,engine)!=INTEL_CS_OK)
            goto close;
        for (unsigned i=0;i<9;i++)
            if (intel_gpu_buffer_sync(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto close;
        const uint32_t *saved=context+1024;
        if ((saved[5]&0x1ffffc)!=rcs_memory.tail || (saved[7]&0x1ffff8)!=rcs_memory.tail ||
            saved[9]!=rcs_memory.bindings[1].gpu_address ||
            (((uint64_t)saved[49]<<32)|saved[51])!=rcs_memory.buffers[3].dma ||
            mmio_read64((uint8_t *)context+INTEL_RCS_TOKEN_OFFSET)!=rcs_memory.token) goto close;
        rcs_memory.completed++;
        if (job) {
            const uint32_t *actual=rcs_memory.buffers[6].cpu;
            for (unsigned i=0;i<sizeof(fixture->expected)/4;i++) {
                if (actual[i]!=fixture->expected[i]) {
                    serial_puts("[IGFX] sampled mismatch case/index=");serial_putdec(job-1);
                    serial_puts("/");serial_putdec(i);serial_puts(" got=");serial_puthex(actual[i],8);
                    serial_puts(" expected=");serial_puthex(fixture->expected[i],8);serial_puts("\n");
                    goto close;
                }
                if (actual[i]!=0x5a5a5a5a) rcs_memory.pixels++;
            }
            if (memcmp(rcs_memory.buffers[5].cpu,fixture->source,sizeof(fixture->source)) ||
                memcmp(rcs_memory.buffers[8].cpu,fixture->vertices,sizeof(fixture->vertices)) ||
                memcmp(rcs_memory.buffers[7].cpu,(const uint8_t *)program+program->code_offset,program->code_bytes))
                goto close;
            if (vs && memcmp((uint8_t *)rcs_memory.buffers[7].cpu+65536,
                (const uint8_t *)vs+vs->base.code_offset,vs->base.code_bytes)) goto close;
            rcs_memory.draws++;
            serial_puts(indexed ? "[IGFX] native indexed case=" : vs ? "[IGFX] native vertex case=" : "[IGFX] native sampled case=");serial_putdec(job-1);
            serial_puts(" mask=");serial_putdec(test->dispatch_mask);serial_puts(" passed\n");
            entry+=sizeof(*test)+test->program_bytes+test->reserved;
        }
    }
    rcs_memory.passed=true;
close:
    if (engine->started && intel_cs_close(&io,NULL,engine)!=INTEL_CS_OK) rcs_memory.passed=false;
}

static void rcs_test_work(void *argument,void *result)
{
    (void)argument; (void)result;
    extern uint32_t smp_current_cpu(void);
    rcs_memory.cpu=smp_current_cpu();
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    intel_cs_state_t *engine=&rcs_memory.engine;
    bool queued=rcs_memory.program_version==2;
    intel_bcs_context_t *native=&rcs_memory.native;
    uint32_t *context=queued ? native->state.cpu : rcs_memory.buffers[0].cpu;
    uint32_t *ring=queued ? context+INTEL_RCS_CONTEXT_BYTES/4 : rcs_memory.buffers[1].cpu;
    uint64_t base=0x7263730000000000ULL|(idt_get_monotonic_ns()&0xffffffff);
    if (intel_cs_enable(INTEL_CS_RENDER,&io,NULL,rcs_memory.bindings[2].gpu_address,engine)!=INTEL_CS_OK)
        goto close;
    const uint8_t *entry=rcs_memory.program_cases;
    if (queued) {
        const intel_bcs_queue_ops_t ops={NULL,rcs_test_token,rcs_test_queue_acquire,rcs_test_queue_retire};
        if (intel_bcs_queue_init_draw(&rcs_memory.queue,engine,&io,NULL,&ops,rcs_test_queue_prepare,base+1)!=INTEL_BCS_OK ||
            intel_bcs_context_activate_draw(native,1,&rcs_memory.buffers[8],&rcs_memory.buffers[6],&rcs_memory.buffers[7])!=INTEL_BCS_OK ||
            !intel_bcs_context_attach(native,&rcs_memory.queue,NULL)) goto close;
    }
    unsigned jobs=entry ? rcs_memory.program_count+1 : 9;
    unsigned buffers=rcs_memory.program_version==2 ? 9 : entry ? 8 : 7;
    for (unsigned job=0;job<jobs;job++) {
        if (queued && !job) continue; /* First queued draw initializes its context. */
        intel_rcs_queue_draw_t work={0};
        unsigned variant=job ? 1+(job-1)%8 : 0;
        const intel_rcs_program_test_case_t *test=entry ? (const void *)entry : NULL;
        const struct intel_d3d9_program *program=test ? (const void *)(test+1) : NULL;
        const intel_rcs_program_geometry_t *geometry=rcs_memory.program_version==2 ?
            (const void *)((const uint8_t *)program+test->program_bytes) : NULL;
        const uint8_t *mask=geometry ? (const uint8_t *)(geometry+1)+geometry->vertex_bytes : NULL;
        rcs_memory.token=base+job+1;
        intel_rcs_draw_t draw={0x200000,0x210000,0x220000,20480,29952,
            64,64,320,96,72,416,3+variant,5+variant,7+variant,11+variant,37-variant,29-variant};
        /* Replace all three backing allocations under the shared context's
         * acknowledged TLB gate. No context/ring/GGTT allocation changes. */
        intel_gpu_buffer_t *active=queued && !(job&1) ? rcs_memory.alternate : &rcs_memory.buffers[6];
        intel_gpu_buffer_t *inactive=queued && !(job&1) ? &rcs_memory.buffers[6] : rcs_memory.alternate;
        if (queued && job>1 && intel_bcs_context_rebind_draw(native,&active[2],&active[0],&active[1])!=INTEL_BCS_OK) goto close;
        uint32_t *source=rcs_memory.buffers[5].cpu,*destination=active[0].cpu;
        if (!job) {
            if (!intel_rcs_build(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
                rcs_memory.bindings[0].gpu_address,rcs_memory.bindings[1].gpu_address,
                rcs_memory.buffers[3].dma,0x2001,rcs_memory.token,&rcs_memory.descriptor)) goto close;
            rcs_memory.tail=INTEL_RCS_FLUSH_BYTES;
        } else {
            for (unsigned y=0;y<64;y++) for (unsigned x=0;x<80;x++)
                source[y*80+x]=0xff000000u|((x*3+job*7)&255)|(((y*5+job*11)&255)<<8)|
                    (((x+y+job*13)&255)<<16);
            memset(destination,0x5a,active[0].pages*4096);
            if (queued) {
                /* A stale destination translation must not silently pass. */
                memset(inactive[0].cpu,0xa6,inactive[0].pages*4096);
                if (intel_gpu_buffer_sync(&inactive[0],1)!=INTEL_GPU_BUFFER_OK) goto close;
            }
            if (program) {
                intel_rcs_pixel_draw_t pixel={draw.batch_gpu,0x230000,draw.destination_gpu,32768,
                    draw.destination_width,draw.destination_height,draw.destination_pitch,
                    draw.destination_x,draw.destination_y,draw.width,draw.height,test->dispatch_mask};
                memset(active[1].cpu,0,active[1].bytes);
                unsigned code_offset=queued ? (program->bytecode_offset+program->bytecode_bytes+4095)&~4095u : 0;
                if ((uint64_t)code_offset+program->code_bytes>active[1].bytes) goto close;
                memcpy((uint8_t *)active[1].cpu+code_offset,(const uint8_t *)program+program->code_offset,program->code_bytes);
                if (geometry) {
                    pixel.program_gpu=native->program_gpu+code_offset;
                    pixel.x=geometry->x; pixel.y=geometry->y;
                    pixel.width=geometry->width; pixel.height=geometry->height;
                    intel_rcs_primitive_draw_t primitive={pixel,0x250000,INTEL_RCS_TEST_VERTEX_BYTES,
                        0,16,geometry->vertices,geometry->topology,{0}};
                    memset(active[2].cpu,0,active[2].bytes);
                    memcpy(active[2].cpu,geometry+1,geometry->vertex_bytes);
                    work=(intel_rcs_queue_draw_t){primitive,*program};
                    work.program.code_offset=code_offset;
                    work.program.allocation_bytes=code_offset+program->code_bytes;
                    memcpy(active[1].cpu,&work.program,sizeof(work.program));
                    memcpy((uint8_t *)active[1].cpu+program->bytecode_offset,
                        (const uint8_t *)program+program->bytecode_offset,program->bytecode_bytes);
                } else if (!intel_rcs_pixel_build(rcs_memory.buffers[4].cpu,8192,&pixel,program,test->program_bytes)) goto close;
            } else if (!intel_rcs_draw_build(rcs_memory.buffers[4].cpu,8192,&draw)) goto close;
            if (!queued && !intel_rcs_append_batch(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
                rcs_memory.tail,draw.batch_gpu,rcs_memory.token,&rcs_memory.tail)) goto close;
        }
        if (!queued) for (unsigned i=0;i<buffers;i++)
            if (intel_gpu_buffer_sync(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto close;
        if (queued) {
            uint64_t fence=0;
            if (intel_bcs_queue_enqueue_draw(&rcs_memory.queue,1,native->queue_id,&work,&fence)!=INTEL_BCS_OK) goto close;
            rcs_memory.token=fence;
            /* The queue owns its complete metadata copy after enqueue. */
            memset(&work,0xa5,sizeof(work));
            intel_bcs_job_result_t completed;
            uint64_t deadline=idt_get_monotonic_ns()+5000000000ULL;
            for (;;) {
                if (intel_bcs_queue_result(&rcs_memory.queue,1,fence,false,&completed)!=INTEL_BCS_OK) goto close;
                if (completed.phase>=INTEL_BCS_JOB_COMPLETE) break;
                enum intel_bcs_status status=intel_bcs_queue_step(&rcs_memory.queue);
                if ((status!=INTEL_BCS_OK && status!=INTEL_BCS_PENDING && status!=INTEL_BCS_BUSY) ||
                    idt_get_monotonic_ns()>deadline) goto close;
                gt_hw_relax(NULL);
            }
            if (intel_bcs_queue_result(&rcs_memory.queue,1,fence,true,&completed)!=INTEL_BCS_OK ||
                completed.phase!=INTEL_BCS_JOB_COMPLETE || native->access_token) goto close;
        } else if (intel_cs_submit(&io,NULL,rcs_test_token,rcs_memory.descriptor,rcs_memory.token,
            rcs_memory.tail,engine)!=INTEL_CS_OK ||
            intel_cs_wait(&io,NULL,rcs_test_token,engine)!=INTEL_CS_OK) goto close;
        if (queued) {
            if (intel_gpu_buffer_sync(&native->state,1)!=INTEL_GPU_BUFFER_OK ||
                intel_gpu_buffer_sync(&inactive[0],1)!=INTEL_GPU_BUFFER_OK) goto close;
            for (unsigned i=0;i<inactive[0].pages*1024;i++)
                if (((uint32_t *)inactive[0].cpu)[i]!=0xa6a6a6a6) goto close;
        } else for (unsigned i=0;i<buffers;i++)
            if (intel_gpu_buffer_sync(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto close;
        uint32_t *saved=context+1024;
        if ((saved[5]&0x1ffffc)!=rcs_memory.tail || (saved[7]&0x1ffff8)!=rcs_memory.tail ||
            saved[9]!=(queued ? native->binding.gpu_address+INTEL_RCS_CONTEXT_BYTES : rcs_memory.bindings[1].gpu_address) ||
            (((uint64_t)saved[49]<<32)|saved[51])!=(queued ? native->tables.dma : rcs_memory.buffers[3].dma) ||
            mmio_read64((uint8_t *)context+INTEL_RCS_TOKEN_OFFSET)!=rcs_memory.token) goto close;
        rcs_memory.completed++;
        if (job) {
            for (unsigned i=0;i<active[0].pages*1024;i++) {
                unsigned x=i%104,y=i/104;
                uint32_t expected=0x5a5a5a5a;
                bool covered=geometry ? x<96 && y<72 && ((mask[(y*96+x)/8]>>((y*96+x)%8))&1) :
                    x>=draw.destination_x && x<draw.destination_x+draw.width &&
                    y>=draw.destination_y && y<draw.destination_y+draw.height;
                if (covered) {
                    unsigned sx=x-draw.destination_x+draw.source_x,sy=y-draw.destination_y+draw.source_y;
                    expected=0xff000000u|((sx*3+job*7)&255)|(((sy*5+job*11)&255)<<8)|
                        (((sx+sy+job*13)&255)<<16);
                    if (program) expected=test->expected_bgra;
                    rcs_memory.pixels++;
                }
                if (destination[i]!=expected) {
                    serial_puts("[IGFX] RCS pixel mismatch index="); serial_putdec(i);
                    serial_puts(" got="); serial_puthex(destination[i],8);
                    serial_puts(" expected="); serial_puthex(expected,8); serial_puts("\n");
                    goto close;
                }
            }
            for (unsigned y=0;y<64;y++) for (unsigned x=0;x<80;x++) {
                uint32_t expected=0xff000000u|((x*3+job*7)&255)|(((y*5+job*11)&255)<<8)|
                    (((x+y+job*13)&255)<<16);
                if (source[y*80+x]!=expected) goto close;
            }
            if (geometry) {
                const uint8_t *vertices=active[2].cpu,*input=(const void *)(geometry+1);
                for (unsigned i=0;i<INTEL_RCS_TEST_VERTEX_BYTES;i++)
                    if (vertices[i]!=(i<geometry->vertex_bytes ? input[i] : 0)) goto close;
            }
            rcs_memory.draws++;
            if (test) {
                serial_puts("[IGFX] native program case="); serial_putdec(job-1);
                serial_puts(" mask="); serial_putdec(test->dispatch_mask);
                serial_puts(" bgra="); serial_puthex(test->expected_bgra,8); serial_puts(" passed\n");
                entry+=sizeof(*test)+test->program_bytes+(geometry ? test->reserved : 0);
            }
        }
    }
    rcs_memory.passed=true;
close:
    if (rcs_memory.queue.initialized) {
        if (intel_bcs_queue_shutdown(&rcs_memory.queue)!=INTEL_BCS_OK)
            rcs_memory.passed=false;
    } else if (engine->started && intel_cs_close(&io,NULL,engine)!=INTEL_CS_OK) rcs_memory.passed=false;
}

#endif

void intel_gfx_irq(void)
{
#if INTEL_BCS_DIAGNOSTIC
    intel_bcs_irq_handle(&bcs_memory.irq);
#endif
}

bool intel_gfx_kexec_ready(void)
{
    if (!intel_gpu_executor_quiescent()) return false;
#if INTEL_BCS_DIAGNOSTIC
    if (capture_owner.self && !capture_owner.closed) return false;
    /* Token completion alone releases neither saved DMA nor a parked task. */
    return !__atomic_load_n(&bcs_service_busy,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&bcs_memory.worker.started,__ATOMIC_ACQUIRE) &&
        (!bcs_memory.irq.started || bcs_memory.irq.closed) &&
        (!bcs_memory.engine.referenced || bcs_memory.engine.retired) &&
        (!rcs_memory.engine.referenced || rcs_memory.engine.retired) &&
        !rcs_memory.pat.changed && !rcs_memory.cache.changed &&
        rcs_memory.ram.allocated_pages==rcs_memory.ram.freed_pages;
#else
    return true;
#endif
}

bool intel_gfx_capture_available(void)
{
#if INTEL_BCS_DIAGNOSTIC
    return native_memory.device && native_memory.gt.ready && native_memory.space.ready &&
        !__atomic_load_n(&bcs_service_busy,__ATOMIC_ACQUIRE) && intel_gfx_kexec_ready();
#else
    return false;
#endif
}
#if INTEL_BCS_DIAGNOSTIC
static bool gfx_capture_token(void *opaque,uint64_t gpu,uint64_t *value)
{
    (void)opaque;
    if (gpu>native_memory.device->memory.aperture_bytes-8 || (gpu&7)) return false;
    *value=mmio_read64(PHYS_TO_VIRT(native_memory.device->bar2_phys+gpu));
    return true;
}
static void gfx_capture_mapping(void *opaque,bool stopped)
{
    (void)opaque; native_memory.gt.ready=stopped;
}
#endif
bool intel_gfx_capture_clean(void)
{
#if INTEL_BCS_DIAGNOSTIC
    return (!capture_owner.self || capture_owner.closed) && !capture_ram.guard_failures &&
        capture_ram.allocated_pages==capture_ram.freed_pages;
#else
    return true;
#endif
}
bool intel_gfx_capture_memory_init(intel_gpu_memory_t *memory)
{
#if INTEL_BCS_DIAGNOSTIC
    if (!intel_gfx_capture_available() || !intel_gfx_capture_clean()) return false;
    memset(&capture_ram,0,sizeof(capture_ram));
    return intel_gpu_memory_init(memory,&gpu_ram,&capture_ram,64,32);
#else
    (void)memory; return false;
#endif
}
static NTSTATUS gfx_capture_open_engine(intel_gpu_jobs_t *jobs,bool render)
{
#if INTEL_BCS_DIAGNOSTIC
    if (!intel_gfx_capture_available()) return STATUS_NOT_SUPPORTED;
    if (__atomic_exchange_n(&bcs_service_busy,1,__ATOMIC_ACQ_REL)) return D3DKMT_STATUS_DEVICE_BUSY;
    memset(&capture_owner,0,sizeof(capture_owner));
    const intel_gpu_bcs_config_t config={.space=&native_memory.space,.allocator=gpu_ram,
        .allocator_context=&capture_ram,.io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax},
        .token_read=gfx_capture_token,.mapping_stopped=gfx_capture_mapping,
        .ggtt_limit=native_memory.device->memory.aperture_bytes,
        .source_gpu=0x100000000ULL,.destination_gpu=0x110000000ULL,
        .first_fence=0x6f736a0000000000ULL|(idt_get_monotonic_ns()&0xffffffff),.first_hardware_id=0x2001,
        .render=render,.batch_gpu=render ? 0x120000000ULL : 0,
        .program_gpu=render ? 0x130000000ULL : 0};
    const char *stage="owner";
    NTSTATUS status=intel_gpu_bcs_owner_init(&capture_owner,jobs,&config);
    uint32_t slots=0;
    if (status==0) {
        stage="capacity";
        enum intel_ggtt_bind_status available=intel_ggtt_space_capacity(&native_memory.space,
            render ? 23 : 3,1,config.ggtt_limit,&slots);
        if (available!=INTEL_GGTT_BIND_OK || !slots) status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if (slots>INTEL_BCS_QUEUE_CONTEXTS) slots=INTEL_BCS_QUEUE_CONTEXTS;
    if (status==0) { stage="reserve"; status=intel_gpu_bcs_owner_reserve(&capture_owner,slots,(render || jobs->contexts->fences) ? 64 : 8); }
    if (status==0) { stage="start"; status=intel_gpu_bcs_owner_start(&capture_owner); }
    serial_puts("[IGFX] native open render="); serial_putdec(render);
    serial_puts(" stage="); serial_puts(stage);
    serial_puts(" status="); serial_puthex((uint32_t)status,8);
    serial_puts(" slots="); serial_putdec(capture_owner.count);
    serial_puts(" engine="); serial_putdec(capture_owner.engine.status);
    serial_puts(" reg/value="); serial_puthex(capture_owner.engine.fault_reg,8);
    serial_puts("/"); serial_puthex(capture_owner.engine.last_value,8);
    serial_puts(" cache="); serial_putdec(capture_owner.cache.changed);
    if (capture_owner.count) {
        intel_bcs_context_t *last=&capture_owner.slots[capture_owner.count-1].native;
        serial_puts(" last_state/tables/batch/bind="); serial_putdec(last->state.ready);
        serial_puts("/"); serial_putdec(last->tables.ready);
        serial_puts("/"); serial_putdec(last->batch.ready);
        serial_puts("/"); serial_putdec(last->binding.phase);
    }
    serial_puts("\n");
    return status;
#else
    (void)jobs; (void)render; return STATUS_NOT_SUPPORTED;
#endif
}
NTSTATUS intel_gfx_capture_open(intel_gpu_jobs_t *jobs)
{
    return gfx_capture_open_engine(jobs,false);
}
NTSTATUS intel_gfx_capture_open_render(intel_gpu_jobs_t *jobs)
{
    return gfx_capture_open_engine(jobs,true);
}
NTSTATUS intel_gfx_capture_close(void)
{
#if INTEL_BCS_DIAGNOSTIC
    NTSTATUS status=capture_owner.self ? intel_gpu_bcs_owner_close(&capture_owner) : STATUS_SUCCESS;
    if (status==0) __atomic_store_n(&bcs_service_busy,0,__ATOMIC_RELEASE);
    return status;
#else
    return STATUS_SUCCESS;
#endif
}

int intel_gfx_bcs_service_test(void)
{
#if INTEL_BCS_DIAGNOSTIC
    extern uint32_t smp_current_cpu(void);
    if (!native_memory.device || !native_memory.space.ready) return 2;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (smp_current_cpu()!=0 || !(flags&(1ULL<<9)) ||
        __atomic_exchange_n(&bcs_service_busy,1,__ATOMIC_ACQ_REL)) return 1;
    bool reusable=native_memory.gt.ready && !bcs_memory.worker.started &&
        (!bcs_memory.irq.started || bcs_memory.irq.closed) &&
        (!bcs_memory.engine.referenced || bcs_memory.engine.retired) &&
        !bcs_memory.queue.registered && !bcs_memory.queue.wakeup && !bcs_memory.queue.notifying &&
        !bcs_memory.resources.registered && !bcs_memory.resources.pages &&
        !bcs_memory.hwsp.live && bcs_memory.hwsp_binding.phase==INTEL_GGTT_EMPTY &&
        bcs_memory.ram.allocated_pages==bcs_memory.ram.freed_pages;
    for (unsigned i=0;i<2;i++)
        reusable=reusable && !bcs_memory.context[i].native.state.live &&
            !bcs_memory.context[i].native.tables.live;
    bool passed=false;
    if (reusable) {
        memset(&bcs_memory,0,sizeof(bcs_memory));
        passed=intel_gfx_bcs_test(true);
        if (passed) passed=intel_gfx_fence_test();
    }
    __atomic_store_n(&bcs_service_busy,0,__ATOMIC_RELEASE);
    return passed ? 0 : 1;
#else
    return 2;
#endif
}

static int intel_gfx_rcs_test(const uint8_t *program_cases,uint32_t program_count,uint32_t program_version)
{
#if INTEL_BCS_DIAGNOSTIC
    extern uint32_t smp_current_cpu(void);
    if (!native_memory.device || !native_memory.space.ready) return 2;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (smp_current_cpu()!=0 || !(flags&(1ULL<<9)) ||
        __atomic_exchange_n(&bcs_service_busy,1,__ATOMIC_ACQ_REL)) return 1;
    bool passed=false,restored=false;
    const char *stage="admission";
    uint64_t before=14695981039346656037ULL,after=before;
    const intel_gfx_state_t *d=native_memory.device;
    const intel_gt_io_t io={gt_hw_read,gt_hw_write,gt_hw_now,gt_hw_relax};
    if (!native_memory.gt.ready || bcs_memory.worker.started ||
        (bcs_memory.irq.started && !bcs_memory.irq.closed) ||
        (bcs_memory.engine.referenced && !bcs_memory.engine.retired) ||
        (rcs_memory.engine.referenced && !rcs_memory.engine.retired) ||
        rcs_memory.pat.changed || rcs_memory.cache.changed ||
        rcs_memory.ram.allocated_pages!=rcs_memory.ram.freed_pages ||
        !ggtt_test_preflight(d)) goto done;
    memset(&rcs_memory,0,sizeof(rcs_memory));
    rcs_memory.program_cases=program_cases; rcs_memory.program_count=program_count; rcs_memory.program_version=program_version;
    for (uint64_t off=0;off<d->memory.ggtt_bytes;off+=8)
        before=(before^mmio_read64((volatile void *)(native_memory.mmio+INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
    stage="owned render context";
    bool shared=program_version==2;
    bool sampled=program_version>=3;
    const unsigned buffers=shared || sampled ? 9 : program_count ? 8 : 7;
    const unsigned pages[9]={22,1,1,4,sampled ? 4 : 2,5,8,shared ? 33 : program_version>=4 ? 32 : 16,2};
    for (unsigned i=0;i<buffers;i++) {
        if (shared && (i<2 || i==3 || i==4)) continue;
        if (intel_gpu_buffer_create(&rcs_memory.buffers[i],&gpu_ram,&rcs_memory.ram,1,
            (uint64_t)pages[i]*4096)!=INTEL_GPU_BUFFER_OK) goto release;
        if (shared && i>=6) continue; /* The shared context owns residency. */
        enum intel_gpu_buffer_status pin=intel_gpu_buffer_pin(&rcs_memory.buffers[i],1);
        if (pin!=INTEL_GPU_BUFFER_OK) goto release;
        rcs_memory.buffer_pins|=1u<<i;
    }
    uint32_t used;
    const intel_ppgtt_range_t ranges[]={
        {0x200000,rcs_memory.buffers[4].dma,sampled ? INTEL_RCS_SAMPLED_BYTES : 8192,false},
        {0x210000,rcs_memory.buffers[5].dma,20480,false},
        {0x220000,rcs_memory.buffers[6].dma,32768,true},
        {0x230000,rcs_memory.buffers[7].dma,program_version>=4 ? 2*INTEL_RCS_PIXEL_MAX_CODE : INTEL_RCS_PIXEL_MAX_CODE,false},
        {0x250000,rcs_memory.buffers[8].dma,INTEL_RCS_TEST_VERTEX_BYTES,false}
    };
    if (shared) {
        for (unsigned i=0;i<3;i++)
            if (intel_gpu_buffer_create(&rcs_memory.alternate[i],&gpu_ram,&rcs_memory.ram,1,
                (uint64_t)pages[i+6]*4096)!=INTEL_GPU_BUFFER_OK) goto release;
        if (!intel_bcs_context_reserve_draw(&rcs_memory.native,&native_memory.space,&gpu_ram,&rcs_memory.ram,
            1,0x2001,0x250000,0x220000,4,d->memory.aperture_bytes,0x200000,0x300000)) goto release;
    } else if (!intel_ppgtt_build_ranges(rcs_memory.buffers[3].cpu,16384,rcs_memory.buffers[3].dma,ranges,
        sampled ? 5 : program_count ? 4 : 3,&used)) goto release;
    memset((uint8_t *)rcs_memory.buffers[2].cpu+0x370,0xff,48);
    for (unsigned i=0;i<buffers;i++)
        if (rcs_memory.buffers[i].live && intel_gpu_buffer_sync(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto release;
    stage="render GGTT bindings";
    for (unsigned i=0,original=0;i<3;i++) {
        if (shared && i<2) continue;
        uint64_t physical[22];
        for (unsigned j=0;j<pages[i];j++) physical[j]=rcs_memory.buffers[i].dma+(uint64_t)j*4096;
        if (intel_ggtt_bind(&native_memory.space,&rcs_memory.bindings[i],physical,pages[i],1,
            d->memory.aperture_bytes,rcs_memory.originals+original)!=INTEL_GGTT_BIND_OK) goto release;
        original+=pages[i];
        if (intel_ggtt_pin(&native_memory.space,&rcs_memory.bindings[i],rcs_memory.bindings[i].cookie)!=INTEL_GGTT_BIND_OK)
            goto release;
        rcs_memory.pins|=1u<<i;
    }
    stage="native render cache policy";
    if (!intel_ppgtt_pat_acquire(&io,NULL,&rcs_memory.pat) ||
        !intel_rcs_cache_acquire(&io,NULL,&rcs_memory.cache)) goto release;
    stage=sampled ? "native sampled programs" : shared ? "shared primitive context and three-resource rebind" : "textured rectangles and saved render context reuse";
    native_memory.gt.ready=false;
    int task=smp_submit_any(sampled ? rcs_sampled_test_work : rcs_test_work,NULL,NULL);
    if (task>=0) smp_wait(task);
    else if (sampled) rcs_sampled_test_work(NULL,NULL);
    else rcs_test_work(NULL,NULL);
    if (rcs_memory.engine.referenced && !rcs_memory.engine.retired) goto done;
    native_memory.gt.ready=true;
    passed=rcs_memory.passed && rcs_memory.completed==(program_version==2 ? program_count : program_count ? program_count+1 : 9) &&
        rcs_memory.draws==(program_count ? program_count : 8) &&
        (!shared || (rcs_memory.native.rebindings==program_count-1 && rcs_memory.engine.tlb_invalidations==program_count-1));
release:
    if (!intel_rcs_cache_restore(&io,NULL,&rcs_memory.cache) ||
        (rcs_memory.pat.started && !intel_ppgtt_pat_restore(&io,NULL,&rcs_memory.pat))) {
        native_memory.gt.ready=false;
        goto done;
    }
    if (shared && !intel_bcs_context_destroy(&rcs_memory.native)) goto done;
    for (unsigned i=3;i--;) {
        intel_ggtt_binding_t *binding=&rcs_memory.bindings[i];
        if (rcs_memory.pins&(1u<<i)) {
            if (intel_ggtt_unpin(&native_memory.space,binding,binding->cookie)!=INTEL_GGTT_BIND_OK) goto done;
            rcs_memory.pins&=~(1u<<i);
        }
        if (binding->phase!=INTEL_GGTT_EMPTY &&
            intel_ggtt_unbind(&native_memory.space,binding,binding->cookie)!=INTEL_GGTT_BIND_OK) goto done;
    }
    for (unsigned i=0;i<buffers;i++) {
        if (rcs_memory.buffer_pins&(1u<<i)) {
            enum intel_gpu_buffer_status pin=intel_gpu_buffer_unpin(&rcs_memory.buffers[i],1);
            if (pin!=INTEL_GPU_BUFFER_OK) goto done;
            rcs_memory.buffer_pins&=~(1u<<i);
        }
        if (rcs_memory.buffers[i].started &&
            intel_gpu_buffer_close(&rcs_memory.buffers[i],1)!=INTEL_GPU_BUFFER_OK) goto done;
    }
    for (unsigned i=0;i<3;i++)
        if (rcs_memory.alternate[i].started && intel_gpu_buffer_close(&rcs_memory.alternate[i],1)!=INTEL_GPU_BUFFER_OK) goto done;
    for (uint64_t off=0;off<d->memory.ggtt_bytes;off+=8)
        after=(after^mmio_read64((volatile void *)(native_memory.mmio+INTEL_GGTT_MMIO_OFFSET+off)))*1099511628211ULL;
    restored=before==after;
done:
    passed=passed && restored && !rcs_memory.pins && !rcs_memory.buffer_pins &&
        !rcs_memory.cache.changed && !rcs_memory.pat.changed && !rcs_memory.ram.guard_failures &&
        rcs_memory.ram.allocated_pages==rcs_memory.ram.freed_pages;
    serial_puts(program_count ? "[IGFX] native RCS program passed=" : "[IGFX] native RCS passed="); serial_putdec(passed);
    serial_puts(" stage="); serial_puts(stage);
    serial_puts(" jobs="); serial_putdec(rcs_memory.completed);
    serial_puts("/"); serial_putdec(rcs_memory.engine.submitted_jobs);
    serial_puts(" draws="); serial_putdec(rcs_memory.draws);
    serial_puts(" pixels="); serial_putdec(rcs_memory.pixels);
    serial_puts(" rebindings="); serial_putdec(rcs_memory.native.rebindings);
    serial_puts(" tlb_invalidations="); serial_putdec(rcs_memory.engine.tlb_invalidations);
    serial_puts(" cache_restored="); serial_putdec(!rcs_memory.cache.changed && !rcs_memory.pat.changed);
    serial_puts(" status="); serial_putdec(rcs_memory.engine.status);
    serial_puts(" retired="); serial_putdec(rcs_memory.engine.retired);
    serial_puts(" restored="); serial_putdec(restored);
    serial_puts(" pages="); serial_putdec(rcs_memory.ram.freed_pages);
    serial_puts("/"); serial_putdec(rcs_memory.ram.allocated_pages);
    serial_puts(" guards="); serial_putdec(rcs_memory.ram.guard_failures);
    serial_puts(" cpu="); serial_putdec(rcs_memory.cpu);
    serial_puts(" token="); serial_puthex(rcs_memory.engine.observed_token,16);
    serial_puts(" expected="); serial_puthex(rcs_memory.token,16);
    serial_puts(" head/tail="); serial_puthex(rcs_memory.engine.head,8);
    serial_puts("/"); serial_puthex(rcs_memory.engine.tail,8);
    serial_puts(" csb="); serial_puthex(rcs_memory.engine.csb_status,8);
    serial_puts("/"); serial_puthex(rcs_memory.engine.csb_id,8);
    serial_puts(" error="); serial_puthex(rcs_memory.engine.engine_error,8);
    serial_puts(" reg="); serial_puthex(rcs_memory.engine.fault_reg,8);
    serial_puts(" value="); serial_puthex(rcs_memory.engine.last_value,8);
    serial_puts("\n");
    __atomic_store_n(&bcs_service_busy,0,__ATOMIC_RELEASE);
    return passed ? 0 : 1;
#else
    (void)program_cases; (void)program_count; (void)program_version;
    return 2;
#endif
}

int intel_gfx_rcs_service_test(void)
{
    return intel_gfx_rcs_test(NULL,0,0);
}

static bool rcs_program_geometry_validate(const uint8_t *data,uint32_t bytes)
{
    if (bytes<sizeof(intel_rcs_program_geometry_t)) return false;
    const intel_rcs_program_geometry_t *g=(const void *)data;
    if (g->reserved[0] || g->reserved[1] || g->topology<INTEL_RCS_TRIANGLE_LIST ||
        g->topology>INTEL_RCS_TRIANGLE_FAN || g->vertices<3 || g->vertices>INTEL_RCS_TEST_VERTEX_BYTES/16 ||
        (g->topology==INTEL_RCS_TRIANGLE_LIST && g->vertices%3) || g->vertex_bytes!=g->vertices*16 ||
        g->mask_bytes!=INTEL_RCS_TEST_MASK_BYTES || bytes!=sizeof(*g)+g->vertex_bytes+g->mask_bytes ||
        !g->width || g->width>96 || !g->height || g->height>72 || g->x>96-g->width || g->y>72-g->height)
        return false;
    /* The fixture uses finite screen coordinates within the Gen9 guard band.
     * This check does not transform vertices; the GPU consumes original bits. */
    const uint32_t *v=(const void *)(g+1);
    for (unsigned i=0;i<g->vertices;i++)
        if ((v[i*4]&0x7fffffffu)>0x46800000u || (v[i*4+1]&0x7fffffffu)>0x46800000u ||
            v[i*4+2]>0x3f800000u || v[i*4+3]!=0x3f800000u) return false;
    return true;
}

int intel_gfx_rcs_program_file(const char *path)
{
#if INTEL_BCS_DIAGNOSTIC
    extern void *osfs2_find(const char *);
    extern uint64_t osfs2_file_size(void *),osfs2_file_revision(void *);
    extern int osfs2_file_retain(void *),osfs2_read(void *,uint64_t,void *,uint64_t);
    extern void osfs2_file_release(void *);
    extern void *kmalloc(uint64_t);
    extern void kfree(void *);
    if (!path || !*path) return 1;
    void *file=osfs2_find(path);
    if (!file || osfs2_file_retain(file)<0) return 1;
    uint64_t bytes=osfs2_file_size(file),revision=osfs2_file_revision(file);
    void *data=NULL;
    int result=1;
    if (bytes<sizeof(intel_rcs_program_test_header_t) || bytes>INTEL_RCS_PROGRAM_TEST_MAX_BYTES ||
        (revision&1)) goto release;
    data=kmalloc(bytes);
    if (!data || osfs2_read(file,0,data,bytes)!=(int)bytes || osfs2_file_revision(file)!=revision) goto release;
    const intel_rcs_program_test_header_t *header=data;
    if (header->magic!=INTEL_RCS_PROGRAM_TEST_MAGIC || header->version<1 || header->version>5 || header->bytes!=bytes ||
        !header->cases || header->cases>INTEL_RCS_PROGRAM_TEST_MAX_CASES) goto release;
    uint64_t offset=sizeof(*header);
    for (unsigned i=0;i<header->cases;i++) {
        if (bytes-offset<sizeof(intel_rcs_program_test_case_t)) goto release;
        const intel_rcs_program_test_case_t *test=(const void *)((const uint8_t *)data+offset);
        offset+=sizeof(*test);
        const struct intel_d3d9_program *program=(const void *)((const uint8_t *)data+offset);
        if ((header->version==1 && test->reserved) || test->program_bytes>bytes-offset ||
            (header->version<3 && !intel_rcs_pixel_validate(program,test->program_bytes)) ||
            test->program_bytes<sizeof(*program) || !test->dispatch_mask ||
            (test->dispatch_mask&~program->simd_mask)) goto release;
        offset+=test->program_bytes;
        if (header->version==2) {
            if (test->reserved>bytes-offset ||
                !rcs_program_geometry_validate((const uint8_t *)data+offset,test->reserved)) goto release;
            offset+=test->reserved;
        } else if (header->version>=3) {
            if (test->expected_bgra || test->reserved>bytes-offset) goto release;
            bool valid=header->version==5 ?
                intel_rcs_indexed_test_validate(program,test->program_bytes,(const uint8_t *)data+offset,
                    test->reserved,test->dispatch_mask) : header->version==4 ?
                intel_rcs_vertex_test_validate(program,test->program_bytes,(const uint8_t *)data+offset,
                    test->reserved,test->dispatch_mask) :
                intel_rcs_sampled_test_validate(program,test->program_bytes,(const uint8_t *)data+offset,
                    test->reserved,test->dispatch_mask);
            if (!valid) goto release;
            offset+=test->reserved;
        }
    }
    if (offset!=bytes) goto release;
    /* The DMA allocations own code copies. The input file can be released
     * after the synchronous worker returns, even if engine retirement fails. */
    result=intel_gfx_rcs_test((const uint8_t *)(header+1),header->cases,header->version);
release:
    osfs2_file_release(file);
    if (data) kfree(data);
    return result;
#else
    (void)path;
    return 2;
#endif
}

static bool ggtt_test_preflight(const intel_gfx_state_t *d)
{
    if (!d || d->device_id != 0x3e9b || d->vendor_id != 0x8086 ||
        d->bus || d->dev != 2 || d->func || !d->memory_valid ||
        !d->gop_in_aperture || !d->gop_sample_present ||
        !d->fence_snapshot_valid || d->fence_valid_mask ||
        d->memory.aperture_bytes < 128 * 1024 * 1024ULL ||
        !ggtt_mapping_uc(d->bar0_phys, INTEL_GGTT_BAR_BYTES)) return false;
    /* Leave at least one 2 MiB region between the test and aperture end;
     * never remap a CPU large page that contains any GOP byte. */
    uint64_t start = d->memory.aperture_bytes - 4 * 1024 * 1024;
    uint64_t gop_end = d->gop_ggtt_offset + (uint64_t)d->gop_pitch_bytes * d->gop_height;
    if ((start & 0x1fffff) || gop_end > start) return false;
    native_memory.first = (uint32_t)(start >> 12);
    uint32_t id, command;
    if (!pci_diag_read32(0, 2, 0, 0, &id) || id != 0x3e9b8086u ||
        !pci_diag_read32(0, 2, 0, 4, &command) || !(command & 2)) return false;
    for (unsigned i = 0; i < 32; i++) {
        uint64_t value = mmio_read64((volatile void *)(native_memory.mmio + 0x100000 + 8*i));
        if (value != d->fence[i] || (value & 1)) return false;
    }
    pci_dev_t device = {.bus = 0, .dev = 2, .vendor_id = 0x8086,
        .device_id = 0x3e9b, .class_code = 3,
        .bar = {d->bar0_phys, 0, d->bar2_phys}};
    static dma_diag_t dma;
    dma_diag_prepare(&device, &dma);
    const dma_iommu_sample_t *m = &dma.sample[0].iommu;
    return dma.prepared && dma.iommu_ready && !dma.path_note &&
        dma.sample[0].captured && m->valid && !m->changed &&
        !(m->gsts & 0x80000000u) && !(m->pmen & 0x80000001u) && !m->target;
}

static bool ggtt_test_find_window(void)
{
    const intel_gfx_state_t *d = native_memory.device;
    uint64_t gop_end = d->gop_ggtt_offset + (uint64_t)d->gop_pitch_bytes * d->gop_height;
    /* Preflight validated at least 128 MiB of aperture. Keep the low 64 MiB
     * and all GOP CPU large pages reserved, as well as the final 2 MiB. */
    for (uint64_t start = d->memory.aperture_bytes - 0x400000;
         start >= 0x4000000 && start >= gop_end; start -= 0x200000) {
        bool stable = true, available = false;
        for (unsigned page = 0; page < 512; page++) {
            uint32_t reg;
            if (!intel_ggtt_pte_offset(&d->memory, start + 4096*page, &reg)) return false;
            volatile void *address = (volatile void *)(native_memory.mmio + reg);
            uint64_t value = mmio_read64(address);
            if (value == UINT64_MAX || mmio_read64(address) != value) {
                stable = false;
                break;
            }
            if (!(value & 1)) available = true;
        }
        if (stable && available) {
            native_memory.first = (uint32_t)(start >> 12);
            return true;
        }
    }
    return false;
}

void intel_gfx_memory_test(const intel_gfx_state_t *d)
{
    if (native_memory.attempted) return;
    native_memory.attempted = true;
    native_memory.device = d;
    native_memory.mmio = d ? PHYS_TO_VIRT(d->bar0_phys) : NULL;
    const char *stage = "preflight";
    bool passed = false, restored = false;
    uint64_t before_hash = 14695981039346656037ULL, after_hash = before_hash;
    uint64_t gpu_address = 0;
    unsigned rounds = 0, present = 0, unknown = 0;
    enum intel_ggtt_bind_status bind_status = INTEL_GGTT_BIND_INVALID;
    if (!ggtt_test_preflight(d)) goto done;
    uint32_t a = 1, b, c, e;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(e));
    if (!(e & (1u << 19)) || ((b >> 8) & 255) * 8 != 64) goto done;
    /* UC pages select PAT3, ordinary direct-mapped RAM selects PAT0. */
    __asm__ volatile ("rdmsr" : "=a"(a), "=d"(e) : "c"(0x277));
    if ((a & 255) != 6 || (a >> 24) != 0) goto done;
    const intel_gt_io_t gt_io = {gt_hw_read, gt_hw_write, gt_hw_now, gt_hw_relax};
    stage = "GT takeover";
    if (intel_gt_takeover(0x3e9b8086, &gt_io, NULL, &native_memory.gt) != INTEL_GT_OK)
        goto done;
    /* Recheck platform after reset before authorizing any table mutation. */
    stage = "post-reset preflight";
    if (!ggtt_test_preflight(d)) goto done;
    stage = "firmware inventory";
    for (uint64_t off = 0; off < d->memory.ggtt_bytes; off += 8) {
        uint64_t pte = mmio_read64((volatile void *)(native_memory.mmio + INTEL_GGTT_MMIO_OFFSET + off));
        /* Unknown/all-ones entries elsewhere are reserved, not reusable.
         * They need not prevent selecting a separately verified window. */
        unknown += pte == UINT64_MAX;
        before_hash = (before_hash ^ pte) * 1099511628211ULL;
        present += (unsigned)(pte & 1);
    }
    stage = "verified window";
    if (!ggtt_test_find_window()) goto done;
    const intel_ggtt_io_t io = {ggtt_hw_read, ggtt_hw_write, ggtt_hw_sync};
    stage = "GGTT ownership";
    bind_status = intel_ggtt_space_init(&native_memory.space, &d->memory,
        native_memory.first, 512, native_memory.bitmap, sizeof(native_memory.bitmap), &io, NULL);
    if (bind_status != INTEL_GGTT_BIND_OK) goto done;
    stage = "CPU aperture mapping";
    uint64_t aperture = d->bar2_phys + ((uint64_t)native_memory.first << 12);
    if (paging_map_mmio(aperture, 0x200000) || !ggtt_mapping_uc(aperture, 0x200000)) goto done;
    stage = "owned RAM";
    native_memory.ram_phys = (uint64_t)mem_alloc_pages(4);
    uint64_t ram_phys = native_memory.ram_phys;
    if (!ram_phys) goto done;
    /* PMM gives ordinary owned RAM, not a GPU address. Reject aliasing any
     * stolen or BAR interval; an invalid allocator result is quarantined. */
    if ((ram_phys & 4095) || ram_phys >= INTEL_GGTT_PHYS_LIMIT - 16384 ||
        (ram_phys < d->memory.stolen_base + d->memory.stolen_bytes &&
         ram_phys + 16384 > d->memory.table_stolen_base) ||
        (ram_phys < d->bar0_phys + INTEL_GGTT_BAR_BYTES && ram_phys + 16384 > d->bar0_phys) ||
        (ram_phys < d->bar2_phys + d->memory.aperture_bytes && ram_phys + 16384 > d->bar2_phys))
        goto done;
    volatile uint64_t *ram = PHYS_TO_VIRT(ram_phys);
    for (unsigned i = 0; i < 4; i++) {
        uint64_t va = (uint64_t)ram + 4096*i, flags, size;
        if (paging_translate_in_cr3(paging_get_kernel_cr3(), va) != ram_phys + 4096*i ||
            paging_query_mapping_in_cr3(paging_get_kernel_cr3(), va, &flags, &size) ||
            (flags & 0x1b) != 3 || (flags & 4) ||
            (flags & (size == 4096 ? 0x80 : 0x1000)))
            goto release;
    }
    for (unsigned i = 0; i < 2048; i++) ram[i] = 0x6a09e667f3bcc909ULL ^ (uint64_t)i * 0x9e3779b97f4a7c15ULL;
    ggtt_test_cache_flush((void *)ram, 4);
    for (unsigned round = 0; round < 2; round++) {
        uint64_t dma_page = ram_phys + (round + 1) * 4096;
        stage = "bind";
        bind_status = intel_ggtt_bind(&native_memory.space, &native_memory.binding,
            &dma_page, 1, 1, d->memory.aperture_bytes, &native_memory.original);
        if (bind_status != INTEL_GGTT_BIND_OK) goto release;
        uint64_t address = native_memory.binding.gpu_address;
        if (round && address != gpu_address) { stage = "remap address"; goto release; }
        gpu_address = address;
        volatile uint64_t *window = PHYS_TO_VIRT(d->bar2_phys + address);
        stage = "aperture read";
        for (unsigned i = 0; i < 512; i++)
            if (window[i] != ram[(round + 1) * 512 + i]) goto release;
        stage = "aperture write";
        for (unsigned i = 0; i < 512; i++)
            window[i] = ~(0x6a09e667f3bcc909ULL ^ (uint64_t)((round + 1)*512+i) * 0x9e3779b97f4a7c15ULL);
        __asm__ volatile ("mfence" ::: "memory");
        ggtt_test_cache_flush((void *)ram, 4);
        stage = "RAM and guards";
        for (unsigned i = 0; i < 2048; i++) {
            uint64_t want = 0x6a09e667f3bcc909ULL ^ (uint64_t)i * 0x9e3779b97f4a7c15ULL;
            if (i >= 512 && i < (round + 2) * 512) want = ~want;
            if (ram[i] != want) goto release;
        }
        stage = "unbind";
        bind_status = intel_ggtt_unbind(&native_memory.space, &native_memory.binding,
                                        native_memory.binding.cookie);
        if (bind_status != INTEL_GGTT_BIND_OK) goto release;
        rounds++;
    }
    passed = true;
release:
    if (native_memory.binding.phase != INTEL_GGTT_EMPTY)
        bind_status = intel_ggtt_unbind(&native_memory.space, &native_memory.binding,
                                        native_memory.binding.cookie);
    if (native_memory.binding.phase == INTEL_GGTT_EMPTY) {
        for (uint64_t off = 0; off < d->memory.ggtt_bytes; off += 8) {
            uint64_t pte = mmio_read64((volatile void *)(native_memory.mmio + INTEL_GGTT_MMIO_OFFSET + off));
            after_hash = (after_hash ^ pte) * 1099511628211ULL;
        }
        restored = before_hash == after_hash;
        if (restored) {
            mem_free_pages((void *)ram_phys, 4);
            native_memory.ram_phys = 0;
        }
    }
    passed = passed && restored;
done:
    serial_puts("[IGFX] native GGTT test passed="); serial_putdec(passed);
    serial_puts(" stage="); serial_puts(stage);
    serial_puts(" rounds="); serial_putdec(rounds);
    serial_puts(" restored="); serial_putdec(restored);
    serial_puts(" present="); serial_putdec(present);
    serial_puts(" unknown="); serial_putdec(unknown);
    serial_puts(" gpu="); serial_puthex(gpu_address, 16);
    serial_puts(" retained_ram="); serial_puthex(native_memory.ram_phys, 16);
    serial_puts(" bind="); serial_putdec(bind_status);
    serial_puts("\n[IGFX] GT takeover status="); serial_putdec(native_memory.gt.status);
    serial_puts(" reset="); serial_putdec(native_memory.gt.reset_issued);
    serial_puts(" ready="); serial_putdec(native_memory.gt.ready);
    serial_puts(" cleanup="); serial_putdec(native_memory.gt.cleanup_ok);
    serial_puts(" wake_held="); serial_putdec(native_memory.gt.wake_owned);
    serial_puts(" reg="); serial_puthex(native_memory.gt.fault_reg, 8);
    serial_puts(" value="); serial_puthex(native_memory.gt.last_value, 8);
    serial_puts(" guc="); serial_puthex(native_memory.gt.guc_status, 8);
    serial_puts(" dma="); serial_puthex(native_memory.gt.dma_control, 8);
    serial_puts("\n");
#if INTEL_BCS_DIAGNOSTIC
    if (passed) intel_gfx_bcs_test(false);
#endif
}
