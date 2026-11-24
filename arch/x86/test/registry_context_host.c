/* Keep libc's context types out of the freestanding kernel translation unit. */
#define _XOPEN_SOURCE 700
#include <ucontext.h>
#include <stdlib.h>

static ucontext_t main_context, contexts[2];
static unsigned char stacks[2][65536];
static void (*tasks[2])(void);
static int current, done[2];

void registry_context_yield(void)
{
    int old = current, next = old ^ 1;
    if (done[next]) return;
    current = next;
    if (swapcontext(&contexts[old], &contexts[next])) abort();
}

static void enter(int id)
{
    tasks[id]();
    done[id] = 1;
    registry_context_yield();
    if (setcontext(&main_context)) abort();
}

void registry_context_run(void (*first)(void), void (*second)(void))
{
    tasks[0] = first; tasks[1] = second;
    done[0] = done[1] = current = 0;
    for (volatile int i = 0; i < 2; i++) {
        if (getcontext(&contexts[i])) abort();
        contexts[i].uc_stack.ss_sp = stacks[i];
        contexts[i].uc_stack.ss_size = sizeof(stacks[i]);
        contexts[i].uc_link = &main_context;
        makecontext(&contexts[i], (void (*)(void))enter, 1, i);
    }
    if (swapcontext(&main_context, &contexts[0])) abort();
    if (!done[0] || !done[1]) abort();
}
