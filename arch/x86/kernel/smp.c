/*
 * OsitoK x86-64 — SMP (Symmetric Multi-Processing)
 *
 * X-SMP: Detect CPU cores via ACPI MADT, wake APs with INIT-SIPI-SIPI,
 * per-CPU state, spinlocks. Each AP runs its own APIC timer.
 *
 * AP startup flow:
 *   1. BSP copies trampoline code to 0x8000 (below 1MB, identity-mapped)
 *   2. BSP sends INIT IPI → 10ms delay → SIPI → 200μs → SIPI
 *   3. AP starts in 16-bit real mode at 0x8000
 *   4. Trampoline: real → protected → long mode → call smp_ap_entry()
 *   5. AP initializes its LAPIC, signals ready, enters idle loop
 *
 * Trampoline is a self-contained byte array (avoids EFI linking issues).
 * Data block at 0x8000+0x100 carries GDT, CR3, stack, entry point.
 */

#include "smp.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void *kmalloc(uint64_t size);

/* From idt.c */
extern volatile uint32_t *idt_get_apic_base(void);
extern uint64_t idt_get_ticks(void);

/* From paging.c */
extern uint64_t paging_get_kernel_cr3(void);

/* From efi_main.c — RSDP address found in EFI configuration table */
extern uint64_t efi_acpi_rsdp;

/* ── ACPI MADT Structures ────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

/* MADT header (after standard SDT header) */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t local_apic_addr;
    uint32_t flags;
    /* Variable-length entries follow */
} acpi_madt_t;

/* MADT entry header */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_entry_t;

/* Type 0: Processor Local APIC */
typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 0 */
    uint8_t  length;      /* 8 */
    uint8_t  acpi_proc_id;
    uint8_t  apic_id;
    uint32_t flags;       /* bit 0: enabled, bit 1: online capable */
} madt_lapic_t;

#define MADT_TYPE_LAPIC      0
#define MADT_TYPE_IOAPIC     1
#define MADT_TYPE_LAPIC_X2   9

#define MADT_LAPIC_ENABLED   (1 << 0)
#define MADT_LAPIC_CAPABLE   (1 << 1)

/* ── LAPIC Registers ─────────────────────────────────────────── */

#define APIC_ID          0x020
#define APIC_SVR         0x0F0
#define APIC_ICR_LOW     0x300
#define APIC_ICR_HIGH    0x310
#define APIC_LVT_TIMER   0x320
#define APIC_TIMER_INIT  0x380
#define APIC_TIMER_DIV   0x3E0
#define APIC_EOI         0x0B0

#define APIC_SVR_ENABLE  0x100
#define APIC_TIMER_PERIODIC 0x20000

/* ICR delivery modes */
#define ICR_INIT         (5 << 8)
#define ICR_SIPI         (6 << 8)
#define ICR_ASSERT       (1 << 14)
#define ICR_LEVEL        (1 << 15)
#define ICR_DEASSERT     0

/* ── SMP State ───────────────────────────────────────────────── */

static cpu_info_t cpus[SMP_MAX_CPUS];
static uint32_t   cpu_count;
static uint32_t   bsp_apic_id;

/* ── Trampoline ──────────────────────────────────────────────── */

/*
 * AP trampoline code — copied to physical address 0x8000.
 * Assembled by hand for simplicity (avoids EFI PIE linking issues).
 *
 * Data block at 0x8100 (offset 0x100 from trampoline base):
 *   +0x00: GDT (4 entries × 8 = 32 bytes)
 *   +0x20: GDTR (6 bytes: 2 limit + 4 base)
 *   +0x28: Kernel CR3 (8 bytes)
 *   +0x30: Kernel GDTR limit (2 bytes)
 *   +0x32: Kernel GDTR base (8 bytes)
 *   +0x3C: Kernel IDTR limit (2 bytes)
 *   +0x3E: Kernel IDTR base (8 bytes)
 *   +0x48: AP entry point (8 bytes)
 *   +0x50: AP stack (8 bytes)
 *   +0x58: AP CPU index (4 bytes)
 *   +0x5C: AP ready flag (4 bytes)
 */

#define TRAMP_BASE       0x8000
#define TRAMP_DATA       0x8100  /* data block at +0x100 */

#define DATA_GDT         0x00
#define DATA_GDTR        0x20
#define DATA_CR3         0x28
#define DATA_KGDTR_LIM   0x30
#define DATA_KGDTR_BASE  0x32
#define DATA_KIDTR_LIM   0x3C
#define DATA_KIDTR_BASE  0x3E
#define DATA_ENTRY       0x48
#define DATA_STACK       0x50
#define DATA_CPU_IDX     0x58
#define DATA_READY       0x5C

