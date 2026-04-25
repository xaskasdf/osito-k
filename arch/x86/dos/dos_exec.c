/*
 * OsitoK — DOS Execution Orchestrator
 *
 * Allocates and initializes a DOS VM, loads the binary,
 * runs the CPU emulator, and cleans up.
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

/* Memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* OsitoFS */
extern int      osfs2_is_mounted(void);
extern void    *osfs2_find(const char *name);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* Kernel ticks */
extern uint64_t idt_get_ticks(void);

/* DOS subsystem */
extern int  dos_is_initialized(void);
extern void dos_mem_init(dos_vm_t *vm);
extern int  dos_detect_format(const uint8_t *data, uint64_t size);
extern int  dos_load_com(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                         const char *cmdline);
extern int  dos_load_mz(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                        const char *cmdline);
extern void cpu8086_init(cpu8086_state_t *cpu, dos_vm_t *vm);
extern int  cpu8086_run(dos_vm_t *vm);

/* ── IVT initialization: ROM stubs ──────────────────────────────── */

static void dos_init_ivt(dos_vm_t *vm)
{
    uint16_t rom_seg = DOS_ROM_BASE >> 4;  /* 0xF000 */
    uint16_t stub_off = 0;

    /* Only initialize IVT entries for vectors 0x80+ (above the GDT range).
     * DOS4GW puts its GDT at address 0 with limit 0x02FF (768 bytes = 96 entries).
     * IVT entries 0-95 (addresses 0x000-0x17F) overlap with GDT space.
     * Leave those as zero so DOS4GW can use them for GDT descriptors.
     * Our INT dispatch handles vectors 0x00-0x7F via dos_int_dispatch anyway. */
    for (int i = 0; i < 256; i++) {
        /* ROM stub: IRET (0xCF) for all vectors */
        vm->mem[DOS_ROM_BASE + stub_off] = 0xCF;

        /* Only write IVT entries above the GDT range */
        if (i >= 0xC0) {  /* vectors 0xC0+ (address 0x300+) are above GDT limit 0x2FF */
            dos_mem_write16(vm, i * 4, stub_off);
            dos_mem_write16(vm, i * 4 + 2, rom_seg);
        }
        stub_off++;
    }
}

/* ── Initialize BDA (BIOS Data Area) ───────────────────────────── */

static void dos_init_bda(dos_vm_t *vm)
{
    /* Equipment word at 0040:0010 */
    dos_mem_write16(vm, 0x410, 0x0021);  /* color 80x25 display */

    /* Memory size in KB at 0040:0013 */
    dos_mem_write16(vm, 0x413, 640);

    /* Active video mode at 0040:0049 */
    vm->mem[0x449] = 0x03;  /* mode 3 */

    /* Columns at 0040:004A */
    dos_mem_write16(vm, 0x44A, 80);

    /* Video page size at 0040:004C */
    dos_mem_write16(vm, 0x44C, 4096);

    /* Cursor position for page 0 at 0040:0050 */
    dos_mem_write16(vm, 0x450, 0x0000);

    /* Rows minus 1 at 0040:0084 */
    vm->mem[0x484] = 24;

    /* Char height at 0040:0085 */
    dos_mem_write16(vm, 0x485, 16);
}

/* ── Check if filename ends with .COM ───────────────────────────── */

static int ends_with_com(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'C' || ext[1] == 'c') &&
            (ext[2] == 'O' || ext[2] == 'o') &&
            (ext[3] == 'M' || ext[3] == 'm'));
}

/* ── Run a DOS binary from OsitoFS ──────────────────────────────── */

