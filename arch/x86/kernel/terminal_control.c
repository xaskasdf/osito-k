#include "terminal_control.h"

extern int sched_spawn(const char *, void (*)(void));
extern int sched_sleep_ticks(uint64_t ticks);
extern void sched_yield(void);
extern int serial_getc(void);
extern void serial_puts(const char *);
extern void kb_push(char);

static uint32_t next_generation;
static uint32_t foreground_generation;
static terminal_signal_fn foreground_request;
static uint64_t pending_signal;
static bool worker_started;

bool terminal_control_key(unsigned char key)
{
    unsigned signal = key == 3 ? 15U : key == 11 ? 9U : 0U;
    if (!signal) return false;
    uint32_t generation = __atomic_load_n(&foreground_generation, __ATOMIC_ACQUIRE);
    if (!generation) return false;
    uint64_t request = ((uint64_t)generation << 32) | signal;
    uint64_t old = __atomic_load_n(&pending_signal, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((uint32_t)(old >> 32) == generation && (uint32_t)old == 9U)
            return true; /* SIGTERM cannot replace a queued SIGKILL. */
        if (__atomic_compare_exchange_n(&pending_signal, &old, request, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}

void terminal_control_poll(void)
{
    uint64_t pending = __atomic_exchange_n(&pending_signal, 0, __ATOMIC_ACQ_REL);
    uint32_t generation = (uint32_t)(pending >> 32);
    if (!generation || generation !=
        __atomic_load_n(&foreground_generation, __ATOMIC_ACQUIRE)) return;
    terminal_signal_fn request = __atomic_load_n(&foreground_request, __ATOMIC_ACQUIRE);
    if (request && request(generation, (int)(uint32_t)pending))
        serial_puts((uint32_t)pending == 9U
            ? "[TERM] SIGKILL requested\n" : "[TERM] SIGTERM requested\n");
}

static void terminal_control_worker(void)
{
    for (;;) {
        if (__atomic_load_n(&foreground_generation, __ATOMIC_ACQUIRE)) {
            /* Preserve ordinary stdin bytes while observing serial console
             * control keys even when its foreground program never reads. */
            for (unsigned n = 0; n < 32; n++) {
                int key = serial_getc();
                if (key < 0) break;
                kb_push((char)key);
            }
        }
        terminal_control_poll();
        if (sched_sleep_ticks(1) < 0) sched_yield();
    }
}

uint32_t terminal_control_bind(terminal_signal_fn request)
{
    if (!request) return 0;
    if (!worker_started) {
        if (sched_spawn("term-control", terminal_control_worker) < 0) return 0;
        worker_started = true;
    }
    uint32_t generation = ++next_generation;
    if (!generation) generation = ++next_generation;
    __atomic_store_n(&foreground_generation, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pending_signal, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&foreground_request, request, __ATOMIC_RELEASE);
    __atomic_store_n(&foreground_generation, generation, __ATOMIC_RELEASE);
    return generation;
}

void terminal_control_release(uint32_t generation)
{
    if (!generation) return;
    (void)__atomic_compare_exchange_n(&foreground_generation, &generation, 0,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