/* Trampoline GDT (used for mode transition only):
 * Entry 0: null
 * Entry 1 (0x08): 32-bit code (P=1, DPL=0, S=1, Type=0xA, D=1, G=1)
 * Entry 2 (0x10): data (P=1, DPL=0, S=1, Type=0x2, G=1)
 * Entry 3 (0x18): 64-bit code (L=1, P=1, DPL=0, S=1, Type=0xA) */

static const uint64_t tramp_gdt[4] = {
    0x0000000000000000ULL,  /* null */
    0x00CF9A000000FFFFULL,  /* 0x08: 32-bit code (D=1, L=0) */
    0x00CF92000000FFFFULL,  /* 0x10: data */
    0x00AF9A000000FFFFULL,  /* 0x18: 64-bit code (L=1, D=0) */
};

/*
 * Hand-assembled 16-bit → 32-bit → 64-bit trampoline.
 *
 * All absolute addresses reference 0x8000 base and 0x8100 data block.
 * The code is position-dependent (always runs at 0x8000).
 */
/*
 * AP trampoline binary — assembled from ap_trampoline.S
 *
 * Build: as --32 ap_trampoline.S -o /tmp/ap_t.o && objcopy -O binary /tmp/ap_t.o /tmp/ap_t.bin
 *
 * Layout: 0x00-0x3F 16-bit real → 0x40-0x8F 32-bit PM → 0x90-0xEF 64-bit LM
 * Data block at 0x8100 (filled by setup_trampoline)
 */
static const uint8_t trampoline_code[] = {
    /* 0x00: 16-bit real mode — cli, cld, DS=0, lgdt [0x8120], PE, jmp32 0x8040 */
    0xfa, 0xfc, 0x31, 0xc0, 0x8e, 0xd8, 0x0f, 0x01, 0x16, 0x20, 0x81, 0x0f,
    0x20, 0xc0, 0x66, 0x83, 0xc8, 0x01, 0x0f, 0x22, 0xc0, 0x66, 0xea, 0x40,
    0x80, 0x00, 0x00, 0x08, 0x00,
    /* 0x1D-0x3F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x40: 32-bit PM — segments, PAE, CR3, LME, paging, jmp64 0x8090 */
    0x66, 0xb8, 0x10, 0x00, 0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0, 0x0f, 0x20,
    0xe0, 0x0f, 0xba, 0xe8, 0x05, 0x0f, 0x22, 0xe0, 0xa1, 0x28, 0x81, 0x00,
    0x00, 0x0f, 0x22, 0xd8, 0xb9, 0x80, 0x00, 0x00, 0xc0, 0x0f, 0x32, 0x0f,
    0xba, 0xe8, 0x08, 0x0f, 0x30, 0x0f, 0x20, 0xc0, 0x0f, 0xba, 0xe8, 0x1f,
    0x0f, 0x22, 0xc0, 0xea, 0x90, 0x80, 0x00, 0x00, 0x18, 0x00,
    /* 0x7A-0x8F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x90: 64-bit LM — load stack, lgdt/lidt kernel, CS reload, segments, ready, call */
    0x48, 0x8b, 0x24, 0x25, 0x50, 0x81, 0x00, 0x00, 0x0f, 0x01, 0x14, 0x25,
    0x30, 0x81, 0x00, 0x00, 0x0f, 0x01, 0x1c, 0x25, 0x3c, 0x81, 0x00, 0x00,
    0x6a, 0x38, 0x48, 0x8d, 0x05, 0x03, 0x00, 0x00, 0x00, 0x50, 0x48, 0xcb,
    0xb8, 0x30, 0x00, 0x00, 0x00, 0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0, 0x31,
    0xc0, 0x8e, 0xe0, 0x8e, 0xe8, 0xc7, 0x04, 0x25, 0x5c, 0x81, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x8b, 0x3c, 0x25, 0x58, 0x81, 0x00, 0x00, 0x48,
    0x8b, 0x04, 0x25, 0x48, 0x81, 0x00, 0x00, 0xff, 0xd0, 0xf4, 0xeb, 0xfd,
};

/* ── MADT Parsing ────────────────────────────────────────────── */

static acpi_rsdp_t *find_rsdp(void)
{
    /* Prefer RSDP from EFI System Table (set by efi_main) */
    if (efi_acpi_rsdp) {
        serial_puts("[SMP] Using EFI RSDP at ");
        serial_puthex(efi_acpi_rsdp, 16);
        serial_puts("\n");
        return (acpi_rsdp_t *)efi_acpi_rsdp;
    }

    /* Fallback: scan BIOS ROM area for "RSD PTR " */
    for (uint64_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        const char *sig = (const char *)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == ' ' &&
            sig[4] == 'P' && sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ')
            return (acpi_rsdp_t *)addr;
    }
    return NULL;
}

