/* Raw Linux-syscall probe for fork/execve across the OSS audio test. */

#define SYS_WRITE  1
#define SYS_FORK   57
#define SYS_EXECVE 59
#define SYS_EXIT   60
#define SYS_WAIT4  61

static long syscall0(long number)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result) : "a"(number)
        : "rcx", "r11", "memory");
    return result;
}

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

static long syscall4(long number, long arg1, long arg2, long arg3, long arg4)
{
    register long r10 __asm__("r10") = arg4;
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
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

static void fail(int code)
{
    char message[] = "OSS execve transition test: FAIL 0\n";
    message[33] = (char)('0' + code);
    print(message);
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

static __attribute__((used, noinline, noreturn)) void probe_main(void)
{
    static char executable[] = "ossaud.elf";
    static char *arguments[] = { executable, 0 };
    int status = -1;

    long child = syscall0(SYS_FORK);
    if (child < 0)
        fail(1);
    if (child == 0) {
        syscall3(SYS_EXECVE, (long)executable, (long)arguments, 0);
        fail(2);
    }

    long waited = syscall4(SYS_WAIT4, child, (long)&status, 0, 0);
    if (waited != child)
        fail(3);
    if (status != 0)
        fail(4);

    print("OSS execve transition test: PASS\n");
    syscall1(SYS_EXIT, 0);
    for (;;) {}
}

/* ELF entry is reached by JMP, not CALL. Establish the SysV function-entry
 * alignment before invoking C so stack-local SSE stores remain valid. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile (
        "andq $-16, %rsp\n"
        "call probe_main\n"
        "ud2\n");
}
