.text
.global x86_read_msr_safe
.global x86_read_msr_fault
.global x86_read_msr_fixup
.type x86_read_msr_safe, @function
x86_read_msr_safe:
    test %rsi, %rsi
    jz x86_read_msr_fixup
    mov %edi, %ecx
x86_read_msr_fault:
    rdmsr
    shl $32, %rdx
    or %rdx, %rax
    mov %rax, (%rsi)
    mov $1, %eax
    ret
x86_read_msr_fixup:
    xor %eax, %eax
    ret
.size x86_read_msr_safe, .-x86_read_msr_safe
.section .note.GNU-stack,"",@progbits
