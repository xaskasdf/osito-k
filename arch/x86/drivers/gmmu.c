/*
 * OsitoK x86-64 — GPU MMU Page Tables (X38)
 *
 * Minimal GMMU identity-map for compute kernel dispatch.
 * Without GMMU, the compute engine cannot access VRAM or system memory
 * (all QMD addresses are GPU virtual addresses that pass through GMMU).
 *
 * Page table format: GP100+ MMU v2 (Volta/Turing/Ampere)
 *   5 levels: PDB(2b) → PD2(9b) → PD1(9b) → PD0(8b,dual) → SPT(9b)
 *   4KB small pages, 64KB big pages (only small pages used here)
 *   49-bit virtual address space
 *
 * PTE format (8 bytes, leaf level):
 *   data = (phys_addr >> 4) | flags
 *   Bit 0:     VALID
 *   Bits 2:1:  APERTURE (0=VRAM, 2=SYS_COHERENT)
 *   Bit 3:     VOL (bypass L2)
 *   Bits 63:8: Frame number (phys_addr >> 12)
 *
 * PDE format (8 bytes, PD1/PD2/PDB):
 *   data = (table_addr >> 4) | flags  (same as PTE)
 *
 * PD0 format (16 bytes dual, only small PDE used):
 *   small_pde = table_addr | flags (NOT shifted!)
 *   big_pde = 0 (disabled)
 *
 * Instance block PDB pointer (offset 0x200):
 *   Bits 1:0:   TARGET (0=VID_MEM, 2=SYS_COHERENT)
 *   Bit  2:     VOL
 *   Bits 31:12: ADDR_LO (PDB phys bits 31:12)
 *   Bits 51:32: ADDR_HI (PDB phys bits 51:32)
 *
 * Reference: nouveau vmmgp100.c, NVIDIA dev_ram.ref.txt,
 *            GP100 MMU format documentation.
 */

#include "../include/types.h"
#include "gpu.h"
#include "sass.h"

/* ── External Functions ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* ── GMMU Constants ──────────────────────────────────────── */

/* Page table entry flags */
#define PTE_VALID           (1ULL << 0)
#define PTE_APERTURE_VRAM   (0ULL << 1)
#define PTE_APERTURE_SYS    (2ULL << 1)
#define PTE_VOL             (1ULL << 3)

/* VRAM PTE: valid, aperture=VRAM, cached */
#define PTE_VRAM   (PTE_VALID | PTE_APERTURE_VRAM)
/* Sysmem PTE: valid, aperture=SYS_COHERENT, volatile (bypass L2) */
#define PTE_SYS    (PTE_VALID | PTE_APERTURE_SYS | PTE_VOL)

/* PDE flags (same encoding, for upper levels) */
#define PDE_VALID           (1ULL << 0)
#define PDE_APERTURE_SYS    (2ULL << 1)

/* PDE for tables in system RAM */
#define PDE_SYS    (PDE_VALID | PDE_APERTURE_SYS)

/* PD0 small PDE flags (NOT shifted, direct address) */
#define PD0_VALID           (1ULL << 0)
#define PD0_APERTURE_SYS    (2ULL << 1)
#define PD0_SYS    (PD0_VALID | PD0_APERTURE_SYS)

/* Page table sizes */
#define PDB_ENTRIES   4     /* 2 bits */
#define PD2_ENTRIES   512   /* 9 bits */
#define PD1_ENTRIES   512   /* 9 bits */
#define PD0_ENTRIES   256   /* 8 bits, 16B each (dual PDE) */
#define SPT_ENTRIES   512   /* 9 bits, 8B each */

/* VA index extraction */
#define PDB_IDX(va)  (((va) >> 47) & 0x3)
#define PD2_IDX(va)  (((va) >> 38) & 0x1FF)
#define PD1_IDX(va)  (((va) >> 29) & 0x1FF)
#define PD0_IDX(va)  (((va) >> 21) & 0xFF)
#define SPT_IDX(va)  (((va) >> 12) & 0x1FF)

