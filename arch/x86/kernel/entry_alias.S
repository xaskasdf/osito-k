/*
 * _start → kernel_entry alias for TCC linker.
 *
 * TCC's built-in linker requires _start as the entry symbol
 * (no -e flag support). The GCC/ld build uses ENTRY(kernel_entry)
 * in kernel.ld instead. This file is only needed for TCC-linked
 * kernel builds (Phase 1 cross-compile and Phase 2 self-build).
 */
    .text
    .globl _start
_start:
    jmp kernel_entry

.section .note.GNU-stack,"",%progbits
