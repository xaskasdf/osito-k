/*
 * OsitoK -- DPMI 0.9 Host Types
 *
 * Data structures for the DOS Protected Mode Interface host.
 * Manages LDT descriptors, extended memory, real-mode callbacks,
 * and protected/real mode switching for DOS extenders.
 */

#ifndef DOS_DPMI_H
#define DOS_DPMI_H

#include "../include/types.h"

/* Forward declaration; the full definition is in dos_types.h. */
typedef struct dos_vm dos_vm_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t base_mid;
    uint8_t access;
    uint8_t flags_lim;
    uint8_t base_hi;
} dpmi_descriptor_t;

/* Descriptor access byte bits. */
#define DESC_PRESENT       0x80
#define DESC_DPL_MASK      0x60
#define DESC_DPL3          0x60
#define DESC_SEGMENT       0x10
#define DESC_CODE          0x08
#define DESC_READABLE      0x02
#define DESC_WRITABLE      0x02
#define DESC_ACCESSED      0x01

/* Descriptor flags byte bits. */
#define DESC_GRANULARITY   0x80
#define DESC_32BIT         0x40

#define DPMI_MAX_DESCRIPTORS            256
#define DPMI_SPECIFIC_DESCRIPTOR_COUNT   16
#define DPMI_MAX_MEM_BLOCKS               64
#define DPMI_MAX_CALLBACKS                16
#define DPMI_MAX_EXCEPTION_DEPTH          32

enum {
    DPMI_DESC_FREE = 0,
    DPMI_DESC_MUTABLE,
    DPMI_DESC_RM_ALIAS,
    DPMI_DESC_DOS_MEMORY,
    DPMI_DESC_CLIENT_SYSTEM,
    DPMI_DESC_HOST
};

/* Selector = (index << 3) | TI | RPL, with TI=1 and RPL=3. */
#define DPMI_SEL_BASE  0x0007
#define DPMI_SEL_INC   0x0008

/* DPMI host entry points and private transition interrupts. */
#define DPMI_ENTRY_SEG                0xF000
#define DPMI_ENTRY_OFF                0x0100
#define DPMI_ENTRY_INT                0xFE
#define DPMI_CALLBACK_RETURN_INT      0xFA
#define DPMI_CALLBACK_ENTRY_INT       0xFB
#define DPMI_RAW_SWITCH_INT           0xFC
#define DPMI_EXCEPTION_RETURN_INT     0xFD
#define DPMI_DEFAULT_REFLECT_INT      0xF9
#define DPMI_SAVE_STATE_OFF           0x0110
#define DPMI_RAW_SWITCH_OFF           0x0118
#define DPMI_EXCEPTION_RETURN_OFF     0x0120
#define DPMI_RM_RETURN_OFF            0x0128
#define DPMI_RM_INT_STUB_OFF          0x0130
#define DPMI_CALLBACK_RETURN_OFF      0x0140
#define DPMI_CALLBACK_BASE_OFF        0x0200
#define DPMI_CALLBACK_STUB_SIZE       4
#define DPMI_PM_REFLECT_BASE_OFF      0x1000
#define DPMI_PM_HW_REFLECT_BASE_OFF   0x1400
#define DPMI_PM_REFLECT_STUB_SIZE     4
#define DPMI_RM_STACK_TOP             0xFFFE

/* Extended-memory allocator. Callback stacks live at its upper edge. */
#define DPMI_EXT_BASE                 0x100000
#define DOS_TOTAL_MEM                 (16 * 1024 * 1024)
#define DPMI_CALLBACK_STACK_SIZE      0x1000
#define DPMI_CALLBACK_STACK_AREA \
    (DPMI_MAX_CALLBACKS * DPMI_CALLBACK_STACK_SIZE)
#define DPMI_EXT_SIZE \
    (DOS_TOTAL_MEM - DPMI_EXT_BASE - DPMI_CALLBACK_STACK_AREA)
#define DPMI_EXT_PAGE_SIZE            4096u
#define DPMI_EXT_MAX_PAGES \
    (DPMI_EXT_SIZE / DPMI_EXT_PAGE_SIZE)
#define DPMI_EXT_BITMAP_SIZE \
    ((DPMI_EXT_MAX_PAGES + 7u) / 8u)

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t handle;
    bool allocated;
} dpmi_mem_block_t;

typedef struct {
    uint16_t segment;
    uint16_t paragraphs;
    uint16_t descriptor_count;
    bool allocated;
} dpmi_dos_block_t;

typedef struct {
    uint16_t rm_seg;
    uint16_t rm_off;
    uint16_t pm_sel;
    uint32_t pm_off;
    uint16_t rm_regs_sel;
    uint32_t rm_regs_off;
    uint16_t rm_stack_sel;
    bool active;
} dpmi_callback_t;

/* DPMI real-mode register structure (50 bytes). */
typedef struct __attribute__((packed)) {
    uint32_t edi, esi, ebp, reserved_sp;
    uint32_t ebx, edx, ecx, eax;
    uint16_t flags;
    uint16_t es, ds, fs, gs;
    uint16_t ip, cs;
    uint16_t sp, ss;
} dpmi_rm_regs_t;

