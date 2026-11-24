/*
 * OsitoK — Test dynamic module
 *
 * Compile on Linux host:
 *   gcc -shared -fPIC -nostdlib -Wl,--hash-style=sysv -o testmod.so testmod.c
 *
 * Write to OsitoFS disk:
 *   tools/ositofs/write build/nvme.img testmod.so
 *
 * Use in OsitoK shell:
 *   dl load testmod.so
 *   dl call testmod.so mod_hello
 *   dl list
 *   dl close testmod.so
 */

/* Kernel functions — resolved by dynamic linker via ksym_table */
extern void serial_puts(const char *s);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, unsigned int color);
extern void fb_putdec(unsigned long long val);

/* ── Exported functions ─────────────────────────────────────── */

int mod_add(int a, int b)
{
    return a + b;
}

void mod_hello(void)
{
    serial_puts("[MODULE] Hello from dynamically loaded module!\n");
    fb_puts_color(" [MOD] ", 0x00FF00FF);
    fb_puts("Hello from dynamically loaded module!\n");
}

int mod_factorial(int n)
{
    int result = 1;
    for (int i = 2; i <= n; i++)
        result *= i;
    return result;
}

/* Pure function — no kernel deps, useful for dl_sym + call test */
int mod_square(int x)
{
    return x * x;
}
