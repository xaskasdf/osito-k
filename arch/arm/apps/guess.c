/* guess.c -- console input: guess a number from 1 to 100 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    srand((unsigned)clock());
    int secret = rand() % 100 + 1, tries = 0;
    char line[32];
    printf("I picked a number from 1 to 100.\n");
    for (;;) {
        printf("Your guess: ");
        if (!fgets(line, sizeof(line), stdin))
            return 1;
        int g = atoi(line);
        tries++;
        if (g < secret)
            printf("Higher.\n");
        else if (g > secret)
            printf("Lower.\n");
        else
            break;
    }
    printf("Yes, %d, in %d tries.\n", secret, tries);
    return 0;
}
