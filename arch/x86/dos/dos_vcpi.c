/*
 * OsitoK -- VCPI 1.0 Server (INT 67h)
 *
 * Virtual Control Program Interface server for DOS extenders that
 * probe for EMS/VCPI before attempting a raw mode switch.  Modeled
 * on the MS-DOS 6.0 EMM386 VCPI implementation.
 *
 * DOS4GW (embedded in DOOM.EXE) checks for VCPI via INT 67h AH=DEh
 * before falling back to raw PM entry.  When VCPI is present it uses
 * function DE0Ch (Switch to Protected Mode), which lets the client
 * supply its own CR3, GDT, IDT, LDT, TR, and CS:EIP -- giving us a
 * proper IDT with valid gate descriptors for interrupt delivery.
 *
 * EMS stubs (AH=40h-46h) satisfy the prerequisite EMS-manager check
 * that every VCPI-aware extender performs before issuing DE xx calls.
 */

#include "cpu8086.h"
#include "dos_dpmi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* ── EMS state ─────────────────────────────────────────────────────── */

#define EMS_PAGE_FRAME_SEG   0xE000   /* conventional-memory window */
#define EMS_TOTAL_PAGES      256      /* 4MB of EMS (256 * 16KB) */
#define EMS_VERSION          0x40     /* EMS 4.0 */

static uint16_t ems_next_handle = 1;

/* ── VCPI constants ────────────────────────────────────────────────── */

#define VCPI_VERSION_MAJOR   1
#define VCPI_VERSION_MINOR   0

/* 8259 PIC default base vectors */
static uint16_t vcpi_pic_master_base = 0x08;
static uint16_t vcpi_pic_slave_base  = 0x70;

/* Page allocation -- reuses DPMI's bump allocator base */
#define VCPI_PAGE_POOL_BASE  0x200000   /* 2MB -- above DPMI ext pool */
#define VCPI_PAGE_SIZE       4096

static uint32_t vcpi_page_next = VCPI_PAGE_POOL_BASE;

/* ── Helpers ───────────────────────────────────────────────────────── */

static void vcpi_zero(void *dst, uint64_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++)
        p[i] = 0;
}

/* Build an 8-byte GDT descriptor in a guest memory buffer */
static void vcpi_write_descriptor(dos_vm_t *vm, uint32_t addr,
                                  uint32_t base, uint32_t limit,
                                  uint8_t access, uint8_t flags)
{
    /* limit low 16 bits */
    dos_mem_write16(vm, addr + 0, (uint16_t)(limit & 0xFFFF));
    /* base low 16 bits */
    dos_mem_write16(vm, addr + 2, (uint16_t)(base & 0xFFFF));
    /* base mid byte */
    dos_mem_write8(vm, addr + 4, (uint8_t)((base >> 16) & 0xFF));
    /* access byte */
    dos_mem_write8(vm, addr + 5, access);
    /* flags (high nibble) | limit high nibble (low nibble) */
    dos_mem_write8(vm, addr + 6, (flags & 0xF0) | (uint8_t)((limit >> 16) & 0x0F));
    /* base high byte */
    dos_mem_write8(vm, addr + 7, (uint8_t)((base >> 24) & 0xFF));
}

/* ══════════════════════════════════════════════════════════════════════
 * 1. EMS Dispatch -- AH = 40h-46h stubs
 * ══════════════════════════════════════════════════════════════════════ */

