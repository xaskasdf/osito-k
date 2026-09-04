/* End-to-end INT 21h/AH=4Bh and AH=4Dh parent contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    movw $0x2ff0, %sp

    /* Leave enough conventional memory for a child process. */
    pushw %cs
    popw %es
    movw $0x0300, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail_memory

    /* Use a non-default DTA so restoration is observable. */
    movw $parent_dta, %dx
    movb $0x1a, %ah
    int $0x21

    /* Capture vectors DOS must restore when the child terminates. */
    movw $0x3522, %ax
    int $0x21
    movw %bx, vector22_offset
    movw %es, vector22_segment
    movw $0x3523, %ax
    int $0x21
    movw %bx, vector23_offset
    movw %es, vector23_segment
    movw $0x3524, %ax
    int $0x21
    movw %bx, vector24_offset
    movw %es, vector24_segment

    /* The first non-standard handle is explicitly non-inheritable. */
    pushw %cs
    popw %ds
    movw $nul_name, %dx
    movw $0x3d82, %ax
    int $0x21
    jc fail_handle
    cmpw $5, %ax
    jne fail_handle
    movw %ax, parent_handle

    movw $parent_file_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail_handle
    cmpw $6, %ax
    jne fail_handle
    movw %ax, parent_file_handle

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_memory
    cmpw $8, %ax
    jne fail_memory
    movw %bx, largest_before

    movw %cs, %ax
    movw %ax, exec_block + 4
    movw %ax, exec_block + 8
    movw %ax, exec_block + 12
    pushw %cs
    popw %es

    movw $0x1357, %si
    movw $0x2468, %di
    movw $0x369c, %bp
    movw %sp, parent_sp
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc fail_exec

    cmpw $0x1357, %si
    jne fail_restore
    cmpw $0x2468, %di
    jne fail_restore
    cmpw $0x369c, %bp
    jne fail_restore
    cmpw parent_sp, %sp
    jne fail_restore
    movw %ds, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_restore
    movw %es, %ax
    cmpw %dx, %ax
    jne fail_restore

    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002a, %ax
    jne fail_status
    movw $0x4dff, %ax
    int $0x21
    testw %ax, %ax
    jne fail_status

    movw $0x2f00, %ax
    int $0x21
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_restore
    cmpw $parent_dta, %bx
    jne fail_restore

    movw $0x3522, %ax
    int $0x21
    cmpw vector22_offset, %bx
    jne fail_vector
    movw %es, %ax
    cmpw vector22_segment, %ax
    jne fail_vector
    movw $0x3523, %ax
    int $0x21
    cmpw vector23_offset, %bx
    jne fail_vector
    movw %es, %ax
    cmpw vector23_segment, %ax
    jne fail_vector
    movw $0x3524, %ax
    int $0x21
    cmpw vector24_offset, %bx
    jne fail_vector
    movw %es, %ax
    cmpw vector24_segment, %ax
    jne fail_vector

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_memory
    cmpw largest_before, %bx
    jne fail_memory

    movw parent_handle, %bx
    xorw %dx, %dx
    movw $0x4400, %ax
    int $0x21
    jc fail_handle
    testw $0x8000, %dx
    jz fail_handle
    movw $0x3e00, %ax
    int $0x21
    jc fail_handle

    /* The child deliberately over-closed this inherited SFT by forging a
     * second JFT slot. The parent's retained object must still be usable. */
    movw parent_file_handle, %bx
    movw $read_byte, %dx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail_handle
    cmpw $1, %ax
    jne fail_handle
    movw $0x3e00, %ax
    int $0x21
    jc fail_handle

    /* A relocatable MZ child must run and report its own exit status. */
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $mz_child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc fail_mz
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002b, %ax
    jne fail_mz

    /* A DPMI child may enter native USE32 and still return to this parent. */
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $dpmi_child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc fail_dpmi
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002a, %ax
    jne fail_dpmi

    /* Overlay mode uses caller-owned memory and the caller's relocation
     * factor. It must not allocate another MCB or construct a PSP. */
    movw $0x0100, %bx
    movw $0x4800, %ax
    int $0x21
    jc fail_overlay
    movw %ax, overlay_segment
    movw %ax, overlay_block

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_overlay
    cmpw $8, %ax
    jne fail_overlay
    movw %bx, overlay_largest

    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $mz_child_name, %dx
    movw $overlay_block, %bx
    movw $0x4b03, %ax
    int $0x21
    jc fail_overlay

    movw overlay_segment, %ax
    movw %ax, %es
    cmpw $0x1f0e, %es:0
    jne fail_overlay
    cmpw $0x1234, %es:0x25
    jne fail_overlay
    pushw %cs
    popw %es

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_overlay
    cmpw overlay_largest, %bx
    jne fail_overlay
    movw overlay_segment, %ax
    movw %ax, %es
    movw $0x4900, %ax
    int $0x21
    jc fail_overlay

    /* Missing files and unsupported EXEC modes fail without losing state. */
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $missing_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jnc fail_missing
    cmpw $2, %ax
    jne fail_missing
    movw $0x5900, %ax
    int $0x21
    cmpw $2, %ax
    jne fail_missing
    cmpb $3, %bl
    jne fail_missing
    cmpb $8, %bh
    jne fail_missing
    cmpb $2, %ch
    jne fail_missing

    pushw %cs
    popw %es
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b02, %ax
    int $0x21
    jnc fail_mode
    cmpw $1, %ax
    jne fail_mode

    /* Load-only creates a child process and returns its initial machine
     * state in the extended EXEC block without running the entry point. */
    pushw %cs
    popw %ds
    movw $nul_name, %dx
    movw $0x3d82, %ax
    int $0x21
    jc fail_load_only
    cmpw $5, %ax
    jne fail_load_only
    movw %ax, parent_handle

    movw $parent_file_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail_load_only
    cmpw $6, %ax
    jne fail_load_only
    movw %ax, parent_file_handle

    movw $0x5100, %ax
    int $0x21
    movw %bx, parent_psp
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b01, %ax
    int $0x21
