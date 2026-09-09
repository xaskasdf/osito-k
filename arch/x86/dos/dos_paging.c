/* Non-PAE 32-bit guest paging. The vCPU advertises neither PSE nor PAE;
 * CR4.PSE is effectively zero, so PDE.PS and PTE bit 7 are ignored. */
#include "cpu8086.h"
#include "dos_paging.h"

bool dos_page_probe(dos_vm_t *vm, uint32_t linear, unsigned access,
                    dos_page_translation_t *translation, dos_page_fault_t *fault)
{
    if (fault) *fault = (dos_page_fault_t){0};
    if (!vm || !vm->cpu || !vm->mem || !translation) return false;
    dos_page_translation_t result = {
        .physical = linear, .pde = UINT32_MAX, .pte = UINT32_MAX
    };
    unsigned error = access & (DOS_PAGE_WRITE | DOS_PAGE_USER);
    if (vm->cpu->cr0 & DOS_CR0_PG) {
        result.pde = (vm->cpu->cr3 & 0xFFFFF000u) + (linear >> 22) * 4u;
        uint32_t pde = dos_mem_read32(vm, result.pde);
        if (!(pde & 1u)) goto page_fault;
        result.pte = (pde & 0xFFFFF000u) + ((linear >> 12) & 0x3FFu) * 4u;
        uint32_t pte = dos_mem_read32(vm, result.pte);
        if (!(pte & 1u)) goto page_fault;

        uint32_t permissions = pde & pte;
        bool user = (access & DOS_PAGE_USER) != 0;
        if ((user && !(permissions & 4u)) ||
            ((access & DOS_PAGE_WRITE) && !(permissions & 2u) &&
             (user || (vm->cpu->cr0 & DOS_CR0_WP)))) {
            error |= 1u;
            goto page_fault;
        }
        result.physical = (pte & 0xFFFFF000u) | (linear & 0xFFFu);
    }
    *translation = result;
    return true;

page_fault:
    if (fault) *fault = (dos_page_fault_t){ linear, error, true };
    return false;
}

void dos_page_commit(dos_vm_t *vm, const dos_page_translation_t *translation,
                     bool write)
{
    if (translation->pde == UINT32_MAX) return;
    uint32_t pde = dos_mem_read32(vm, translation->pde);
    if (!(pde & 0x20u)) dos_mem_write32(vm, translation->pde, pde | 0x20u);
    uint32_t pte = dos_mem_read32(vm, translation->pte);
    uint32_t bits = write ? 0x60u : 0x20u;
    if ((pte & bits) != bits) dos_mem_write32(vm, translation->pte, pte | bits);
}