/* Each SPT covers 2MB (512 × 4KB) */
#define SPT_COVERAGE  (512ULL * 4096)

/* Instance block PDB offset */
#define RAMIN_PDB_OFFSET  0x200

/* Instance block TARGET for sysmem */
#define RAMIN_TARGET_SYS_COHERENT  2

/* ── GMMU State ──────────────────────────────────────────── */

#define MAX_SPT_PAGES  512  /* Enough to map 1GB of VRAM */
#define MAX_PD0_PAGES    4  /* Each PD1 entry → PD0, each covers 512MB */

typedef struct {
    uint64_t *pdb;           /* Page Directory Base: 4 entries × 8B */
    uint64_t *pd2;           /* PD2: 512 entries × 8B = 4KB */
    uint64_t *pd1;           /* PD1: 512 entries × 8B = 4KB */
    uint64_t *pd0[MAX_PD0_PAGES]; /* PD0 pages, one per PD1 entry used */
    uint32_t  pd0_count;
    uint64_t *spt[MAX_SPT_PAGES]; /* SPT pages (4KB each) */
    uint32_t  spt_count;
    uint64_t  pdb_phys;
    uint64_t  map_vram_base; /* Start of VRAM identity map */
    uint64_t  map_vram_size; /* Size of mapped region */
    bool      initialized;
} gmmu_state_t;

static gmmu_state_t gmmu;

/* ── Page Table Allocation ───────────────────────────────── */

static uint64_t *alloc_pt_page(void)
{
    uint64_t *p = (uint64_t *)mem_alloc_aligned(4096, 4096);
    if (p) memset(p, 0, 4096);
    return p;
}

static inline uint64_t pt_phys(void *p)
{
    return (uint64_t)(uintptr_t)p;
}

/* ── Build GMMU Identity Map ─────────────────────────────── */

/*
 * Identity-map a VRAM range: GPU VA = VRAM physical address.
 * Maps the range [vram_base, vram_base + size).
 *
 * Handles ranges that span multiple PD1 entries (>512MB).
 * Each PD1 entry → one PD0 page (256 entries × 2MB = 512MB).
 *
 * For X-INF3 model weights at VRAM+256MB, maps up to 768MB:
 *   1 PDB + 1 PD2 + 1 PD1 (shared) + 2 PD0 pages + 384 SPT pages
 *   Page tables: ~1.5MB from system RAM.
 */