load_only_return:
    jc fail_load_only
    cmpb $0, load_only_started
    jne load_only_resumed
    cmpw $0xfffa, exec_block + 14
    jne fail_load_only
    cmpw $0x0100, exec_block + 18
    jne fail_load_only
    movw exec_block + 16, %ax
    cmpw exec_block + 20, %ax
    jne fail_load_only

    movw $0x5100, %ax
    int $0x21
    cmpw parent_psp, %bx
    je fail_load_only
    cmpw exec_block + 20, %bx
    jne fail_load_only
    movw %bx, %es
    movw parent_psp, %dx
    cmpw %dx, %es:0x16
    jne fail_load_only
    cmpw $0, %es:0xfffa
    jne fail_load_only
    movw $0x2f00, %ax
    int $0x21
    cmpw $0x0080, %bx
    jne fail_load_only
    movw exec_block + 20, %ax
    movw %es, %dx
    cmpw %ax, %dx
    jne fail_load_only

    /* A debugger starts a 4B01 child from the returned SS:SP and CS:IP.
     * Its termination must unwind to this parent's saved INT 21h frame. */
    movb $1, load_only_started
    movw exec_block + 14, %di
    movw exec_block + 16, %bp
    cli
    movw %bp, %ss
    movw %di, %sp
    sti
    ljmp *exec_block + 18

load_only_resumed:
    movw $0x5100, %ax
    int $0x21
    cmpw parent_psp, %bx
    jne fail_load_only
    movw $0x2f00, %ax
    int $0x21
    cmpw $parent_dta, %bx
    jne fail_load_only
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_load_only
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002a, %ax
    jne fail_load_only

    movw parent_handle, %bx
    xorw %dx, %dx
    movw $0x4400, %ax
    int $0x21
    jc fail_load_only
    testw $0x8000, %dx
    jz fail_load_only
    movw $0x3e00, %ax
    int $0x21
    jc fail_load_only

    movw parent_file_handle, %bx
    movw $read_byte, %dx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail_load_only
    cmpw $1, %ax
    jne fail_load_only
    movw $0x3e00, %ax
    int $0x21
    jc fail_load_only

    movw $0x3522, %ax
    int $0x21
    cmpw $load_only_return, %bx
    jne fail_load_only
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_load_only
    movw $0x3523, %ax
    int $0x21
    cmpw vector23_offset, %bx
    jne fail_load_only
    movw %es, %ax
    cmpw vector23_segment, %ax
    jne fail_load_only
    movw $0x3524, %ax
    int $0x21
    cmpw vector24_offset, %bx
    jne fail_load_only
    movw %es, %ax
    cmpw vector24_segment, %ax
    jne fail_load_only

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_load_only
    cmpw largest_before, %bx
    jne fail_load_only

    /* Repeat the load-only lifecycle with a DPMI child. The child begins in
     * real mode, enters native USE32, and must still unwind to this frame. */
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $dpmi_child_name, %dx
    movw $exec_block, %bx
    movw $0x4b01, %ax
    int $0x21
