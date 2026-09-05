/*
 * OsitoK — DOS Execution Orchestrator
 *
 * Allocates and initializes a DOS VM, loads the binary,
 * runs the CPU emulator, and cleans up.
 */

#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_audio.h"
#include "dos_io.h"
#include "dos_jit.h"
#include "dos_loader.h"
#include "dos_mouse.h"
#include "dos_vbe.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

/* OsitoFS */
extern bool     osfs2_is_mounted(void);
extern void    *osfs2_find_ci(const char *name);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* Kernel ticks */
extern uint64_t idt_get_ticks(void);

/* DOS subsystem */
extern int  dos_is_initialized(void);
extern void dos_mem_init(dos_vm_t *vm);
extern void cpu8086_init(cpu8086_state_t *cpu, dos_vm_t *vm);
extern int  cpu8086_run(dos_vm_t *vm);
extern uint64_t *dos_native_exit_jmpbuf;
extern void dos_vga_bind_vm(dos_vm_t *vm);
extern void dos_vga_unbind_vm(dos_vm_t *vm);
extern void dos_vga_set_direct_writes(dos_vm_t *vm, bool enabled);

static void dos_jit_release(dos_vm_t *vm)
{
    if (!vm || !vm->jit) return;
    jit_state_t *jit = (jit_state_t *)vm->jit;
    jit_destroy(jit);
    dos_host_free_pages(jit, (sizeof(jit_state_t) + 4095u) / 4096u);
    vm->jit = NULL;
}

/* ── IVT initialization: ROM stubs ──────────────────────────────── */

static void dos_init_ivt(dos_vm_t *vm)
{
    uint16_t rom_seg = DOS_ROM_BASE >> 4;  /* 0xF000 */
    uint16_t stub_off = 0;

    /* Initialize a conventional real-mode IVT. A protected-mode client may
     * later reuse this memory for its own GDT after loading GDTR. */
    for (int i = 0; i < 256; i++) {
        /* ROM stub: IRET (0xCF) for all vectors */
        vm->mem[DOS_ROM_BASE + stub_off] = 0xCF;

        dos_mem_write16(vm, i * 4, stub_off);
        dos_mem_write16(vm, i * 4 + 2, rom_seg);
        stub_off++;
    }

    /* Installed mouse drivers must not publish an IRET as the first byte of
     * vector 33h; legacy programs use that probe before calling function 0.
     * INT instructions are host-dispatched, while NOP;IRET remains a valid
     * fallback if guest code reaches the ROM vector directly. */
    vm->mem[DOS_ROM_BASE + 0x33U] = 0x90;
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

    /* Cursor shape and active display page. */
    dos_mem_write16(vm, 0x460, 0x0607);
    vm->mem[0x462] = 0;

    /* Rows minus 1 at 0040:0084 */
    vm->mem[0x484] = 24;

    /* Char height at 0040:0085 */
    dos_mem_write16(vm, 0x485, 16);

    /* Hardware text memory powers up with implementation-defined contents;
     * BIOS mode 3 presents a cleared page to applications. */
    for (uint8_t page = 0; page < 8U; page++) {
        for (uint32_t cell = 0; cell < 80U * 25U; cell++) {
            uint32_t addr = DOS_VRAM_BASE + (uint32_t)page * 4096U +
                            cell * 2U;
            vm->mem[addr] = ' ';
            vm->mem[addr + 1U] = 0x07;
        }
    }
    memset(vm->vga_dirty, 0xFF, sizeof(vm->vga_dirty));
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
    void *file = osfs2_find_ci(filename);
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
    uint8_t *buf = (uint8_t *)dos_host_alloc_pages(buf_pages);
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

    /* System RAM remains 16 MB; VBE contributes a separate 4 MB physical
     * aperture after it so framebuffer bytes never consume DPMI memory. */
    uint64_t total_mem = DOS_VM_ADDRESS_SPACE_SIZE;
    uint64_t mem_pages = (total_mem + 0xFFF) / 4096;
    vm.mem = (uint8_t *)dos_host_alloc_pages(mem_pages);
    if (!vm.mem) {
        serial_puts("[DOS] Failed to allocate DOS address space\n");
        dos_host_free_pages(buf, buf_pages);
        return -1;
    }
    vm.total_mem_size = (uint32_t)total_mem;
    vm.system_mem_size = DOS_TOTAL_MEM;
    vm.mem_pages = mem_pages;

    /* Zero emulated memory */
    for (uint64_t i = 0; i < total_mem; i++) vm.mem[i] = 0;

    /* Initialize subsystems */
    dos_init_ivt(&vm);
    dos_init_bda(&vm);
    dos_mem_init(&vm);
    extern void dpmi_init(dos_vm_t *vm);
    dpmi_init(&vm);

    cpu8086_init(&cpu, &vm);

    /* Set defaults */
    vm.current_drive = 2;  /* C: */
    vm.vga_mode = 0x03;
    vm.text_attr = 0x07;
    vm.cursor_start = 6;
    vm.cursor_end = 7;
    vm.start_ticks = idt_get_ticks();
    vm.last_timer_tick = vm.start_ticks;
    dos_mouse_init(&vm);
    dos_vbe_init(&vm);
    dos_api_init(&vm);

    if (!dos_io_init(&vm)) {
        serial_puts("[DOS] Failed to initialize virtual ISA devices\n");
        dos_host_free_pages(buf, buf_pages);
        dos_native_cleanup(&vm);
        return -1;
    }
    dos_audio_init(&vm);

    /* DTA defaults to PSP:0080 */
    vm.dta_off = 0x0080;

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
        rc = dos_load_mz(&vm, buf, size, filename, cmdline);
    } else if (fmt == DOS_FMT_NONE && ends_with_com(filename)) {
        rc = dos_load_com(&vm, buf, size, filename, cmdline);
    } else {
        serial_puts("[DOS] Unknown binary format\n");
        dos_host_free_pages(buf, buf_pages);
        dos_native_cleanup(&vm);
        return -1;
    }

    /* Free file buffer (data is copied into emulated memory) */
    dos_host_free_pages(buf, buf_pages);

    if (rc != 0) {
        serial_puts("[DOS] Failed to load binary\n");
        dos_native_cleanup(&vm);
        return -1;
    }

    /* Initialize the optional JIT only after the image loaded successfully. */
    {
        uint64_t jit_pages = (sizeof(jit_state_t) + 4095u) / 4096u;
        jit_state_t *jit = (jit_state_t *)dos_host_alloc_pages(jit_pages);
        if (jit) {
            uint8_t *p = (uint8_t *)jit;
            for (uint64_t i = 0; i < jit_pages * 4096u; i++) p[i] = 0;
            jit_init(jit);
            if (jit->code_buf) {
                vm.jit = jit;
                serial_puts("[DOS] JIT engine initialized\n");
            } else {
                dos_host_free_pages(jit, jit_pages);
            }
        }
    }

    /* DTA segment = PSP */
    vm.dta_seg = vm.current_psp;

    /* Run! */
    serial_puts("[DOS] Starting...\n");
    dos_vga_bind_vm(&vm);
    int exit_code = cpu8086_run(&vm);

    serial_puts("[DOS] Exit code ");
    serial_putdec((uint64_t)(uint32_t)exit_code);
    serial_puts(" (");
    serial_putdec(cpu.insn_count);
    serial_puts(" instructions)\n");

    dos_native_cleanup(&vm);

    return exit_code;
}

