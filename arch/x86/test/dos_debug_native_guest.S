/* Linux KVM diagnostic, independent of the Osito-K exception path.
 * The host loads this handler at GPA 9000h and reads results at 7000h.
 * Report the original hardware frame without editing or returning through it. */
.text
.code64
.global _start
_start:
    cld
    movq 0(%rsp), %rax
    movq %rax, 0x7000
    movq 8(%rsp), %rax
    movq %rax, 0x7008
    movq 16(%rsp), %rax
    movq %rax, 0x7010
    movq 24(%rsp), %rax
    movq %rax, 0x7018
    movq 32(%rsp), %rax
    movq %rax, 0x7020
    movq %dr6, %rax
    movq %rax, 0x7028
    movq %dr7, %rax
    movq %rax, 0x7030
    movw $0xe9, %dx
    movb $0xdb, %al
    outb %al, %dx
    hlt
.section .note.GNU-stack,"",@progbits
