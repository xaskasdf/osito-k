/* PE32+ child used to verify the status of an unhandled CPU exception. */

void mainCRTStartup(void)
{
    __asm__ volatile ("ud2");
    for (;;) { }
}