typedef struct dpmi_state {
    bool active;
    bool is_32bit;
    bool virtual_interrupts_enabled;

    dpmi_descriptor_t ldt[DPMI_MAX_DESCRIPTORS];
    uint8_t descriptor_state[DPMI_MAX_DESCRIPTORS];
    uint16_t next_free_index;
    dpmi_dos_block_t dos_blocks[DPMI_MAX_DESCRIPTORS];

    uint16_t sel_code;
    uint16_t sel_data;
    uint16_t sel_stack;
    uint16_t sel_psp;
    uint16_t sel_env;
    uint16_t sel_host_code;

    uint16_t saved_cs, saved_ip;
    uint16_t saved_ss, saved_sp;
    uint16_t saved_ds, saved_es;

    dpmi_mem_block_t mem_blocks[DPMI_MAX_MEM_BLOCKS];
    uint8_t ext_page_bitmap[DPMI_EXT_BITMAP_SIZE];
    uint32_t next_handle;

    dpmi_callback_t callbacks[DPMI_MAX_CALLBACKS];
    uint16_t callback_stack_sel;
    uint8_t callback_slots[DPMI_MAX_CALLBACKS];
    uint8_t callback_depth;

    struct { uint16_t sel; uint32_t off; } exception_vectors[32];
    struct { uint16_t sel; uint32_t off; } pm_vectors[256];
    bool exception_virtual_interrupts[DPMI_MAX_EXCEPTION_DEPTH];
    uint8_t exception_depth;
} dpmi_state_t;

static inline uint32_t dpmi_desc_get_base(const dpmi_descriptor_t *d)
{
    return ((uint32_t)d->base_hi << 24) |
           ((uint32_t)d->base_mid << 16) |
           (uint32_t)d->base_lo;
}

static inline void dpmi_desc_set_base(dpmi_descriptor_t *d, uint32_t base)
{
    d->base_lo = (uint16_t)(base & 0xFFFFu);
    d->base_mid = (uint8_t)((base >> 16) & 0xFFu);
    d->base_hi = (uint8_t)((base >> 24) & 0xFFu);
}

static inline uint32_t dpmi_desc_get_limit(const dpmi_descriptor_t *d)
{
    uint32_t limit = ((uint32_t)(d->flags_lim & 0x0Fu) << 16) |
                     d->limit_lo;
    if (d->flags_lim & DESC_GRANULARITY)
        limit = ((limit + 1u) << 12) - 1u;
    return limit;
}

static inline void dpmi_desc_set_limit(dpmi_descriptor_t *d, uint32_t limit)
{
    if (limit > 0xFFFFFu) {
        limit >>= 12;
        d->flags_lim = (d->flags_lim & 0xF0u) |
                       (uint8_t)((limit >> 16) & 0x0Fu);
        d->flags_lim |= DESC_GRANULARITY;
    } else {
        d->flags_lim = (d->flags_lim & 0xF0u) |
                       (uint8_t)((limit >> 16) & 0x0Fu);
        d->flags_lim &= (uint8_t)~DESC_GRANULARITY;
    }
    d->limit_lo = (uint16_t)(limit & 0xFFFFu);
}

static inline uint16_t dpmi_sel_to_index(uint16_t sel)
{
    return sel >> 3;
}

static inline uint16_t dpmi_index_to_sel(uint16_t index)
{
    return (uint16_t)((index << 3) | DPMI_SEL_BASE);
}

void dpmi_init(dos_vm_t *vm);
void dos_int2f_dispatch(dos_vm_t *vm);
void dos_int31_dpmi(dos_vm_t *vm);
int dpmi_selftest(void);
void dpmi_enter_protected_mode(dos_vm_t *vm);
bool dpmi_raw_mode_switch(dos_vm_t *vm);
bool dpmi_callback_enter(dos_vm_t *vm);
bool dpmi_callback_return(dos_vm_t *vm, bool discard_private_int_frame);
bool dpmi_dispatch_default_interrupt(dos_vm_t *vm, uint8_t int_num,
                                     uint8_t private_frame_bytes);
uint16_t dpmi_get_host_code_selector(dos_vm_t *vm);

uint32_t dpmi_translate(dos_vm_t *vm, uint16_t selector, uint32_t offset);
bool dpmi_guest_descriptor(dos_vm_t *vm, uint16_t selector,
                           dpmi_descriptor_t *descriptor);

uint32_t dpmi_ext_total_pages(const dos_vm_t *vm);
uint32_t dpmi_ext_free_page_count(const dos_vm_t *vm);
uint32_t dpmi_ext_largest_free_page_count(const dos_vm_t *vm);
uint32_t dpmi_ext_alloc_pages(dos_vm_t *vm, uint32_t count, bool high);
bool dpmi_ext_free_pages(dos_vm_t *vm, uint32_t base, uint32_t count);

#endif /* DOS_DPMI_H */