int dos_run(const char *filename, int argc, const char **argv)
{
    if (!dos_is_initialized()) {
        extern void dos_init(void);
        dos_init();
    }

    if (!osfs2_is_mounted()) {
        serial_puts("[DOS] No filesystem mounted\n");
        return -1;
    }

    /* Find file */
    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[DOS] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    uint64_t size = osfs2_file_size(file);
    if (size < 2) {
        serial_puts("[DOS] File too small\n");
        return -1;
    }

    serial_puts("[DOS] Loading ");
    serial_puts(filename);
    serial_puts(" (");
    serial_putdec(size);
    serial_puts(" bytes)\n");

    /* Read file into buffer */
    uint64_t buf_pages = (size + 0xFFF) / 4096;
    uint8_t *buf = (uint8_t *)mem_alloc_pages(buf_pages);
    if (!buf) {
        serial_puts("[DOS] Failed to allocate read buffer\n");
        return -1;
    }
    osfs2_read(file, 0, buf, size);

    /* Allocate DOS VM */
    dos_vm_t vm;
    cpu8086_state_t cpu;

    /* Zero structures */
    {
        uint8_t *p = (uint8_t *)&vm;
        for (uint64_t i = 0; i < sizeof(vm); i++) p[i] = 0;
        p = (uint8_t *)&cpu;
        for (uint64_t i = 0; i < sizeof(cpu); i++) p[i] = 0;
    }

    vm.cpu = &cpu;
    cpu.vm = &vm;

    /* Allocate 16MB emulated memory (1MB conventional + 15MB extended for DPMI) */
    uint64_t total_mem = DOS_TOTAL_MEM;
    uint64_t mem_pages = (total_mem + 0xFFF) / 4096;
    vm.mem = (uint8_t *)mem_alloc_pages(mem_pages);
    if (!vm.mem) {
        serial_puts("[DOS] Failed to allocate 16MB emulated memory\n");
        mem_free_pages(buf, buf_pages);
        return -1;
    }
    vm.total_mem_size = (uint32_t)total_mem;

    /* Zero emulated memory */
    for (uint64_t i = 0; i < total_mem; i++) vm.mem[i] = 0;

    /* Initialize subsystems */
    dos_init_ivt(&vm);
    dos_init_bda(&vm);
    dos_mem_init(&vm);
    extern void dpmi_init(dos_vm_t *vm);
    dpmi_init(&vm);

    /* Initialize JIT/DBT engine */
    {
        #include "dos_jit.h"
        /* jit_state_t is large (~200KB+), allocate via pages */
        uint64_t jit_pages = (sizeof(jit_state_t) + 4095) / 4096;
        jit_state_t *jit = (jit_state_t *)mem_alloc_pages(jit_pages);
        if (jit) {
            uint8_t *p = (uint8_t *)jit;
            for (uint64_t i = 0; i < jit_pages * 4096; i++) p[i] = 0;
            jit_init(jit);
            vm.jit = jit;
            serial_puts("[DOS] JIT engine initialized\n");
        }
    }

    cpu8086_init(&cpu, &vm);

    /* Set defaults */
    vm.current_drive = 2;  /* C: */
    vm.vga_mode = 0x03;
    vm.text_attr = 0x07;
    vm.cursor_start = 6;
    vm.cursor_end = 7;
    vm.start_ticks = idt_get_ticks();
    vm.last_timer_tick = vm.start_ticks;

    /* DTA defaults to PSP:0080 */
    vm.dta_off = 0x0080;

    /* Set up standard handles */
    for (int i = 0; i < 5; i++) {
        vm.handles[i].open = true;
        vm.handles[i].is_device = true;
    }

    /* Build command line from argv */
    char cmdline[128];
    int pos = 0;
    for (int i = 1; i < argc && pos < 126; i++) {
        if (i > 1 && pos < 126) cmdline[pos++] = ' ';
        for (int j = 0; argv[i][j] && pos < 126; j++)
            cmdline[pos++] = argv[i][j];
    }
    cmdline[pos] = 0;

    /* Detect format and load */
    int fmt = dos_detect_format(buf, size);
    int rc;

    if (fmt == DOS_FMT_MZ) {
        rc = dos_load_mz(&vm, buf, size, cmdline);
    } else if (fmt == DOS_FMT_NONE && ends_with_com(filename)) {
        rc = dos_load_com(&vm, buf, size, cmdline);
    } else {
        serial_puts("[DOS] Unknown binary format\n");
        mem_free_pages(buf, buf_pages);
        mem_free_pages(vm.mem, mem_pages);
        return -1;
    }

    /* Free file buffer (data is copied into emulated memory) */
    mem_free_pages(buf, buf_pages);

    if (rc != 0) {
        serial_puts("[DOS] Failed to load binary\n");
        mem_free_pages(vm.mem, mem_pages);
        return -1;
    }

    /* Write program name to environment block AND to DOS4GW's internal
     * buffer area. DOS4GW reads from env at PSP:0x2C then copies to its
     * own data area. We also write directly to 0x18A0 as a workaround. */
    {
        /* Environment block at 0x500. Format:
         * VAR=VALUE\0 ... \0\0  (double null = end of vars)
         * \x01\x00              (word: 1 string follows)
         * DOOM.EXE\0            (program name) */
        uint32_t env_addr = 0x500;
        uint32_t p = env_addr;

        /* Empty environment variables: just double-null */
        vm.mem[p++] = 0;   /* end of (empty) var list */
        vm.mem[p++] = 0;   /* second null = end of environment */

        /* Count word: 1 additional string follows */
        vm.mem[p++] = 0x01;
        vm.mem[p++] = 0x00;

        /* Program name (full path as DOS4GW expects) */
        int k;
        for (k = 0; filename[k] && k < 60; k++)
            vm.mem[p++] = filename[k];
        vm.mem[p] = 0;

        serial_puts("[DOS] Env @0x500: '");
        serial_puts(filename);
        serial_puts("'\n");
    }

    /* DTA segment = PSP */
    vm.dta_seg = vm.current_psp;

    /* Run! */
    serial_puts("[DOS] Starting...\n");
    int exit_code = cpu8086_run(&vm);

    serial_puts("[DOS] Exit code ");
    serial_putdec((uint64_t)(uint32_t)exit_code);
    serial_puts(" (");
    serial_putdec(cpu.insn_count);
    serial_puts(" instructions)\n");

    /* Cleanup */
    mem_free_pages(vm.mem, mem_pages);

    return exit_code;
}

