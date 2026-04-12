/*
 * OsitoK — DOS Memory Control Block (MCB) Manager
 *
 * Implements DOS conventional memory management using a linked list
 * of Memory Control Blocks (MCBs). Each MCB is 16 bytes (1 paragraph)
 * and precedes the allocated block.
 *
 * Used by INT 21h functions 48h (alloc), 49h (free), 4Ah (resize).
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* ── Initialize conventional memory ─────────────────────────────── */

void dos_mem_init(dos_vm_t *vm)
{
    /* First MCB at the start of conventional memory */
    vm->first_mcb = DOS_CONV_BASE / 16;

    uint32_t mcb_addr = (uint32_t)vm->first_mcb << 4;
    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + mcb_addr);
    mcb->type  = 'Z';   /* last block */
    mcb->owner = 0;     /* free */
    /* Total paragraphs from first MCB to 640KB, minus the MCB itself */
    mcb->size  = (DOS_CONV_TOP / 16) - vm->first_mcb - 1;

    for (int i = 0; i < 8; i++) mcb->name[i] = 0;
}

/* ── Allocate memory (INT 21h/48h) ──────────────────────────────── */
/* Returns segment of allocated block, or 0 on failure.
 * On failure, *largest is set to the largest free block size. */

uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest)
{
    uint16_t seg = vm->first_mcb;
    uint16_t best_free = 0;

    while (1) {
        uint32_t addr = (uint32_t)seg << 4;
        dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + addr);

        if (mcb->owner == 0 && mcb->size >= paragraphs) {
            /* Found a free block large enough */
            /* Split if remainder is > 1 paragraph (room for a new MCB + data) */
            if (mcb->size > paragraphs + 1) {
                uint16_t new_seg = seg + 1 + paragraphs;
                uint32_t new_addr = (uint32_t)new_seg << 4;
                dos_mcb_t *new_mcb = (dos_mcb_t *)(vm->mem + new_addr);
                new_mcb->type  = mcb->type;
                new_mcb->owner = 0;
                new_mcb->size  = mcb->size - paragraphs - 1;
                for (int i = 0; i < 8; i++) new_mcb->name[i] = 0;

                mcb->type = 'M';
                mcb->size = paragraphs;
            }

            mcb->owner = vm->current_psp;
            serial_puts("[MCB] alloc: mcb@");
            serial_puthex(seg, 4);
            serial_puts(" owner=");
            serial_puthex(mcb->owner, 4);
            serial_puts(" sz=");
            serial_puthex(mcb->size, 4);
            serial_puts(" -> seg=");
            serial_puthex(seg + 1, 4);
            serial_puts("\n");
            return seg + 1;  /* return segment after MCB */
        }

        if (mcb->owner == 0 && mcb->size > best_free)
            best_free = mcb->size;

        if (mcb->type == 'Z') break;
        seg += mcb->size + 1;
    }

    if (largest) *largest = best_free;
    return 0;  /* no block found */
}

/* ── Free memory (INT 21h/49h) ──────────────────────────────────── */
/* segment = the segment returned by alloc (MCB is at segment-1) */

int dos_mem_free(dos_vm_t *vm, uint16_t segment)
{
    uint16_t mcb_seg = segment - 1;
    uint32_t addr = (uint32_t)mcb_seg << 4;

    if (addr >= DOS_CONV_TOP) return -1;

    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + addr);

    /* Validate: must be owned */
    if (mcb->owner == 0) return -1;

    mcb->owner = 0;
    for (int i = 0; i < 8; i++) mcb->name[i] = 0;

    /* Coalesce with next block if also free */
    if (mcb->type == 'M') {
        uint16_t next_seg = mcb_seg + mcb->size + 1;
        uint32_t next_addr = (uint32_t)next_seg << 4;
        dos_mcb_t *next = (dos_mcb_t *)(vm->mem + next_addr);
        if (next->owner == 0) {
            mcb->size += next->size + 1;
            mcb->type = next->type;
        }
    }

    return 0;
}

/* ── Resize memory (INT 21h/4Ah) ────────────────────────────────── */

int dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size, uint16_t *max_avail)
{
    uint16_t mcb_seg = segment - 1;
    uint32_t addr = (uint32_t)mcb_seg << 4;
    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + addr);

    serial_puts("[MCB] resize: mcb@");
    serial_puthex(mcb_seg, 4);
    serial_puts(" owner=");
    serial_puthex(mcb->owner, 4);
    serial_puts(" sz=");
    serial_puthex(mcb->size, 4);
    serial_puts(" -> ");
    serial_puthex(new_size, 4);
    serial_puts("\n");

    if (new_size == mcb->size) return 0;  /* no change */

    if (new_size < mcb->size) {
        /* Shrink: split off a free block */
        if (mcb->size - new_size > 1) {
            uint16_t free_seg = mcb_seg + 1 + new_size;
            uint32_t free_addr = (uint32_t)free_seg << 4;
            dos_mcb_t *free_mcb = (dos_mcb_t *)(vm->mem + free_addr);
            free_mcb->type  = mcb->type;
            free_mcb->owner = 0;
            free_mcb->size  = mcb->size - new_size - 1;
            for (int i = 0; i < 8; i++) free_mcb->name[i] = 0;

            mcb->type = 'M';
            mcb->size = new_size;
        }
        return 0;
    }

    /* Grow: check if next block is free and large enough */
    if (mcb->type == 'M') {
        uint16_t next_seg = mcb_seg + mcb->size + 1;
        uint32_t next_addr = (uint32_t)next_seg << 4;
        dos_mcb_t *next = (dos_mcb_t *)(vm->mem + next_addr);

        uint16_t extra_needed = new_size - mcb->size;
        if (next->owner == 0 && (next->size + 1) >= extra_needed) {
            /* Absorb next block */
            uint16_t total = mcb->size + next->size + 1;
            mcb->size = new_size;
            if (total - new_size > 1) {
                /* Create remainder free block */
                uint16_t rem_seg = mcb_seg + 1 + new_size;
                uint32_t rem_addr = (uint32_t)rem_seg << 4;
                dos_mcb_t *rem = (dos_mcb_t *)(vm->mem + rem_addr);
                rem->type  = next->type;
                rem->owner = 0;
                rem->size  = total - new_size - 1;
                for (int i = 0; i < 8; i++) rem->name[i] = 0;
                mcb->type = 'M';
            } else {
                mcb->size = total;
                mcb->type = next->type;
            }
            return 0;
        }
    }

    /* Cannot grow */
    if (max_avail) *max_avail = mcb->size;
    return -1;
}
