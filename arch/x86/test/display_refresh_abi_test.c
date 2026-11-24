/*
 * display_refresh_abi_test.c -- compile-time ABI check for display refresh
 * pacing control.
 */

#include "../include/sys/display_syscalls.h"

#ifndef DISPLAY_SET_REFRESH_ONLY
#error "DISPLAY_SET_REFRESH_ONLY is required for refresh-only modeset"
#endif

typedef char refresh_field_must_exist[
    sizeof(((display_mode_info_t *)0)->refresh_hz) == sizeof(uint32_t) ? 1 : -1
];

void _start(void)
{
    volatile uint32_t flag = DISPLAY_SET_REFRESH_ONLY;
    volatile uint32_t syscall_nr = SYS_DISPLAY_SET_MODE;
    (void)flag;
    (void)syscall_nr;
    for (;;);
}
