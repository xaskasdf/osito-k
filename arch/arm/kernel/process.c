/*
 * process.c -- Simplified process execution for AArch64
 *
 * Phase 3: single-process. Loads ELF from OsitoFS,
 * sets up user stack, calls entry point directly (EL1).
 * Process uses SVC for syscalls, exits via SVC exit.
 */

#include "../include/hal.h"
#include "../include/types.h"

/* From elf.c */
typedef struct {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t brk_base;
} elf_info_t;

extern int elf_load(const void *data, uint64_t size, elf_info_t *info);

/* From syscall.c */
extern void proc_set_brk(uint64_t base);
extern void proc_mark_running(void);
extern int  proc_is_running(void);
extern int  proc_get_exit_code(void);
extern void proc_set_exit_jmpbuf(void *buf[5]);

/* ── Run an ELF binary from OsitoFS ──────────────────────── */

int proc_run(const char *filename)
{
    serial_puts("[PROC] Loading: ");
    serial_puts(filename);
    serial_puts("\n");

    /* Find file in OsitoFS */
    osfs2_file_t *f = osfs2_find(filename);
    if (!f) {
        serial_puts("[PROC] File not found\n");
        return -1;
    }

    /* Allocate buffer and read file */
    uint64_t pages = (f->size + 4095) / 4096;
    void *buf = mem_alloc_pages(pages);
    if (!buf) {
        serial_puts("[PROC] alloc failed\n");
        return -1;
    }

    if (osfs2_read(f, 0, buf, f->size) < 0) {
        serial_puts("[PROC] read failed\n");
        mem_free_pages(buf, pages);
        return -1;
    }

    /* Load ELF */
    elf_info_t info;
    memset(&info, 0, sizeof(info));
    if (elf_load(buf, f->size, &info) < 0) {
        mem_free_pages(buf, pages);
        return -1;
    }

    /* Free the file buffer (segments copied to allocated pages) */
    mem_free_pages(buf, pages);

    /* Set up process state */
    proc_set_brk(info.brk_base);
    proc_mark_running();

    serial_puts("[PROC] Running at ");
    serial_puthex(info.entry, 16);
    serial_puts("\n");

    /* Set up longjmp for sys_exit to return here */
    void *jmpbuf[5];
    if (__builtin_setjmp(jmpbuf) == 0) {
        proc_set_exit_jmpbuf(jmpbuf);
        /* Call entry point directly (EL1, identity-mapped, kernel stack). */
        typedef void (*entry_fn_t)(void);
        ((entry_fn_t)info.entry)();
    }
    /* sys_exit longjmps here */

    int code = proc_get_exit_code();
    serial_puts("[PROC] Exit code ");
    serial_putdec(code);
    serial_puts("\n");

    return code;
}