/* ── Native protected-mode backend ────────────────────────────── */
/*
 * The interpreter establishes the guest GDT and DPMI LDT first. Native
 * execution mirrors them into per-VM hardware tables, preserving the guest
 * contract and all kernel-owned descriptors.
 */

extern void dos_set_native_vm(dos_vm_t *vm);

extern uint64_t paging_create_process_cr3(void);
extern int      paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                       uint64_t phys, uint64_t flags);
extern uint64_t paging_get_kernel_cr3(void);
extern void     paging_switch(uint64_t cr3);
extern void     paging_free_process_cr3(uint64_t cr3);

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

void dos_native_ems_map_frame(dos_vm_t *vm, unsigned frame,
                              uint32_t backing)
{
    if (!vm || !vm->mem || !vm->native_cr3 ||
        frame >= DOS_EMS_FRAME_PAGES)
        return;

    uint64_t mem_pa = dos_nt_va_to_pa(vm->mem);
    uint64_t window = DOS_EMS_PAGE_FRAME_BASE +
                      (uint64_t)frame * DOS_EMS_PAGE_SIZE;
    uint64_t source = backing ? backing : window;
    if (source + DOS_EMS_PAGE_SIZE > vm->total_mem_size)
        return;

    for (uint64_t off = 0; off < DOS_EMS_PAGE_SIZE; off += 4096u) {
        if (paging_map_page_in_cr3(vm->native_cr3, window + off,
                                   mem_pa + source + off,
                                   PTE_PRESENT | PTE_WRITABLE |
                                   PTE_USER) == 0)
            dos_nt_propagate_user(vm->native_cr3, window + off);
    }
}

void dos_native_map_vbe_window(dos_vm_t *vm)
{
    if (!vm || !vm->mem || !vm->native_cr3) return;

    uint32_t source = DOS_VBE_WINDOW_BASE;
    if (vm->vbe_active && !vm->vbe_linear) {
        uint64_t bank_offset =
            (uint64_t)vm->vbe_bank * DOS_VBE_WINDOW_SIZE;
        if (bank_offset + DOS_VBE_WINDOW_SIZE > DOS_VBE_FB_SIZE)
            return;
        source = DOS_VBE_FB_BASE + (uint32_t)bank_offset;
    }
    if ((uint64_t)source + DOS_VBE_WINDOW_SIZE > vm->total_mem_size)
        return;

    uint64_t mem_pa = dos_nt_va_to_pa(vm->mem);
    for (uint64_t offset = 0; offset < DOS_VBE_WINDOW_SIZE;
         offset += 4096u) {
        if (paging_map_page_in_cr3(vm->native_cr3,
                                   DOS_VBE_WINDOW_BASE + offset,
                                   mem_pa + source + offset,
                                   PTE_PRESENT | PTE_WRITABLE |
                                   PTE_USER) == 0) {
            dos_nt_propagate_user(vm->native_cr3,
                                  DOS_VBE_WINDOW_BASE + offset);
        }
    }
}

/* GDT slot reserved for the DOS LDT descriptor (2 slots, 16 bytes).
 * CPU-local TSS descriptors live at slot 32 and above. Keep the historical
 * DOS selector at slot 12 so guest assumptions remain unchanged. */
