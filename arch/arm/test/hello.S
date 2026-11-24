/*
 * hello.S -- Minimal AArch64 userspace test binary
 *
 * Writes "Hello from userspace!\n" via SVC write(1, msg, len),
 * then exits via SVC exit(0).
 *
 * Build: aarch64-linux-gnu-gcc -nostdlib -static -o hello hello.S
 */

.global _start

.text
_start:
    /* write(1, msg, 22) */
    mov     x0, #1              /* fd = stdout */
    adr     x1, msg             /* buf = msg (PC-relative) */
    mov     x2, #22             /* count */
    mov     x8, #64             /* SYS_write (ARM64) */
    svc     #0

    /* exit(0) */
    mov     x0, #0              /* status = 0 */
    mov     x8, #93             /* SYS_exit (ARM64) */
    svc     #0

    /* Should not reach here */
    b       .

.section .rodata
msg:
    .ascii  "Hello from userspace!\n"