/* ── Transfer from 8086 interpreter to native 32-bit execution ──── */
/*
 * Called when the interpreter detects MOV CR0 with PE bit set.
 * DOS4GW has set up its GDT/IDT in emulated memory and is switching
 * to protected mode. We take over: install real GDT segments and
 * jump to the 32-bit code natively via LRETQ.
 *
 * Pattern: same as win32/compat32.c:compat32_enter()
 */

extern void dos_set_native_vm(dos_vm_t *vm);

/* ── Native transfer: emulator → 32-bit compat mode ──────────────
 *
 * Approach: LDT-based. DOS4GW installs segment descriptors in an LDT
 * (TI=1 in selectors like 0x47, 0x4F). The dpmi->ldt[] array in the
 * dos_vm_t is already in Intel 8-byte descriptor format, so we can
 * point the hardware LDTR at it directly.
 *
 * Mapping: a dedicated DOS CR3 (via paging_create_process_cr3) maps
 * vm->mem physical pages at VA 0..total_mem_size, giving DOS4GW a
 * flat base=0 address space it expects. The kernel higher-half is
 * preserved so IDT handlers (INT 21h, etc.) can run normally from
 * the IST2 stack when DOS code issues software interrupts.
 *
 * After LRETQ, DOS native code runs at hardware speed. INTs trap via
 * dos_int_stub.S → dos_int_native_dispatch, handle the DOS API call,
 * and IRETQ back into DOS. */

extern uint64_t paging_create_process_cr3(void);
extern int      paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                       uint64_t phys, uint64_t flags);

/* Kernel GDT and its GDTR (shared with idt.c/win32_init.c) */
extern uint64_t kernel_gdt[] __attribute__((weak));
extern struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} kernel_gdtr __attribute__((weak));

/* PHYS_TO_VIRT / VIRT_TO_PHYS: KERNEL_VBASE = 0xFFFF800000000000.
 * Addresses above KERNEL_VBASE are in the kernel direct map (use
 * subtraction). Addresses below are identity-mapped (PA == VA). */
#define DOS_NT_KERNEL_VBASE  0xFFFF800000000000ULL
static inline uint64_t dos_nt_va_to_pa(const void *va)
{
    uint64_t v = (uint64_t)(uintptr_t)va;
    if (v >= DOS_NT_KERNEL_VBASE) return v - DOS_NT_KERNEL_VBASE;
    return v;  /* lower-half: identity-mapped, PA == VA */
}

/* PTE flags (duplicated from idt.c since they aren't in paging.h) */
#ifndef PTE_PRESENT
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#endif
#define PTE_USER      (1ULL << 2)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define DOS_NT_PHYS_TO_VIRT(p) ((uint64_t *)((uintptr_t)(p) + DOS_NT_KERNEL_VBASE))

/* Walk all 4 levels of the page table for `va` in `cr3` and OR PTE_USER
 * into each live entry. Needed because paging_map_page_in_cr3 creates
 * intermediate PDPT/PD/PT entries with just PRESENT|WRITABLE. Ring-3
 * access requires USER=1 at every level of the walk. */
