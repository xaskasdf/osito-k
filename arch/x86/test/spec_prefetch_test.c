#include "../kernel/spec_prefetch.c"

extern int printf(const char *, ...);

int ap_worker_count = 3;
static uint64_t now, missing_page = UINT64_MAX;
static unsigned walks, submits;
static int reject, paging_allowed = 1, failures, checks;
static void (*queued_func)(void *, void *);
static void *queued_arg;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

uint64_t idt_get_ticks(void) { return now; }

uint64_t paging_translate_in_cr3(uint64_t cr3, uint64_t va)
{
    CHECK(paging_allowed);
    CHECK(cr3 == 0x1000);
    CHECK((va & 4095) == 0);
    walks++;
    if (va == missing_page)
        return UINT64_MAX;
    return (va & 4096) ? 0x810000 : 0x120000;
}

int smp_submit_ff(void (*func)(void *, void *), void *arg, void *result)
{
    (void)result;
    submits++;
    if (reject)
        return -1;
    CHECK(queued_func == NULL);
    queued_func = func;
    queued_arg = arg;
    return 0;
}

static void finish(void)
{
    CHECK(queued_func != NULL);
    if (!queued_func) return;
    unsigned saved_walks = walks;
    paging_allowed = 0;
    queued_func(queued_arg, NULL);
    paging_allowed = 1;
    queued_func = NULL;
    CHECK(walks == saved_walks);
    CHECK(prefetch_pending == 0);
}

static void reset(void)
{
    CHECK(queued_func == NULL);
    now = 10;
    last_prefetch_tick = prefetch_count = 0;
    prefetch_pending = 0;
    walks = submits = 0;
    reject = 0;
    missing_page = UINT64_MAX;
    ap_worker_count = 3;
}

int main(void)
{
    reset();
    spec_prefetch_ahead(0x401023, 0x1000);
    CHECK(walks == 1 && submits == 1);
    CHECK(prefetch_arg.count == 32);
    CHECK(prefetch_arg.targets[0] == (uint64_t)PHYS_TO_VIRT(0x810000));
    CHECK(prefetch_arg.targets[31] == (uint64_t)PHYS_TO_VIRT(0x8107C0));
    prefetch_task_t snapshot = prefetch_arg;
    now += 100;
    spec_prefetch_ahead(0x600000, 0x1000);
    CHECK(submits == 1 && walks == 1);
    CHECK(memcmp(&snapshot, &prefetch_arg, sizeof(snapshot)) == 0);
    finish();

    reset();
    spec_prefetch_ahead(0x40FFFF, 0x1000);
    CHECK(walks == 2 && prefetch_arg.count == 32);
    CHECK(prefetch_arg.targets[0] == (uint64_t)PHYS_TO_VIRT(0x810FC0));
    CHECK(prefetch_arg.targets[1] == (uint64_t)PHYS_TO_VIRT(0x120000));
    finish();

    reset();
    missing_page = 0x410000;
    spec_prefetch_ahead(0x40FFFF, 0x1000);
    CHECK(walks == 2 && prefetch_arg.count == 1);
    finish();

    reset();
    missing_page = 0x40F000;
    spec_prefetch_ahead(0x40FFFF, 0x1000);
    CHECK(walks == 2 && prefetch_arg.count == 31);
    CHECK(prefetch_arg.targets[0] == (uint64_t)PHYS_TO_VIRT(0x120000));
    finish();

    reset();
    missing_page = 0x401000;
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(walks == 1 && submits == 0 && !prefetch_pending);
    reset();
    spec_prefetch_ahead(0x401000, 0);
    CHECK(walks == 0 && submits == 0 && !prefetch_pending);
    reset();
    spec_prefetch_ahead(0xFFFF800001234567ULL, 0);
    CHECK(walks == 0 && prefetch_arg.count == 32);
    CHECK(prefetch_arg.targets[0] == 0xFFFF800001234540ULL);
    finish();

    reset();
    spec_prefetch_ahead(UINT64_MAX, 0);
    CHECK(prefetch_arg.count == 1 && walks == 0);
    CHECK(prefetch_arg.targets[0] == (UINT64_MAX & ~63ULL));
    finish();
    reset();
    spec_prefetch_ahead(0x0000800000000000ULL, 0x1000);
    CHECK(submits == 0 && walks == 0 && !prefetch_pending);

    reset();
    reject = 1;
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(submits == 1 && !prefetch_pending && prefetch_count == 0);
    reject = 0;
    now += 2;
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(submits == 2 && prefetch_count == 1);
    finish();

    reset();
    ap_worker_count = 0;
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(submits == 0 && walks == 0);
    ap_worker_count = 3;
    now = 1;
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(submits == 0 && walks == 0);
    now = 2;
    spec_prefetch_ahead(0x401000, 0x1000);
    finish();
    spec_prefetch_ahead(0x401000, 0x1000);
    CHECK(submits == 1);
    for (unsigned i = 0; i < 100; i++) {
        now += 2;
        spec_prefetch_ahead(0x40FFFF, 0x1000);
        finish();
    }
    CHECK(prefetch_count == 101);
    printf("spec prefetch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
