/* Ring-0 syscall bridge for dynamically loaded graphics modules. */

#include <stddef.h>
#include <stdint.h>

/* Native applications retain brk. A Win32 module configures process-owned
 * arenas before its first allocation, so discarding the module cannot orphan
 * a native heap in the shell's TGID. malloc.c serializes calls to this hook. */
typedef void *(*okgl_heap_grow_fn)(void *, long);
static okgl_heap_grow_fn module_heap_grow;
static void *module_heap_owner;
static int module_heap_started;
extern void *sbrk(long);

__attribute__((visibility("default")))
int okGLConfigureHeap(void *owner, okgl_heap_grow_fn grow)
{
    if (module_heap_started || module_heap_grow || !grow) return -1;
    module_heap_owner = owner;
    module_heap_grow = grow;
    return 0;
}

void *okgl_heap_grow(long increment)
{
    module_heap_started = 1;
    if (module_heap_grow)
        return module_heap_grow(module_heap_owner, increment);
    return sbrk(increment);
}

extern void serial_puts(const char *message)
    __attribute__((visibility("default")));

static void okgl_serial_putc(char c)
{
    char message[2] = {c, 0};
    serial_puts(message);
}

static void okgl_serial_puts(const char *message)
{
    if (!message)
        return;
    serial_puts(message);
}

static void okgl_serial_puthex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        okgl_serial_putc(digits[(value >> shift) & 0xf]);
}

void okgl_diag3(const char *message, uint64_t a, uint64_t b, uint64_t c)
{
    okgl_serial_puts(message);
    okgl_serial_puts(" a=0x");
    okgl_serial_puthex64(a);
    okgl_serial_puts(" b=0x");
    okgl_serial_puthex64(b);
    okgl_serial_puts(" c=0x");
    okgl_serial_puthex64(c);
    okgl_serial_puts("\n");
}

void okgl_trace(const char *message)
{
#ifndef OKGL_SERIAL_TRACE
    (void)message;
#else
    okgl_serial_puts(message);
#endif
}

extern long syscall_dispatch(unsigned long nr, unsigned long a1,
                             unsigned long a2, unsigned long a3,
                             unsigned long a4, unsigned long a5,
                             unsigned long a6)
    __attribute__((visibility("default")));

long __syscall0(long nr)
{
    return syscall_dispatch((unsigned long)nr, 0, 0, 0, 0, 0, 0);
}

long __syscall1(long nr, long a1)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            0, 0, 0, 0, 0);
}

long __syscall2(long nr, long a1, long a2)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, 0, 0, 0, 0);
}

long __syscall3(long nr, long a1, long a2, long a3)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            0, 0, 0);
}

long __syscall4(long nr, long a1, long a2, long a3, long a4)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, 0, 0);
}

long __syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, (unsigned long)a5, 0);
}

long __syscall6(long nr, long a1, long a2, long a3, long a4, long a5,
                long a6)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, (unsigned long)a5,
                            (unsigned long)a6);
}
