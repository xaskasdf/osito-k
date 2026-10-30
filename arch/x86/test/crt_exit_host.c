#include <stdio.h>
#include <stdlib.h>
#define sched_yield exit_test_sched_yield
#include "../win32/crt_exit.c"
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks, allocations, called, simulated32;
static DWORD pid = 5;
static int fail_allocation;
int g_compat32_mode;
void *kmalloc(uint64_t bytes) { if (fail_allocation) return NULL; ++allocations; return malloc(bytes); }
void kfree(void *p) { CHECK(p && allocations); --allocations; free(p); }
void sched_yield(void) { CHECK(0); }
DWORD win32_current_process_id(void) { return pid; }
void compat32_callback(uint32_t function) { CHECK(function == 1); ++simulated32; }
static void WINAPI callback_one(void) { ++called; }
static void WINAPI callback_nested(void) { CHECK(!crt_exit_callbacks_register((void *)callback_one)); }
int main(void)
{
    CHECK(crt_exit_callbacks_register(NULL) < 0);
    fail_allocation = 1;
    CHECK(crt_exit_callbacks_register((void *)callback_one) < 0 && !allocations);
    fail_allocation = 0;
    for (unsigned repeat = 0; repeat < 200; ++repeat) {
        pid = 5;
        for (unsigned i = 0; i < 400; ++i) CHECK(!crt_exit_callbacks_register((void *)callback_one));
        pid = 6;
        CHECK(!crt_exit_callbacks_register((void *)callback_one));
        pid = 5;
        CHECK(!crt_exit_callbacks_register((void *)callback_nested));
        called = 0;
        crt_exit_callbacks_run();
        CHECK(called == 401 && allocations == 1);
        crt_exit_callbacks_run();
        CHECK(called == 401);
        crt_exit_callbacks_release(6);
        CHECK(!allocations && !crt_exit_callbacks && called == 401);
        g_compat32_mode = 1;
        CHECK(crt_exit_callbacks_register((PVOID)(ULONG_PTR)0x100000000ULL) < 0);
        CHECK(!crt_exit_callbacks_register((void *)1));
        crt_exit_callbacks_run();
        CHECK(simulated32 == repeat + 1 && !allocations);
        g_compat32_mode = 0;
    }
    printf("CRT EXIT HOST PASS checks=%u\n", checks);
    return 0;
}
