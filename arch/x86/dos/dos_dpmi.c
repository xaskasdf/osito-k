/*
 * OsitoK -- DPMI 0.9 Host Implementation
 *
 * DOS Protected Mode Interface host for 32-bit DOS extenders.
 * Provides LDT management, extended memory allocation, real-mode
 * interrupt simulation, and mode switching for DOS4GW-style clients.
 * This is the path DOOM takes to run: INT 2Fh/1687h detects DPMI,
 * the entry stub switches to protected mode, and INT 31h services
 * handle memory allocation and real-mode INT callbacks.
 */

#include "cpu8086.h"
#include "dos_dpmi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* DOS memory manager */
extern uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest);
extern int      dos_mem_free(dos_vm_t *vm, uint16_t segment);

/* DOS INT dispatch (for real-mode interrupt simulation) */
extern void dos_int21_dispatch(dos_vm_t *vm);
extern void dos_int10_video(dos_vm_t *vm);
extern void dos_int16_keyboard(dos_vm_t *vm);
extern void dos_int1a_timer(dos_vm_t *vm);
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);

/* ── Helper: zero memory (no libc) ─────────────────────────────── */

static void dpmi_zero(void *dst, uint64_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++)
        p[i] = 0;
}

/* ── Helper: build a descriptor ────────────────────────────────── */

static void dpmi_build_desc(dpmi_descriptor_t *d, uint32_t base,
                            uint32_t limit, uint8_t access, uint8_t flags)
{
    dpmi_zero(d, sizeof(*d));
    dpmi_desc_set_base(d, base);
    d->access = access;
    d->flags_lim = flags;
    dpmi_desc_set_limit(d, limit);
}

/* ══════════════════════════════════════════════════════════════════
 * 1. dpmi_init -- Initialize DPMI host state
 * ══════════════════════════════════════════════════════════════════ */

void dpmi_init(dos_vm_t *vm)
{
    dpmi_state_t *dpmi = &vm->dpmi;

    /* Zero entire DPMI state */
    dpmi_zero(dpmi, sizeof(*dpmi));

    dpmi->ext_alloc_next = DPMI_EXT_BASE;
    dpmi->next_handle = 1;

    /* Write DPMI entry stub in ROM area at F000:0100
     * CD FE   INT 0xFE
     * CB      RETF
     */
    uint32_t stub_addr = dos_linear(DPMI_ENTRY_SEG, DPMI_ENTRY_OFF);
    if (stub_addr + 3 <= vm->total_mem_size) {
        vm->mem[stub_addr + 0] = 0xCD;  /* INT */
        vm->mem[stub_addr + 1] = 0xFE;  /* 0xFE */
        vm->mem[stub_addr + 2] = 0xCB;  /* RETF */
    }

    /* Save/restore & mode-switch stubs for INT 31h AX=0305h and 0x0306h.
     * All are one-byte 0xCB (RETF) — the client calls FAR into the stub,
     * it immediately returns via the pushed return frame. In 16-bit mode
     * the RETF pops 16-bit IP:CS; in 32-bit it pops 32-bit EIP:CS. Both
     * work, so a single byte serves all four roles.
     *
     *   F000:0110  RETF — save/restore stub (used for both RM and PM)
     *   F000:0118  RETF — raw mode-switch stub (RM↔PM)
     */
    uint32_t save_stub = dos_linear(DPMI_ENTRY_SEG, 0x0110);
    uint32_t ms_stub   = dos_linear(DPMI_ENTRY_SEG, 0x0118);
    if (ms_stub + 1 <= vm->total_mem_size) {
        vm->mem[save_stub] = 0xCB;  /* RETF — save/restore */
        vm->mem[ms_stub]   = 0xCB;  /* RETF — mode switch */
    }

    serial_puts("[DPMI] Host initialized, entry at F000:0100,"
                " save F000:0110, mode-switch F000:0118\n");
}

/* ══════════════════════════════════════════════════════════════════
 * 2. dpmi_translate -- Selector:offset to linear address
 * ══════════════════════════════════════════════════════════════════ */

