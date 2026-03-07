/*
 * fileio.c — Test program for X-OS10 file I/O syscalls
 *
 * Tests: open, read, lseek, fstat, close, brk, writev
 *
 * Build:
 *   gcc -nostdlib -static -no-pie -o fileio.elf fileio.c
 */

/* Inline syscall wrappers */
static long syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall2(long nr, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_FSTAT  5
#define SYS_LSEEK  8
#define SYS_BRK    12
#define SYS_EXIT   60

static long my_write(int fd, const void *buf, long count) {
    return syscall3(SYS_WRITE, fd, (long)buf, count);
}

static long my_read(int fd, void *buf, long count) {
    return syscall3(SYS_READ, fd, (long)buf, count);
}

static long my_open(const char *path, int flags) {
    return syscall3(SYS_OPEN, (long)path, flags, 0);
}

static long my_close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static long my_lseek(int fd, long offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
}

static long my_fstat(int fd, void *statbuf) {
    return syscall2(SYS_FSTAT, fd, (long)statbuf);
}

static long my_brk(long addr) {
    return syscall1(SYS_BRK, addr);
}

static void puts_fd(int fd, const char *s) {
    int len = 0;
    while (s[len]) len++;
    my_write(fd, s, len);
}

static void put_num(int fd, long val) {
    char buf[20];
    int i = 0;
    if (val < 0) { my_write(fd, "-", 1); val = -val; }
    if (val == 0) { my_write(fd, "0", 1); return; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    /* reverse */
    for (int j = i - 1; j >= 0; j--)
        my_write(fd, &buf[j], 1);
}

static void put_hex(int fd, unsigned long val) {
    char hex[] = "0123456789abcdef";
    char buf[16];
    int i = 0;
    if (val == 0) { puts_fd(fd, "0"); return; }
    while (val > 0) { buf[i++] = hex[val & 0xF]; val >>= 4; }
    for (int j = i - 1; j >= 0; j--)
        my_write(fd, &buf[j], 1);
}

/* stat struct (match kernel's linux_stat_t) */
typedef struct {
    unsigned long st_dev, st_ino, st_nlink;
    unsigned int st_mode, st_uid, st_gid, _pad0;
    unsigned long st_rdev;
    long st_size, st_blksize, st_blocks;
    unsigned long st_atime_sec, st_atime_nsec;
    unsigned long st_mtime_sec, st_mtime_nsec;
    unsigned long st_ctime_sec, st_ctime_nsec;
    long _unused[3];
} stat_t;

void _start(void) {
    int pass = 0, fail = 0;

    puts_fd(1, "=== X-OS10 File I/O Test ===\n");

    /* Test 1: open hello.elf */
    puts_fd(1, "T1 open(hello.elf): ");
    long fd = my_open("hello.elf", 0 /* O_RDONLY */);
    if (fd >= 0) {
        puts_fd(1, "OK fd=");
        put_num(1, fd);
        puts_fd(1, "\n");
        pass++;
    } else {
        puts_fd(1, "FAIL err=");
        put_num(1, fd);
        puts_fd(1, "\n");
        fail++;
    }

    /* Test 2: read first 4 bytes (ELF magic) */
    if (fd >= 0) {
        puts_fd(1, "T2 read(4 bytes): ");
        char buf[4] = {0};
        long n = my_read((int)fd, buf, 4);
        if (n == 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
            puts_fd(1, "OK (ELF magic)\n");
            pass++;
        } else {
            puts_fd(1, "FAIL n=");
            put_num(1, n);
            puts_fd(1, " data=");
            put_hex(1, (unsigned char)buf[0]);
            puts_fd(1, " ");
            put_hex(1, (unsigned char)buf[1]);
            puts_fd(1, "\n");
            fail++;
        }

        /* Test 3: lseek SEEK_SET to 0 */
        puts_fd(1, "T3 lseek(0, SET): ");
        long off = my_lseek((int)fd, 0, 0 /* SEEK_SET */);
        if (off == 0) {
            puts_fd(1, "OK\n");
            pass++;
        } else {
            puts_fd(1, "FAIL off=");
            put_num(1, off);
            puts_fd(1, "\n");
            fail++;
        }

        /* Test 4: lseek SEEK_END */
        puts_fd(1, "T4 lseek(0, END): ");
        off = my_lseek((int)fd, 0, 2 /* SEEK_END */);
        if (off > 0) {
            puts_fd(1, "OK size=");
            put_num(1, off);
            puts_fd(1, "\n");
            pass++;
        } else {
            puts_fd(1, "FAIL off=");
            put_num(1, off);
            puts_fd(1, "\n");
            fail++;
        }

        /* Test 5: fstat */
        puts_fd(1, "T5 fstat: ");
        stat_t st;
        long ret = my_fstat((int)fd, &st);
        if (ret == 0 && st.st_size > 0) {
            puts_fd(1, "OK size=");
            put_num(1, st.st_size);
            puts_fd(1, " mode=0");
            put_hex(1, st.st_mode);
            puts_fd(1, "\n");
            pass++;
        } else {
            puts_fd(1, "FAIL ret=");
            put_num(1, ret);
            puts_fd(1, "\n");
            fail++;
        }

        /* Test 6: close */
        puts_fd(1, "T6 close: ");
        ret = my_close((int)fd);
        if (ret == 0) {
            puts_fd(1, "OK\n");
            pass++;
        } else {
            puts_fd(1, "FAIL ret=");
            put_num(1, ret);
            puts_fd(1, "\n");
            fail++;
        }

        /* Test 7: read after close should fail */
        puts_fd(1, "T7 read-after-close: ");
        char tmp[1];
        n = my_read((int)fd, tmp, 1);
        if (n < 0) {
            puts_fd(1, "OK (EBADF)\n");
            pass++;
        } else {
            puts_fd(1, "FAIL n=");
            put_num(1, n);
            puts_fd(1, "\n");
            fail++;
        }
    }

    /* Test 8: open nonexistent file */
    puts_fd(1, "T8 open(nofile): ");
    long fd2 = my_open("nofile.txt", 0);
    if (fd2 < 0) {
        puts_fd(1, "OK (ENOENT)\n");
        pass++;
    } else {
        puts_fd(1, "FAIL fd=");
        put_num(1, fd2);
        puts_fd(1, "\n");
        fail++;
    }

    /* Test 9: brk */
    puts_fd(1, "T9 brk(0): ");
    long brk = my_brk(0);
    if (brk > 0) {
        puts_fd(1, "OK base=0x");
        put_hex(1, (unsigned long)brk);
        puts_fd(1, "\n");
        pass++;

        /* brk grow */
        puts_fd(1, "T10 brk(+4096): ");
        long new_brk = my_brk(brk + 4096);
        if (new_brk == brk + 4096) {
            puts_fd(1, "OK new=0x");
            put_hex(1, (unsigned long)new_brk);
            puts_fd(1, "\n");
            pass++;

            /* Write to brk memory */
            char *p = (char *)brk;
            p[0] = 'X';
            p[4095] = 'Y';
            if (p[0] == 'X' && p[4095] == 'Y') {
                puts_fd(1, "T11 brk-rw: OK\n");
                pass++;
            } else {
                puts_fd(1, "T11 brk-rw: FAIL\n");
                fail++;
            }
        } else {
            puts_fd(1, "FAIL new_brk=0x");
            put_hex(1, (unsigned long)new_brk);
            puts_fd(1, "\n");
            fail++;
        }
    } else {
        puts_fd(1, "FAIL brk=");
        put_num(1, brk);
        puts_fd(1, "\n");
        fail++;
    }

    /* Summary */
    puts_fd(1, "\n=== Results: ");
    put_num(1, pass);
    puts_fd(1, " pass, ");
    put_num(1, fail);
    puts_fd(1, " fail ===\n");

    /* Exit with fail count */
    syscall1(SYS_EXIT, fail);
}