#define DOS_LDT_GDT_SLOT   12
#define DOS_LDT_SELECTOR   (DOS_LDT_GDT_SLOT << 3)
#define DOS_NT_GDT_ENTRIES 68
#define DOS_NT_TABLE_PAGES 1
#define DOS_NT_DESC_PRESENT_RAW (0x80ULL << 40)
#define DOS_NT_DESC_SEGMENT_RAW (0x10ULL << 40)
#define DOS_NT_DESC_ACCESSED_RAW (0x01ULL << 40)

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} dos_nt_idt_entry_t;

extern dos_nt_idt_entry_t idt[];

static const uint8_t dos_nt_idt_vectors[12] = {
    0x01, 0x10, 0x16, 0x21, 0x2F, 0x31, 0x33,
    0x67,
    DPMI_DEFAULT_REFLECT_INT, DPMI_CALLBACK_RETURN_INT,
    DPMI_EXCEPTION_RETURN_INT,
    DPMI_RAW_SWITCH_INT
};

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dos_nt_gdtr_t;

/*
 * The native CR3 intentionally does not map the host task's lower-half
 * kernel stack. Keep the final transition operands in kernel BSS so no local
 * stack access occurs after CR3/RSP are replaced with the guest values.
 */
typedef struct {
    dos_nt_gdtr_t gdtr;
    uint16_t ldt;
    uint32_t reserved;
    uint64_t cr3, cs, ds, es, fs, gs, ss, ip, sp, rflags, kernel_sp;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
} dos_nt_enter_state_t;

static dos_nt_enter_state_t dos_nt_enter_state;
static uint8_t dos_nt_enter_stack[4096] __attribute__((aligned(16)));

extern void kern_longjmp(uint64_t *buf, int value) __attribute__((noreturn));

static uint64_t dos_nt_descriptor_raw(const dpmi_descriptor_t *desc)
{
    return (uint64_t)desc->limit_lo |
           ((uint64_t)desc->base_lo << 16) |
           ((uint64_t)desc->base_mid << 32) |
           ((uint64_t)desc->access << 40) |
           ((uint64_t)desc->flags_lim << 48) |
           ((uint64_t)desc->base_hi << 56);
}

static uint64_t dos_nt_backend_descriptor(uint64_t raw)
{
    if ((raw & (DOS_NT_DESC_PRESENT_RAW | DOS_NT_DESC_SEGMENT_RAW)) ==
        (DOS_NT_DESC_PRESENT_RAW | DOS_NT_DESC_SEGMENT_RAW))
        raw |= DOS_NT_DESC_ACCESSED_RAW;
    return raw;
}

static bool dos_nt_guest_gdt_slot_allowed(unsigned index)
{
    if (index == 0 || index >= 32 || index >= DOS_NT_GDT_ENTRIES)
        return false;
    if ((index >= 5 && index <= 9) || index == 12 || index == 13 ||
        index == 18 || index == 19)
        return false;
    return true;
}

static bool dos_nt_guest_gdt_read(dos_vm_t *vm, unsigned index,
                                  uint64_t *raw_out)
{
    if (!vm || !vm->cpu || !raw_out || !dos_nt_guest_gdt_slot_allowed(index))
        return false;

    uint32_t offset = index * 8u;
    if (offset + 7u > vm->cpu->gdtr.limit)
        return false;

    uint64_t address = (uint64_t)vm->cpu->gdtr.base + offset;
    if (address + 7u >= vm->total_mem_size || address + 7u < address)
        return false;

    uint64_t raw = 0;
    for (unsigned i = 0; i < 8; i++)
        raw |= (uint64_t)vm->mem[address + i] << (i * 8);
    *raw_out = raw;
    return true;
}

static void *dos_nt_alloc_table_page(void)
{
    uint8_t *bytes = (uint8_t *)dos_host_alloc_pages(DOS_NT_TABLE_PAGES);
    if (!bytes) return NULL;
    for (unsigned i = 0; i < 4096; i++) bytes[i] = 0;
    return bytes;
}

static void dos_nt_free_table_page(void **table)
{
    if (!table || !*table) return;
    dos_host_free_pages(*table, DOS_NT_TABLE_PAGES);
    *table = NULL;
}

static void dos_host_tls_save(dos_host_tls_t *state)
{
    uint64_t flags;
    uint32_t low, high;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile ("mov %%fs, %0; mov %%gs, %1"
                      : "=rm"(state->fs), "=rm"(state->gs));
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000100u));
    state->fs_base = ((uint64_t)high << 32) | low;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000101u));
    state->gs_base = ((uint64_t)high << 32) | low;
    state->saved = true;
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static void dos_host_tls_restore(dos_host_tls_t *state)
{
    if (!state->saved) return;
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    /* The caller has restored the host GDT. Load descriptors before bases:
     * a selector load can overwrite even a 64-bit MSR-programmed base. */
    __asm__ volatile ("mov %0, %%fs; mov %1, %%gs"
                      : : "rm"(state->fs), "rm"(state->gs) : "memory");
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100u),
                      "a"((uint32_t)state->fs_base),
                      "d"((uint32_t)(state->fs_base >> 32)) : "memory");
    __asm__ volatile ("wrmsr" : : "c"(0xC0000101u),
                      "a"((uint32_t)state->gs_base),
                      "d"((uint32_t)(state->gs_base >> 32)) : "memory");
    state->saved = false;
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static bool dos_hostmem_mapped(uint64_t cr3, const void *address, uint64_t pages)
{
    if ((uintptr_t)address < KERNEL_VBASE) return false;
    uint64_t physical = VIRT_TO_PHYS(address);
    for (uint64_t offset = 0; offset < pages * 4096u; offset += 4096u) {
        if (paging_translate_in_cr3(cr3, (uintptr_t)address + offset) !=
                physical + offset ||
            paging_translate_in_cr3(cr3, physical + offset) != UINT64_MAX)
            return false;
    }
    return true;
}

