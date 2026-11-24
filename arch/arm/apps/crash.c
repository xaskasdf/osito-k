/* crash.c -- a program that faults: the kernel ends it and the shell goes on */
#include <stdio.h>

int main(void)
{
    printf("Writing through a null pointer...\n");
    fflush(stdout);
    volatile int *p = (volatile int *)0;
    *p = 42;
    printf("not reached\n");
    return 0;
}