static void dos_nt_propagate_user(uint64_t cr3, uint64_t va)
{
    uint64_t *pml4 = DOS_NT_PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    int i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT)) return;
    pml4[i4] |= PTE_USER;
    uint64_t *pdpt = DOS_NT_PHYS_TO_VIRT(pml4[i4] & PTE_ADDR_MASK);
    int i3 = (va >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_PRESENT)) return;
    pdpt[i3] |= PTE_USER;
    uint64_t *pd = DOS_NT_PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
    int i2 = (va >> 21) & 0x1FF;
    if (!(pd[i2] & PTE_PRESENT)) return;
    pd[i2] |= PTE_USER;
    if (pd[i2] & (1ULL << 7)) return;  /* 2 MB large page, no PT */
    uint64_t *pt = DOS_NT_PHYS_TO_VIRT(pd[i2] & PTE_ADDR_MASK);
    int i1 = (va >> 12) & 0x1FF;
    if (!(pt[i1] & PTE_PRESENT)) return;
    pt[i1] |= PTE_USER;
}

/* GDT slot reserved for the DOS LDT descriptor (2 slots, 16 bytes).
 * Slots 0-9 are claimed (null, kernel CS/DS, 64-bit CS/DS, CODE32, DATA32);
 * slots 10-11 hold the TSS descriptor (see idt.c:382-383). Slots 12-13
 * are free. Selector = 12 << 3 = 0x60. */
#define DOS_LDT_GDT_SLOT   12
#define DOS_LDT_SELECTOR   (DOS_LDT_GDT_SLOT << 3)

static uint64_t dos_cr3    = 0;
static int      dos_ldt_ok = 0;

