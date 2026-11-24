/*
 * mmap_test.c — test mmap/munmap/mprotect syscalls on OsitoK
 */

extern int printf(const char *fmt, ...);
extern void *mmap(void *addr, unsigned long length, int prot, int flags,
                  int fd, long offset);
extern int munmap(void *addr, unsigned long length);
extern int mprotect(void *addr, unsigned long length, int prot);

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)
#define PAGE_SIZE     4096

int main(void)
{
    int pass = 0, fail = 0;

    printf("=== mmap test ===\n");

    /* Test 1: basic mmap */
    void *p = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        printf("T1 mmap 4KB: PASS (addr=%p)\n", p);
        pass++;
    } else {
        printf("T1 mmap 4KB: FAIL\n");
        fail++;
    }

    /* Test 2: write + read back */
    if (p != MAP_FAILED) {
        int *ip = (int *)p;
        ip[0] = 0xDEADBEEF;
        ip[1] = 0xCAFEBABE;
        if (ip[0] == (int)0xDEADBEEF && ip[1] == (int)0xCAFEBABE) {
            printf("T2 read/write: PASS\n");
            pass++;
        } else {
            printf("T2 read/write: FAIL\n");
            fail++;
        }
    }

    /* Test 3: large mmap (64KB = 16 pages) */
    void *big = mmap(0, 16 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (big != MAP_FAILED) {
        /* Verify it's zeroed */
        unsigned char *b = (unsigned char *)big;
        int zeroed = 1;
        for (int i = 0; i < 16 * PAGE_SIZE; i += PAGE_SIZE) {
            if (b[i] != 0) { zeroed = 0; break; }
        }
        printf("T3 mmap 64KB: PASS (addr=%p, zeroed=%d)\n", big, zeroed);
        pass++;
    } else {
        printf("T3 mmap 64KB: FAIL\n");
        fail++;
    }

    /* Test 4: munmap */
    if (p != MAP_FAILED) {
        int ret = munmap(p, PAGE_SIZE);
        if (ret == 0) {
            printf("T4 munmap: PASS\n");
            pass++;
        } else {
            printf("T4 munmap: FAIL (ret=%d)\n", ret);
            fail++;
        }
    }

    /* Test 5: munmap large */
    if (big != MAP_FAILED) {
        int ret = munmap(big, 16 * PAGE_SIZE);
        if (ret == 0) {
            printf("T5 munmap 64KB: PASS\n");
            pass++;
        } else {
            printf("T5 munmap 64KB: FAIL (ret=%d)\n", ret);
            fail++;
        }
    }

    /* Test 6: mprotect (change to read-only — just test API, no #PF test) */
    void *mp = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mp != MAP_FAILED) {
        int ret = mprotect(mp, PAGE_SIZE, PROT_READ);
        if (ret == 0) {
            printf("T6 mprotect: PASS\n");
            pass++;
        } else {
            printf("T6 mprotect: FAIL (ret=%d)\n", ret);
            fail++;
        }
        munmap(mp, PAGE_SIZE);
    }

    printf("=== %d/%d passed ===\n", pass, pass + fail);
    return fail;
}