static int gmmu_build_identity_map(uint64_t vram_base, uint64_t vram_size)
{
    /* Align base down and size up to 2MB boundaries (SPT coverage) */
    uint64_t aligned_base = vram_base & ~(SPT_COVERAGE - 1);
    uint64_t aligned_end  = (vram_base + vram_size + SPT_COVERAGE - 1)
                            & ~(SPT_COVERAGE - 1);
    uint64_t aligned_size = aligned_end - aligned_base;
    uint32_t num_spt = (uint32_t)(aligned_size / SPT_COVERAGE);

    if (num_spt > MAX_SPT_PAGES) {
        serial_puts("[GMMU] Too many SPT pages needed (");
        serial_putdec(num_spt);
        serial_puts(" > ");
        serial_putdec(MAX_SPT_PAGES);
        serial_puts(")\n");
        return -1;
    }

    serial_puts("[GMMU] Identity map: VA 0x");
    serial_puthex(aligned_base, 8);
    serial_puts("-0x");
    serial_puthex(aligned_end, 8);
    serial_puts(" (");
    serial_putdec(aligned_size / (1024 * 1024));
    serial_puts("MB, ");
    serial_putdec(num_spt);
    serial_puts(" SPT pages)\n");

    /* Allocate upper-level page tables (shared across all entries) */
    gmmu.pdb = alloc_pt_page();
    gmmu.pd2 = alloc_pt_page();
    gmmu.pd1 = alloc_pt_page();

    if (!gmmu.pdb || !gmmu.pd2 || !gmmu.pd1) {
        serial_puts("[GMMU] Failed to allocate page table pages\n");
        return -1;
    }
    gmmu.pdb_phys = pt_phys(gmmu.pdb);

    /* Wire upper levels (PDB → PD2 → PD1) — all our VAs share these */
    gmmu.pdb[PDB_IDX(aligned_base)] = (pt_phys(gmmu.pd2) >> 4) | PDE_SYS;
    gmmu.pd2[PD2_IDX(aligned_base)] = (pt_phys(gmmu.pd1) >> 4) | PDE_SYS;

    /* Determine which PD1 entries we need (each covers 512MB) */
    uint32_t pd1_first = PD1_IDX(aligned_base);
    uint32_t pd1_last  = PD1_IDX(aligned_end - 1);
    uint32_t num_pd0   = pd1_last - pd1_first + 1;

    if (num_pd0 > MAX_PD0_PAGES) {
        serial_puts("[GMMU] Too many PD0 pages needed\n");
        return -1;
    }

    /* Allocate PD0 pages and wire to PD1 */
    for (uint32_t p = 0; p < num_pd0; p++) {
        gmmu.pd0[p] = alloc_pt_page();
        if (!gmmu.pd0[p]) {
            serial_puts("[GMMU] Failed to allocate PD0 page\n");
            return -1;
        }
        gmmu.pd1[pd1_first + p] = (pt_phys(gmmu.pd0[p]) >> 4) | PDE_SYS;
    }
    gmmu.pd0_count = num_pd0;

    serial_puts("[GMMU] PD1 entries ");
    serial_putdec(pd1_first);
    serial_puts("-");
    serial_putdec(pd1_last);
    serial_puts(" (");
    serial_putdec(num_pd0);
    serial_puts(" PD0 pages)\n");

    /* Allocate SPT pages */
    for (uint32_t i = 0; i < num_spt; i++) {
        gmmu.spt[i] = alloc_pt_page();
        if (!gmmu.spt[i]) {
            serial_puts("[GMMU] Failed to allocate SPT page ");
            serial_putdec(i);
            serial_puts("\n");
            return -1;
        }
    }
    gmmu.spt_count = num_spt;

    /* Wire PD0 → SPT and fill SPTs with identity-mapped PTEs */
    for (uint32_t i = 0; i < num_spt; i++) {
        uint64_t va = aligned_base + (uint64_t)i * SPT_COVERAGE;

        /* Find which PD0 page this VA belongs to */
        uint32_t pd1_idx = PD1_IDX(va);
        uint32_t pd0_page = pd1_idx - pd1_first;
        uint32_t pd0_idx = PD0_IDX(va);

        /* PD0 dual PDE: [small_pde, big_pde=0] at pd0_idx*2 */
        gmmu.pd0[pd0_page][pd0_idx * 2]     = pt_phys(gmmu.spt[i]) | PD0_SYS;
        gmmu.pd0[pd0_page][pd0_idx * 2 + 1] = 0;

        /* Fill SPT with identity-mapped VRAM PTEs */
        for (uint32_t e = 0; e < SPT_ENTRIES; e++) {
            uint64_t page_phys = va + (uint64_t)e * 4096;
            gmmu.spt[i][e] = (page_phys >> 4) | PTE_VRAM;
        }
    }

    wmb();

    gmmu.map_vram_base = aligned_base;
    gmmu.map_vram_size = aligned_size;

    serial_puts("[GMMU] Page tables: PDB=0x");
    serial_puthex(gmmu.pdb_phys, 8);
    serial_puts(" ");
    serial_putdec(num_spt);
    serial_puts(" SPT + ");
    serial_putdec(num_pd0);
    serial_puts(" PD0 (~");
    serial_putdec((num_spt + num_pd0 + 3) * 4);
    serial_puts("KB)\n");

    return 0;
}

/* ── Configure Channel Instance Block ────────────────────── */

/*
 * Write PDB physical address to the channel's instance memory (RAMIN)
 * at offset 0x200. This tells the GPU where to find page tables for
 * this channel's virtual address space.
 *
 * Format (GP100+):
 *   Bits 1:0:   TARGET (2 = SYS_COHERENT for page tables in system RAM)
 *   Bits 31:12: PDB physical address bits 31:12
 *   Bits 51:32: PDB physical address bits 51:32 (in second dword)
 */
