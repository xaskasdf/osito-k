#include "../kernel/timer_deadline.h"

extern int printf(const char *, ...);
static unsigned checks, failures, scheduler_calls, audio_calls;
static uint64_t scheduler_us, audio_us;

#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

uint64_t sched_get_next_deadline_us(void)
{
    scheduler_calls++;
    return scheduler_us;
}

uint64_t audio_get_next_deadline_us(void)
{
    audio_calls++;
    return audio_us;
}

static void test_deadlines(void)
{
    static const uint64_t deadlines[] = {
        0, 1, 100, 1000, 9999, 10000, 10001, 20000, UINT64_MAX
    };
    for (unsigned native = 0; native < 2; native++) {
        for (unsigned s = 0; s < sizeof(deadlines) / sizeof(deadlines[0]); s++) {
            for (unsigned a = 0; a < sizeof(deadlines) / sizeof(deadlines[0]); a++) {
                scheduler_calls = audio_calls = 0;
                scheduler_us = deadlines[s];
                audio_us = deadlines[a];
                uint64_t result = idt_next_deadline_us(native != 0);
                CHECK(scheduler_calls == (native ? 0u : 1u));
                CHECK(audio_calls == 1);
                uint64_t service_us = native ? 10000 : scheduler_us;
                uint64_t expected = !service_us ? audio_us :
                    !audio_us || service_us <= audio_us ? service_us : audio_us;
                CHECK(result == expected);
            }
        }
    }
}

static void test_expired_sleeper(void)
{
    /* A sleep expires after the DOS client takes exclusive CPU ownership.
     * Model repeated hardware rearming with no sched_tick() to retire it. */
    scheduler_calls = audio_calls = 0;
    scheduler_us = 1;
    audio_us = 0;
    uint64_t elapsed_us = 0;
    unsigned interrupts = 0;
    while (elapsed_us < 40000000 && interrupts < 10000) {
        uint64_t next = idt_next_deadline_us(true);
        CHECK(next == 10000);
        elapsed_us += next;
        interrupts++;
    }
    CHECK(elapsed_us == 40000000);
    CHECK(interrupts == 4000);
    CHECK(scheduler_calls == 0);
    CHECK(audio_calls == interrupts);

    /* Leaving DOS immediately restores the overdue scheduler deadline. */
    CHECK(idt_next_deadline_us(false) == 1);
    CHECK(scheduler_calls == 1);
    scheduler_us = 20000;
    CHECK(idt_next_deadline_us(false) == 20000);

    /* A client entered from tickless idle still needs timer/input service. */
    scheduler_us = 0;
    CHECK(idt_next_deadline_us(true) == 10000);
    CHECK(idt_next_deadline_us(false) == 0);
    audio_us = 500;
    CHECK(idt_next_deadline_us(true) == 500);
    CHECK(idt_next_deadline_us(false) == 500);
}

int main(void)
{
    test_deadlines();
    test_expired_sleeper();
    printf("[TIMER-DEADLINE] checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