int dos_hostmem_selftest(void)
{
    const uint64_t jit_pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    uint64_t cr3 = paging_create_process_cr3();
    uint64_t *memory = (uint64_t *)dos_host_alloc_pages(2);
    void *table = dos_nt_alloc_table_page();
    jit_state_t *jit = (jit_state_t *)dos_host_alloc_pages(jit_pages);
    if (jit) jit_init(jit);
    int checks = 0, failures = 0;
#define HOSTMEM_CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        serial_puts("[DOS-HOSTMEM] FAIL: " #condition "\n"); \
    } \
} while (0)
    HOSTMEM_CHECK(cr3 && memory && table && jit && jit->code_buf);
    if (!cr3 || !memory || !table || !jit || !jit->code_buf) goto cleanup;

    HOSTMEM_CHECK(dos_host_alloc_pages(0) == NULL);
    bool memory_mapped = dos_hostmem_mapped(cr3, memory, 2);
    HOSTMEM_CHECK(memory_mapped);
    HOSTMEM_CHECK(dos_hostmem_mapped(cr3, table, DOS_NT_TABLE_PAGES));
    HOSTMEM_CHECK(dos_hostmem_mapped(cr3, jit, jit_pages));
    HOSTMEM_CHECK(dos_hostmem_mapped(cr3, jit->code_buf,
                                     (JIT_CACHE_SIZE + 4095u) / 4096u));
    bool table_zeroed = true;
    for (unsigned i = 0; i < 4096; i++)
        if (((uint8_t *)table)[i]) table_zeroed = false;
    HOSTMEM_CHECK(table_zeroed);

    if (memory_mapped) {
        const uint64_t value = 0xA17E0123456789ABULL;
        uint64_t flags, previous_cr3, observed;
        /* The private root has no lower-half mappings, including this C
         * stack. Touch only the validated high pointer until CR3 is restored. */
        __asm__ volatile (
            "pushfq; popq %[flags]; cli\n"
            "mov %%cr3, %[previous]\n"
            "mov %[root], %%cr3\n"
            "mov %[value], (%[address])\n"
            "mov (%[address]), %[observed]\n"
            "mov %[previous], %%cr3\n"
            : [flags] "=&r"(flags), [previous] "=&r"(previous_cr3),
              [observed] "=&r"(observed)
            : [root] "r"(cr3), [value] "r"(value),
              [address] "r"(&memory[1023])
            : "memory");
        if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
        HOSTMEM_CHECK(observed == value && memory[1023] == value);
    }

    jit_block_t *block = jit_get_block(jit, 0, 0x100);
    HOSTMEM_CHECK(block != NULL);
    if (block) {
        block->length = 3;
        block->ir_count = 2;
        block->ir[0] = (ir_inst_t){ .op = IR_MOV_REG_IMM,
                                   .a = REG_AX, .b = 0x1234, .width = 2 };
        block->ir[1] = (ir_inst_t){ .op = IR_EXIT_BLOCK };
        int compiled = jit_compile_block(jit, block);
        HOSTMEM_CHECK(compiled == 0 && block->compiled);
        if (compiled == 0 && block->compiled) {
            cpu8086_state_t cpu = {0};
            dos_vm_t vm = { .cpu = &cpu };
            jit_exec_block(&vm, block);
            HOSTMEM_CHECK(cpu.ax == 0x1234 && cpu.ip == 0x103 &&
                          block->exec_count == 1);
        }
    }

    extern uint64_t *tss_ist3_ptr;
    extern void sched_reset_current_compat_ist3(void);
    HOSTMEM_CHECK(tss_ist3_ptr != NULL);
    if (tss_ist3_ptr) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli"
                          : "=r"(flags) :: "memory");
        uint64_t expected = *tss_ist3_ptr;
        *tss_ist3_ptr = 0;
        sched_reset_current_compat_ist3();
        uint64_t restored = *tss_ist3_ptr;
        *tss_ist3_ptr = expected;
        if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
        HOSTMEM_CHECK(expected != 0 && restored == expected);
    }

    {
        dos_host_tls_t before, after;
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        dos_host_tls_save(&before);
        __asm__ volatile ("mov %0, %%fs; mov %0, %%gs"
                          : : "r"((uint16_t)0) : "memory");
        __asm__ volatile ("wrmsr" : : "c"(0xC0000100u),
                          "a"(0x12345000u), "d"(0) : "memory");
        __asm__ volatile ("wrmsr" : : "c"(0xC0000101u),
                          "a"(0x23456000u), "d"(0) : "memory");
        dos_host_tls_restore(&before);
        dos_host_tls_save(&after);
        if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
        HOSTMEM_CHECK(before.fs == after.fs && before.gs == after.gs);
        HOSTMEM_CHECK(before.fs_base == after.fs_base &&
                      before.gs_base == after.gs_base && !before.saved);
    }

cleanup:
    if (jit) {
        jit_destroy(jit);
        dos_host_free_pages(jit, jit_pages);
    }
    dos_nt_free_table_page(&table);
    dos_host_free_pages(memory, 2);
    if (cr3) paging_free_process_cr3(cr3);
    serial_puts("[DOS-HOSTMEM] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef HOSTMEM_CHECK
    return failures;
}