void dos_transfer_to_native(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    serial_puts("[DOS-NT] enter cs=0x");  serial_puthex(cpu->cs, 4);
    serial_puts(" eip=0x");                serial_puthex(cpu->eip, 8);
    serial_puts(" ss=0x");                 serial_puthex(cpu->ss, 4);
    serial_puts(" esp=0x");                serial_puthex(cpu->esp, 8);
    serial_puts(" ds=0x");                 serial_puthex(cpu->ds, 4);
    serial_puts("\n");

    /* Skip if DOS4GW isn't using LDT yet — stay in interpreter. */
    if ((cpu->cs & 0x04) == 0) {
        serial_puts("[DOS-NT] CS is GDT selector (not LDT), skipping\n");
        return;
    }

    /* ── One-time CR3 + LDT-descriptor setup ────────────────── */
    if (!dos_cr3) {
        dos_cr3 = paging_create_process_cr3();
        if (!dos_cr3) {
            serial_puts("[DOS-NT] paging_create_process_cr3 FAILED\n");
            return;
        }

        /* Identity-map vm->mem pages at DOS CR3 VA 0..total_mem_size. */
        uint64_t mem_pa = dos_nt_va_to_pa(vm->mem);
        uint64_t mapped = 0;
        for (uint64_t off = 0; off < vm->total_mem_size; off += 4096) {
            if (paging_map_page_in_cr3(dos_cr3, off, mem_pa + off,
                                       PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
                serial_puts("[DOS-NT] map FAILED at off=0x");
                serial_puthex(off, 8); serial_puts("\n");
                return;
            }
            dos_nt_propagate_user(dos_cr3, off);
            mapped += 4096;
        }
        serial_puts("[DOS-NT] CR3=0x");    serial_puthex(dos_cr3, 16);
        serial_puts(" vm->mem pa=0x");     serial_puthex(mem_pa, 16);
        serial_puts(" mapped=");           serial_putdec(mapped >> 10);
        serial_puts(" KB @ VA 0\n");

        /* Also map the dos_vm_t struct pages at their own kernel VA in
         * dos_cr3. The LDT (dpmi->ldt) and VM state live here; hardware
         * LDT access on segment-register loads dereferences the kernel
         * VA, and INT handlers read the VM pointer through this mapping. */
        uint64_t vm_va    = (uint64_t)vm;
        uint64_t vm_end   = vm_va + sizeof(*vm);
        uint64_t vm_pbase = vm_va & ~0xFFFULL;
        uint64_t vm_pages = 0;
        for (uint64_t va = vm_pbase; va < vm_end; va += 4096) {
            uint64_t pa = dos_nt_va_to_pa((void *)va);
            if (paging_map_page_in_cr3(dos_cr3, va, pa,
                                       PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
                serial_puts("[DOS-NT] vm-struct map FAILED va=0x");
                serial_puthex(va, 16); serial_puts("\n");
                return;
            }
            dos_nt_propagate_user(dos_cr3, va);
            vm_pages++;
        }
        serial_puts("[DOS-NT] vm struct mapped: ");
        serial_putdec(vm_pages); serial_puts(" pages @ VA 0x");
        serial_puthex(vm_pbase, 16); serial_puts("\n");

        /* Build the long-mode LDT descriptor pointing at dpmi->ldt[].
         * System descriptor, 16 bytes. Low 64 bits hold base[31:0] +
         * limit[19:0] + access/flags; high 64 bits hold base[63:32]. */
        uint64_t ldt_base = (uint64_t)&vm->dpmi.ldt[0];
        uint32_t ldt_limit = (uint32_t)(sizeof(vm->dpmi.ldt) - 1);

        uint64_t desc_lo =
              ((uint64_t)(ldt_limit & 0xFFFF))
            | (((uint64_t)(ldt_base) & 0xFFFFFF) << 16)
            | ((uint64_t)0x82 << 40)                       /* P=1, Type=2 LDT */
            | ((uint64_t)((ldt_limit >> 16) & 0xF) << 48)
            | (((uint64_t)(ldt_base >> 24) & 0xFF) << 56);
        uint64_t desc_hi = (ldt_base >> 32);               /* base[63:32] */

        kernel_gdt[DOS_LDT_GDT_SLOT]     = desc_lo;
        kernel_gdt[DOS_LDT_GDT_SLOT + 1] = desc_hi;

        /* Extend GDTR limit to cover slot 13 and reload. */
        uint16_t needed = ((DOS_LDT_GDT_SLOT + 2) * 8) - 1;
        if (kernel_gdtr.limit < needed) {
            kernel_gdtr.limit = needed;
            __asm__ volatile ("lgdt %0" : : "m"(kernel_gdtr));
        }

        serial_puts("[DOS-NT] LDT installed: sel=0x");
        serial_puthex(DOS_LDT_SELECTOR, 4);
        serial_puts(" base=0x");  serial_puthex(ldt_base, 16);
        serial_puts(" limit=0x"); serial_puthex(ldt_limit, 4);
        serial_puts("\n");

        /* NOTE: DOS4GW hard-codes selector 0x18 as a 32-bit flat code
         * segment for raw-mode-switch returns. Tried installing a flat
         * 32-bit descriptor at GDT[3] (commit reverted) but DOOM then
         * jumps to linear addresses in vm->mem that contain IVT bytes
         * rather than valid 32-bit code, hits #UD. Sticking with the
         * surgical emulator path: the LRETW/JMP-FAR emulator redirects
         * GDT-selector targets to current-CS-relative offsets, which
         * keeps DOOM in 16-bit DOOM-CS mode where its actual code lives.
         * Future fix: implement DPMI AX=0306 mode-switch routines so
         * DOOM gets proper PM↔RM transition stubs instead of computing
         * its own. */

        /* Install DOS INT handlers in the IDT with DPL=3 so ring-3 DOS
         * code can invoke them via the INT instruction. Without DPL=3
         * the CPU raises #GP on INT 21h etc. */
        extern void dos_int08_stub(void);
        extern void dos_int10_stub(void);
        extern void dos_int16_stub(void);
        extern void dos_int20_stub(void);
        extern void dos_int21_stub(void);
        extern void dos_int2f_stub(void);
        extern void dos_int31_stub(void);
        extern void dos_int33_stub(void);
        struct __attribute__((packed)) idt_entry_64 {
            uint16_t offset_low;
            uint16_t selector;
            uint8_t  ist;
            uint8_t  type_attr;
            uint16_t offset_mid;
            uint32_t offset_high;
            uint32_t reserved;
        };
        extern struct idt_entry_64 idt[];
        uint16_t cs;
        __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
        /* Vector 0x20 is the APIC timer — MUST NOT overwrite. DOS programs
         * rarely use INT 20h (they use INT 21h AH=4Ch). Vector 0x08 is the
         * legacy PIC IRQ0 (also timer); skip it too for safety. */
        struct { uint8_t vec; void (*h)(void); } dos_gates[] = {
            { 0x10, dos_int10_stub },
            { 0x16, dos_int16_stub },
            { 0x21, dos_int21_stub }, { 0x2F, dos_int2f_stub },
            { 0x31, dos_int31_stub }, { 0x33, dos_int33_stub },
        };
        (void)dos_int08_stub; (void)dos_int20_stub;
        for (unsigned i = 0; i < sizeof(dos_gates)/sizeof(dos_gates[0]); i++) {
            uint64_t addr = (uint64_t)dos_gates[i].h;
            uint8_t v = dos_gates[i].vec;
            idt[v].offset_low  = (uint16_t)(addr & 0xFFFF);
            idt[v].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
            idt[v].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
            idt[v].selector    = cs;
            idt[v].ist         = 2;    /* IST2 for DOS */
            idt[v].type_attr   = 0xEF; /* P=1, DPL=3, 64-bit trap gate */
            idt[v].reserved    = 0;
        }
        serial_puts("[DOS-NT] DOS IDT gates installed (DPL=3) on IST2\n");

        dos_ldt_ok = 1;
    }

    if (!dos_ldt_ok) return;

    /* Record the VM pointer so native INT handlers can find it. */
    dos_set_native_vm(vm);

    /* Safety net for uninitialised DOS function-pointer tables: plant
     * a near-RET (0xC3) at CS:0 so a bad `callw *[X]` with a zero
     * target returns harmlessly instead of crashing on invalid opcodes
     * deeper in the data area. Touches one byte of DOOM's data but its
     * offset 0 is typically padding/zero. */
    {
        uint16_t cs_idx = (cpu->cs >> 3) & 0x1FFF;
        if (cs_idx < DPMI_MAX_DESCRIPTORS) {
            dpmi_descriptor_t *d = &vm->dpmi.ldt[cs_idx];
            uint32_t base = (uint32_t)d->base_lo
                          | ((uint32_t)d->base_mid << 16)
                          | ((uint32_t)d->base_hi  << 24);
            if (base + 1 < vm->total_mem_size) {
                uint8_t orig = vm->mem[base];
                if (orig == 0x71 || orig == 0x00) {  /* padding-looking */
                    vm->mem[base] = 0xC3;  /* RET near */
                    serial_puts("[DOS-NT] patched CS:0 with C3 (RET) safety\n");
                }
            }
        }
    }

    /* Set TSS.RSP0 so ring-3→ring-0 transitions (timer interrupt and any
     * other non-IST vector) land on a valid kernel stack. Without this
     * the CPU pushes the iret frame at offset 0 and faults at -8. */
    {
        extern struct __attribute__((packed)) {
            uint32_t reserved0;
            uint64_t rsp0;
            uint64_t rsp1;
            uint64_t rsp2;
            uint64_t reserved1;
            uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
            uint64_t reserved2;
            uint16_t reserved3;
            uint16_t iopb_offset;
        } kernel_tss;
        extern uint8_t ist1_stack[];
        /* Reuse top of IST1 — it's 64 KB, plenty even with nested ints. */
        kernel_tss.rsp0 = (uint64_t)(ist1_stack + 65536);
        serial_puts("[DOS-NT] TSS.RSP0 set to IST1 top = 0x");
        serial_puthex(kernel_tss.rsp0, 16); serial_puts("\n");
    }

    /* ── The jump ───────────────────────────────────────────── */
    /* Load DS/ES/SS from DOS4GW's LDT selectors first (they refer to
     * the LDT we're about to LLDT). Then switch CR3 so VA 0 maps to
     * vm->mem. Finally LRETQ to cs:eip in 32-bit compat mode. */
    uint64_t cr3_new = dos_cr3;
    uint16_t ldt_sel = DOS_LDT_SELECTOR;
    uint64_t cs64    = cpu->cs;
    uint64_t ip64    = cpu->eip;
    uint64_t ds64    = cpu->ds;
    /* For SS/ES, fall back to DS if the emulated value isn't a valid LDT
     * selector (e.g. stale real-mode DOS segment like 0x147D). */
    #define DOS_NT_SEL_OK(s) (((s) & 0x04) && (((s) >> 3) < DPMI_MAX_DESCRIPTORS))
    uint64_t ss64    = DOS_NT_SEL_OK(cpu->ss) ? cpu->ss : cpu->ds;
    uint64_t es64    = DOS_NT_SEL_OK(cpu->es) ? cpu->es : cpu->ds;
    uint64_t sp64    = cpu->esp;

    /* Ring-0 (CPL=0) transition: selectors must have RPL=0 to match
     * the promoted DPL=0 descriptors. */
    cs64 &= ~3ULL;
    ds64 &= ~3ULL;
    ss64 &= ~3ULL;
    es64 &= ~3ULL;

    serial_puts("[DOS-NT] LRETQ cs:eip=0x");
    serial_puthex(cs64, 4); serial_puts(":0x");
    serial_puthex(ip64, 8); serial_puts(" ss:esp=0x");
    serial_puthex(ss64, 4); serial_puts(":0x");
    serial_puthex(sp64, 8); serial_puts(" ds=0x");
    serial_puthex(ds64, 4); serial_puts("\n");

    /* DOS4GW is a ring-0 DPMI client: it expects CPL=0 with DPL=0
     * descriptors and RPL=0 selectors. Promote all LDT entries to
     * DPL=0 so segment register loads from DOS code work under CPL=0. */
    {
        dpmi_descriptor_t *ldt = vm->dpmi.ldt;
        int promoted = 0;
        for (int i = 0; i < DPMI_MAX_DESCRIPTORS; i++) {
            if ((ldt[i].access & 0x80) && (ldt[i].access & 0x60) != 0) {
                ldt[i].access &= ~0x60;  /* clear DPL → DPL=0 */
                promoted++;
            }
        }
        if (promoted) {
            serial_puts("[DOS-NT] promoted ");
            serial_putdec(promoted);
            serial_puts(" LDT entries to DPL=0 (ring-0 DPMI)\n");
        }
    }

    /* Dump LDT entries for cs/ds/ss/es to verify they're sane before LRETQ */
    {
        dpmi_descriptor_t *ldt = vm->dpmi.ldt;
        uint16_t sels[4] = { (uint16_t)cs64, (uint16_t)ds64,
                             (uint16_t)ss64, (uint16_t)es64 };
        const char *names[4] = { "CS", "DS", "SS", "ES" };
        for (int i = 0; i < 4; i++) {
            uint16_t sel = sels[i];
            uint16_t idx = sel >> 3;
            if ((sel & 0x04) == 0 || idx >= DPMI_MAX_DESCRIPTORS) {
                serial_puts("[DOS-NT] "); serial_puts(names[i]);
                serial_puts("=0x"); serial_puthex(sel, 4);
                serial_puts(" NOT LDT or OOB\n");
                continue;
            }
            dpmi_descriptor_t *d = &ldt[idx];
            serial_puts("[DOS-NT] "); serial_puts(names[i]);
            serial_puts("=0x"); serial_puthex(sel, 4);
            serial_puts(" ldt["); serial_putdec(idx); serial_puts("]");
            serial_puts(" base=0x");
            serial_puthex((uint32_t)d->base_lo |
                          ((uint32_t)d->base_mid << 16) |
                          ((uint32_t)d->base_hi << 24), 8);
            serial_puts(" limit=0x");
            serial_puthex((uint32_t)d->limit_lo |
                          ((uint32_t)(d->flags_lim & 0xF) << 16), 5);
            serial_puts(" access=0x"); serial_puthex(d->access, 2);
            serial_puts(" flags=0x"); serial_puthex(d->flags_lim, 2);
            serial_puts("\n");
        }
    }

    /* Build IRETQ frame for ring 0 → ring 3 transition.
     * IRETQ pops (from low addr up): RIP, CS, RFLAGS, RSP, SS.
     * We push in reverse: SS, RSP, RFLAGS, CS, RIP.
     * Also propagate emulated GPRs (EAX/EBX/ECX/EDX/ESI/EDI/EBP) so DOS4GW
     * starts with its expected register state instead of kernel leftovers. */
    /* RFLAGS: IF=1 (bit 9), IOPL=3 (bits 12-13), reserved-1 (bit 1).
     * IOPL=3 lets the ring-3 DOS code execute IN/OUT freely, so DOS4GW
     * and DOOM can write to the VGA DAC ports (0x3C8/0x3C9), PIC, etc.
     * The emulated chipset in QEMU absorbs the writes; future work can
     * snoop specific ports via a #GP-handler port trap. */
    uint64_t rflags = 0x3202;

    /* Debug: show page-table entries along the walk for CS:RIP target
     * (linear = CS_base + EIP) to verify USER/PRESENT/WRITABLE. */
    {
        uint64_t cs_base = (uint32_t)vm->dpmi.ldt[cs64 >> 3].base_lo
                         | ((uint32_t)vm->dpmi.ldt[cs64 >> 3].base_mid << 16)
                         | ((uint32_t)vm->dpmi.ldt[cs64 >> 3].base_hi << 24);
        uint64_t target_linear = cs_base + ip64;

        /* Dump first 16 instruction bytes at the target linear address.
         * vm->mem is a PA identity-mapped in kernel CR3, so we can index
         * directly from the kernel side to read the DOS code. */
        serial_puts("[DOS-NT] bytes @linear=0x");
        serial_puthex(target_linear, 8);
        serial_puts(":");
        for (int i = 0; i < 16 && (target_linear + i) < vm->total_mem_size; i++) {
            serial_puts(" ");
            serial_puthex(vm->mem[target_linear + i], 2);
        }
        serial_puts("\n");
        serial_puts("[DOS-NT] target linear=0x");
        serial_puthex(target_linear, 16); serial_puts("\n");
        uint64_t *pml4 = DOS_NT_PHYS_TO_VIRT(cr3_new & PTE_ADDR_MASK);
        int i4 = (target_linear >> 39) & 0x1FF;
        serial_puts("[DOS-NT] PML4[");  serial_putdec(i4);
        serial_puts("]=0x");             serial_puthex(pml4[i4], 16);
        serial_puts("\n");
        if (pml4[i4] & PTE_PRESENT) {
            uint64_t *pdpt = DOS_NT_PHYS_TO_VIRT(pml4[i4] & PTE_ADDR_MASK);
            int i3 = (target_linear >> 30) & 0x1FF;
            serial_puts("[DOS-NT] PDPT["); serial_putdec(i3);
            serial_puts("]=0x");           serial_puthex(pdpt[i3], 16);
            serial_puts("\n");
            if (pdpt[i3] & PTE_PRESENT) {
                uint64_t *pd = DOS_NT_PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
                int i2 = (target_linear >> 21) & 0x1FF;
                serial_puts("[DOS-NT] PD[");  serial_putdec(i2);
                serial_puts("]=0x");          serial_puthex(pd[i2], 16);
                serial_puts("\n");
                if ((pd[i2] & PTE_PRESENT) && !(pd[i2] & (1ULL<<7))) {
                    uint64_t *pt = DOS_NT_PHYS_TO_VIRT(pd[i2] & PTE_ADDR_MASK);
                    int i1 = (target_linear >> 12) & 0x1FF;
                    serial_puts("[DOS-NT] PT["); serial_putdec(i1);
                    serial_puts("]=0x");         serial_puthex(pt[i1], 16);
                    serial_puts("\n");
                }
            }
        }
    }
    (void)rflags;
    uint64_t reax = cpu->eax, rebx = cpu->ebx, recx = cpu->ecx, redx = cpu->edx;
    uint64_t resi = cpu->esi, redi = cpu->edi, rebp = cpu->ebp;

    /* Same-CPL (ring 0) transition via LRETQ. All LDT descriptors were
     * promoted to DPL=0 and selectors masked to RPL=0, so SS/DS/ES loads
     * in kernel context work. LRETQ pops only RIP:CS; RSP set manually. */
    __asm__ volatile (
        "cli\n"
        "lldt %w[ldt]\n"
        "mov %[cr3], %%rax\n"
        "mov %%rax, %%cr3\n"
        "mov %w[ds], %%ax\n  mov %%ax, %%ds\n"
        "mov %w[es], %%ax\n  mov %%ax, %%es\n"
        "mov %w[ss], %%ax\n  mov %%ax, %%ss\n"
        "mov %[sp], %%rsp\n"
        "pushq %[cs_q]\n"
        "pushq %[ip_q]\n"
        "mov %[r_ax], %%rax\n"
        "mov %[r_bx], %%rbx\n"
        "mov %[r_cx], %%rcx\n"
        "mov %[r_dx], %%rdx\n"
        "mov %[r_si], %%rsi\n"
        "mov %[r_di], %%rdi\n"
        "mov %[r_bp], %%rbp\n"
        "sti\n"
        "lretq\n"
        :
        : [ldt]   "r"(ldt_sel),
          [cr3]   "r"(cr3_new),
          [ds]    "r"(ds64),
          [es]    "r"(es64),
          [ss]    "r"(ss64),
          [sp]    "r"(sp64),
          [cs_q]  "r"(cs64),
          [ip_q]  "r"(ip64),
          [r_ax]  "m"(reax),
          [r_bx]  "m"(rebx),
          [r_cx]  "m"(recx),
          [r_dx]  "m"(redx),
          [r_si]  "m"(resi),
          [r_di]  "m"(redi),
          [r_bp]  "m"(rebp)
        : "memory", "cc"
    );
    /* NOTREACHED */
}
