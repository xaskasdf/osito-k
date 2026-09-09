#ifndef DOS_PAGING_H
#define DOS_PAGING_H

#include "../include/types.h"

typedef struct dos_vm dos_vm_t;

#define DOS_PAGE_SIZE 4096u
#define DOS_CR0_WP    (1u << 16)
#define DOS_CR0_PG    (1u << 31)

enum {
    DOS_PAGE_READ = 0,
    DOS_PAGE_WRITE = 2,
    DOS_PAGE_USER = 4
};

typedef struct {
    uint32_t physical, pde, pte;
} dos_page_translation_t;

typedef struct {
    uint32_t linear, error;
    bool raised;
} dos_page_fault_t;

/* Probe never changes CR2 or A/D bits. Callers deliver faults and commit
 * metadata after validating every page of an operand, before transferring it.
 * Translation is independent of physical backing; host buffers must also
 * validate that their entire span is resident RAM. */
bool dos_page_probe(dos_vm_t *vm, uint32_t linear, unsigned access,
                    dos_page_translation_t *translation, dos_page_fault_t *fault);
void dos_page_commit(dos_vm_t *vm, const dos_page_translation_t *translation,
                     bool write);

#endif
