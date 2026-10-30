/* QEMU-only privileged fixture. Addresses come from the exact kernel's ELF
 * symbols, not from production exports. Never run without an armed observer. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef void *HANDLE;
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void ExitProcess(DWORD);
static unsigned char saved[512] __attribute__((aligned(16)));
static unsigned char expected[512] __attribute__((aligned(16)));
static unsigned char actual[512] __attribute__((aligned(16)));
static unsigned mxcsr = 0x7f80;

static U64 argument(const char **text)
{
    while (**text == ' ') (*text)++;
    U64 n = 0;
    while (**text && **text != ' ') {
        unsigned c = (unsigned char)*(*text)++;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else { ExitProcess(2); return 0; }
        n = n * 16 + c;
    }
    return n;
}

void mainCRTStartup(void)
{
    const char *text = GetCommandLineA();
    while (*text && *text != ' ') text++;
    volatile unsigned *counter = (volatile unsigned *)argument(&text);
    volatile unsigned *preempt = (volatile unsigned *)argument(&text);
    U64 soft = argument(&text), flags = 0;
    if (!counter || !preempt || *counter) { ExitProcess(3); return; }
    if (soft == 1) __atomic_fetch_add(preempt, 1, __ATOMIC_ACQ_REL);
    __asm__ volatile (
        "fxsave64 %0\n\t"
        "fninit\n\t"
        "fld1\n\t"
        "pcmpeqd %%xmm0, %%xmm0\n\t"
        "ldmxcsr %4\n\t"
        "fxsave64 %1\n\t"
        "cmp $1, %5\n\t"
        "je 1f\n\t"
        "cli\n\t"
        "1: std\n\t"
        "2: pause\n\t"
        "cmpl $0, (%6)\n\t"
        "je 2b\n\t"
        "pushfq\n\t"
        "pop %3\n\t"
        "cld\n\t"
        "fxsave64 %2\n\t"
        "fxrstor64 %0\n\t"
        "cmp $2, %5\n\t"
        "je 3f\n\t"
        "sti\n\t"
        "3:\n\t"
        : "+m"(saved), "+m"(expected), "+m"(actual), "=&r"(flags)
        : "m"(mxcsr), "r"(soft), "r"(counter)
        : "memory", "cc", "xmm0");
    if (soft == 2) for (;;) __asm__ volatile ("pause");
    if (soft == 1) __atomic_fetch_sub(preempt, 1, __ATOMIC_ACQ_REL);
    int ok = (flags & 0x400) && ((flags & 0x200) != 0) == (soft != 0);
    for (unsigned i = 0; i < sizeof(actual); i++)
        if (actual[i] != expected[i]) ok = 0;
    const char *message = ok ? "LOCKUP64 PASS\r\n" : "LOCKUP64 FAIL\r\n";
    DWORD written;
    WriteFile(GetStdHandle((DWORD)-11), message, 15, &written, (void *)0);
    ExitProcess(ok ? 0 : 1);
}
