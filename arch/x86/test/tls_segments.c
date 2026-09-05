/* Freestanding Linux ABI probe: no libc/TLS use outside the explicit checks. */
typedef unsigned long u64;
typedef unsigned short u16;
enum { THREADS = 4, ROUNDS = 128 };
typedef struct {
    u64 fs_value, gs_value;
    u16 fs_selector, gs_selector;
    unsigned failures, checks;
    int tid;
} thread_state;
static thread_state states[THREADS];
static unsigned char stacks[THREADS][32768] __attribute__((aligned(16)));
static u64 parent_fs = 0x1122334455667788UL;
static u64 parent_gs = 0x8877665544332211UL;
static u16 flat_selector;

static long call(long nr, long a, long b, long c, long d)
{
    register long r10 __asm__("r10") = d;
    long result;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return result;
}

static int install(u16 fs, u16 gs, u64 *fs_base, u64 *gs_base)
{
    __asm__ volatile ("mov %0, %%fs; mov %1, %%gs"
                      : : "rm"(fs), "rm"(gs) : "memory");
    return call(158, 0x1002, (long)fs_base, 0, 0) == 0 &&
           call(158, 0x1001, (long)gs_base, 0, 0) == 0;
}

static int matches(u16 want_fs, u16 want_gs, u64 fs_value, u64 gs_value)
{
    u16 fs, gs;
    u64 observed_fs, observed_gs;
    __asm__ volatile ("mov %%fs, %0; mov %%gs, %1"
                      : "=rm"(fs), "=rm"(gs));
    __asm__ volatile ("movq %%fs:0, %0; movq %%gs:0, %1"
                      : "=r"(observed_fs), "=r"(observed_gs) : : "memory");
    return fs == want_fs && gs == want_gs &&
           observed_fs == fs_value && observed_gs == gs_value;
}

static void pause_thread(void)
{
    const long duration[2] = {0, 1000000};
    (void)call(35, (long)duration, 0, 0, 0);
}

__attribute__((used, noinline)) static long worker_entry(thread_state *state)
{
    if (!matches(flat_selector, flat_selector, state->fs_value, parent_gs))
        state->failures++;
    if (!install(state->fs_selector, state->gs_selector,
                 &state->fs_value, &state->gs_value)) {
        state->failures++;
        return 2;
    }
    for (unsigned i = 0; i < ROUNDS; i++) {
        (void)call(24, 0, 0, 0, 0);
        pause_thread();
        if (!matches(state->fs_selector, state->gs_selector,
                     state->fs_value, state->gs_value)) state->failures++;
        state->checks++;
    }
    return state->failures ? 3 : 0;
}

extern long spawn(void *stack, int *tid, void *tls);
__asm__(
    ".text\n"
    ".globl spawn\n"
    "spawn:\n"
    "mov %rdx, %r8\n"
    "mov %rsi, %r10\n"
    "mov %rsi, %rdx\n"
    "mov %rdi, %rsi\n"
    "mov $0x3d0f00, %edi\n"
    "mov $56, %eax\n"
    "syscall\n"
    "test %rax, %rax\n"
    "jnz 1f\n"
    "pop %rdi\n"
    "call worker_entry\n"
    "mov %rax, %rdi\n"
    "mov $60, %eax\n"
    "syscall\n"
    "ud2\n"
    "1: ret\n");

__attribute__((used, noinline)) static long run(void)
{
    __asm__ volatile ("mov %%ds, %0" : "=rm"(flat_selector));
    if (!install(flat_selector, flat_selector, &parent_fs, &parent_gs)) return 2;
    unsigned failures = 0;
    for (unsigned i = 0; i < THREADS; i++) {
        states[i].fs_value = 0x1357000000000000UL + i;
        states[i].gs_value = 0x2468000000000000UL + i;
        states[i].fs_selector = i & 1 ? flat_selector : 0;
        states[i].gs_selector = i & 1 ? 0 : flat_selector;
        u64 *sp = (u64 *)(stacks[i] + sizeof(stacks[i]));
        *--sp = (u64)&states[i];
        if (spawn(sp, &states[i].tid, &states[i].fs_value) < 0) return 2;
    }
    for (unsigned round = 0; round < ROUNDS; round++) {
        pause_thread();
        if (!matches(flat_selector, flat_selector, parent_fs, parent_gs))
            failures++;
    }
    for (unsigned i = 0; i < THREADS; i++) {
        unsigned waits = 0;
        while (__atomic_load_n(&states[i].tid, __ATOMIC_ACQUIRE)) {
            if (++waits > 10000) return 4;
            pause_thread();
        }
        failures += states[i].failures;
        if (states[i].checks != ROUNDS) failures++;
    }

    long child = call(57, 0, 0, 0, 0);
    if (child < 0) return 2;
    if (!child) {
        long result = matches(flat_selector, flat_selector, parent_fs, parent_gs)
                    ? 0 : 3;
        (void)call(60, result, 0, 0, 0);
        __builtin_unreachable();
    }
    int status = -1;
    if (call(61, child, (long)&status, 0, 0) != child || status != 0) failures++;
    if (!matches(flat_selector, flat_selector, parent_fs, parent_gs)) failures++;
    const char pass[] = "[TLS-SEGMENTS] PASS\n";
    const char fail[] = "[TLS-SEGMENTS] FAIL\n";
    (void)call(1, 1, (long)(failures ? fail : pass), sizeof(pass) - 1, 0);
    return failures ? 3 : 0;
}

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "andq $-16, %rsp\n"
    "call run\n"
    "mov %rax, %rdi\n"
    "mov $231, %eax\n"
    "syscall\n"
    "ud2\n");
