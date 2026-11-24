#define NET_HOST_TEST
#define NET_HOST_TEST_IRQ
#include "../kernel/net.c"

extern int printf(const char *, ...);
extern void exit(int);
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, irq_depth, wakes[8], yields;
static bool interrupts_enabled = true, blocked[8];
static uint64_t ticks;
static int current, original_slot, next_owner;
static void (*preempt_on_wake)(void), (*pending_preemption)(void);

nic_ops_t nic_ops;
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return ticks; }
bool sched_is_enabled(void) { return true; }
int sched_current_get(void) { return current; }
void sched_yield(void) { yields++; }
int sched_block_current(void) { blocked[current] = true; return current; }

uint64_t net_test_irq_save(void)
{
    uint64_t flags = interrupts_enabled ? 0x200 : 0;
    interrupts_enabled = false;
    irq_depth++;
    return flags;
}

void net_test_irq_restore(uint64_t flags)
{
    CHECK(irq_depth > 0);
    irq_depth--;
    interrupts_enabled = (flags & 0x200) != 0;
    if (interrupts_enabled && pending_preemption) {
        void (*run)(void) = pending_preemption;
        pending_preemption = NULL;
        run();
    }
}

void sched_unblock(int slot)
{
    CHECK(slot >= 0 && slot < 8);
    wakes[slot]++;
    blocked[slot] = false;
    /* Model the first enabled-IRQ boundary after sched_unblock returns. */
    if (preempt_on_wake) {
        void (*run)(void) = preempt_on_wake;
        preempt_on_wake = NULL;
        if (interrupts_enabled) run();
        else {
            CHECK(!pending_preemption);
            pending_preemption = run;
        }
    }
}

static void reset(void)
{
    CHECK(!irq_depth && !pending_preemption && !preempt_on_wake);
    interrupts_enabled = true;
    ticks = 100;
    current = next_owner = 1;
    yields = 0;
    memset(wakes, 0, sizeof(wakes));
    memset(blocked, 0, sizeof(blocked));
    memset(net_waiters, 0, sizeof(net_waiters));
    memset(async_ops, 0, sizeof(async_ops));
    net_waiter_count = 0;
}

static void expire(void)
{
    net_deferred_cb_t deferred[NET_MAX_ASYNC_OPS] = {0};
    net_async_check(deferred);
}

static void rearm_and_sleep(void)
{
    net_waiter_clear(original_slot);
    current = next_owner;
    int slot = net_waiter_register(NETWAIT_TCP_ACCEPT, 0, ticks + 10);
    CHECK(slot == original_slot);
    net_waiter_sleep(slot);
}

static void test_deadlines(void)
{
    reset();
    for (int kind = NETWAIT_ARP; kind <= NETWAIT_TCP_ACCEPT; kind++) {
        current = kind;
        CHECK(net_waiter_register((netwait_type_t)kind, 7, 100 + kind) == kind - 1);
        blocked[kind] = true;
    }
    expire();
    CHECK(net_waiter_count == 5);
    for (int kind = NETWAIT_ARP; kind <= NETWAIT_TCP_ACCEPT; kind++) {
        ticks++;
        expire();
        CHECK(wakes[kind] == 1 && !blocked[kind]);
        CHECK(net_waiter_count == 5 - kind);
    }
    expire();
    CHECK(!net_waiter_count && interrupts_enabled && !irq_depth);
    ticks = 0x100000000ULL;
    CHECK(net_waiter_register(NETWAIT_TCP_ACCEPT, 0, ticks + 1) == 0);
    expire();
    CHECK(net_waiter_count == 1);
    ticks++;
    expire();
    CHECK(!net_waiter_count);
}

static void test_rearm(bool different_owner, bool by_packet)
{
    reset();
    original_slot = net_waiter_register(NETWAIT_TCP_ACCEPT, 0, ticks);
    CHECK(original_slot == 0);
    blocked[current] = true;
    if (different_owner) next_owner = 2;
    preempt_on_wake = rearm_and_sleep;
    if (by_packet) net_waiter_wake(NETWAIT_TCP_ACCEPT, 0);
    else expire();
    CHECK(!preempt_on_wake && !pending_preemption && !irq_depth);
    CHECK(blocked[next_owner] && yields == 1);
    CHECK(net_waiter_count == 1);
    CHECK(net_waiters[0].type == NETWAIT_TCP_ACCEPT);
    CHECK(net_waiters[0].proc_idx == next_owner && net_waiters[0].deadline == ticks + 10);
    ticks += 10;
    expire();
    CHECK(!net_waiter_count && !blocked[next_owner]);
    CHECK(wakes[next_owner] == (different_owner ? 1U : 2U));
}

static void test_irq_state(void)
{
    reset();
    original_slot = net_waiter_register(NETWAIT_TCP_ACCEPT, 0, ticks);
    uint64_t outer = net_test_irq_save();
    preempt_on_wake = rearm_and_sleep;
    expire();
    CHECK(!interrupts_enabled && irq_depth == 1 && !net_waiter_count);
    CHECK(pending_preemption && !preempt_on_wake);
    net_test_irq_restore(outer);
    CHECK(interrupts_enabled && !irq_depth && !pending_preemption);
    CHECK(net_waiter_count == 1 && blocked[current]);
    ticks += 10;
    expire();
    CHECK(!net_waiter_count && !blocked[current]);
}

static void test_churn(void)
{
    reset();
    original_slot = net_waiter_register(NETWAIT_TCP_ACCEPT, 0, ticks);
    blocked[current] = true;
    for (int i = 0; i < 10000; i++) {
        preempt_on_wake = rearm_and_sleep;
        expire();
        CHECK(!pending_preemption && !preempt_on_wake);
        CHECK(net_waiter_count == 1 && net_waiters[0].type == NETWAIT_TCP_ACCEPT);
        CHECK(blocked[current]);
        ticks += 10;
    }
    expire();
    CHECK(!net_waiter_count && !blocked[current] && wakes[current] == 10001);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "deadlines")) test_deadlines();
    else if (!strcmp(argv[1], "rearm")) test_rearm(false, false);
    else if (!strcmp(argv[1], "reuse")) test_rearm(true, false);
    else if (!strcmp(argv[1], "packet")) test_rearm(false, true);
    else if (!strcmp(argv[1], "irq-state")) test_irq_state();
    else if (!strcmp(argv[1], "churn")) test_churn();
    else CHECK(false);
    printf("network waiter %s: %u checks\n", argv[1], checks);
    return 0;
}