dpmi_load_only_return:
    jc fail_dpmi_load_only
    cmpb $0, dpmi_load_only_started
    jne dpmi_load_only_resumed
    cmpw $0xfffa, exec_block + 14
    jne fail_dpmi_load_only
    cmpw $0x0100, exec_block + 18
    jne fail_dpmi_load_only
    movw exec_block + 16, %ax
    cmpw exec_block + 20, %ax
    jne fail_dpmi_load_only

    movw $0x5100, %ax
    int $0x21
    cmpw parent_psp, %bx
    je fail_dpmi_load_only
    cmpw exec_block + 20, %bx
    jne fail_dpmi_load_only
    movw $0x2f00, %ax
    int $0x21
    cmpw $0x0080, %bx
    jne fail_dpmi_load_only
    movw exec_block + 20, %ax
    movw %es, %dx
    cmpw %ax, %dx
    jne fail_dpmi_load_only

    movb $1, dpmi_load_only_started
    movw exec_block + 14, %di
    movw exec_block + 16, %bp
    cli
    movw %bp, %ss
    movw %di, %sp
    sti
    ljmp *exec_block + 18

dpmi_load_only_resumed:
    movw $0x5100, %ax
    int $0x21
    cmpw parent_psp, %bx
    jne fail_dpmi_load_only
    movw $0x2f00, %ax
    int $0x21
    cmpw $parent_dta, %bx
    jne fail_dpmi_load_only
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_dpmi_load_only
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002a, %ax
    jne fail_dpmi_load_only

    movw $0x3522, %ax
    int $0x21
    cmpw $dpmi_load_only_return, %bx
    jne fail_dpmi_load_only
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail_dpmi_load_only
    movw $0x3523, %ax
    int $0x21
    cmpw vector23_offset, %bx
    jne fail_dpmi_load_only
    movw %es, %ax
    cmpw vector23_segment, %ax
    jne fail_dpmi_load_only
    movw $0x3524, %ax
    int $0x21
    cmpw vector24_offset, %bx
    jne fail_dpmi_load_only
    movw %es, %ax
    cmpw vector24_segment, %ax
    jne fail_dpmi_load_only

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_dpmi_load_only
    cmpw largest_before, %bx
    jne fail_dpmi_load_only

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail_memory:
    movb $0x48, %al
    jmp fail
fail_handle:
    movb $0x3d, %al
    jmp fail
fail_exec:
    movb $0x4b, %al
    jmp fail
fail_restore:
    movb $0x52, %al
    jmp fail
fail_status:
    movb $0x4d, %al
    jmp fail
fail_vector:
    movb $0x25, %al
    jmp fail
fail_mz:
    movb $0x5a, %al
    jmp fail
fail_dpmi:
    movb $0x31, %al
    jmp fail
fail_dpmi_load_only:
    movb $0x32, %al
    jmp fail
fail_overlay:
    movb $0x03, %al
    jmp fail
fail_missing:
    movb $0x02, %al
    jmp fail
fail_mode:
    movb $0x01, %al
    jmp fail
fail_load_only:
    movb $0x10, %al

fail:
    pushw %ax
    pushw %cs
    popw %ds
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

child_name:
    .asciz "DEXCHILD.COM"
mz_child_name:
    .asciz "DEXMZ.EXE"
dpmi_child_name:
    .asciz "DEXDPMI.COM"
missing_name:
    .asciz "NOEXEC.COM"
nul_name:
    .asciz "NUL"
parent_file_name:
    .asciz "DOSEXEC.COM"
command_tail:
    .byte 7
    .ascii " /CHILD"
    .byte 0x0d
    .space 119
fcb1:
    .fill 16, 1, 0
fcb2:
    .fill 16, 1, 0

    .balign 2
exec_block:
    .word 0
    .word command_tail, 0
    .word fcb1, 0
    .word fcb2, 0
    .word 0, 0, 0, 0
overlay_block:
    .word 0, 0x1234
overlay_segment:
    .word 0
overlay_largest:
    .word 0
parent_handle:
    .word 0
parent_file_handle:
    .word 0
parent_psp:
    .word 0
load_only_started:
    .byte 0
dpmi_load_only_started:
    .byte 0
read_byte:
    .byte 0
largest_before:
    .word 0
parent_sp:
    .word 0
vector22_offset:
    .word 0
vector22_segment:
    .word 0
vector23_offset:
    .word 0
vector23_segment:
    .word 0
vector24_offset:
    .word 0
vector24_segment:
    .word 0
parent_dta:
    .space 128
pass_message:
    .ascii "DOS EXEC CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS EXEC CONTRACT FAIL\r\n$"
