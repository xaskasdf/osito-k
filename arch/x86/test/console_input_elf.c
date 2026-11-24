/* Blocking Linux-syscall console input contract test. */

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_EXIT   60

static long syscall1(long number, long arg1)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result) : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory");
    return result;
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory");
    return result;
}

static void print(const char *text)
{
    long length = 0;
    while (text[length])
        length++;
    syscall3(SYS_WRITE, 1, (long)text, length);
}

static __attribute__((used, noinline, noreturn)) void probe_main(void)
{
    char input = 0;
    print("ELF CONSOLE INPUT READY: send K\n");
    long count = syscall3(SYS_READ, 0, (long)&input, 1);
    if (count != 1 || input != 'K') {
        print("ELF CONSOLE INPUT CONTRACT FAIL\n");
        syscall1(SYS_EXIT, 0x71);
        for (;;) {}
    }

    print("ELF CONSOLE INPUT CONTRACT PASS\n");
    syscall1(SYS_EXIT, 0);
    for (;;) {}
}

/* ELF entry is reached by JMP, not CALL. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile (
        "andq $-16, %rsp\n"
        "call probe_main\n"
        "ud2\n");
}
