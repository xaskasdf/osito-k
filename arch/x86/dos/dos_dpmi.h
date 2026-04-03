/*
 * OsitoK — DPMI 0.9 Host Types
 *
 * Data structures for the DOS Protected Mode Interface host.
 * Manages LDT descriptors, extended memory, real-mode callbacks,
 * and protected/real mode switching for DOS4GW and similar extenders.
 */

#ifndef DOS_DPMI_H
#define DOS_DPMI_H

#include "../include/types.h"

/* Forward declaration — full definition in dos_types.h */
typedef struct dos_vm dos_vm_t;

/* ── GDT/LDT descriptor entry (8 bytes, Intel format) ──────────── */

typedef struct __attribute__((packed)) {
    uint16_t limit_lo;    /* segment limit bits 0-15 */
    uint16_t base_lo;     /* base address bits 0-15 */
    uint8_t  base_mid;    /* base address bits 16-23 */
    uint8_t  access;      /* P(1)|DPL(2)|S(1)|Type(4) */
    uint8_t  flags_lim;   /* G(1)|D/B(1)|L(1)|AVL(1)|Limit(4) hi */
    uint8_t  base_hi;     /* base address bits 24-31 */
} dpmi_descriptor_t;

/* Descriptor access byte bits */
#define DESC_PRESENT    0x80
#define DESC_DPL_MASK   0x60
#define DESC_DPL3       0x60
#define DESC_SEGMENT    0x10    /* S bit: 1=code/data, 0=system */
#define DESC_CODE       0x08    /* type bit 3: 1=code, 0=data */
#define DESC_READABLE   0x02    /* code: readable */
#define DESC_WRITABLE   0x02    /* data: writable */
#define DESC_ACCESSED   0x01

/* Flags byte bits */
#define DESC_GRANULARITY 0x80   /* G: 1=4KB granularity, 0=byte */
#define DESC_32BIT       0x40   /* D/B: 1=32-bit, 0=16-bit */

/* ── Limits ─────────────────────────────────────────────────────── */

#define DPMI_MAX_DESCRIPTORS  256
#define DPMI_MAX_MEM_BLOCKS    64
#define DPMI_MAX_CALLBACKS     16

/* Selector value = (index << 3) | TI | RPL
 * TI=1 for LDT, RPL=3 for ring 3 */
#define DPMI_SEL_BASE    0x0007   /* first selector: index=0, TI=1(LDT), RPL=3 */
#define DPMI_SEL_INC     0x0008   /* increment per descriptor */

/* DPMI entry point in ROM area */
#define DPMI_ENTRY_SEG   0xF000
#define DPMI_ENTRY_OFF   0x0100
#define DPMI_ENTRY_INT   0xFE     /* INT FEh = DPMI entry trigger */

/* Extended memory starts at 1MB */
#define DPMI_EXT_BASE    0x100000
#define DPMI_EXT_SIZE    (15 * 1024 * 1024)   /* 15MB extended */
#define DOS_TOTAL_MEM    (16 * 1024 * 1024)   /* 16MB total */

/* ── Memory block tracking ──────────────────────────────────────── */

typedef struct {
    uint32_t base;        /* linear address */
    uint32_t size;        /* bytes */
    uint32_t handle;      /* unique handle for this block */
    bool     allocated;
} dpmi_mem_block_t;

/* ── Real-mode callback ─────────────────────────────────────────── */

typedef struct {
    uint16_t rm_seg, rm_off;     /* real-mode entry address */
    uint16_t pm_sel;             /* protected-mode callback selector */
    uint32_t pm_off;             /* protected-mode callback offset */
    bool     active;
} dpmi_callback_t;

/* ── Real-mode register structure (DPMI spec, 50 bytes) ─────────── */

typedef struct __attribute__((packed)) {
    uint32_t edi, esi, ebp, reserved_sp;
    uint32_t ebx, edx, ecx, eax;
    uint16_t flags;
    uint16_t es, ds, fs, gs;
    uint16_t ip, cs;
    uint16_t sp, ss;
} dpmi_rm_regs_t;

/* ── DPMI client state ──────────────────────────────────────────── */