static int gmmu_configure_instance_block(void)
{
    channel_state_t *ch = gsp_get_channel();
    if (!ch || !ch->inst_mem) {
        serial_puts("[GMMU] No channel instance memory\n");
        return -1;
    }

    volatile uint32_t *inst = (volatile uint32_t *)ch->inst_mem;

    /* PDB address with TARGET=SYS_COHERENT */
    uint32_t lo = (uint32_t)(gmmu.pdb_phys & 0xFFFFF000)
                | RAMIN_TARGET_SYS_COHERENT;
    uint32_t hi = (uint32_t)(gmmu.pdb_phys >> 32);

    inst[RAMIN_PDB_OFFSET / 4]     = lo;
    inst[RAMIN_PDB_OFFSET / 4 + 1] = hi;
    wmb();

    serial_puts("[GMMU] Instance block PDB: [0x200]=0x");
    serial_puthex(lo, 8);
    serial_puts(" [0x204]=0x");
    serial_puthex(hi, 8);
    serial_puts("\n");

    /* Verify readback */
    rmb();
    uint32_t read_lo = inst[RAMIN_PDB_OFFSET / 4];
    uint32_t read_hi = inst[RAMIN_PDB_OFFSET / 4 + 1];
    bool ok = (read_lo == lo && read_hi == hi);
    serial_puts("[GMMU] Instance block verify: ");
    serial_puts(ok ? "OK" : "FAIL");
    serial_puts("\n");

    return ok ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────────── */

int gmmu_init(void)
{
    serial_puts("\n[GMMU] == X38: GPU MMU Page Tables ==\n");

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) {
        serial_puts("[GMMU] No GPU detected, skipping\n");
        return -1;
    }

    memset(&gmmu, 0, sizeof(gmmu));

    /* Identity-map VRAM region: SASS kernels (256MB) + tensor/weights (260MB+)
     * X-INF3: 768MB covers SASS (4MB) + scratch (16MB) + model weights (~550MB) + headroom */
    uint64_t vram_base = (uint64_t)SASS_VRAM_OFFSET_MB * 1024 * 1024;
    uint64_t vram_size = 768ULL * 1024 * 1024;  /* 768MB: kernels + tensors + weights */

    if (gmmu_build_identity_map(vram_base, vram_size) < 0) {
        serial_puts("[GMMU] Failed to build page tables\n");
        return -1;
    }

    /* Configure channel instance block with PDB */
    gmmu_configure_instance_block();

    gmmu.initialized = true;

    serial_puts("[GMMU] == GMMU init complete ==\n");
    fb_puts(" GMMU: identity map ");
    fb_putdec(gmmu.map_vram_size / (1024 * 1024));
    fb_puts("MB\n");

    return 0;
}

uint64_t gmmu_get_pdb_phys(void)
{
    return gmmu.pdb_phys;
}

/* ══════════════════════════════════════════════════════════════
 *  VRAM allocator for NVK Vulkan resources
 *
 *  The identity-map covers [vram_base, vram_base+vram_size). SASS
 *  kernels and the tensor arena consume the low/middle portion; the
 *  trailing 64 MB are free for runtime Vulkan allocations (images,
 *  buffers, command-buffer staging, fence semaphore page).
 *
 *  4 KB granularity bitmap. Caller frees with gmmu_free_vram(addr,size).
 *
 *  Returns the GPU virtual address of the allocation (== phys, since
 *  we identity-map). 0 on failure. The address is also a valid CPU
 *  virtual address through PHYS_TO_VIRT for kernel-side fills.
 *  ══════════════════════════════════════════════════════════════ */

#define NVK_VRAM_TAIL_MB   64           /* trailing region of identity map */
#define NVK_VRAM_PAGE      4096
#define NVK_VRAM_MAX_PAGES (NVK_VRAM_TAIL_MB * 1024 * 1024 / NVK_VRAM_PAGE)
#define NVK_VRAM_BITMAP_SZ ((NVK_VRAM_MAX_PAGES + 63) / 64)