static acpi_sdt_header_t *find_acpi_table(acpi_rsdp_t *rsdp, const char *sig4)
{
    int use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_addr != 0);
    acpi_sdt_header_t *root;

    if (use_xsdt)
        root = (acpi_sdt_header_t *)rsdp->xsdt_addr;
    else
        root = (acpi_sdt_header_t *)(uint64_t)rsdp->rsdt_addr;

    int entry_size = use_xsdt ? 8 : 4;
    int entries = (int)(root->length - sizeof(*root)) / entry_size;
    uint8_t *data = (uint8_t *)root + sizeof(*root);

    for (int i = 0; i < entries; i++) {
        uint64_t addr;
        if (use_xsdt)
            addr = *(uint64_t *)(data + i * 8);
        else
            addr = *(uint32_t *)(data + i * 4);

        acpi_sdt_header_t *tbl = (acpi_sdt_header_t *)addr;
        if (tbl->signature[0] == sig4[0] && tbl->signature[1] == sig4[1] &&
            tbl->signature[2] == sig4[2] && tbl->signature[3] == sig4[3])
            return tbl;
    }
    return NULL;
}

static int parse_madt(void)
{
    acpi_rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        serial_puts("[SMP] RSDP not found\n");
        return -1;
    }

    acpi_madt_t *madt = (acpi_madt_t *)find_acpi_table(rsdp, "APIC");
    if (!madt) {
        serial_puts("[SMP] MADT not found\n");
        return -1;
    }

    serial_puts("[SMP] MADT at ");
    serial_puthex((uint64_t)madt, 16);
    serial_puts(", length=");
    serial_putdec(madt->header.length);
    serial_puts(", LAPIC=");
    serial_puthex(madt->local_apic_addr, 8);
    serial_puts("\n");

    /* Get BSP APIC ID */
    volatile uint32_t *apic = idt_get_apic_base();
    if (apic)
        bsp_apic_id = (apic[APIC_ID / 4] >> 24) & 0xFF;

    /* Parse MADT entries */
    uint8_t *p = (uint8_t *)madt + sizeof(acpi_madt_t);
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    cpu_count = 0;

    while (p < end && cpu_count < SMP_MAX_CPUS) {
        madt_entry_t *entry = (madt_entry_t *)p;

        if (entry->type == MADT_TYPE_LAPIC && entry->length >= 8) {
            madt_lapic_t *lapic = (madt_lapic_t *)p;

            if (lapic->flags & (MADT_LAPIC_ENABLED | MADT_LAPIC_CAPABLE)) {
                cpus[cpu_count].apic_id = lapic->apic_id;
                cpus[cpu_count].bsp = (lapic->apic_id == bsp_apic_id);
                cpus[cpu_count].cpu_index = cpu_count;
                cpus[cpu_count].online = cpus[cpu_count].bsp;  /* BSP is already online */

                serial_puts("[SMP]   CPU ");
                serial_putdec(cpu_count);
                serial_puts(": APIC ID ");
                serial_putdec(lapic->apic_id);
                if (cpus[cpu_count].bsp)
                    serial_puts(" (BSP)");
                serial_puts("\n");

                cpu_count++;
            }
        }

        p += entry->length;
        if (entry->length == 0) break;  /* Safety: avoid infinite loop */
    }

    serial_puts("[SMP] Found ");
    serial_putdec(cpu_count);
    serial_puts(" CPU(s)\n");
    return 0;
}

/* ── LAPIC helpers ───────────────────────────────────────────── */

static inline void apic_write_reg(volatile uint32_t *base, uint32_t reg, uint32_t val)
{
    base[reg / 4] = val;
}

static inline uint32_t apic_read_reg(volatile uint32_t *base, uint32_t reg)
{
    return base[reg / 4];
}

/* Delay using BSP APIC timer ticks (already running at ~100Hz) */
static void delay_ms(uint32_t ms)
{
    uint64_t target = idt_get_ticks() + (ms / 10) + 1;
    while (idt_get_ticks() < target)
        __asm__ volatile ("nop");
}

static void delay_us(uint32_t us)
{
    /* Rough delay using nop loop (pause can hang during AP startup) */
    volatile uint32_t count = us * 500;
    while (count--) __asm__ volatile ("nop");
}

