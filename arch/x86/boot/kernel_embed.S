/*
 * OsitoK PXE boot payload.
 *
 * Built only by `make pxe`; the normal USB/ESP boot path keeps loading
 * kernel.elf from the filesystem.
 */

.section .rodata
.balign 16
.global ok_pxe_kernel_elf_start
.global ok_pxe_kernel_elf_end

ok_pxe_kernel_elf_start:
    .incbin "build/kernel.elf"
ok_pxe_kernel_elf_end:
    .balign 16

.section .note.GNU-stack,"",%progbits