static struct {
    uint64_t base;                          /* Region start (phys = GPU VA) */
    uint64_t size;                          /* Region length, bytes */
    uint64_t bitmap[NVK_VRAM_BITMAP_SZ];    /* 1 bit per 4KB page, 1 = used */
    uint32_t pages_used;
    bool     initialized;
} nvk_vram;

static void nvk_vram_lazy_init(void)
{
    if (nvk_vram.initialized) return;
    if (!gmmu.initialized || gmmu.map_vram_size < (uint64_t)NVK_VRAM_TAIL_MB * 1024 * 1024) {
        serial_puts("[GMMU] VRAM allocator: identity map too small or not init'd\n");
        return;
    }

    nvk_vram.size = (uint64_t)NVK_VRAM_TAIL_MB * 1024 * 1024;
    nvk_vram.base = gmmu.map_vram_base + gmmu.map_vram_size - nvk_vram.size;

    for (uint32_t i = 0; i < NVK_VRAM_BITMAP_SZ; i++) nvk_vram.bitmap[i] = 0;
    nvk_vram.pages_used  = 0;
    nvk_vram.initialized = true;

    serial_puts("[GMMU] NVK VRAM allocator: ");
    serial_putdec(NVK_VRAM_TAIL_MB);
    serial_puts(" MB at 0x");
    serial_puthex(nvk_vram.base, 16);
    serial_puts("\n");
}

/*
 * Allocate `size` bytes of GPU-mapped VRAM aligned to 4 KB. Returns the
 * VRAM address on success, 0 on failure.
 */
uint64_t gmmu_alloc_vram(uint64_t size)
{
    nvk_vram_lazy_init();
    if (!nvk_vram.initialized || size == 0) return 0;

    uint32_t need = (uint32_t)((size + NVK_VRAM_PAGE - 1) / NVK_VRAM_PAGE);
    if (need > NVK_VRAM_MAX_PAGES) return 0;

    /* First-fit search over the bitmap. */
    for (uint32_t start = 0; start + need <= NVK_VRAM_MAX_PAGES; start++) {
        bool ok = true;
        for (uint32_t k = 0; k < need; k++) {
            uint32_t b = start + k;
            if (nvk_vram.bitmap[b / 64] & (1ULL << (b % 64))) { ok = false; break; }
        }
        if (!ok) continue;

        for (uint32_t k = 0; k < need; k++) {
            uint32_t b = start + k;
            nvk_vram.bitmap[b / 64] |= (1ULL << (b % 64));
        }
        nvk_vram.pages_used += need;
        return nvk_vram.base + (uint64_t)start * NVK_VRAM_PAGE;
    }
    return 0;
}

void gmmu_free_vram(uint64_t addr, uint64_t size)
{
    if (!nvk_vram.initialized || size == 0) return;
    if (addr < nvk_vram.base || addr >= nvk_vram.base + nvk_vram.size) return;

    uint32_t start = (uint32_t)((addr - nvk_vram.base) / NVK_VRAM_PAGE);
    uint32_t need  = (uint32_t)((size + NVK_VRAM_PAGE - 1) / NVK_VRAM_PAGE);
    for (uint32_t k = 0; k < need; k++) {
        uint32_t b = start + k;
        if (b >= NVK_VRAM_MAX_PAGES) break;
        if (nvk_vram.bitmap[b / 64] & (1ULL << (b % 64))) {
            nvk_vram.bitmap[b / 64] &= ~(1ULL << (b % 64));
            nvk_vram.pages_used--;
        }
    }
}

uint32_t gmmu_vram_pages_used(void)  { return nvk_vram.pages_used; }
uint64_t gmmu_vram_region_base(void) { return nvk_vram.base; }
uint64_t gmmu_vram_region_size(void) { return nvk_vram.size; }

bool gmmu_is_initialized(void)
{
    return gmmu.initialized;
}
