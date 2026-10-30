/* Exercise the actual terminal mailbox, including stale launch ownership. */
#define sched_spawn test_spawn
#define sched_sleep_ticks test_sleep
#define sched_yield test_yield
#define serial_getc test_getc
#define serial_puts test_puts
#define kb_push test_push
#include "../kernel/terminal_control.c"
extern int printf(const char *, ...);
extern void abort(void);
static unsigned checks, spawns, calls_a, calls_b;
static int spawn_result=-1, last_signal;
static uint32_t owner_a,owner_b;
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n",__LINE__,#test);abort(); } } while (0)
int test_spawn(const char *name, void (*entry)(void))
{ (void)name;(void)entry;spawns++;return spawn_result; }
int test_sleep(uint64_t ticks) { (void)ticks;return 0; }
void test_yield(void) { }
int test_getc(void) { return -1; }
void test_puts(const char *text) { (void)text; }
void test_push(char key) { (void)key; }
static bool request_a(uint32_t generation, int signal)
{
    if (generation!=owner_a) return false;
    calls_a++;last_signal=signal;return true;
}
static bool request_b(uint32_t generation, int signal)
{
    if (generation!=owner_b) return false;
    calls_b++;last_signal=signal;return true;
}
int main(void)
{
    CHECK(!terminal_control_key(3) && !terminal_control_key(11));
    CHECK(!terminal_control_bind(NULL) && !spawns);
    CHECK(!terminal_control_bind(request_a) && spawns==1);
    CHECK(!terminal_control_key(3));
    spawn_result=42;
    owner_a=terminal_control_bind(request_a);
    CHECK(owner_a && spawns==2);
    CHECK(!terminal_control_key('a'));
    CHECK(terminal_control_key(3) && !calls_a);
    terminal_control_poll();CHECK(calls_a==1 && last_signal==15);
    terminal_control_poll();CHECK(calls_a==1);
    CHECK(terminal_control_key(3) && terminal_control_key(11) && terminal_control_key(3));
    terminal_control_poll();CHECK(calls_a==2 && last_signal==9);
    CHECK(terminal_control_key(11));
    owner_b=terminal_control_bind(request_b);
    CHECK(owner_b && owner_b!=owner_a && spawns==2);
    terminal_control_poll();CHECK(!calls_b);
    terminal_control_release(owner_a);
    CHECK(terminal_control_key(3));
    terminal_control_poll();CHECK(calls_b==1 && last_signal==15);
    /* Simulate an old IRQ publishing after a replacement bind. */
    pending_signal=((uint64_t)owner_a<<32)|9;
    terminal_control_poll();CHECK(calls_b==1 && calls_a==2);
    CHECK(terminal_control_key(11));
    terminal_control_release(owner_b);
    terminal_control_poll();CHECK(calls_b==1);
    CHECK(!terminal_control_key(3) && !terminal_control_key(11));
    owner_a=terminal_control_bind(request_a);
    CHECK(owner_a && owner_a!=owner_b);
    CHECK(terminal_control_key(11));
    terminal_control_poll();CHECK(calls_a==3 && last_signal==9);
    terminal_control_release(owner_a);
    printf("terminal-control: %u checks passed\n",checks);
    return 0;
}
