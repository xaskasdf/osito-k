/* cat.c -- files from the phone's OsitoFS: `exec cat.elf hola.txt [...]` */
#include <stdio.h>

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            status = 1;
            continue;
        }
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
    }
    if (argc < 2)
        fprintf(stderr, "usage: cat <file>...\n");
    return status;
}
