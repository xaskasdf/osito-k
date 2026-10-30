/* Deterministic scheduler interleavings around the real native wait queue. */
#define __KERNEL_X86__ 1
#define NTSYNC_HOST_TEST 1
#define OK_QUIET 1
#define sched_yield address_test_sched_yield
#include "../win32/ntsync.c"
extern int printf(const char *, ...);
extern void exit(int);
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, lock_depth, wakes[8], yields, last_error;
static int current, compat, block_fails;
static ULONG current_pid = 7;
static BOOL blocked[8];
static uint64_t ticks;
static uint64_t word = 12;
static void (*before_block)(void), (*during_yield)(void);
static NTSYNC_WAITER storage[8];

uint64_t ntsync_host_lock(void) { CHECK(!lock_depth); lock_depth = 1; return 0x200; }
void ntsync_host_unlock(uint64_t flags) { CHECK(lock_depth == 1 && flags == 0x200); lock_depth = 0; }
uint64_t idt_get_ticks(void) { return ticks; }
int sched_current_get(void) { return current; }
int32_t proc_current_pid(void) { return current + 100; }
DWORD win32_current_process_id(void) { return current_pid; }
int *proc_win32_compat32_mode_slot(void) { return &compat; }
void win32_main_termination_checkpoint(void) { CHECK(!lock_depth); }
void WINAPI SetLastError(DWORD error) { last_error = error; }
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
BOOL nt_process_object_signaled(PVOID p) { (void)p; return FALSE; }
int sched_block_current(void)
{
    CHECK(!lock_depth);
    if (before_block) before_block();
    if (block_fails) return -1;
    blocked[current] = TRUE;
    return current;
}
void sched_unblock(int slot) { CHECK(lock_depth && slot >= 0 && slot < 8); blocked[slot] = FALSE; ++wakes[slot]; }
void sched_yield(void)
{
    CHECK(!lock_depth);
    ++yields;
    if (during_yield) during_yield();
    CHECK(!blocked[current]);
}
static void inject_wake(void) { WakeByAddressSingle(&word); }
static void inject_timeout(void) { ticks += 10; ntsync_poll_waiters(ticks); }

/* Preserve a registered task by simulating the scheduler switching to the
 * test driver at yield. The public wait API races are tested separately. */
static void register_waiter(unsigned slot, ULONG pid, volatile void *address)
{
    NTSYNC_WAITER *w = &storage[slot];
    uint32_t b = (uint32_t)(((ULONG_PTR)address >> 3) ^ pid) & 255;
    w->active = TRUE; w->completed = w->timed_out = FALSE;
    w->address = address; w->address_bucket = b;
    w->address_prev = g_address_waiters[b].tail; w->address_next = 0;
    w->owner_pid = pid; w->count = 0; w->proc_idx = slot;
    w->has_deadline = FALSE;
    if (w->address_prev) storage[w->address_prev - 1].address_next = slot + 1;
    else g_address_waiters[b].head = slot + 1;
    g_address_waiters[b].tail = slot + 1;
    blocked[slot] = TRUE;
    ++g_waiter_count;
}

int main(void)
{
    g_waiters = storage; g_waiter_capacity = 8;
    uint64_t equal = 12, different = 13;
    for (unsigned size = 1; size <= 8; size *= 2) {
        CHECK(WaitOnAddress(&word, &different, size, 0));
        CHECK(!WaitOnAddress(&word, &equal, size, 0));
        CHECK(last_error == 1460 && !g_waiter_count);
    }
    CHECK(!WaitOnAddress(&word, &equal, 3, 10) && last_error == 87);
    CHECK(!WaitOnAddress(NULL, &equal, 4, 10) && last_error == 87);
    CHECK(!WaitOnAddress(&word, NULL, 4, 10) && last_error == 87);
    current = -1;
    CHECK(!WaitOnAddress(&word, &equal, 4, 10) && last_error == 8);
    current = 0;
    before_block = inject_wake;
    CHECK(WaitOnAddress(&word, &equal, 8, 0xffffffff));
    CHECK(!g_waiter_count && !blocked[0] && wakes[0] == 2);
    before_block = NULL; during_yield = inject_wake;
    CHECK(WaitOnAddress(&word, &equal, 8, 0xffffffff));
    CHECK(!g_waiter_count && !blocked[0]);
    during_yield = inject_timeout;
    CHECK(!WaitOnAddress(&word, &equal, 8, 51) && last_error == 1460);
    CHECK(!g_waiter_count && !storage[0].address);
    during_yield = NULL; block_fails = 1;
    CHECK(WaitOnAddress(&word, &equal, 4, 0xffffffff));
    CHECK(!g_waiter_count && !storage[0].address);
    block_fails = 0;
    /* Same hash/address in a different process must never receive this wake. */
    register_waiter(5, 263, &word);
    register_waiter(3, 7, &word);
    register_waiter(1, 7, &word);
    register_waiter(2, 7, &word);
    WakeByAddressSingle(&word);
    CHECK(!blocked[3] && blocked[1] && blocked[2] && blocked[5]);
    ntsync_cancel_waiter(1);
    CHECK(!storage[1].address && !storage[1].active && g_waiter_count == 2);
    WakeByAddressAll(&word);
    CHECK(!blocked[2] && blocked[5] && g_waiter_count == 1);
    current_pid = 263;
    WakeByAddressSingle(&word);
    CHECK(!blocked[5] && !g_waiter_count);
    for (unsigned i = 0; i < 256; ++i)
        CHECK(!g_address_waiters[i].head && !g_address_waiters[i].tail);
    /* Cancelling the first, middle and last waiter, followed by slot reuse. */
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        current_pid = 7;
        register_waiter(0, 7, &word); register_waiter(1, 7, &word);
        register_waiter(2, 7, &word);
        ntsync_cancel_waiter(repeat % 3);
        WakeByAddressAll(&word);
        CHECK(!g_waiter_count);
        for (unsigned i = 0; i < 3; ++i)
            CHECK(!storage[i].address && !storage[i].active);
    }
    CHECK(yields == 4);
    printf("ADDRESS WAIT HOST PASS checks=%u\n", checks);
    return 0;
}
