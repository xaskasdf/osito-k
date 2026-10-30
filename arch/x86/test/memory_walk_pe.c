/* Exercise protection/recommit with many unrelated reservations alive. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void *CALL VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
API BOOL CALL VirtualFree(void *, SIZE_T, DWORD);
API BOOL CALL VirtualProtect(void *, SIZE_T, DWORD, DWORD *);

#define REPORT(text) WriteFile(GetStdHandle((DWORD)-11), text, sizeof(text)-1, &written, 0)
#define CHECK(condition, code) do { if (!(condition)) { \
    REPORT("MEMORY-WALK FAIL\r\n"); ExitProcess(code); } } while (0)

void memory_walk_entry(void)
{
    static void *reservations[3072];
    const SIZE_T bytes = 8 * 1024 * 1024;
    DWORD written, previous;
    for (DWORD i = 0; i < 3072; i++) {
        reservations[i] = VirtualAlloc(0, 65536, 0x2000, 4);
        CHECK(reservations[i], 1);
    }
    volatile DWORD *block = VirtualAlloc(0, bytes, 0x2000, 4);
    CHECK(block, 2);
    CHECK(VirtualAlloc((void *)block, bytes, 0x1000, 4) == block, 3);
    for (SIZE_T offset = 0; offset < bytes / sizeof(DWORD); offset += 1024)
        block[offset] = (DWORD)offset ^ 0x15A6CD23;
    REPORT("MEMORY-WALK READY\r\n");

    for (DWORD round = 0; round < 4; round++) {
        unsigned int lo, hi;
        __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
        unsigned long long started = ((unsigned long long)hi << 32) | lo;
        CHECK(VirtualProtect((void *)block, bytes, 2, &previous) && previous == 4, 4);
        CHECK(VirtualAlloc((void *)block, bytes, 0x1000, 4) == block, 5);
        for (SIZE_T offset = 0; offset < bytes / sizeof(DWORD); offset += 1024) {
            CHECK(block[offset] == ((DWORD)offset ^ 0x15A6CD23), 6);
            block[offset] = (DWORD)offset ^ 0x15A6CD23;
        }
        CHECK(VirtualProtect((void *)(block + 1024), 4096, 1, &previous) && previous == 4, 7);
        CHECK(VirtualProtect((void *)(block + 1024), 4096, 4, &previous) && previous == 1, 8);
        CHECK(block[1024] == (1024 ^ 0x15A6CD23), 9);
        __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
        unsigned long long elapsed = (((unsigned long long)hi << 32) | lo) - started;
        static char timing[] = "MEMORY-WALK CYCLES 0x0000000000000000\r\n";
        for (DWORD i = 0; i < 16; i++)
            timing[sizeof("MEMORY-WALK CYCLES 0x") - 1 + i] =
                "0123456789ABCDEF"[(elapsed >> (60 - i * 4)) & 15];
        WriteFile(GetStdHandle((DWORD)-11), timing, sizeof(timing)-1, &written, 0);
        REPORT("MEMORY-WALK ROUND PASS\r\n");
    }
    CHECK(VirtualFree((void *)block, 0, 0x8000), 10);
    for (DWORD i = 0; i < 3072; i++)
        CHECK(VirtualFree(reservations[i], 0, 0x8000), 11);
    REPORT("MEMORY-WALK PASS\r\n");
    ExitProcess(0);
}