void dos_native_sync_ldt(dos_vm_t *vm)
{
    if (!vm || !vm->native_ldt) return;
    uint64_t *native = (uint64_t *)vm->native_ldt;
    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++) {
        dpmi_descriptor_t descriptor;
        uint16_t selector = (uint16_t)((i << 3) | 0x07u);
        native[i] = dpmi_guest_descriptor(vm, selector, &descriptor)
                  ? dos_nt_backend_descriptor(
                        dos_nt_descriptor_raw(&descriptor))
                  : 0;
    }
    __asm__ volatile ("mfence" ::: "memory");
}

static void dos_native_sync_gdt(dos_vm_t *vm)
{
    if (!vm || !vm->native_gdt || !vm->native_ldt) return;
    uint64_t *native = (uint64_t *)vm->native_gdt;

    for (unsigned i = 0; i < DOS_NT_GDT_ENTRIES; i++)
        native[i] = kernel_gdt[i];

    for (unsigned i = 1; i < 32; i++) {
        if (!dos_nt_guest_gdt_slot_allowed(i)) continue;
        native[i] = 0;
        uint64_t raw;
        if (dos_nt_guest_gdt_read(vm, i, &raw) &&
            (raw & DOS_NT_DESC_SEGMENT_RAW))
            native[i] = dos_nt_backend_descriptor(raw);
    }

    uint64_t ldt_base = (uint64_t)vm->native_ldt;
    uint32_t ldt_limit = (uint32_t)(sizeof(vm->dpmi.ldt) - 1);
    native[DOS_LDT_GDT_SLOT] =
          (uint64_t)(ldt_limit & 0xFFFF)
        | ((ldt_base & 0xFFFFFFULL) << 16)
        | ((uint64_t)0x82 << 40)
        | ((uint64_t)((ldt_limit >> 16) & 0xF) << 48)
        | (((ldt_base >> 24) & 0xFFULL) << 56);
    native[DOS_LDT_GDT_SLOT + 1] = ldt_base >> 32;
    __asm__ volatile ("mfence" ::: "memory");
}

static bool dos_nt_selector_descriptor(dos_vm_t *vm, uint16_t selector,
                                       uint64_t *raw_out)
{
    if (!vm || !raw_out || (selector & ~3u) == 0) return false;
    unsigned index = selector >> 3;
    uint64_t raw;

    if (selector & 0x04) {
        dpmi_descriptor_t descriptor;
        if (index >= DPMI_MAX_DESCRIPTORS ||
            !dpmi_guest_descriptor(vm, selector, &descriptor))
            return false;
        raw = dos_nt_descriptor_raw(&descriptor);
    } else if (!dos_nt_guest_gdt_read(vm, index, &raw)) {
        return false;
    }

    if (!(raw & DOS_NT_DESC_SEGMENT_RAW)) return false;
    *raw_out = raw;
    return true;
}

static bool dos_nt_selector_valid(dos_vm_t *vm, uint16_t selector,
                                  bool require_code, bool require_stack)
{
    if ((selector & ~3u) == 0)
        return !require_code && !require_stack;

    uint64_t raw;
    if (!dos_nt_selector_descriptor(vm, selector, &raw) ||
        !(raw & DOS_NT_DESC_PRESENT_RAW))
        return false;

    uint8_t access = (uint8_t)(raw >> 40);
    bool is_code = (access & DESC_CODE) != 0;
    if (require_code) return is_code;
    if (require_stack) return !is_code && (access & DESC_WRITABLE);
    return !is_code || (access & DESC_READABLE);
}

static bool dos_nt_selector_linear(dos_vm_t *vm, uint16_t selector,
                                   uint32_t offset, uint64_t *linear_out)
{
    uint64_t raw;
    if (!linear_out || !dos_nt_selector_descriptor(vm, selector, &raw) ||
        !(raw & DOS_NT_DESC_PRESENT_RAW))
        return false;

    uint32_t base = (uint32_t)((raw >> 16) & 0xFFFF)
                  | (uint32_t)(((raw >> 32) & 0xFF) << 16)
                  | (uint32_t)(((raw >> 56) & 0xFF) << 24);
    uint32_t limit = (uint32_t)(raw & 0xFFFF)
                   | (uint32_t)(((raw >> 48) & 0xF) << 16);
    if (raw & (1ULL << 55))
        limit = (limit << 12) | 0xFFF;
    if (offset > limit) return false;

    uint64_t linear = (uint64_t)base + offset;
    if (linear >= vm->total_mem_size || linear < base) return false;
    *linear_out = linear;
    return true;
}

int dos_native_refresh_guest_selector(dos_vm_t *vm, uint16_t error_code)
{
    if (!vm || !error_code || (error_code & 0x02))
        return 0;

    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        paging_switch(kernel_cr3);

    if (!vm->native_active || !vm->native_gdt || !vm->native_ldt)
        goto unchanged;

    unsigned index = error_code >> 3;
    uint64_t *table;
    uint64_t next;
    if (error_code & 0x04) {
        dpmi_descriptor_t descriptor;
        if (index >= DPMI_MAX_DESCRIPTORS ||
            !dpmi_guest_descriptor(vm, (uint16_t)error_code,
                                   &descriptor))
            goto unchanged;
        table = (uint64_t *)vm->native_ldt;
        next = dos_nt_backend_descriptor(
            dos_nt_descriptor_raw(&descriptor));
    } else {
        uint64_t raw;
        if (!dos_nt_guest_gdt_read(vm, index, &raw) ||
            !(raw & DOS_NT_DESC_SEGMENT_RAW))
            goto unchanged;
        table = (uint64_t *)vm->native_gdt;
        next = dos_nt_backend_descriptor(raw);
    }

    if (table[index] == next) goto unchanged;
    table[index] = next;
    __asm__ volatile ("mfence" ::: "memory");
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        paging_switch(saved_cr3);
    return (next & DOS_NT_DESC_PRESENT_RAW) != 0;

unchanged:
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        paging_switch(saved_cr3);
    return 0;
}