static void ems_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t ah = cpu->ah;

    switch (ah) {

    /* ── 40h: Get Status ──────────────────────────────────────────── */
    case 0x40:
        cpu->ah = 0x00;  /* success */
        serial_puts("[EMS] Get Status -> OK\n");
        break;

    /* ── 41h: Get Page Frame Segment ──────────────────────────────── */
    case 0x41:
        cpu->bx = EMS_PAGE_FRAME_SEG;
        cpu->ah = 0x00;
        serial_puts("[EMS] Page Frame -> E000\n");
        break;

    /* ── 42h: Get Unallocated Page Count ──────────────────────────── */
    case 0x42:
        cpu->bx = EMS_TOTAL_PAGES;   /* free pages */
        cpu->dx = EMS_TOTAL_PAGES;   /* total pages */
        cpu->ah = 0x00;
        serial_puts("[EMS] Page Count -> ");
        serial_putdec(EMS_TOTAL_PAGES);
        serial_puts(" free/total\n");
        break;

    /* ── 43h: Allocate Pages ──────────────────────────────────────── */
    case 0x43:
        cpu->dx = ems_next_handle++;
        cpu->ah = 0x00;
        serial_puts("[EMS] Allocate -> handle ");
        serial_putdec(cpu->dx);
        serial_puts("\n");
        break;

    /* ── 44h: Map Page (stub, just ACK) ───────────────────────────── */
    case 0x44:
        cpu->ah = 0x00;
        break;

    /* ── 45h: Deallocate Pages (stub) ─────────────────────────────── */
    case 0x45:
        cpu->ah = 0x00;
        break;

    /* ── 46h: Get Version ─────────────────────────────────────────── */
    case 0x46:
        cpu->al = EMS_VERSION;
        cpu->ah = 0x00;
        serial_puts("[EMS] Version -> 4.0\n");
        break;

    default:
        serial_puts("[EMS] Unhandled AH=");
        serial_puthex(ah, 2);
        serial_puts("\n");
        cpu->ah = 0x84;  /* function not supported */
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 2. VCPI Dispatch -- AH = DEh, AL = subfunction
 * ══════════════════════════════════════════════════════════════════════ */

/* ── DE00h: VCPI Presence Detection ──────────────────────────────── */

static void vcpi_detect(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    cpu->ah = 0x00;  /* success */
    cpu->bh = VCPI_VERSION_MAJOR;
    cpu->bl = VCPI_VERSION_MINOR;

    serial_puts("[VCPI] Presence check -> v1.0\n");
}

/* ── DE01h: Get Protected Mode Interface ─────────────────────────── */

static void vcpi_get_pm_interface(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /*
     * DS:SI points to client buffer for 3 GDT descriptors (24 bytes).
     * DI = first GDT selector the client wants us to use.
     *
     * We write:
     *   [0]  VCPI code segment  (ring 0, 32-bit code, base=0, limit=4GB)
     *   [1]  VCPI data segment  (ring 0, 32-bit data, base=0, limit=4GB)
     *   [2]  VCPI call gate     (stub -- V86 return entry)
     */
    uint32_t buf = dos_linear(cpu->ds, cpu->si);

    /* Descriptor 0: ring-0 32-bit code, base=0, limit=0xFFFFF, G=1 D=1 */
    vcpi_write_descriptor(vm, buf + 0,
        0x00000000, 0xFFFFF,
        DESC_PRESENT | DESC_SEGMENT | DESC_CODE | DESC_READABLE,
        DESC_GRANULARITY | DESC_32BIT);

    /* Descriptor 1: ring-0 32-bit data, base=0, limit=0xFFFFF, G=1 D=1 */
    vcpi_write_descriptor(vm, buf + 8,
        0x00000000, 0xFFFFF,
        DESC_PRESENT | DESC_SEGMENT | DESC_WRITABLE,
        DESC_GRANULARITY | DESC_32BIT);

    /* Descriptor 2: call gate to VCPI V86-return entry (stub for now).
     * We write a dummy code-segment descriptor; real EMM386 puts a
     * call gate here.  DOS4GW only uses DE0Ch, not the call gate. */
    vcpi_write_descriptor(vm, buf + 16,
        0x00000000, 0xFFFFF,
        DESC_PRESENT | DESC_SEGMENT | DESC_CODE | DESC_READABLE,
        DESC_GRANULARITY | DESC_32BIT);

    /* EBX = physical address of first page table entry.
     * We point to 0x1000 (a page of identity-mapped PTEs the client
     * can inspect; we fill it on demand). */
    cpu->ebx = 0x00001000;

    cpu->ah = 0x00;

    serial_puts("[VCPI] Get PM Interface: buf=");
    serial_puthex(buf, 8);
    serial_puts(" DI=");
    serial_puthex(cpu->di, 4);
    serial_puts("\n");
}

/* ── DE02h: Get Maximum Physical Address ─────────────────────────── */

static void vcpi_max_phys_addr(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    cpu->edx = vm->total_mem_size - VCPI_PAGE_SIZE;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Max phys addr -> ");
    serial_puthex(cpu->edx, 8);
    serial_puts("\n");
}

/* ── DE03h: Get Free Page Count ──────────────────────────────────── */

static void vcpi_free_page_count(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t free_bytes = 0;

    if (vm->total_mem_size > vcpi_page_next)
        free_bytes = vm->total_mem_size - vcpi_page_next;

    cpu->edx = free_bytes / VCPI_PAGE_SIZE;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Free pages -> ");
    serial_putdec(cpu->edx);
    serial_puts("\n");
}

/* ── DE04h: Allocate Page ────────────────────────────────────────── */

static void vcpi_alloc_page(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    if (vcpi_page_next + VCPI_PAGE_SIZE > vm->total_mem_size) {
        cpu->ah = 0x87;  /* not enough pages */
        serial_puts("[VCPI] Alloc page FAILED (OOM)\n");
        return;
    }

    uint32_t page = vcpi_page_next;
    vcpi_page_next += VCPI_PAGE_SIZE;

    /* Zero the page */
    vcpi_zero(vm->mem + page, VCPI_PAGE_SIZE);

    cpu->edx = page;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Alloc page -> ");
    serial_puthex(page, 8);
    serial_puts("\n");
}

/* ── DE05h: Free Page ────────────────────────────────────────────── */

static void vcpi_free_page(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* Simple bump allocator -- no real free, just acknowledge */
    cpu->ah = 0x00;
}

/* ── DE06h: Get Physical Address of Page ─────────────────────────── */

static void vcpi_get_phys_addr(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* Identity mapping: physical = linear.
     * CX:DI = page table entry (linear address in CX:DI on input,
     * but EMM386 just returns the physical address of the given page).
     * In our flat identity-mapped model, EDX = input address. */
    cpu->edx = ((uint32_t)cpu->cx << 16) | cpu->di;
    cpu->ah = 0x00;
}

/* ── DE07h: Read CR0 ─────────────────────────────────────────────── */

static void vcpi_read_cr0(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    cpu->ebx = cpu->cr0;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Read CR0 -> ");
    serial_puthex(cpu->cr0, 8);
    serial_puts("\n");
}

/* ── DE08h: Read Debug Registers (stub) ──────────────────────────── */

static void vcpi_read_debug_regs(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* Not implemented -- return zeros */
    cpu->ah = 0x00;
}

/* ── DE09h: Load Debug Registers (stub) ──────────────────────────── */

static void vcpi_load_debug_regs(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu->ah = 0x00;
}

/* ── DE0Ah: Get 8259 Interrupt Vector Mappings ───────────────────── */

static void vcpi_get_pic_mappings(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    cpu->bx = vcpi_pic_master_base;
    cpu->cx = vcpi_pic_slave_base;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Get PIC mappings: master=");
    serial_puthex(vcpi_pic_master_base, 2);
    serial_puts(" slave=");
    serial_puthex(vcpi_pic_slave_base, 2);
    serial_puts("\n");
}

/* ── DE0Bh: Set 8259 Interrupt Vector Mappings ───────────────────── */

static void vcpi_set_pic_mappings(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    vcpi_pic_master_base = cpu->bx;
    vcpi_pic_slave_base  = cpu->cx;
    cpu->ah = 0x00;

    serial_puts("[VCPI] Set PIC mappings: master=");
    serial_puthex(vcpi_pic_master_base, 2);
    serial_puts(" slave=");
    serial_puthex(vcpi_pic_slave_base, 2);
    serial_puts("\n");
}

/* ── DE0Ch: Switch to Protected Mode — THE CRITICAL ONE ──────────── */

/*
 * Switch_to_protected_struc (at linear address ESI):
 *   +00h  CR3          (dword)
 *   +04h  GDTR pointer (dword) → 6 bytes: 2 limit + 4 base
 *   +08h  IDTR pointer (dword) → 6 bytes: 2 limit + 4 base
 *   +0Ch  LDT selector (word)
 *   +0Eh  TR selector  (word)
 *   +10h  EIP          (dword)
 *   +14h  CS selector  (word)
 *
 * Called from V86/real mode, so ESI is a physical address.
 */

static void vcpi_switch_to_pm(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t struc = cpu->esi;

    /* Bounds check */
    if (struc + 0x16 > vm->total_mem_size) {
        serial_puts("[VCPI] Switch to PM: structure out of range at ");
        serial_puthex(struc, 8);
        serial_puts("\n");
        cpu->ah = 0x8B;  /* undefined subfunction */
        return;
    }

    /* Read the switch structure */
    uint32_t new_cr3     = dos_mem_read32(vm, struc + 0x00);
    uint32_t gdtr_ptr    = dos_mem_read32(vm, struc + 0x04);
    uint32_t idtr_ptr    = dos_mem_read32(vm, struc + 0x08);
    uint16_t new_ldt     = dos_mem_read16(vm, struc + 0x0C);
    uint16_t new_tr      = dos_mem_read16(vm, struc + 0x0E);
    uint32_t new_eip     = dos_mem_read32(vm, struc + 0x10);
    uint16_t new_cs      = dos_mem_read16(vm, struc + 0x14);

    /* Read GDTR: the pointer at +04h points to 6 bytes (limit:base) */
    uint16_t gdt_limit = 0;
    uint32_t gdt_base  = 0;
    if (gdtr_ptr + 6 <= vm->total_mem_size) {
        gdt_limit = dos_mem_read16(vm, gdtr_ptr + 0);
        gdt_base  = dos_mem_read32(vm, gdtr_ptr + 2);
    }

    /* Read IDTR: the pointer at +08h points to 6 bytes (limit:base) */
    uint16_t idt_limit = 0;
    uint32_t idt_base  = 0;
    if (idtr_ptr + 6 <= vm->total_mem_size) {
        idt_limit = dos_mem_read16(vm, idtr_ptr + 0);
        idt_base  = dos_mem_read32(vm, idtr_ptr + 2);
    }

    /* Apply to CPU state */
    cpu->cr3 = new_cr3;

    cpu->gdtr.limit = gdt_limit;
    cpu->gdtr.base  = gdt_base;

    cpu->idtr.limit = idt_limit;
    cpu->idtr.base  = idt_base;

    cpu->protected_mode = true;
    cpu->op_size_32     = true;
    cpu->addr_size_32   = true;
    cpu->cr0           |= 1;          /* PE bit */

    cpu->cs  = new_cs;
    cpu->eip = new_eip;

    /* Mark DPMI as active so the address translator uses GDT lookups */
    vm->dpmi.active = true;

    (void)new_ldt;  /* LDT selector -- noted but not used yet */
    (void)new_tr;   /* TR selector  -- noted but not used yet */

    serial_puts("[VCPI] Switch to PM: CR3=");
    serial_puthex(new_cr3, 8);
    serial_puts(" GDT=");
    serial_puthex(gdt_base, 8);
    serial_puts(":");
    serial_puthex(gdt_limit, 4);
    serial_puts(" IDT=");
    serial_puthex(idt_base, 8);
    serial_puts(":");
    serial_puthex(idt_limit, 4);
    serial_puts(" CS:EIP=");
    serial_puthex(new_cs, 4);
    serial_puts(":");
    serial_puthex(new_eip, 8);
    serial_puts(" LDT=");
    serial_puthex(new_ldt, 4);
    serial_puts(" TR=");
    serial_puthex(new_tr, 4);
    serial_puts("\n");
}

/* ── DE0Dh: Switch to V86 Mode (stub) ───────────────────────────── */

static void vcpi_switch_to_v86(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /*
     * The reverse of DE0Ch: PM code calls through the VCPI call gate
     * to return to V86/real mode.  Not yet needed -- DOS4GW stays in
     * PM once it switches.  Stub it so we don't crash.
     */
    serial_puts("[VCPI] Switch to V86 (stub)\n");

    cpu->protected_mode = false;
    cpu->op_size_32     = false;
    cpu->addr_size_32   = false;
    cpu->cr0           &= ~(uint32_t)1;   /* clear PE */

    cpu->ah = 0x00;
}

/* ── VCPI subfunction dispatcher ─────────────────────────────────── */

static void vcpi_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t al = cpu->al;

    switch (al) {
    case 0x00: vcpi_detect(vm);          break;
    case 0x01: vcpi_get_pm_interface(vm); break;
    case 0x02: vcpi_max_phys_addr(vm);   break;
    case 0x03: vcpi_free_page_count(vm); break;
    case 0x04: vcpi_alloc_page(vm);      break;
    case 0x05: vcpi_free_page(vm);       break;
    case 0x06: vcpi_get_phys_addr(vm);   break;
    case 0x07: vcpi_read_cr0(vm);        break;
    case 0x08: vcpi_read_debug_regs(vm); break;
    case 0x09: vcpi_load_debug_regs(vm); break;
    case 0x0A: vcpi_get_pic_mappings(vm); break;
    case 0x0B: vcpi_set_pic_mappings(vm); break;
    case 0x0C: vcpi_switch_to_pm(vm);    break;
    case 0x0D: vcpi_switch_to_v86(vm);   break;
    default:
        serial_puts("[VCPI] Unhandled AL=");
        serial_puthex(al, 2);
        serial_puts("\n");
        cpu->ah = 0x8F;  /* subfunction not supported */
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 3. Top-level INT 67h dispatcher
 * ══════════════════════════════════════════════════════════════════════ */

void dos_int67_dispatch(dos_vm_t *vm)
{
    uint8_t ah = vm->cpu->ah;

    if (ah == 0xDE) {
        vcpi_dispatch(vm);
    } else {
        ems_dispatch(vm);
    }
}
