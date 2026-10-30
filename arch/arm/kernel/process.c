/*
 * process.c -- running a program from OsitoFS (shell `exec <name> [args]`)
 *
 * One at a time, in the calling task (the shell), at EL1 in the kernel's
 * address space: elf.c loads it, it gets its own 256 KB stack and a 32 MB
 * heap region for brk, and its Linux syscalls go through syscall.c. It ends
 * by calling exit (a longjmp back here), by returning from its entry point,
 * or by faulting in its own code (proc_fault). Then everything it took is
 * freed, and the IRQ state it left is put back: an exit or a fault comes
 * back through an exception, with IRQs masked, and on the boot CPU that
 * would stop the tick every sleeping task waits on.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../include/aarch64.h"
#include "elf.h"
#include "../include/fs.h"
#include "../fs/ositofs3.h"

#define HEAP_PAGES  8192                /* 32 MB */

/* From syscall.c */
extern void proc_begin(uint64_t heap, uint64_t heap_size, uint64_t image, uint64_t image_size);
extern void proc_end(void);
extern void proc_set_exit_code(int code);
extern int  proc_get_exit_code(void);
extern void proc_set_exit_jmpbuf(void *buf[5]);

/* entry(argc, argv) on the stack at `sp`, then back on ours */
int proc_call_on_stack(uint64_t entry, int argc, char **argv, uint64_t sp);
__asm__(
    ".text\n"
    ".global proc_call_on_stack\n"
    ".type proc_call_on_stack, %function\n"
    "proc_call_on_stack:\n"
    "    stp   x29, x30, [sp, #-16]!\n"
    "    mov   x29, sp\n"
    "    mov   x9, x0\n"
    "    mov   w0, w1\n"
    "    mov   x1, x2\n"
    "    mov   sp, x3\n"
    "    blr   x9\n"
    "    mov   sp, x29\n"
    "    ldp   x29, x30, [sp], #16\n"
    "    ret\n");

static elf_info_t info;                 /* the running program's */

/* The program in file `name`, with argc/argv as its main gets them */
int proc_run(const char *name, int argc, char **argv)
{
    void *f = fs_find(name);
    if (!f) {
        serial_puts(name);
        serial_puts(": not found\n");
        return -1;
    }
    uint64_t size = osfs3_file_size(f);
    uint64_t pages = (size + 4095) / 4096;
    void *buf = mem_alloc_pages(pages);
    if (!buf) {
        serial_puts("exec: out of memory\n");
        return -1;
    }
    if (osfs3_read_file(f, 0, buf, size) < 0) {
        serial_puts("exec: read failed\n");
        mem_free_pages(buf, pages);
        return -1;
    }
    memset(&info, 0, sizeof(info));
    int loaded = elf_load(buf, size, &info);
    mem_free_pages(buf, pages);
    if (loaded < 0)
        return -1;
    void *heap = mem_alloc_pages(HEAP_PAGES);
    if (!heap) {
        serial_puts("exec: out of memory for the heap\n");
        elf_unload(&info);
        return -1;
    }

    proc_begin((uint64_t)(uintptr_t)heap, HEAP_PAGES * 4096ULL,
               info.image, info.image_pages * 4096);
    uint64_t daif = read_daif();
    void *jmpbuf[5];
    if (__builtin_setjmp(jmpbuf) == 0) {
        proc_set_exit_jmpbuf(jmpbuf);
        proc_set_exit_code(proc_call_on_stack(info.entry, argc, argv, info.stack_top));
    }
    irq_restore(daif);

    int code = proc_get_exit_code();
    proc_end();
    elf_unload(&info);
    mem_free_pages(heap, HEAP_PAGES);
    if (code) {
        serial_puts("[PROC] ");
        serial_puts(name);
        serial_puts(": exit code ");
        serial_putdec((uint32_t)code);
        serial_puts("\n");
    }
    return code;
}