static void dos_native_switch_to_kernel_cr3(void)
{
    uint64_t current_cr3;
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (kernel_cr3 && current_cr3 != kernel_cr3)
        paging_switch(kernel_cr3);
}

static void dos_native_leave_backend(dos_vm_t *vm)
{
    if (!vm) return;

    /* vm may live on a host stack that is absent from the DOS address space. */
    dos_native_switch_to_kernel_cr3();
    dos_vga_set_direct_writes(vm, false);

    bool had_native_state = vm->native_cr3 || vm->native_gdt ||
                            vm->native_ldt || vm->native_idt_saved ||
                            vm->native_active || vm->native_host_tls.saved;
    if (had_native_state) {
        __asm__ volatile ("cli" ::: "memory");

        /* Native DOS traps arrive at CPL0 on an IST stack. Restore host
         * segments before invalidating the client's LDTR. */
        uint16_t host_data = 0x30;
        uint16_t null_ldt = 0;
        __asm__ volatile (
            "mov %0, %%ds\n"
            "mov %0, %%es\n"
            "mov %0, %%ss\n"
            "lldt %1\n"
            : : "r"(host_data), "r"(null_ldt) : "memory");

        if (vm->native_idt_saved) {
            for (unsigned i = 0; i < sizeof(dos_nt_idt_vectors); i++) {
                uint8_t *dst = (uint8_t *)&idt[dos_nt_idt_vectors[i]];
                for (unsigned j = 0; j < 16; j++)
                    dst[j] = vm->native_saved_idt[i][j];
            }
            vm->native_idt_saved = false;
        }

        __asm__ volatile ("lgdt %0" : : "m"(kernel_gdtr) : "memory");
        dos_host_tls_restore(&vm->native_host_tls);

        dos_set_native_vm(NULL);
        vm->native_active = false;
        vm->native_ready = false;

        if (vm->native_cr3) {
            paging_free_process_cr3(vm->native_cr3);
            vm->native_cr3 = 0;
        }
        dos_nt_free_table_page(&vm->native_gdt);
        dos_nt_free_table_page(&vm->native_ldt);
    }

}

void dos_native_release_backend(dos_vm_t *vm)
{
    dos_native_leave_backend(vm);
}

