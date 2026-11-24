/* primes.c -- a sieve in malloc'd memory, timed: `exec primes.elf [limit]` */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    long limit = argc > 1 ? atol(argv[1]) : 10000000;
    if (limit < 2) {
        fprintf(stderr, "usage: primes [limit >= 2]\n");
        return 1;
    }
    clock_t t0 = clock();
    char *composite = malloc((size_t)limit + 1);
    if (!composite) {
        perror("malloc");
        return 1;
    }
    memset(composite, 0, (size_t)limit + 1);
    long count = 0, last = 0;
    for (long i = 2; i <= limit; i++) {
        if (composite[i])
            continue;
        count++;
        last = i;
        for (long j = i * i; j <= limit; j += i)
            composite[j] = 1;
    }
    free(composite);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%ld primes up to %ld, the last %ld, in %.3f s\n", count, limit, last, secs);
    return 0;
}
