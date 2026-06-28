/*
 * display_modes_test.c — minimal display modes syscall ABI smoke test.
 *
 * Build outside/inside OsitoK as a freestanding user ELF. Runtime requires
 * the desktop/display subsystem to have initialized the mode table.
 */

#include "../include/sys/display_syscalls.h"

static long _syscall1(long nr, long a1)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static long _syscall2(long nr, long a1, long a2)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static long _syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

static void _puts(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    _syscall3(1, 1, (long)s, len);
}

void _start(void)
{
    display_mode_info_t cur;
    display_mode_info_t first;

    long count = _syscall1(SYS_DISPLAY_GET_MODE_COUNT, 0);
    if (count <= 0) {
        _puts("display modes: no modes\n");
        _syscall1(60, 1);
    }

    if (_syscall2(SYS_DISPLAY_GET_CURRENT_MODE, (long)&cur, 0) < 0) {
        _puts("display modes: current failed\n");
        _syscall1(60, 2);
    }

    if (_syscall2(SYS_DISPLAY_GET_MODE, 0, (long)&first) < 0) {
        _puts("display modes: mode[0] failed\n");
        _syscall1(60, 3);
    }

    if (!cur.width || !cur.height || !first.width || !first.height) {
        _puts("display modes: bad dimensions\n");
        _syscall1(60, 4);
    }

    _puts("display modes: PASS\n");
    _syscall1(60, 0);
    for (;;);
}
