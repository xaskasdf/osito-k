/* stdio.h -- Osito-K AArch64 libc: console and read-only files */
#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define EOF         (-1)
#define BUFSIZ      1024
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

typedef struct _FILE FILE;
extern FILE *stdin, *stdout, *stderr;

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

int putchar(int c);
int puts(const char *s);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fputs(const char *s, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fflush(FILE *f);

int getchar(void);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *s, int n, FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);

FILE *fopen(const char *path, const char *mode);   /* "r" / "rb" only */
int fclose(FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);

void perror(const char *s);

#endif
