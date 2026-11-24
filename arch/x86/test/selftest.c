/*
 * selftest.c — Self-contained C program compiled+linked+run inside OsitoK.
 * No CRT needed: defines _start, inline syscalls.
 *
 * Compile inside OsitoK:
 *   tcc -nostdlib -nostdinc -static selftest.c -o selftest.elf
 */

static long _syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

static long _syscall1(long nr, long a1)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static void _puts(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    _syscall3(1, 1, (long)s, len);
}

void _start(void)
{
    _puts("=== Compiled AND executed inside OsitoK! ===\n");
    _puts("This C program was:\n");
    _puts("  1. Read from OsitoFS\n");
    _puts("  2. Compiled by TCC running in OsitoK\n");
    _puts("  3. Linked into an ELF by TCC\n");
    _puts("  4. Executed by the OsitoK kernel\n");
    _puts("=== Self-hosting test PASSED ===\n");
    _syscall1(60, 0);
    for (;;);
}
