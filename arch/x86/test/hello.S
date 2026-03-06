/*
 * hello.S — Minimal ELF64 test program for OsitoK
 *
 * Uses SYSCALL to do:
 *   write(1, "Hello from OsitoK!\n", 19)
 *   exit(0)
 *
 * Build:
 *   gcc -nostdlib -static -no-pie -o hello.elf hello.S
 *
 * This creates a static ELF64 binary with no libc dependency.
 */

.section .text
.globl _start
_start:
    /* write(1, msg, 19) */
    mov $1, %rax        /* syscall: write */
    mov $1, %rdi        /* fd: stdout */
    lea msg(%rip), %rsi /* buf: message */
    mov $19, %rdx       /* count */
    syscall

    /* exit(0) */
    mov $60, %rax       /* syscall: exit */
    xor %rdi, %rdi      /* status: 0 */
    syscall

.section .rodata
msg:
    .ascii "Hello from OsitoK!\n"
