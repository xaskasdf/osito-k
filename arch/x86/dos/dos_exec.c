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

void dos_transfer_to_native(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    serial_puts("[DOS] Attempting native transfer...\n");
    serial_puts("[DOS] CS=");
    serial_puthex(cpu->cs, 4);
    serial_puts(" EIP=");
    serial_puthex(cpu->eip, 8);
    serial_puts(" SS=");
    serial_puthex(cpu->ss, 4);
    serial_puts(" ESP=");
    serial_puthex(cpu->esp, 8);
    serial_puts("\n");

    /* DOS4GW's GDT is in emulated memory. The GDT base was loaded via LGDT.
     * For flat model (base=0, limit=4GB), we can use the existing kernel
     * GDT entries at indices 8-9 (CODE32=0x40, DATA32=0x48) which are
     * already installed by win32_init(). */

    /* Validate: the GDT must be accessible */
    if (cpu->gdtr.base >= vm->total_mem_size) {
        serial_puts("[DOS] GDT base out of range, staying in interpreter\n");
        return;
    }
    /* GDT base=0 is valid — DOS4GW puts GDT at start of memory */

    /* Read DOS4GW's code segment descriptor to verify it's flat 32-bit */
    uint16_t cs_idx = cpu->cs >> 3;
    uint32_t cs_desc_addr = cpu->gdtr.base + cs_idx * 8;
    if (cs_desc_addr + 7 >= vm->total_mem_size) {
        serial_puts("[DOS] CS descriptor out of range, staying in interpreter\n");
        return;
    }

    /* Log the descriptor */
    serial_puts("[DOS] CS descriptor at GDT[");
    serial_puthex(cs_idx, 4);
    serial_puts("]: ");
    for (int i = 0; i < 8; i++) {
        serial_puthex(vm->mem[cs_desc_addr + i], 2);
        serial_puts(" ");
    }
    serial_puts("\n");

    /* Set up the native VM state for INT dispatch */
    dos_set_native_vm(vm);

    /* The emulated memory (vm->mem) IS the physical memory that the 32-bit
     * code will access. Since OsitoK uses identity mapping, and the emulated
     * memory is allocated via mem_alloc_pages(), the 32-bit code can access
     * it directly IF it uses flat model (base=0).
     *
     * However, the emulated memory starts at some address in OsitoK's
     * address space, NOT at physical address 0. DOS4GW expects base=0.
     * We'd need to map the emulated memory at address 0 or adjust the
     * GDT base to point to our emulated memory.
     *
     * For now, log the state and return to the interpreter. The full
     * native transfer requires address space setup that we'll implement
     * after verifying the concept works. */

    serial_puts("[DOS] Native transfer: concept validated. ");
    serial_puts("Need address space mapping for base=0 flat model.\n");
    serial_puts("[DOS] Continuing in interpreter for now...\n");

    /* TODO: The full implementation will:
     * 1. Map emulated memory at linear address 0 (or adjust GDT bases)
     * 2. Install DOS4GW's GDT entries (or use flat CODE32/DATA32)
     * 3. Set up ESP from cpu->ss:cpu->esp
     * 4. LRETQ to cpu->cs:cpu->eip in 32-bit compat mode
     *
     * This requires the kernel's paging to map the emulated memory
     * region at virtual address 0, which needs paging.c changes. */
}
