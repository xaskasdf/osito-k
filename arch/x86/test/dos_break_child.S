    .section .text
    .code16
    .global _start
_start:
    movw $0x0800, %ax
    int $0x21
    movw $0x4cff, %ax /* Ctrl-C should have terminated this child. */
    int $0x21
