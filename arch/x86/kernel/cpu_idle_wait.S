.text
.global x86_mwait_safe
.global x86_monitor_fault
.global x86_mwait_fault
.global x86_mwait_fixup
.type x86_mwait_safe, @function
x86_mwait_safe:
    mov %edx, %r8d
    mov %rdi, %rax
    xor %ecx, %ecx
    xor %edx, %edx
x86_monitor_fault:
    monitor
    /* Arm before rechecking: a store before MONITOR must not be missed. */
    cmpl %esi, (%rdi)
    jne .Lidle_changed
    mov %r8d, %eax
    /* CPUID.5:ECX[1] is required: pending IRQs break even with IF=0. */
    mov $1, %ecx
x86_mwait_fault:
    mwait
    mov $1, %eax
    ret
.Lidle_changed:
    mov $2, %eax
    ret
x86_mwait_fixup:
    xor %eax, %eax
    ret
.size x86_mwait_safe, .-x86_mwait_safe
.section .note.GNU-stack,"",@progbits
