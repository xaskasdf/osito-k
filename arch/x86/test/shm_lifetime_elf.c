/* Exercise the native SHM syscall lifetime contract inside Osito-K. */
enum { WRITE = 1, EXIT = 60, SHM_CREATE = 500, SHM_MAP = 501,
       SHM_UNMAP = 502, SHM_DESTROY = 503, SHM_SIZE = 505, SHM_SURFACE = 506 };
static unsigned checks;

static long call(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile ("syscall" : "=a"(result)
        : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static void print(const char *text)
{
    long length = 0;
    while (text[length]) length++;
    call(WRITE, 1, (long)text, length);
}

static void check(int condition)
{
    checks++;
    if (condition) return;
    print("SHM LIFETIME FAIL\n");
    call(EXIT, 1, 0, 0);
    for (;;) {}
}

void _start(void)
{
    check(!call(SHM_CREATE, 0, 3, 0));
    check(!call(SHM_CREATE, -1, 3, 0));
    check(!call(SHM_SURFACE, 0, 2, 3));
    check(!call(SHM_SURFACE, 2, 65536, 3));
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        long handle = call(SHM_SURFACE, 16, 8, 3);
        check(handle > 0 && call(SHM_SIZE, handle, 0, 0) == 4096);
        long first = call(SHM_MAP, handle, 0, 0), second = call(SHM_MAP, handle, 0, 0);
        check(first && second == first);
        check(!call(SHM_DESTROY, handle, 0, 0));
        check(!call(SHM_DESTROY, handle, 0, 0));
        check(!call(SHM_UNMAP, handle, 0, 0));
        check(call(SHM_SIZE, handle, 0, 0) == 4096);
        check(call(SHM_MAP, handle, 0, 0) == second);
        check(!call(SHM_UNMAP, handle, 0, 0));
        check(call(SHM_SIZE, handle, 0, 0) == 4096);
        check(!call(SHM_UNMAP, handle, 0, 0));
        check(!call(SHM_SIZE, handle, 0, 0));
        check(!call(SHM_MAP, handle, 0, 0));
        check(!call(SHM_DESTROY, handle, 0, 0));
    }
    check(checks == 1668);
    print("SHM LIFETIME PASS: 1669 checks\n");
    call(EXIT, 0, 0, 0);
    for (;;) {}
}