void dos_native_cleanup(dos_vm_t *vm)
{
    if (!vm) return;

    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags));

    /* Fault recovery can enter here while the DOS process CR3 is active. */
    dos_native_switch_to_kernel_cr3();
    dos_exec_cleanup(vm);
    dos_native_leave_backend(vm);
    vm->native_resume_armed = false;
    dos_vga_unbind_vm(vm);

    dos_audio_shutdown(vm);
    dos_io_shutdown(vm);
    dos_api_close_all(vm);
    dos_jit_release(vm);
    dos_vcpi_cleanup(vm);
    if (vm->mem && vm->mem_pages) {
        dos_host_free_pages(vm->mem, vm->mem_pages);
        vm->mem = NULL;
        vm->mem_pages = 0;
        vm->total_mem_size = 0;
    }

    if (saved_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void dos_native_suspend(dos_vm_t *vm)
{
    if (!vm || !vm->native_resume_armed) {
        serial_puts("[DOS-NT] raw mode switch has no interpreter context\n");
        dos_native_cleanup(vm);
        if (dos_native_exit_jmpbuf)
            kern_longjmp(dos_native_exit_jmpbuf, 2);
        __asm__ volatile ("cli" ::: "memory");
        for (;;) __asm__ volatile ("hlt");
    }

    dos_native_leave_backend(vm);
    extern void x86_tss_reset_ist2(void);
    x86_tss_reset_ist2();
    kern_longjmp(vm->native_resume_jmpbuf, 1);
}

void dos_transfer_to_native(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* A translated real-mode service may invoke a callback while its outer
     * native INT frame is still live. Interpret that nested protected-mode
     * callback so the outer CR3/IDT context remains intact. */
    if (vm->native_dispatch_depth)
        return;

    if (!vm->native_resume_armed) {
        serial_puts("[DOS-NT] native entry has no interpreter context\n");
        return;
    }

    serial_puts("[DOS-NT] enter cs=0x");  serial_puthex(cpu->cs, 4);
    serial_puts(" eip=0x");                serial_puthex(cpu->eip, 8);
    serial_puts(" ss=0x");                 serial_puthex(cpu->ss, 4);
    serial_puts(" esp=0x");                serial_puthex(cpu->esp, 8);
    serial_puts(" ds=0x");                 serial_puthex(cpu->ds, 4);
    serial_puts("\n");

    uint64_t target_linear;
    if (!dos_nt_selector_valid(vm, cpu->cs, true, false) ||
        !dos_nt_selector_valid(vm, cpu->ss, false, true) ||
        !dos_nt_selector_valid(vm, cpu->ds, false, false) ||
        !dos_nt_selector_valid(vm, cpu->es, false, false) ||
        !dos_nt_selector_linear(vm, cpu->cs, cpu->eip, &target_linear)) {
        serial_puts("[DOS-NT] descriptor state is not ready; continuing in interpreter\n");
        return;
    }

    /* ── One-time CR3 + LDT-descriptor setup ────────────────── */
    if (!vm->native_ready) {
        vm->native_cr3 = paging_create_process_cr3();
        if (!vm->native_cr3) {
            serial_puts("[DOS-NT] paging_create_process_cr3 FAILED\n");
            return;
        }

        /* Identity-map vm->mem pages at DOS CR3 VA 0..total_mem_size. */
        uint64_t mem_pa = dos_nt_va_to_pa(vm->mem);
        uint64_t mapped = 0;
        for (uint64_t off = 0; off < vm->total_mem_size; off += 4096) {
            if (paging_map_page_in_cr3(vm->native_cr3, off, mem_pa + off,
                                       PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
                serial_puts("[DOS-NT] map FAILED at off=0x");
                serial_puthex(off, 8); serial_puts("\n");
                paging_free_process_cr3(vm->native_cr3);
                vm->native_cr3 = 0;
                return;
            }
            dos_nt_propagate_user(vm->native_cr3, off);
            mapped += 4096;
        }
        for (unsigned frame = 0; frame < DOS_EMS_FRAME_PAGES; frame++) {
            if (vm->ems_frame_bases[frame])
                dos_native_ems_map_frame(vm, frame,
                                         vm->ems_frame_bases[frame]);
        }
        dos_native_map_vbe_window(vm);
        serial_puts("[DOS-NT] CR3=0x");    serial_puthex(vm->native_cr3, 16);
        serial_puts(" vm->mem pa=0x");     serial_puthex(mem_pa, 16);
        serial_puts(" mapped=");           serial_putdec(mapped >> 10);
        serial_puts(" KB @ VA 0\n");

        vm->native_gdt = dos_nt_alloc_table_page();
        vm->native_ldt = dos_nt_alloc_table_page();
        if (!vm->native_gdt || !vm->native_ldt) {
            serial_puts("[DOS-NT] descriptor-table allocation FAILED\n");
            dos_nt_free_table_page(&vm->native_gdt);
            dos_nt_free_table_page(&vm->native_ldt);
            paging_free_process_cr3(vm->native_cr3);
            vm->native_cr3 = 0;
            return;
        }

        dos_native_sync_ldt(vm);
        dos_native_sync_gdt(vm);
        serial_puts("[DOS-NT] private GDT/LDT prepared from guest tables\n");
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
        extern void dos_int67_stub(void);
        extern void dos_intf9_stub(void);
        extern void dos_intfa_stub(void);
        extern void dos_intfc_stub(void);
        extern void dos_intfd_stub(void);
        extern void dos_exc01_stub(void);
        uint16_t cs;
        __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
        /* Vector 0x20 is the APIC timer — MUST NOT overwrite. DOS programs
         * rarely use INT 20h (they use INT 21h AH=4Ch). Vector 0x08 is the
         * legacy PIC IRQ0 (also timer); skip it too for safety. */
        struct { uint8_t vec; void (*h)(void); } dos_gates[] = {
            { 0x01, dos_exc01_stub },
            { 0x10, dos_int10_stub },
            { 0x16, dos_int16_stub },
            { 0x21, dos_int21_stub }, { 0x2F, dos_int2f_stub },
            { 0x31, dos_int31_stub }, { 0x33, dos_int33_stub },
            { 0x67, dos_int67_stub },
            { DPMI_DEFAULT_REFLECT_INT, dos_intf9_stub },
            { DPMI_CALLBACK_RETURN_INT, dos_intfa_stub },
            { DPMI_EXCEPTION_RETURN_INT, dos_intfd_stub },
            { DPMI_RAW_SWITCH_INT, dos_intfc_stub },
        };
        (void)dos_int08_stub; (void)dos_int20_stub;
        if (!vm->native_idt_saved) {
            for (unsigned i = 0; i < sizeof(dos_nt_idt_vectors); i++) {
                const uint8_t *src = (const uint8_t *)&idt[dos_nt_idt_vectors[i]];
                for (unsigned j = 0; j < 16; j++)
                    vm->native_saved_idt[i][j] = src[j];
            }
            vm->native_idt_saved = true;
        }
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

        vm->native_ready = true;
    }

    if (!vm->native_ready) return;

    dos_native_sync_ldt(vm);
    dos_native_sync_gdt(vm);

    /* Native guests own FS/GS until exit or a raw-mode suspension. */
    if (!vm->native_host_tls.saved)
        dos_host_tls_save(&vm->native_host_tls);

    /* Record the VM pointer so native INT handlers can find it. */
    dos_vga_set_direct_writes(vm, true);
    vm->native_active = true;
    dos_set_native_vm(vm);

    /* Ring-3-to-ring-0 compatibility transitions use the BSP's permanent IRQ
     * stack through RSP0. Ordinary external interrupts select IST4 directly. */
    {
        extern void x86_tss_reset_rsp0(void);
        x86_tss_reset_rsp0();
        serial_puts("[DOS-NT] TSS.RSP0 reset to IRQ stack\n");
    }

    dos_nt_enter_state_t *enter = &dos_nt_enter_state;
    enter->gdtr = (dos_nt_gdtr_t) {
        .limit = (uint16_t)(DOS_NT_GDT_ENTRIES * 8 - 1),
        .base = (uint64_t)vm->native_gdt,
    };
    enter->ldt = DOS_LDT_SELECTOR;
    enter->cr3 = vm->native_cr3;
    enter->cs = cpu->cs;
    enter->ds = cpu->ds;
    enter->es = cpu->es;
    enter->fs = cpu->fs;
    enter->gs = cpu->gs;
    enter->ss = cpu->ss;
    enter->ip = cpu->eip;
    enter->sp = cpu->esp;
    /* Host interrupts must remain enabled while a CPL3 DOS client runs.
     * IOPL stays at zero so IN/OUT are mediated by the virtual-device bus. */
    enter->rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                    ~(uint64_t)FLAG_IOPL_MASK;
    enter->kernel_sp = (uint64_t)(dos_nt_enter_stack +
                                  sizeof(dos_nt_enter_stack));
    enter->rax = cpu->eax;
    enter->rbx = cpu->ebx;
    enter->rcx = cpu->ecx;
    enter->rdx = cpu->edx;
    enter->rsi = cpu->esi;
    enter->rdi = cpu->edi;
    enter->rbp = cpu->ebp;

    serial_puts("[DOS-NT] IRETQ cs:eip=0x");
    serial_puthex(enter->cs, 4); serial_puts(":0x");
    serial_puthex(enter->ip, 8); serial_puts(" ss:esp=0x");
    serial_puthex(enter->ss, 4); serial_puts(":0x");
    serial_puthex(enter->sp, 8); serial_puts(" ds=0x");
    serial_puthex(enter->ds, 4); serial_puts(" linear=0x");
    serial_puthex(target_linear, 8); serial_puts("\n");

    serial_puts("[DOS-NT] bytes @linear=0x");
    serial_puthex(target_linear, 8);
    serial_puts(":");
    for (unsigned i = 0; i < 16 && target_linear + i < vm->total_mem_size; i++) {
        serial_puts(" ");
        serial_puthex(vm->mem[target_linear + i], 2);
    }
    serial_puts("\n");

    /* Enter the 32-bit compatibility client at CPL3. The transition frame
     * lives on a high-half kernel stack mapped by both address spaces. */
    __asm__ volatile (
        "mov %[enter], %%r11\n"
        "cli\n"
        "lgdt %c[gdtr](%%r11)\n"
        "lldt %c[ldt](%%r11)\n"
        "movq %c[kernel_sp](%%r11), %%rsp\n"
        "mov %c[cr3](%%r11), %%rax\n"
        "mov %%rax, %%cr3\n"
        "movw %c[ds](%%r11), %%ax\n  mov %%ax, %%ds\n"
        "movw %c[es](%%r11), %%ax\n  mov %%ax, %%es\n"
        "movw %c[fs](%%r11), %%ax\n  mov %%ax, %%fs\n"
        "movw %c[gs](%%r11), %%ax\n  mov %%ax, %%gs\n"
        "pushq %c[ss](%%r11)\n"
        "pushq %c[sp](%%r11)\n"
        "pushq %c[rflags](%%r11)\n"
        "pushq %c[cs](%%r11)\n"
        "pushq %c[ip](%%r11)\n"
        "movq %c[rbx](%%r11), %%rbx\n"
        "movq %c[rcx](%%r11), %%rcx\n"
        "movq %c[rdx](%%r11), %%rdx\n"
        "movq %c[rsi](%%r11), %%rsi\n"
        "movq %c[rdi](%%r11), %%rdi\n"
        "movq %c[rbp](%%r11), %%rbp\n"
        "movq %c[rax](%%r11), %%rax\n"
        "iretq\n"
        :
        : [enter] "r"(enter),
          [gdtr] "i"(__builtin_offsetof(dos_nt_enter_state_t, gdtr)),
          [ldt]  "i"(__builtin_offsetof(dos_nt_enter_state_t, ldt)),
          [cr3]  "i"(__builtin_offsetof(dos_nt_enter_state_t, cr3)),
          [cs]   "i"(__builtin_offsetof(dos_nt_enter_state_t, cs)),
          [ds]   "i"(__builtin_offsetof(dos_nt_enter_state_t, ds)),
          [es]   "i"(__builtin_offsetof(dos_nt_enter_state_t, es)),
          [fs]   "i"(__builtin_offsetof(dos_nt_enter_state_t, fs)),
          [gs]   "i"(__builtin_offsetof(dos_nt_enter_state_t, gs)),
          [ss]   "i"(__builtin_offsetof(dos_nt_enter_state_t, ss)),
          [ip]   "i"(__builtin_offsetof(dos_nt_enter_state_t, ip)),
          [sp]   "i"(__builtin_offsetof(dos_nt_enter_state_t, sp)),
          [rflags] "i"(__builtin_offsetof(dos_nt_enter_state_t, rflags)),
          [kernel_sp] "i"(__builtin_offsetof(dos_nt_enter_state_t, kernel_sp)),
          [rax]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rax)),
          [rbx]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rbx)),
          [rcx]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rcx)),
          [rdx]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rdx)),
          [rsi]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rsi)),
          [rdi]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rdi)),
          [rbp]  "i"(__builtin_offsetof(dos_nt_enter_state_t, rbp))
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r11",
          "memory", "cc"
    );
    /* NOTREACHED */
}
