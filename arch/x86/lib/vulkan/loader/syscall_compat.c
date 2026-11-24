extern long syscall(long number, ...);

long __syscall1(long number, long argument)
{
    return syscall(number, argument);
}

long __syscall2(long number, long argument1, long argument2)
{
    return syscall(number, argument1, argument2);
}
