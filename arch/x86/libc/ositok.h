/*
 * ositok.h — Single header for OsitoK userspace programs.
 *
 * Provides standard C library functions (printf, malloc, file I/O, etc.)
 * backed by crt.o + syscall.o + tcclib.o.
 *
 * Usage inside OsitoK:
 *   cc myapp.c              → myapp.elf (auto-links with CRT+libc)
 *   cc -run myapp.c         → compile + run
 */

#ifndef OSITOK_H
#define OSITOK_H

/* ── Basic types ─────────────────────────────────────────────── */

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef long           off_t;
typedef int            pid_t;
typedef unsigned int   mode_t;
typedef long           time_t;
typedef long           clock_t;
typedef long           intmax_t;
typedef unsigned long  uintmax_t;

#define NULL ((void *)0)
#define EOF  (-1)

/* ── stdarg (built into TCC) ─────────────────────────────────── */

typedef __builtin_va_list va_list;
#define va_start(v,l)  __builtin_va_start(v,l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v,l)    __builtin_va_arg(v,l)

/* ── stdio ───────────────────────────────────────────────────── */

typedef struct _FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int     printf(const char *fmt, ...);
int     fprintf(FILE *f, const char *fmt, ...);
int     sprintf(char *buf, const char *fmt, ...);
int     snprintf(char *buf, size_t n, const char *fmt, ...);
int     vfprintf(FILE *f, const char *fmt, va_list ap);
int     vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int     puts(const char *s);
int     putchar(int c);
int     fputs(const char *s, FILE *f);
int     fputc(int c, FILE *f);
int     fgetc(FILE *f);
FILE   *fopen(const char *path, const char *mode);
FILE   *fdopen(int fd, const char *mode);
FILE   *freopen(const char *path, const char *mode, FILE *f);
int     fclose(FILE *f);
size_t  fread(void *buf, size_t size, size_t count, FILE *f);
size_t  fwrite(const void *buf, size_t size, size_t count, FILE *f);
int     fseek(FILE *f, long offset, int whence);
long    ftell(FILE *f);
int     fflush(FILE *f);
int     feof(FILE *f);
int     ferror(FILE *f);
void    clearerr(FILE *f);
int     fileno(FILE *f);
int     remove(const char *path);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ── stdlib ──────────────────────────────────────────────────── */

void   *malloc(size_t size);
void    free(void *ptr);
void   *calloc(size_t count, size_t size);
void   *realloc(void *ptr, size_t size);
void    exit(int code);
void    _exit(int code);
void    abort(void);
int     atoi(const char *s);
long    atol(const char *s);
long    strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double  strtod(const char *s, char **end);
float   strtof(const char *s, char **end);
intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
int     abs(int x);
long    labs(long x);
void    qsort(void *base, size_t count, size_t size,
              int (*cmp)(const void *, const void *));
void   *bsearch(const void *key, const void *base, size_t count,
                size_t size, int (*cmp)(const void *, const void *));
char   *getenv(const char *name);
char   *realpath(const char *path, char *resolved);

/* ── string ──────────────────────────────────────────────────── */

size_t  strlen(const char *s);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *haystack, const char *needle);
char   *strpbrk(const char *s, const char *accept);
size_t  strspn(const char *s, const char *accept);
size_t  strcspn(const char *s, const char *reject);
char   *strdup(const char *s);
char   *strerror(int errnum);
void   *memset(void *s, int c, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

/* ── ctype ───────────────────────────────────────────────────── */

int     isalpha(int c);
int     isdigit(int c);
int     isalnum(int c);
int     isspace(int c);
int     isupper(int c);
int     islower(int c);
int     isprint(int c);
int     isxdigit(int c);
int     tolower(int c);
int     toupper(int c);

/* ── POSIX I/O ───────────────────────────────────────────────── */

int     open(const char *path, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);
int     stat(const char *path, void *buf);
int     fstat(int fd, void *buf);
int     lstat(const char *path, void *buf);
int     access(const char *path, int mode);
int     chdir(const char *path);
int     umask(int mask);
int     ioctl(int fd, unsigned long request, void *arg);

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int     poll(struct pollfd *fds, unsigned long nfds, int timeout);

int     fcntl(int fd, int cmd, ...);
long    getdents64(int fd, void *dirp, size_t count);
int     unlink(const char *path);
int     pipe(int pipefd[2]);
int     dup2(int oldfd, int newfd);
char   *getcwd(char *buf, size_t size);
pid_t   getpid(void);

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* ── mmap ────────────────────────────────────────────────────── */

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)

void   *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int     munmap(void *addr, size_t len);
int     mprotect(void *addr, size_t len, int prot);

/* ── signals ─────────────────────────────────────────────────── */

#define SIGINT    2
#define SIGTERM  15
#define SIGKILL   9
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

int     kill(pid_t pid, int sig);

/* ── setjmp ──────────────────────────────────────────────────── */

typedef long jmp_buf[8];

int     setjmp(jmp_buf env);
int     _setjmp(jmp_buf env);
void    longjmp(jmp_buf env, int val);

/* ── time ────────────────────────────────────────────────────── */

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t  time(time_t *t);
clock_t clock(void);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *result);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *result);
time_t  mktime(struct tm *tm);
size_t  strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

/* ── errno ───────────────────────────────────────────────────── */

int *__errno_location(void);
#define errno (*__errno_location())

#define ENOENT  2
#define EBADF   9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define EPIPE  32
#define ENOSYS 38
#define EAGAIN 11

#endif /* OSITOK_H */