uint32_t dpmi_translate(dos_vm_t *vm, uint16_t selector, uint32_t offset)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t index = selector >> 3;
    bool ti = (selector >> 2) & 1;  /* TI=0: GDT, TI=1: LDT */
    uint32_t base = 0;

    if (ti) {
        /* LDT lookup (our DPMI descriptors) */
        if (index < DPMI_MAX_DESCRIPTORS) {
            base = dpmi_desc_get_base(&vm->dpmi.ldt[index]);
        }
    } else {
        /* GDT lookup (guest's GDT, loaded via LGDT) */
        if (cpu->gdtr.base && index > 0) {
            uint32_t desc_addr = cpu->gdtr.base + (uint32_t)(index << 3);
            if (desc_addr + 7 < vm->total_mem_size) {
                dpmi_descriptor_t *d = (dpmi_descriptor_t *)(vm->mem + desc_addr);
                base = dpmi_desc_get_base(d);
            }
        }
    }

    uint32_t linear = base + offset;

    /* If paging is enabled (CR0.PG bit 31) and address is out of range,
     * walk the guest's page tables to find the physical address */
    if ((cpu->cr0 & 0x80000000) && linear >= vm->total_mem_size) {
        uint32_t cr3 = cpu->cr3 & 0xFFFFF000;
        if (cr3 < vm->total_mem_size) {
            /* 32-bit paging: PDE (bits 31:22) → PTE (bits 21:12) → offset (bits 11:0) */
            uint32_t pde_idx = (linear >> 22) & 0x3FF;
            uint32_t pde_addr = cr3 + pde_idx * 4;
            if (pde_addr + 3 < vm->total_mem_size) {
                uint32_t pde = dos_mem_read32(vm, pde_addr);
                if (pde & 1) {  /* present */
                    if (pde & 0x80) {
                        /* 4MB large page (PSE) */
                        uint32_t phys = (pde & 0xFFC00000) | (linear & 0x003FFFFF);
                        if (phys < vm->total_mem_size) return phys;
                    } else {
                        /* 4KB page: walk PTE */
                        uint32_t pt_base = pde & 0xFFFFF000;
                        uint32_t pte_idx = (linear >> 12) & 0x3FF;
                        uint32_t pte_addr = pt_base + pte_idx * 4;
                        if (pte_addr + 3 < vm->total_mem_size) {
                            uint32_t pte = dos_mem_read32(vm, pte_addr);
                            if (pte & 1) {  /* present */
                                uint32_t phys = (pte & 0xFFFFF000) | (linear & 0xFFF);
                                if (phys < vm->total_mem_size) return phys;
                            }
                        }
                    }
                }
            }
        }
    }

    if (linear >= vm->total_mem_size)
        linear %= vm->total_mem_size;

    return linear;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. dos_int2f_dispatch -- INT 2Fh multiplex
 * ══════════════════════════════════════════════════════════════════ */

