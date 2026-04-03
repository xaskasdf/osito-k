/*
 * OsitoK — DOS Compatibility Layer Initialization
 *
 * Init hook called from main.c after win32_init().
 * Prints banner and marks the DOS subsystem as ready.
 */

#include "dos_types.h"

extern void serial_puts(const char *s);
extern void fb_puts(const char *s);

static int dos_initialized = 0;

void dos_init(void)
{
    if (dos_initialized) return;

    serial_puts("\n[DOS] Initializing DOS 16-bit compatibility layer...\n");
    serial_puts("[DOS] 8086 software emulator, INT 21h/10h/16h services\n");

    dos_initialized = 1;

    serial_puts("[DOS] Ready. Use 'dosrun <file.com|file.exe>' to run DOS binaries.\n");
    fb_puts(" DOS 16-bit compat layer ready\n");
}

int dos_is_initialized(void)
{
    return dos_initialized;
}
