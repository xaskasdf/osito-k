/*
 * AP Trampoline — assembled separately, bytes extracted into smp.c
 *
 * Build: as --32 ap_trampoline.S -o /tmp/ap_t.o && objcopy -O binary /tmp/ap_t.o /tmp/ap_t.bin
 *        xxd -i /tmp/ap_t.bin
 *
 * Runs at physical 0x8000. Data block at 0x8100.
 *
 * Layout:
 *   0x00-0x3F  16-bit real mode
 *   0x40-0x8F  32-bit protected mode
 *   0x90-0xFF  64-bit long mode
 *   0x100+     Data block
 */

.code16
.org 0

_start:
    cli
    cld
    xor     %ax, %ax
    mov     %ax, %ds

    /* lgdt [0x8120] — trampoline GDTR in data block */
    lgdt    0x8120

    /* Enable PE (CR0 bit 0) */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    /* Far jump to 32-bit: selector 0x08, target 0x8040 */
    .byte   0x66, 0xEA
    .long   0x00008040
    .word   0x0008

.org 0x40
.code32
pm32:
    /* Load data segments */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss

    /* Enable PAE (CR4 bit 5) */
    mov     %cr4, %eax
    bts     $5, %eax
    mov     %eax, %cr4

    /* Load CR3 from data block [0x8128] */
    mov     0x8128, %eax
    mov     %eax, %cr3

    /* Enable LME (MSR 0xC0000080, bit 8) */
    mov     $0xC0000080, %ecx
    rdmsr
    bts     $8, %eax
    wrmsr

    /* Enable paging (CR0 bit 31) */
    mov     %cr0, %eax
    bts     $31, %eax
    mov     %eax, %cr0

    /* Far jump to 64-bit: selector 0x18, target 0x8090 */
    ljmp    $0x18, $0x00008090

.org 0x90
.code64
lm64:
    /* Still using trampoline GDT (selectors 0x00-0x18).
     * Must NOT touch segments with kernel selectors (0x30/0x38) yet.
     * First: load kernel GDT, then reload CS+segments. */

    /* Load AP stack FIRST (need stack for lretq push sequence) */
    mov     0x8150, %rsp

    /* lgdt from kernel GDTR at [0x8130] */
    lgdt    0x8130

    /* lidt from kernel IDTR at [0x813C] */
    lidt    0x813C

    /* Reload CS via far return to kernel CS=0x38 */
    pushq   $0x38
    lea     1f(%rip), %rax
    push    %rax
    lretq
1:
    /* Now running with kernel GDT. Load kernel data segments. */
    mov     $0x30, %eax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    xor     %eax, %eax
    mov     %ax, %fs
    mov     %ax, %gs

    /* Set ready flag: [0x815C] = 1 */
    movl    $1, 0x815C

    /* Load CPU index as first arg (RDI) from [0x8158] */
    mov     0x8158, %edi

    /* Call AP entry: [0x8148] */
    mov     0x8148, %rax
    call    *%rax

    /* Halt loop */
2:  hlt
    jmp     2b

.section .note.GNU-stack,"",%progbits
