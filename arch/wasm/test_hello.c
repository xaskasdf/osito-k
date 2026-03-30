/* Minimal SIDE_MODULE test — verify dlopen/dlsym works */
#include <stdio.h>

int hello_main(int argc, char **argv)
{
    (void)argv;
    printf("[HELLO] Hello from side module! argc=%d\n", argc);
    return 42;
}
