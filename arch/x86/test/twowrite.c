/* Minimal test: two writes + exit to verify syscall return works */

static long syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

void _start(void) {
    syscall3(1, 1, (long)"Write 1\n", 8);
    syscall3(1, 1, (long)"Write 2\n", 8);
    syscall3(1, 1, (long)"Write 3\n", 8);
    syscall1(60, 0);
}
