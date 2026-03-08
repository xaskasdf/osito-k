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

#define MAX_SPT_PAGES  16  /* Enough to map 32MB of VRAM */

typedef struct {
    uint64_t *pdb;           /* Page Directory Base: 4 entries × 8B */
    uint64_t *pd2;           /* PD2: 512 entries × 8B = 4KB */
    uint64_t *pd1;           /* PD1: 512 entries × 8B = 4KB */
    uint64_t *pd0;           /* PD0: 256 entries × 16B = 4KB (stored as uint64_t pairs) */
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
 * Only maps the range [vram_base, vram_base + size).
 *
 * For SASS kernels at VRAM+256MB, we map 256MB-260MB (4MB).
 * This requires:
 *   1 PDB page (32B, in 4KB allocation)
 *   1 PD2 page (4KB)
 *   1 PD1 page (4KB)
 *   1 PD0 page (4KB)
 *   N SPT pages (each covers 2MB, so 2 pages for 4MB)
 * Total: ~24KB
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

    /* Allocate page table levels */
    gmmu.pdb = alloc_pt_page();
    gmmu.pd2 = alloc_pt_page();
    gmmu.pd1 = alloc_pt_page();
    gmmu.pd0 = alloc_pt_page();

    if (!gmmu.pdb || !gmmu.pd2 || !gmmu.pd1 || !gmmu.pd0) {
        serial_puts("[GMMU] Failed to allocate page table pages\n");
        return -1;
    }

    gmmu.pdb_phys = pt_phys(gmmu.pdb);

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

    /* ── Wire up page table hierarchy ── */

    /* PDB[0] → PD2 (all addresses in our range have PDB index 0) */
    gmmu.pdb[PDB_IDX(aligned_base)] = (pt_phys(gmmu.pd2) >> 4) | PDE_SYS;

    /* PD2[idx] → PD1 */
    gmmu.pd2[PD2_IDX(aligned_base)] = (pt_phys(gmmu.pd1) >> 4) | PDE_SYS;

    /* PD1[idx] → PD0 */
    gmmu.pd1[PD1_IDX(aligned_base)] = (pt_phys(gmmu.pd0) >> 4) | PDE_SYS;

    /* PD0 entries → SPT pages (dual PDE format: small_pde at even offset, big=0) */
    for (uint32_t i = 0; i < num_spt; i++) {
        uint64_t va = aligned_base + i * SPT_COVERAGE;
        uint32_t pd0_idx = PD0_IDX(va);

        /* PD0 is dual PDE: each entry is 16 bytes (2 × uint64_t).
         * Entry layout: [small_pde, big_pde]
         * small_pde = phys_addr | flags (NOT shifted for PD0!)
         * big_pde = 0 (disabled) */
        gmmu.pd0[pd0_idx * 2]     = pt_phys(gmmu.spt[i]) | PD0_SYS;
        gmmu.pd0[pd0_idx * 2 + 1] = 0;  /* big page PDE disabled */
    }

    /* Fill SPT entries with identity-mapped VRAM PTEs */
    for (uint32_t s = 0; s < num_spt; s++) {
        uint64_t spt_base_va = aligned_base + s * SPT_COVERAGE;
        for (uint32_t e = 0; e < SPT_ENTRIES; e++) {
            uint64_t page_phys = spt_base_va + e * 4096;
            /* PTE = (phys >> 4) | flags */
            gmmu.spt[s][e] = (page_phys >> 4) | PTE_VRAM;
        }
    }

    /* Memory barriers — ensure all PTs are visible before GPU reads them */
    wmb();

    gmmu.map_vram_base = aligned_base;
    gmmu.map_vram_size = aligned_size;

    serial_puts("[GMMU] Page tables built:\n");
    serial_puts("[GMMU]   PDB=0x");
    serial_puthex(gmmu.pdb_phys, 16);
    serial_puts(" PD2=0x");
    serial_puthex(pt_phys(gmmu.pd2), 16);
    serial_puts("\n");
    serial_puts("[GMMU]   PD1=0x");
    serial_puthex(pt_phys(gmmu.pd1), 16);
    serial_puts(" PD0=0x");
    serial_puthex(pt_phys(gmmu.pd0), 16);
    serial_puts("\n");
    for (uint32_t i = 0; i < num_spt; i++) {
        serial_puts("[GMMU]   SPT[");
        serial_putdec(i);
        serial_puts("]=0x");
        serial_puthex(pt_phys(gmmu.spt[i]), 16);
        serial_puts(" (VA 0x");
        serial_puthex(aligned_base + i * SPT_COVERAGE, 8);
        serial_puts("-0x");
        serial_puthex(aligned_base + (i + 1) * SPT_COVERAGE, 8);
        serial_puts(")\n");
    }

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

    /* Identity-map VRAM region: SASS kernels (256MB) + tensor buffers (260MB, 16MB) */
    uint64_t vram_base = (uint64_t)SASS_VRAM_OFFSET_MB * 1024 * 1024;
    uint64_t vram_size = 24 * 1024 * 1024;  /* 24MB: kernels (4MB) + tensor data (16MB) + headroom */

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

bool gmmu_is_initialized(void)
{
    return gmmu.initialized;
}
