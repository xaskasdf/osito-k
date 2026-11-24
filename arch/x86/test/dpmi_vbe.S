/* Native DPMI client for the VBE 2.0 linear-framebuffer contract. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es

    movw $0x4f01, %ax
    movw $0x0111, %cx
    movw $mode_info, %di
    int $0x10
    cmpw $0x004f, %ax
    jne real_vbe_fail
    testw $0x0080, mode_info
    jz real_vbe_fail
    cmpl $0, mode_info + 40
    je real_vbe_fail
    movl mode_info + 40, %eax
    movl %eax, lfb_physical

    movw $0x4f02, %ax
    movw $0x4111, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne real_vbe_fail

    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz real_dpmi_fail
    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $1, %ax
    lcall *dpmi_entry

pm16_entry:
    /* Clone the current code segment as a USE32 selector. */
    movw %cs, %bx
    movw $0x0006, %ax
    int $0x31
    jc pm16_fail
    movw %cx, code_base_hi
    movw %dx, code_base_lo

    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc pm16_fail
    movw %ax, native_selector
    movw %ax, native_pointer + 2

    movw %ax, %bx
    movw code_base_hi, %cx
    movw code_base_lo, %dx
    movw $0x0007, %ax
    int $0x31
    jc pm16_fail

    movw native_selector, %bx
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $0x0008, %ax
    int $0x31
    jc pm16_fail

    movw native_selector, %bx
    movw $0x40fa, %cx
    movw $0x0009, %ax
    int $0x31
    jc pm16_fail
    ljmp *native_pointer

real_vbe_fail:
    movw $real_vbe_message, %dx
    movb $9, %ah
    int $0x21
    movw $0x4c81, %ax
    int $0x21

real_dpmi_fail:
    movw $real_dpmi_message, %dx
    movb $9, %ah
    int $0x21
    movw $0x4c82, %ax
    int $0x21

pm16_fail:
    movw $0x4c83, %ax
    int $0x21

    .align 2
dpmi_entry:
    .word 0, 0
native_pointer:
    .word native32_entry, 0
native_selector:
    .word 0
code_base_hi:
    .word 0
code_base_lo:
    .word 0
lfb_physical:
    .long 0
mapped_linear:
    .long 0
flat_selector:
    .word 0
real_vbe_message:
    .ascii "VBE linear framebuffer unavailable\r\n$"
real_dpmi_message:
    .ascii "DPMI host unavailable\r\n$"
pass_message:
    .ascii "Native DPMI VBE LFB contract: PASS\r\n$"
    .align 16
mode_info:
    .space 256

    .code32
native32_entry:
    /* DPMI 0800h maps the physical aperture and returns a linear address. */
    movl lfb_physical, %edx
    movw %dx, %cx
    shrl $16, %edx
    movw %dx, %bx
    xorl %esi, %esi
    movw $9, %si
    xorl %edi, %edi
    movw $0x6000, %di
    movl $0x0800, %eax
    int $0x31
    jc native_map_fail

    movzwl %bx, %eax
    shll $16, %eax
    movzwl %cx, %edx
    orl %edx, %eax
    cmpl lfb_physical, %eax
    jne native_map_fail
    movl %eax, mapped_linear

    /* Build a writable flat data selector covering the VM aperture. */
    movl $1, %ecx
    xorl %eax, %eax
    int $0x31
    jc native_selector_fail
    movw %ax, flat_selector

    movw %ax, %bx
    xorl %ecx, %ecx
    xorl %edx, %edx
    movl $0x0007, %eax
    int $0x31
    jc native_selector_fail

    movw flat_selector, %bx
    movl $0x013f, %ecx
    movl $0xffff, %edx
    movl $0x0008, %eax
    int $0x31
    jc native_selector_fail

    movw flat_selector, %bx
    movl $0xc0f2, %ecx
    movl $0x0009, %eax
    int $0x31
    jc native_selector_fail

    movw flat_selector, %ax
    movw %ax, %es
    movl mapped_linear, %edi
    movl $0xf800001f, %eax
    movl $153600, %ecx
    cld
    rep stosl

    movl mapped_linear, %edi
    cmpl $0xf800001f, %es:(%edi)
    jne native_memory_fail
    cmpl $0xf800001f, %es:0x95ffc(%edi)
    jne native_memory_fail

    /* Keep the native client alive across several timer presentations. */
    movl $0x04000000, %ecx
1:
    decl %ecx
    jnz 1b

    movw %ds, %ax
    movw %ax, %es
    movl mapped_linear, %edx
    movw %dx, %cx
    shrl $16, %edx
    movw %dx, %bx
    movl $0x0801, %eax
    int $0x31
    jc native_release_fail

    movw flat_selector, %bx
    movl $0x0001, %eax
    int $0x31
    jc native_release_fail

    /* Native INT 10h must restore VGA and make Function 03h observable. */
    movl $0x0003, %eax
    int $0x10
    movl $0x4f03, %eax
    int $0x10
    cmpw $0x004f, %ax
    jne native_video_fail
    cmpw $0x0003, %bx
    jne native_video_fail

    movl $pass_message, %edx
    movl $0x0900, %eax
    int $0x21
    movl $0x4c00, %eax
    int $0x21

native_map_fail:
    movl $0x4c84, %eax
    int $0x21
native_selector_fail:
    movl $0x4c85, %eax
    int $0x21
native_memory_fail:
    movl $0x4c86, %eax
    int $0x21
native_release_fail:
    movl $0x4c87, %eax
    int $0x21
native_video_fail:
    movl $0x4c88, %eax
    int $0x21
