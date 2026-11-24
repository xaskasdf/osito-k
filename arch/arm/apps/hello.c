/* hello.c -- arguments, printf and time: `exec hello.elf uno dos` */
#include <stdio.h>
#include <time.h>

static const char *greetings[] = { "Hola", "Hello", "Ola", "Salut" };

int main(int argc, char **argv)
{
    printf("%s from an Osito-K app on the ROG Phone 5!\n", greetings[0]);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    for (unsigned i = 1; i < sizeof(greetings) / sizeof(greetings[0]); i++)
        printf("  %-6s|%5d|%-5x|%08.3f|%+.2e|\n", greetings[i], (int)i * 42, i * 255,
               3.14159 * i, 12345.678 / i);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("up %ld.%03ld s since the phone booted\n", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    return 0;
}