void dos_int2f_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    switch (cpu->ax) {
    case 0x1687:
        /* DPMI detection: report DPMI 0.90 available */
        serial_puts("[DPMI] INT 2Fh/1687h: DPMI detected\n");
        cpu->ax = 0x0000;    /* DPMI host present */
        cpu->bx = 0x0001;    /* 32-bit programs supported */
        cpu->cl = 0x03;      /* processor type: 386 */
        cpu->dh = 0x00;      /* version major: 0 */
        cpu->dl = 0x5A;      /* version minor: 90 (0x5A) */
        cpu->si = 0x0000;    /* no private data area needed */
        cpu->es = DPMI_ENTRY_SEG;
        cpu->di = DPMI_ENTRY_OFF;
        cpu->eflags &= ~FLAG_CF;  /* success */
        break;

    default:
        /* Unhandled INT 2Fh function */
        cpu->eflags |= FLAG_CF;
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 5. dpmi_alloc_descriptor -- Allocate an LDT entry
 * ══════════════════════════════════════════════════════════════════ */

static uint16_t dpmi_alloc_descriptor(dpmi_state_t *dpmi)
{
    uint16_t start = dpmi->next_free_index;
    uint16_t i = start;

    do {
        dpmi_descriptor_t *d = &dpmi->ldt[i];
        /* A slot is free if its access byte has the present bit clear */
        if (!(d->access & DESC_PRESENT)) {
            /* Mark as allocated (present, but minimal -- caller fills in) */
            d->access = DESC_PRESENT;
            dpmi->next_free_index = (i + 1) % DPMI_MAX_DESCRIPTORS;
            return dpmi_index_to_sel(i);
        }
        i = (i + 1) % DPMI_MAX_DESCRIPTORS;
    } while (i != start);

    /* No free descriptors */
    serial_puts("[DPMI] Out of LDT descriptors!\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. dpmi_enter_protected_mode -- Real→protected mode switch
 * ══════════════════════════════════════════════════════════════════ */

void dpmi_enter_protected_mode(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;

    /* Save real-mode state */
    dpmi->saved_cs = cpu->cs;
    dpmi->saved_ip = cpu->ip;
    dpmi->saved_ss = cpu->ss;
    dpmi->saved_sp = cpu->sp;
    dpmi->saved_ds = cpu->ds;
    dpmi->saved_es = cpu->es;

    /* Activate DPMI */
    dpmi->active   = true;
    dpmi->is_32bit = (cpu->ax & 1) ? true : false;

    serial_puts("[DPMI] Mode switch: ");
    serial_puts(dpmi->is_32bit ? "32-bit" : "16-bit");
    serial_puts(" client\n");

    /* Per DPMI spec, initial selectors map the client's real-mode segments.
     * cpu->cs is F000 (the stub), so read the CALLER's CS from the stack.
     * DS/ES/SS are the client's original values (unchanged by CALL FAR). */
    uint32_t stack_linear_pre = ((uint32_t)cpu->ss << 4) + cpu->sp;
    uint16_t caller_cs_val = dos_mem_read16(vm, stack_linear_pre + 8);
    uint32_t cs_base = (uint32_t)caller_cs_val << 4;
    uint32_t ds_base = (uint32_t)cpu->ds << 4;
    uint32_t ss_base = (uint32_t)cpu->ss << 4;
    uint32_t es_base = (uint32_t)cpu->es << 4;

    dpmi->sel_code = dpmi_alloc_descriptor(dpmi);
    {
        uint16_t idx = dpmi_sel_to_index(dpmi->sel_code);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                         DESC_CODE | DESC_READABLE;
        dpmi_build_desc(d, cs_base, 0xFFFF, access, 0);  /* USE16, byte gran */
    }

    dpmi->sel_data = dpmi_alloc_descriptor(dpmi);
    {
        uint16_t idx = dpmi_sel_to_index(dpmi->sel_data);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                         DESC_WRITABLE;
        dpmi_build_desc(d, ds_base, 0xFFFF, access, 0);  /* USE16 */
    }

    dpmi->sel_stack = dpmi_alloc_descriptor(dpmi);
    {
        uint16_t idx = dpmi_sel_to_index(dpmi->sel_stack);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                         DESC_WRITABLE;
        dpmi_build_desc(d, ss_base, 0xFFFF, access, 0);  /* USE16 */
    }

    dpmi->sel_psp = dpmi_alloc_descriptor(dpmi);
    {
        uint16_t idx = dpmi_sel_to_index(dpmi->sel_psp);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        uint32_t psp_base = (uint32_t)vm->current_psp << 4;
        uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                         DESC_WRITABLE;
        dpmi_build_desc(d, psp_base, 0xFF, access, 0);
    }

    /* ES selector maps the PSP (per DPMI spec) */
    dpmi->sel_es = dpmi_alloc_descriptor(dpmi);
    {
        uint16_t idx = dpmi_sel_to_index(dpmi->sel_es);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                         DESC_WRITABLE;
        dpmi_build_desc(d, es_base, 0xFFFF, access, 0);
    }

    /* Read caller's return address from the stack.
     * Stack layout (16-bit real mode, growing down):
     *   SP+0: INT return IP (stub+2)
     *   SP+2: INT return CS (F000)
     *   SP+4: FLAGS
     *   SP+6: caller IP (DOS4GW code after CALL FAR)
     *   SP+8: caller CS
     * Skip both frames. EIP/ESP are offsets within their segments. */
    uint16_t caller_ip = dos_mem_read16(vm, stack_linear_pre + 6);
    uint16_t orig_sp = cpu->sp + 10;  /* skip INT frame (6) + CALL frame (4) */

    /* Switch CPU to protected mode (USE16) */
    cpu->protected_mode = true;
    cpu->pm_cs_loaded   = true;
    cpu->op_size_32     = false;
    cpu->addr_size_32   = false;
    cpu->cr0           |= 1;

    /* Set segment registers to PM selectors */
    cpu->cs = dpmi->sel_code;
    cpu->ds = dpmi->sel_data;
    cpu->es = dpmi->sel_es;
    cpu->ss = dpmi->sel_stack;
    cpu->fs = dpmi->sel_data;
    cpu->gs = dpmi->sel_data;

    /* EIP = offset within CS, ESP = offset within SS */
    cpu->eip = caller_ip;
    cpu->esp = orig_sp;

    /* Return success in AX */
    cpu->eax = 0;

    serial_puts("[DPMI] CS=");
    serial_puthex(cpu->cs, 4);
    serial_puts(" DS=");
    serial_puthex(cpu->ds, 4);
    serial_puts(" SS=");
    serial_puthex(cpu->ss, 4);
    serial_puts(" EIP=");
    serial_puthex(cpu->eip, 8);
    serial_puts(" ESP=");
    serial_puthex(cpu->esp, 8);
    serial_puts("\n  cs_base=");
    serial_puthex(cs_base, 8);
    serial_puts(" caller_ip=");
    serial_puthex(caller_ip, 4);
    serial_puts(" bytes@CS:EIP:");
    for (int i = 0; i < 16; i++) {
        serial_puts(" ");
        serial_puthex(vm->mem[cs_base + caller_ip + i], 2);
    }
    serial_puts("\n");
}

/* ══════════════════════════════════════════════════════════════════
 * 6. dos_int31_dpmi -- INT 31h DPMI services dispatcher
 * ══════════════════════════════════════════════════════════════════ */

void dos_int31_dpmi(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    uint16_t func = cpu->ax;

    switch (func) {

    /* ── AX=0000h: Allocate LDT Descriptors ────────────────────── */
    case 0x0000: {
        uint16_t count = cpu->cx;
        if (count == 0) count = 1;

        /* Find 'count' contiguous free slots */
        uint16_t first_sel = 0;
        uint16_t found = 0;
        uint16_t start = 0;

        for (uint16_t i = 0; i < DPMI_MAX_DESCRIPTORS; i++) {
            if (!(dpmi->ldt[i].access & DESC_PRESENT)) {
                if (found == 0) start = i;
                found++;
                if (found >= count) {
                    /* Mark all as allocated */
                    for (uint16_t j = start; j < start + count; j++) {
                        dpmi->ldt[j].access = DESC_PRESENT | DESC_DPL3 |
                                              DESC_SEGMENT;
                    }
                    first_sel = dpmi_index_to_sel(start);
                    dpmi->next_free_index = (start + count) %
                                            DPMI_MAX_DESCRIPTORS;
                    break;
                }
            } else {
                found = 0;
            }
        }

        if (first_sel) {
            cpu->ax = first_sel;
            cpu->eflags &= ~FLAG_CF;
        } else {
            serial_puts("[DPMI] 0000h: Cannot allocate ");
            serial_putdec(count);
            serial_puts(" descriptors\n");
            cpu->ax = 0x8011;  /* descriptor unavailable */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0001h: Free LDT Descriptor ─────────────────────────── */
    case 0x0001: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx < DPMI_MAX_DESCRIPTORS) {
            dpmi_zero(&dpmi->ldt[idx], sizeof(dpmi_descriptor_t));
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;  /* invalid selector */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0003h: Get Selector Increment ──────────────────────── */
    case 0x0003:
        cpu->ax = DPMI_SEL_INC;
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0006h: Get Segment Base Address ────────────────────── */
    case 0x0006: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx < DPMI_MAX_DESCRIPTORS) {
            uint32_t base = dpmi_desc_get_base(&dpmi->ldt[idx]);
            cpu->cx = (uint16_t)(base >> 16);
            cpu->dx = (uint16_t)(base & 0xFFFF);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0007h: Set Segment Base Address ────────────────────── */
    case 0x0007: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx < DPMI_MAX_DESCRIPTORS) {
            uint32_t base = ((uint32_t)cpu->cx << 16) | cpu->dx;
            dpmi_desc_set_base(&dpmi->ldt[idx], base);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0008h: Set Segment Limit ───────────────────────────── */
    case 0x0008: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx < DPMI_MAX_DESCRIPTORS) {
            uint32_t limit = ((uint32_t)cpu->cx << 16) | cpu->dx;
            dpmi_desc_set_limit(&dpmi->ldt[idx], limit);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0009h: Set Descriptor Access Rights ────────────────── */
    case 0x0009: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx < DPMI_MAX_DESCRIPTORS) {
            dpmi_descriptor_t *d = &dpmi->ldt[idx];
            d->access = cpu->cl;
            /* CH contains flags_lim upper nibble merged with limit hi nibble.
             * Preserve the limit bits (low nibble), replace flags (high nibble). */
            d->flags_lim = (d->flags_lim & 0x0F) | (cpu->ch & 0xF0);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=000Ah: Create Alias Descriptor ─────────────────────── */
    case 0x000A: {
        uint16_t src_sel = cpu->bx;
        uint16_t src_idx = dpmi_sel_to_index(src_sel);
        if (src_idx >= DPMI_MAX_DESCRIPTORS) {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t new_sel = dpmi_alloc_descriptor(dpmi);
        if (!new_sel) {
            cpu->ax = 0x8011;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t new_idx = dpmi_sel_to_index(new_sel);
        dpmi_descriptor_t *src = &dpmi->ldt[src_idx];
        dpmi_descriptor_t *dst = &dpmi->ldt[new_idx];

        /* Copy base and limit from source */
        uint32_t base = dpmi_desc_get_base(src);
        uint32_t limit = dpmi_desc_get_limit(src);

        /* Make it a data segment (clear code bit, set writable) */
        uint8_t access = (src->access & ~DESC_CODE) | DESC_WRITABLE;
        access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE;
        uint8_t flags = src->flags_lim & 0xF0;  /* preserve G and D bits */

        dpmi_build_desc(dst, base, limit, access, flags);

        cpu->ax = new_sel;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=000Bh: Get Descriptor ────────────────────────────────── */
    case 0x000B: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx >= DPMI_MAX_DESCRIPTORS || !(dpmi->ldt[idx].access & DESC_PRESENT)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        /* Copy 8-byte descriptor to ES:EDI (or ES:DI in 16-bit) */
        uint32_t dst = dpmi_translate(vm, cpu->es,
                       dpmi->is_32bit ? cpu->edi : cpu->di);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        for (int i = 0; i < 8; i++)
            dos_mem_write8(vm, dst + i, ((uint8_t *)d)[i]);
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=000Ch: Set Descriptor ────────────────────────────────── */
    case 0x000C: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);
        if (idx >= DPMI_MAX_DESCRIPTORS) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        /* Read 8-byte descriptor from ES:EDI (or ES:DI in 16-bit) */
        uint32_t src = dpmi_translate(vm, cpu->es,
                       dpmi->is_32bit ? cpu->edi : cpu->di);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        for (int i = 0; i < 8; i++)
            ((uint8_t *)d)[i] = dos_mem_read8(vm, src + i);

        /* Log descriptor details for debugging */
        uint32_t base = dpmi_desc_get_base(d);
        uint32_t limit = dpmi_desc_get_limit(d);
        bool is_code = (d->access & DESC_CODE) != 0;
        bool is_32 = (d->flags_lim & 0x40) != 0;
        serial_puts("[DPMI] SetDesc sel=");
        serial_puthex(sel, 4);
        serial_puts(" base=");
        serial_puthex(base, 8);
        serial_puts(" lim=");
        serial_puthex(limit, 8);
        serial_puts(is_code ? " CODE" : " DATA");
        serial_puts(is_32 ? " 32" : " 16");
        serial_puts(" acc=");
        serial_puthex(d->access, 2);
        serial_puts("\n");

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0305h: Get State Save/Restore Addresses ──────────────── */
    case 0x0305:
        /* Point RM and PM save/restore routines at our 1-byte RETF stub
         * at F000:0110 (set up in dpmi_init). AX=0 means no state save
         * buffer is needed — the stubs just return immediately when the
         * client calls through them. This stops DOOM from reading 0:0
         * and later CALL-FARing to an invalid address. */
        cpu->ax  = 0;                   /* state buffer size = 0 */
        cpu->bx  = DPMI_ENTRY_SEG;      /* RM seg */
        cpu->cx  = 0x0110;              /* RM off */
        cpu->esi = DPMI_ENTRY_SEG;      /* PM seg (treat as 16-bit) */
        cpu->edi = 0x0110;              /* PM off */
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0306h: Get Raw Mode Switch Addresses ─────────────────── */
    case 0x0306:
        /* Point mode-switch routines at the RETF stub at F000:0118. We
         * already handle mode switching via INT FEh; DOOM rarely calls
         * these but now it won't see 0:0 if it does. */
        cpu->bx  = DPMI_ENTRY_SEG;      /* RM→PM seg */
        cpu->cx  = 0x0118;              /* RM→PM off */
        cpu->esi = DPMI_ENTRY_SEG;      /* PM→RM seg */
        cpu->edi = 0x0118;              /* PM→RM off */
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0A00h: Get Vendor-Specific API Entry Point ───────────── */
    case 0x0A00:
        /* No vendor extensions — return error (carry set) */
        cpu->eflags |= FLAG_CF;
        cpu->ax = 0x8001;  /* unsupported */
        break;

    /* ── AX=0100h: Allocate DOS Memory Block ───────────────────── */
    case 0x0100: {
        uint16_t paragraphs = cpu->bx;
        uint16_t largest = 0;
        uint16_t seg = dos_mem_alloc(vm, paragraphs, &largest);

        if (seg) {
            /* Create a descriptor for this block */
            uint16_t sel = dpmi_alloc_descriptor(dpmi);
            if (sel) {
                uint16_t idx = dpmi_sel_to_index(sel);
                uint32_t base = (uint32_t)seg << 4;
                uint32_t limit = (uint32_t)paragraphs * 16 - 1;
                uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                                 DESC_WRITABLE;
                uint8_t flags = DESC_32BIT;
                dpmi_build_desc(&dpmi->ldt[idx], base, limit, access, flags);

                cpu->ax = seg;
                cpu->dx = sel;
                cpu->eflags &= ~FLAG_CF;
            } else {
                dos_mem_free(vm, seg);
                cpu->ax = 0x8011;
                cpu->bx = largest;
                cpu->eflags |= FLAG_CF;
            }
        } else {
            cpu->ax = 0x0008;  /* insufficient memory */
            cpu->bx = largest;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0101h: Free DOS Memory Block ───────────────────────── */
    case 0x0101: {
        uint16_t sel = cpu->dx;
        uint16_t idx = dpmi_sel_to_index(sel);

        if (idx < DPMI_MAX_DESCRIPTORS) {
            uint32_t base = dpmi_desc_get_base(&dpmi->ldt[idx]);
            uint16_t seg = (uint16_t)(base >> 4);
            dos_mem_free(vm, seg);
            dpmi_zero(&dpmi->ldt[idx], sizeof(dpmi_descriptor_t));
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0200h: Get Real-Mode Interrupt Vector ──────────────── */
    case 0x0200: {
        uint8_t int_num = cpu->bl;
        uint32_t ivt_addr = (uint32_t)int_num * 4;
        cpu->dx = dos_mem_read16(vm, ivt_addr);       /* offset */
        cpu->cx = dos_mem_read16(vm, ivt_addr + 2);   /* segment */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0201h: Set Real-Mode Interrupt Vector ──────────────── */
    case 0x0201: {
        uint8_t int_num = cpu->bl;
        uint32_t ivt_addr = (uint32_t)int_num * 4;
        dos_mem_write16(vm, ivt_addr, cpu->dx);         /* offset */
        dos_mem_write16(vm, ivt_addr + 2, cpu->cx);     /* segment */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0202h: Get Processor Exception Handler Vector ────────── */
    case 0x0202: {
        uint8_t exc_num = cpu->bl;
        if (exc_num > 31) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        /* Return from pm_vectors — exceptions use vectors 0-31 */
        cpu->cx  = dpmi->pm_vectors[exc_num].sel;
        cpu->edx = dpmi->pm_vectors[exc_num].off;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0203h: Set Processor Exception Handler Vector ────────── */
    case 0x0203: {
        uint8_t exc_num = cpu->bl;
        if (exc_num > 31) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        dpmi->pm_vectors[exc_num].sel = cpu->cx;
        dpmi->pm_vectors[exc_num].off = cpu->edx;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0204h: Get Protected-Mode Interrupt Vector ─────────── */
    case 0x0204: {
        uint8_t int_num = cpu->bl;
        cpu->cx  = dpmi->pm_vectors[int_num].sel;
        cpu->edx = dpmi->pm_vectors[int_num].off;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0205h: Set Protected-Mode Interrupt Vector ─────────── */
    case 0x0205: {
        uint8_t int_num = cpu->bl;
        dpmi->pm_vectors[int_num].sel = cpu->cx;
        dpmi->pm_vectors[int_num].off = cpu->edx;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0300h: Simulate Real-Mode Interrupt ────────────────── */
    /* This is CRITICAL: DOOM does ALL file I/O through this call.
     * It builds a dpmi_rm_regs_t with e.g. AH=3Dh (open file),
     * DS:DX pointing to the filename, and calls INT 31h/0300h
     * with BL=0x21. We load those regs, call dos_int_dispatch in
     * real mode, then copy the results back. */
    case 0x0300: {
        uint8_t int_num = cpu->bl;

        /* ES:EDI points to dpmi_rm_regs_t (50 bytes) */
        uint32_t regs_addr = dpmi_translate(vm, cpu->es, cpu->edi);

        /* Read the real-mode register structure from memory */
        dpmi_rm_regs_t rm;
        {
            uint8_t *p = (uint8_t *)&rm;
            for (uint64_t i = 0; i < sizeof(dpmi_rm_regs_t); i++)
                p[i] = dos_mem_read8(vm, regs_addr + i);
        }

        /* Save current protected-mode CPU state */
        uint32_t save_eax = cpu->eax, save_ebx = cpu->ebx;
        uint32_t save_ecx = cpu->ecx, save_edx = cpu->edx;
        uint32_t save_esi = cpu->esi, save_edi = cpu->edi;
        uint32_t save_ebp = cpu->ebp;
        uint16_t save_ds  = cpu->ds,  save_es  = cpu->es;
        uint16_t save_fs  = cpu->fs,  save_gs  = cpu->gs;
        uint16_t save_cs  = cpu->cs,  save_ss  = cpu->ss;
        uint32_t save_esp = cpu->esp;
        uint32_t save_eip = cpu->eip;
        uint32_t save_eflags = cpu->eflags;
        bool save_pm    = cpu->protected_mode;
        bool save_op32  = cpu->op_size_32;
        bool save_addr32 = cpu->addr_size_32;

        /* Load real-mode registers from the structure */
        cpu->eax = rm.eax;
        cpu->ebx = rm.ebx;
        cpu->ecx = rm.ecx;
        cpu->edx = rm.edx;
        cpu->esi = rm.esi;
        cpu->edi = rm.edi;
        cpu->ebp = rm.ebp;
        cpu->ds  = rm.ds;
        cpu->es  = rm.es;
        cpu->fs  = rm.fs;
        cpu->gs  = rm.gs;
        cpu->flags = rm.flags | FLAGS_FIXED;

        /* Switch to real mode for the interrupt call */
        cpu->protected_mode = false;
        cpu->op_size_32     = false;
        cpu->addr_size_32   = false;

        /* If the structure specifies SS:SP, use them; otherwise keep ours */
        if (rm.ss || rm.sp) {
            cpu->ss  = rm.ss;
            cpu->esp = rm.sp;
        }

        /* Dispatch the real-mode interrupt */
        dos_int_dispatch(vm, int_num);

        /* Copy CPU regs back into the structure */
        rm.eax = cpu->eax;
        rm.ebx = cpu->ebx;
        rm.ecx = cpu->ecx;
        rm.edx = cpu->edx;
        rm.esi = cpu->esi;
        rm.edi = cpu->edi;
        rm.ebp = cpu->ebp;
        rm.ds  = cpu->ds;
        rm.es  = cpu->es;
        rm.fs  = cpu->fs;
        rm.gs  = cpu->gs;
        rm.flags = cpu->flags;

        /* Write the structure back to memory */
        {
            uint8_t *p = (uint8_t *)&rm;
            for (uint64_t i = 0; i < sizeof(dpmi_rm_regs_t); i++)
                dos_mem_write8(vm, regs_addr + i, p[i]);
        }

        /* Restore protected-mode CPU state */
        cpu->eax = save_eax;
        cpu->ebx = save_ebx;
        cpu->ecx = save_ecx;
        cpu->edx = save_edx;
        cpu->esi = save_esi;
        cpu->edi = save_edi;
        cpu->ebp = save_ebp;
        cpu->ds  = save_ds;
        cpu->es  = save_es;
        cpu->fs  = save_fs;
        cpu->gs  = save_gs;
        cpu->cs  = save_cs;
        cpu->ss  = save_ss;
        cpu->esp = save_esp;
        cpu->eip = save_eip;
        cpu->eflags = save_eflags;
        cpu->protected_mode = save_pm;
        cpu->op_size_32     = save_op32;
        cpu->addr_size_32   = save_addr32;

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0301h: Call Real-Mode FAR Procedure ────────────────── */
    case 0x0301: {
        /* ES:EDI points to dpmi_rm_regs_t */
        uint32_t regs_addr = dpmi_translate(vm, cpu->es, cpu->edi);

        dpmi_rm_regs_t rm;
        {
            uint8_t *p = (uint8_t *)&rm;
            for (uint64_t i = 0; i < sizeof(dpmi_rm_regs_t); i++)
                p[i] = dos_mem_read8(vm, regs_addr + i);
        }

        /* Save protected-mode state */
        uint32_t save_eax = cpu->eax, save_ebx = cpu->ebx;
        uint32_t save_ecx = cpu->ecx, save_edx = cpu->edx;
        uint32_t save_esi = cpu->esi, save_edi = cpu->edi;
        uint32_t save_ebp = cpu->ebp;
        uint16_t save_ds  = cpu->ds,  save_es  = cpu->es;
        uint16_t save_fs  = cpu->fs,  save_gs  = cpu->gs;
        uint16_t save_cs  = cpu->cs,  save_ss  = cpu->ss;
        uint32_t save_esp = cpu->esp;
        uint32_t save_eip = cpu->eip;
        uint32_t save_eflags = cpu->eflags;
        bool save_pm    = cpu->protected_mode;
        bool save_op32  = cpu->op_size_32;
        bool save_addr32 = cpu->addr_size_32;

        /* Load real-mode registers */
        cpu->eax = rm.eax;
        cpu->ebx = rm.ebx;
        cpu->ecx = rm.ecx;
        cpu->edx = rm.edx;
        cpu->esi = rm.esi;
        cpu->edi = rm.edi;
        cpu->ebp = rm.ebp;
        cpu->ds  = rm.ds;
        cpu->es  = rm.es;
        cpu->fs  = rm.fs;
        cpu->gs  = rm.gs;
        cpu->flags = rm.flags | FLAGS_FIXED;

        /* Switch to real mode */
        cpu->protected_mode = false;
        cpu->op_size_32     = false;
        cpu->addr_size_32   = false;

        if (rm.ss || rm.sp) {
            cpu->ss  = rm.ss;
            cpu->esp = rm.sp;
        }

        /* Set CS:IP from the structure to call the far procedure.
         * We don't actually execute -- the caller expects us to
         * simulate the call, so we just set up and return.
         * For a minimal host, treat it like 0300h with INT 0 (nop). */
        cpu->cs = rm.cs;
        cpu->ip = rm.ip;

        /* Copy results back */
        rm.eax = cpu->eax;
        rm.ebx = cpu->ebx;
        rm.ecx = cpu->ecx;
        rm.edx = cpu->edx;
        rm.esi = cpu->esi;
        rm.edi = cpu->edi;
        rm.ebp = cpu->ebp;
        rm.ds  = cpu->ds;
        rm.es  = cpu->es;
        rm.fs  = cpu->fs;
        rm.gs  = cpu->gs;
        rm.flags = cpu->flags;

        {
            uint8_t *p = (uint8_t *)&rm;
            for (uint64_t i = 0; i < sizeof(dpmi_rm_regs_t); i++)
                dos_mem_write8(vm, regs_addr + i, p[i]);
        }

        /* Restore protected-mode state */
        cpu->eax = save_eax;
        cpu->ebx = save_ebx;
        cpu->ecx = save_ecx;
        cpu->edx = save_edx;
        cpu->esi = save_esi;
        cpu->edi = save_edi;
        cpu->ebp = save_ebp;
        cpu->ds  = save_ds;
        cpu->es  = save_es;
        cpu->fs  = save_fs;
        cpu->gs  = save_gs;
        cpu->cs  = save_cs;
        cpu->ss  = save_ss;
        cpu->esp = save_esp;
        cpu->eip = save_eip;
        cpu->eflags = save_eflags;
        cpu->protected_mode = save_pm;
        cpu->op_size_32     = save_op32;
        cpu->addr_size_32   = save_addr32;

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0303h: Allocate Real-Mode Callback ─────────────────── */
    case 0x0303: {
        /* DS:ESI = PM procedure, ES:EDI = RM register structure */
        int slot = -1;
        for (int i = 0; i < DPMI_MAX_CALLBACKS; i++) {
            if (!dpmi->callbacks[i].active) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            cpu->ax = 0x8015;  /* callback unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi->callbacks[slot].active = true;
        dpmi->callbacks[slot].pm_sel = cpu->ds;
        dpmi->callbacks[slot].pm_off = cpu->esi;

        /* Assign a real-mode address in ROM area (after the entry stub) */
        uint16_t cb_off = DPMI_ENTRY_OFF + 0x10 + (uint16_t)(slot * 4);
        dpmi->callbacks[slot].rm_seg = DPMI_ENTRY_SEG;
        dpmi->callbacks[slot].rm_off = cb_off;

        /* Write a stub: INT FEh; RETF (same mechanism as entry) */
        uint32_t cb_addr = dos_linear(DPMI_ENTRY_SEG, cb_off);
        if (cb_addr + 3 <= vm->total_mem_size) {
            vm->mem[cb_addr + 0] = 0xCD;
            vm->mem[cb_addr + 1] = 0xFE;
            vm->mem[cb_addr + 2] = 0xCB;
        }

        cpu->cx = DPMI_ENTRY_SEG;
        cpu->dx = cb_off;
        cpu->eflags &= ~FLAG_CF;

        serial_puts("[DPMI] Callback allocated: slot ");
        serial_putdec((uint64_t)slot);
        serial_puts("\n");
        break;
    }

    /* ── AX=0304h: Free Real-Mode Callback ─────────────────────── */
    case 0x0304: {
        uint16_t seg = cpu->cx;
        uint16_t off = cpu->dx;
        bool found = false;

        for (int i = 0; i < DPMI_MAX_CALLBACKS; i++) {
            if (dpmi->callbacks[i].active &&
                dpmi->callbacks[i].rm_seg == seg &&
                dpmi->callbacks[i].rm_off == off) {
                dpmi->callbacks[i].active = false;
                found = true;
                break;
            }
        }

        if (found) {
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8024;  /* invalid callback address */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0400h: Get DPMI Version ────────────────────────────── */
    case 0x0400:
        cpu->ah = 0x00;    /* major version */
        cpu->al = 0x5A;    /* minor version (90 = 0x5A) */
        cpu->bx = 0x0005;  /* flags: 32-bit, reflect to real mode */
        cpu->cl = 0x05;    /* processor type: 586 */
        cpu->dh = 0x00;    /* master PIC base */
        cpu->dl = 0x08;    /* slave PIC base */
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0500h: Get Free Memory Information ─────────────────── */
    case 0x0500: {
        uint32_t info_addr = dpmi_translate(vm, cpu->es, cpu->edi);
        uint32_t available = DOS_TOTAL_MEM - dpmi->ext_alloc_next;

        /* First DWORD: largest available free block in bytes */
        dos_mem_write32(vm, info_addr + 0, available);

        /* Remaining 11 DWORDs: set to -1 (unknown) per spec */
        for (int i = 1; i < 12; i++)
            dos_mem_write32(vm, info_addr + i * 4, 0xFFFFFFFF);

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0501h: Allocate Memory Block ───────────────────────── */
    case 0x0501: {
        uint32_t size = ((uint32_t)cpu->bx << 16) | cpu->cx;
        if (size == 0) size = 1;

        /* Align to 16 bytes */
        size = (size + 15) & ~(uint32_t)15;

        uint32_t addr = dpmi->ext_alloc_next;

        if (addr + size > DOS_TOTAL_MEM) {
            serial_puts("[DPMI] 0501h: Out of memory (need ");
            serial_puthex(size, 8);
            serial_puts(", have ");
            serial_puthex(DOS_TOTAL_MEM - addr, 8);
            serial_puts(")\n");
            cpu->ax = 0x8012;  /* linear memory unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        /* Find a free mem_block slot */
        int slot = -1;
        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (!dpmi->mem_blocks[i].allocated) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            cpu->ax = 0x8012;
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi->mem_blocks[slot].base      = addr;
        dpmi->mem_blocks[slot].size      = size;
        dpmi->mem_blocks[slot].handle    = dpmi->next_handle;
        dpmi->mem_blocks[slot].allocated = true;

        dpmi->ext_alloc_next = addr + size;

        /* Return BX:CX = linear address, SI:DI = handle */
        cpu->bx = (uint16_t)(addr >> 16);
        cpu->cx = (uint16_t)(addr & 0xFFFF);
        cpu->si = (uint16_t)(dpmi->next_handle >> 16);
        cpu->di = (uint16_t)(dpmi->next_handle & 0xFFFF);

        serial_puts("[DPMI] Alloc ");
        serial_puthex(size, 8);
        serial_puts(" bytes at ");
        serial_puthex(addr, 8);
        serial_puts(" handle=");
        serial_puthex(dpmi->next_handle, 8);
        serial_puts("\n");

        dpmi->next_handle++;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0502h: Free Memory Block ───────────────────────────── */
    case 0x0502: {
        uint32_t handle = ((uint32_t)cpu->si << 16) | cpu->di;
        bool found = false;

        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (dpmi->mem_blocks[i].allocated &&
                dpmi->mem_blocks[i].handle == handle) {
                dpmi->mem_blocks[i].allocated = false;
                found = true;
                break;
            }
        }

        if (found) {
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8023;  /* invalid handle */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0503h: Resize Memory Block ─────────────────────────── */
    case 0x0503: {
        uint32_t handle   = ((uint32_t)cpu->si << 16) | cpu->di;
        uint32_t new_size = ((uint32_t)cpu->bx << 16) | cpu->cx;
        if (new_size == 0) new_size = 1;
        new_size = (new_size + 15) & ~(uint32_t)15;

        int slot = -1;
        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (dpmi->mem_blocks[i].allocated &&
                dpmi->mem_blocks[i].handle == handle) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            cpu->ax = 0x8023;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint32_t old_base = dpmi->mem_blocks[slot].base;
        uint32_t old_size = dpmi->mem_blocks[slot].size;

        if (new_size <= old_size) {
            /* Shrinking: just update the size */
            dpmi->mem_blocks[slot].size = new_size;
            cpu->bx = (uint16_t)(old_base >> 16);
            cpu->cx = (uint16_t)(old_base & 0xFFFF);
            cpu->si = (uint16_t)(handle >> 16);
            cpu->di = (uint16_t)(handle & 0xFFFF);
            cpu->eflags &= ~FLAG_CF;
        } else if (old_base + old_size == dpmi->ext_alloc_next) {
            /* This block is at the top of the heap -- extend in place */
            uint32_t extra = new_size - old_size;
            if (dpmi->ext_alloc_next + extra <= DOS_TOTAL_MEM) {
                dpmi->mem_blocks[slot].size = new_size;
                dpmi->ext_alloc_next += extra;
                cpu->bx = (uint16_t)(old_base >> 16);
                cpu->cx = (uint16_t)(old_base & 0xFFFF);
                cpu->si = (uint16_t)(handle >> 16);
                cpu->di = (uint16_t)(handle & 0xFFFF);
                cpu->eflags &= ~FLAG_CF;
            } else {
                cpu->ax = 0x8012;
                cpu->eflags |= FLAG_CF;
            }
        } else {
            /* Cannot resize in place, and we have no relocation support.
             * Try allocating a new block at the top. */
            uint32_t new_addr = dpmi->ext_alloc_next;
            if (new_addr + new_size <= DOS_TOTAL_MEM) {
                /* Copy old data to new location */
                for (uint32_t i = 0; i < old_size; i++)
                    dos_mem_write8(vm, new_addr + i,
                                   dos_mem_read8(vm, old_base + i));

                dpmi->mem_blocks[slot].base = new_addr;
                dpmi->mem_blocks[slot].size = new_size;
                dpmi->ext_alloc_next = new_addr + new_size;

                cpu->bx = (uint16_t)(new_addr >> 16);
                cpu->cx = (uint16_t)(new_addr & 0xFFFF);
                cpu->si = (uint16_t)(handle >> 16);
                cpu->di = (uint16_t)(handle & 0xFFFF);
                cpu->eflags &= ~FLAG_CF;
            } else {
                cpu->ax = 0x8012;
                cpu->eflags |= FLAG_CF;
            }
        }
        break;
    }

    /* ── AX=0800h: Physical Address Mapping ────────────────────── */
    case 0x0800: {
        /* Identity map: linear = physical (no paging) */
        uint32_t phys = ((uint32_t)cpu->bx << 16) | cpu->cx;
        /* Return same address as linear */
        cpu->bx = (uint16_t)(phys >> 16);
        cpu->cx = (uint16_t)(phys & 0xFFFF);
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── Default: unhandled DPMI function ──────────────────────── */
    default:
        serial_puts("[DPMI] Unhandled INT 31h AX=");
        serial_puthex(func, 4);
        serial_puts("\n");
        cpu->eflags |= FLAG_CF;
        break;
    }
}