/* ── AP Entry Point (called from trampoline in 64-bit mode) ── */

static volatile uint32_t ap_started_count;

void smp_ap_entry(uint32_t cpu_index)
{
    /* Initialize this AP's LAPIC */
    volatile uint32_t *apic = idt_get_apic_base();
    if (apic) {
        /* Enable LAPIC with spurious vector 0xFF */
        apic_write_reg(apic, APIC_SVR, APIC_SVR_ENABLE | 0xFF);

        /* Do NOT start APIC timer on APs. The scheduler only runs on
         * the BSP, and AP timer interrupts cause sched_switch_rsp races
         * where an AP steals the context switch value meant for the BSP,
         * leading to #GP on IRETQ with corrupted CS/SS. APs stay in HLT
         * loop and only wake on IPIs (future SMP work scheduling). */
    }

    /* Mark CPU as online */
    if (cpu_index < SMP_MAX_CPUS)
        cpus[cpu_index].online = true;

    __sync_fetch_and_add(&ap_started_count, 1);

    serial_puts("[SMP] AP ");
    serial_putdec(cpu_index);
    serial_puts(" online (APIC ID ");
    serial_putdec(apic ? (apic_read_reg(apic, APIC_ID) >> 24) & 0xFF : 0);
    serial_puts(")\n");

    /* Idle loop — AP waits for work */
    for (;;) {
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}

/* ── AP Startup (INIT-SIPI-SIPI) ─────────────────────────────── */

static void setup_trampoline(uint32_t cpu_index, uint64_t stack_top)
{
    /* Copy trampoline code to 0x8000 */
    uint8_t *dest = (uint8_t *)TRAMP_BASE;
    for (uint32_t i = 0; i < sizeof(trampoline_code); i++)
        dest[i] = trampoline_code[i];

    /* Setup data block at 0x8100 */
    volatile uint8_t *data = (volatile uint8_t *)TRAMP_DATA;

    /* Zero data block */
    for (int i = 0; i < 0x80; i++)
        data[i] = 0;

    /* GDT entries (4 × 8 = 32 bytes at +0x00) */
    volatile uint64_t *gdt = (volatile uint64_t *)(data + DATA_GDT);
    gdt[0] = tramp_gdt[0];
    gdt[1] = tramp_gdt[1];
    gdt[2] = tramp_gdt[2];
    gdt[3] = tramp_gdt[3];

    /* GDTR at +0x20: limit (2 bytes) + base (4 bytes) for 16/32-bit lgdt */
    volatile uint16_t *gdtr_limit = (volatile uint16_t *)(data + DATA_GDTR);
    volatile uint32_t *gdtr_base  = (volatile uint32_t *)(data + DATA_GDTR + 2);
    *gdtr_limit = 4 * 8 - 1;     /* 31 */
    *gdtr_base  = TRAMP_DATA + DATA_GDT;  /* physical addr of GDT */

    /* Kernel CR3 at +0x28 */
    volatile uint64_t *cr3 = (volatile uint64_t *)(data + DATA_CR3);
    *cr3 = paging_get_kernel_cr3();

    /* Kernel GDTR at +0x30: limit (2 bytes) + base (8 bytes) */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } kgdtr;
    __asm__ volatile ("sgdt %0" : "=m"(kgdtr));
    volatile uint16_t *kgdtr_lim = (volatile uint16_t *)(data + DATA_KGDTR_LIM);
    volatile uint64_t *kgdtr_bas = (volatile uint64_t *)(data + DATA_KGDTR_BASE);
    *kgdtr_lim = kgdtr.limit;
    *kgdtr_bas = kgdtr.base;

    /* Kernel IDTR at +0x3C: limit (2 bytes) + base (8 bytes) */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } kidtr;
    __asm__ volatile ("sidt %0" : "=m"(kidtr));
    volatile uint16_t *kidtr_lim = (volatile uint16_t *)(data + DATA_KIDTR_LIM);
    volatile uint64_t *kidtr_bas = (volatile uint64_t *)(data + DATA_KIDTR_BASE);
    *kidtr_lim = kidtr.limit;
    *kidtr_bas = kidtr.base;

    /* AP entry point at +0x48 */
    volatile uint64_t *entry = (volatile uint64_t *)(data + DATA_ENTRY);
    *entry = (uint64_t)smp_ap_entry;

    /* AP stack at +0x50 */
    volatile uint64_t *stack = (volatile uint64_t *)(data + DATA_STACK);
    *stack = stack_top;

    /* CPU index at +0x58 */
    volatile uint32_t *idx = (volatile uint32_t *)(data + DATA_CPU_IDX);
    *idx = cpu_index;

    /* Ready flag at +0x5C (cleared — AP sets to 1) */
    volatile uint32_t *ready = (volatile uint32_t *)(data + DATA_READY);
    *ready = 0;

    /* Memory fence */
    __asm__ volatile ("mfence" ::: "memory");
}

