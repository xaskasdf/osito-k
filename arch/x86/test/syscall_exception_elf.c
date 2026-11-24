/* Guest exceptions must not claim native ELF faults sharing SYSCALL's CS. */
void _start(void)
{
    long pid;
    __asm__ volatile ("syscall" : "=a"(pid) : "a"(39L)
                      : "rcx", "r11", "cc", "memory");
    unsigned short cs;
    __asm__ volatile ("mov %%cs,%0" : "=r"(cs));
    const char good[] = "NATIVE-FAULT CS 0090\n";
    const char bad[] = "NATIVE-FAULT FAIL selector\n";
    const char *text = pid > 0 && cs == 0x90 ? good : bad;
    long size = text == good ? sizeof(good)-1 : sizeof(bad)-1;
    __asm__ volatile ("syscall" : : "a"(1L), "D"(1L), "S"(text), "d"(size)
                      : "rcx", "r11", "cc", "memory");
    __asm__ volatile ("int3");
    for (;;) {}
}