typedef struct dpmi_state {
    bool     active;              /* DPMI client currently running */
    bool     is_32bit;            /* 32-bit DPMI client */

    /* LDT */
    dpmi_descriptor_t ldt[DPMI_MAX_DESCRIPTORS];
    uint16_t next_free_index;     /* next LDT index to allocate */

    /* Initial selectors (set up at mode switch) */
    uint16_t sel_code;            /* flat code segment selector */
    uint16_t sel_data;            /* flat data segment selector */
    uint16_t sel_stack;           /* stack segment selector */
    uint16_t sel_psp;             /* PSP data selector */

    /* Saved real-mode state (for return from protected mode) */
    uint16_t saved_cs, saved_ip;
    uint16_t saved_ss, saved_sp;
    uint16_t saved_ds, saved_es;

    /* Extended memory pool (simple bump allocator) */
    dpmi_mem_block_t mem_blocks[DPMI_MAX_MEM_BLOCKS];
    uint32_t ext_alloc_next;      /* next free address in extended memory */
    uint32_t next_handle;         /* handle counter */

    /* Real-mode callbacks */
    dpmi_callback_t callbacks[DPMI_MAX_CALLBACKS];

    /* Interrupt vectors (protected mode) */
    struct { uint16_t sel; uint32_t off; } pm_vectors[256];

} dpmi_state_t;

/* ── Descriptor helper functions ────────────────────────────────── */

static inline uint32_t dpmi_desc_get_base(const dpmi_descriptor_t *d)
{
    return ((uint32_t)d->base_hi << 24) |
           ((uint32_t)d->base_mid << 16) |
           (uint32_t)d->base_lo;
}

static inline void dpmi_desc_set_base(dpmi_descriptor_t *d, uint32_t base)
{
    d->base_lo  = (uint16_t)(base & 0xFFFF);
    d->base_mid = (uint8_t)((base >> 16) & 0xFF);
    d->base_hi  = (uint8_t)((base >> 24) & 0xFF);
}

static inline uint32_t dpmi_desc_get_limit(const dpmi_descriptor_t *d)
{
    uint32_t limit = ((uint32_t)(d->flags_lim & 0x0F) << 16) | d->limit_lo;
    if (d->flags_lim & DESC_GRANULARITY)
        limit = ((limit + 1) << 12) - 1;  /* 4KB granularity */
    return limit;
}

static inline void dpmi_desc_set_limit(dpmi_descriptor_t *d, uint32_t limit)
{
    if (limit > 0xFFFFF) {
        /* Need 4KB granularity */
        limit = (limit + 0xFFF) >> 12;
        d->flags_lim = (d->flags_lim & 0xF0) | (uint8_t)((limit >> 16) & 0x0F);
        d->flags_lim |= DESC_GRANULARITY;
    } else {
        d->flags_lim = (d->flags_lim & 0xF0) | (uint8_t)((limit >> 16) & 0x0F);
        d->flags_lim &= ~DESC_GRANULARITY;
    }
    d->limit_lo = (uint16_t)(limit & 0xFFFF);
}

/* Convert selector to LDT index */
static inline uint16_t dpmi_sel_to_index(uint16_t sel)
{
    return (sel >> 3);
}

/* Convert LDT index to selector */
static inline uint16_t dpmi_index_to_sel(uint16_t index)
{
    return (index << 3) | 0x07;  /* TI=1(LDT), RPL=3 */
}

/* ── Public API ─────────────────────────────────────────────────── */

/* Initialize DPMI host state */
void dpmi_init(dos_vm_t *vm);

/* INT 2Fh multiplex handler (includes AX=1687h DPMI detection) */
void dos_int2f_dispatch(dos_vm_t *vm);

/* INT 31h DPMI services handler */
void dos_int31_dpmi(dos_vm_t *vm);

/* DPMI entry point handler (mode switch real→protected) */
void dpmi_enter_protected_mode(dos_vm_t *vm);

/* Address translation: selector:offset → linear */
uint32_t dpmi_translate(dos_vm_t *vm, uint16_t selector, uint32_t offset);

#endif /* DOS_DPMI_H */