static int start_ap(uint32_t cpu_index, uint8_t apic_id)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return -1;

    /* Allocate AP stack */
    uint8_t *stack_mem = (uint8_t *)kmalloc(SMP_AP_STACK_SIZE);
    if (!stack_mem) {
        serial_puts("[SMP] Failed to allocate AP stack\n");
        return -1;
    }
    uint64_t stack_top = (uint64_t)(stack_mem + SMP_AP_STACK_SIZE - 16);
    stack_top &= ~0xFULL;  /* 16-byte align */

    cpus[cpu_index].stack_top = stack_top;

    /* Setup trampoline with this AP's data */
    setup_trampoline(cpu_index, stack_top);

    /* SIPI vector = trampoline page number (0x8000 → vector 0x08) */
    uint8_t sipi_vector = (uint8_t)(TRAMP_BASE >> 12);

    /* Send INIT IPI */
    apic_write_reg(apic, APIC_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write_reg(apic, APIC_ICR_LOW, ICR_INIT | ICR_ASSERT | ICR_LEVEL);
    delay_us(200);

    /* Deassert INIT */
    apic_write_reg(apic, APIC_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write_reg(apic, APIC_ICR_LOW, ICR_INIT | ICR_LEVEL);
    delay_ms(10);

    /* SIPI 1 */
    apic_write_reg(apic, APIC_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write_reg(apic, APIC_ICR_LOW, ICR_SIPI | sipi_vector);
    delay_us(200);

    /* SIPI 2 */
    apic_write_reg(apic, APIC_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write_reg(apic, APIC_ICR_LOW, ICR_SIPI | sipi_vector);
    delay_us(200);

    /* Wait for AP to signal ready (up to 500ms) */
    volatile uint32_t *ready = (volatile uint32_t *)(TRAMP_DATA + DATA_READY);
    uint64_t deadline = idt_get_ticks() + 50;  /* 500ms at 100Hz */
    while (*ready == 0 && idt_get_ticks() < deadline)
        __asm__ volatile ("pause");

    if (*ready) {
        serial_puts("[SMP] AP ");
        serial_putdec(cpu_index);
        serial_puts(" started successfully\n");
        return 0;
    } else {
        serial_puts("[SMP] AP ");
        serial_putdec(cpu_index);
        serial_puts(" TIMEOUT\n");
        return -1;
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void smp_init(void)
{
    serial_puts("\n[SMP] == X-SMP: Multi-Core Startup ==\n");

    memset(cpus, 0, sizeof(cpus));
    cpu_count = 0;
    ap_started_count = 0;

    /* Step 1: Parse MADT to find CPUs */
    if (parse_madt() < 0) {
        serial_puts("[SMP] Defaulting to single CPU\n");
        cpu_count = 1;
        cpus[0].apic_id = 0;
        cpus[0].bsp = true;
        cpus[0].online = true;
        cpus[0].cpu_index = 0;
    }

    if (cpu_count <= 1) {
        serial_puts("[SMP] Single CPU — no APs to start\n");
        fb_puts(" SMP: 1 CPU\n");
        return;
    }

    /* Step 2: Start each AP */
    uint32_t aps_started = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].bsp) continue;  /* Skip BSP */

        serial_puts("[SMP] Starting AP ");
        serial_putdec(i);
        serial_puts(" (APIC ID ");
        serial_putdec(cpus[i].apic_id);
        serial_puts(")...\n");

        if (start_ap(i, (uint8_t)cpus[i].apic_id) == 0)
            aps_started++;
    }

    serial_puts("[SMP] == ");
    serial_putdec(aps_started);
    serial_puts(" AP(s) started, ");
    serial_putdec(cpu_count);
    serial_puts(" total CPU(s) ==\n");

    fb_puts(" SMP: ");
    fb_putdec(aps_started + 1);
    fb_puts(" CPUs online\n");
}

uint32_t smp_cpu_count(void)
{
    return cpu_count;
}

cpu_info_t *smp_cpu_info(uint32_t index)
{
    if (index >= cpu_count) return NULL;
    return &cpus[index];
}

uint32_t smp_current_cpu(void)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return 0;
    uint32_t id = (apic_read_reg(apic, APIC_ID) >> 24) & 0xFF;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].apic_id == id) return i;
    }
    return 0;
}
