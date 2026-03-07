/*
 * hello_c.c — First C program compiled with TCC for OsitoK.
 * Uses our minimal crt.c (printf, malloc, file I/O).
 */

int printf(const char *fmt, ...);
void *malloc(unsigned long size);
void free(void *ptr);
int puts(const char *s);
int open(const char *path, int flags, ...);
int close(int fd);
long read(int fd, void *buf, unsigned long count);

int main(int argc, char **argv)
{
    printf("Hello from TCC on OsitoK!\n");
    printf("argc = %d\n", argc);

    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    /* Test malloc */
    char *buf = (char *)malloc(64);
    if (buf) {
        buf[0] = 'O'; buf[1] = 'K'; buf[2] = '\n'; buf[3] = 0;
        printf("malloc test: %s", buf);
        free(buf);
    }

    /* Test file I/O */
    int fd = open("hello.elf", 0, 0);
    if (fd >= 0) {
        char header[4];
        long n = read(fd, header, 4);
        if (n == 4 && header[0] == 0x7f && header[1] == 'E')
            printf("file I/O test: OK (read ELF magic)\n");
        else
            printf("file I/O test: FAIL (read %ld bytes)\n", n);
        close(fd);
    } else {
        printf("file I/O test: SKIP (no hello.elf)\n");
    }

    printf("=== TCC test passed ===\n");
    return 0;
}
