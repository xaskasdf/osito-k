/* Raw Linux-syscall SHM surface contract test for OsitoK. */

#define SYS_WRITE 1
#define SYS_EXIT 60
#define SYS_SHM_DESTROY 503
#define SYS_SHM_GETSIZE 505
#define SYS_SHM_MKSURFACE 506

#define SHM_FLAG_CPU_WRITE 1
#define SHM_FLAG_CPU_READ 2
#define SHM_FLAG_GPU_SCANOUT 4

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

static void fail(int code)
{
    char message[] = "SHM surface contract test: FAIL 00\n";
    message[32] = (char)('0' + code / 10);
    message[33] = (char)('0' + code % 10);
    print(message);
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

void _start(void)
{
    const long flags = SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ |
                       SHM_FLAG_GPU_SCANOUT;

    if (syscall3(SYS_SHM_MKSURFACE, 0, 48, flags) != 0)
        fail(1);
    if (syscall3(SYS_SHM_MKSURFACE, 64, 0, flags) != 0)
        fail(2);
    if (syscall3(SYS_SHM_MKSURFACE, 0x10000, 48, flags) != 0)
        fail(3);
    if (syscall3(SYS_SHM_MKSURFACE, 64, 0x10000, flags) != 0)
        fail(4);

    long handle = syscall3(SYS_SHM_MKSURFACE, 64, 48, flags);
    if (handle <= 0)
        fail(5);
    if (syscall1(SYS_SHM_GETSIZE, handle) != 64 * 48 * 4)
        fail(6);
    if (syscall1(SYS_SHM_DESTROY, handle) != 0)
        fail(7);

    print("SHM surface contract test: PASS\n");
    syscall1(SYS_EXIT, 0);
    for (;;) {}
}
