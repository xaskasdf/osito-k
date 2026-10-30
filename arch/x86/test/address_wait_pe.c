#include <stdint.h>
#include <stddef.h>
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long,
                                           unsigned long *, void *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) unsigned long __stdcall GetTickCount(void);
__declspec(dllimport) void *__stdcall CreateThread(void *, size_t,
                      unsigned long (__stdcall *)(void *), void *, unsigned long, unsigned long *);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) int __stdcall TerminateThread(void *, unsigned long);
__declspec(dllimport) void __stdcall Sleep(unsigned long);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);
typedef int (__stdcall *wait_fn)(volatile void *, void *, size_t, unsigned long);
typedef void (__stdcall *wake_fn)(void *);
static wait_fn wait_address;
static wake_fn wake_one, wake_all;
static volatile unsigned word, ready, failures;
#define CHECK(x) do { if (!(x)) __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); } while (0)
#define STAGE(s) do { unsigned long n; WriteFile(GetStdHandle((unsigned long)-11), s "\r\n", sizeof(s "\r\n") - 1, &n, 0); } while (0)

static unsigned long __stdcall address_wait_worker(void *mode)
{
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    unsigned zero = 0;
    unsigned rounds = mode ? 1 : 256;
    for (unsigned i = 0; i < rounds; ++i) {
        while (__atomic_load_n(&word, __ATOMIC_ACQUIRE) == 0) {
            if (!wait_address(&word, &zero, sizeof(word), 5000)) {
                CHECK(0); return 1;
            }
        }
        if (!mode) {
            __atomic_store_n(&word, 0, __ATOMIC_RELEASE);
            wake_one((void *)&word);
        }
    }
    return 0;
}

void address_wait_entry(void)
{
    void *dll = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    wait_address = (wait_fn)GetProcAddress(dll, "WaitOnAddress");
    wake_one = (wake_fn)GetProcAddress(dll, "WakeByAddressSingle");
    wake_all = (wake_fn)GetProcAddress(dll, "WakeByAddressAll");
    CHECK(wait_address && wake_one && wake_all);
    STAGE("ADDRESS WAIT imports");
    if (!failures) {
        uint64_t a = 8, b = 9;
        for (unsigned size = 1; size <= 8; size *= 2) {
            CHECK(wait_address(&a, &b, size, 0));
            CHECK(!wait_address(&a, &a, size, 0) && GetLastError() == 1460);
        }
        CHECK(!wait_address(&a, &b, 3, 0) && GetLastError() == 87);
        unsigned long before = GetTickCount();
        CHECK(!wait_address(&a, &a, 8, 35) && GetLastError() == 1460);
        CHECK(GetTickCount() - before >= 20);
        STAGE("ADDRESS WAIT timeout");
        unsigned long tid;
        void *thread = CreateThread(0, 0, address_wait_worker, 0, 0, &tid);
        CHECK(thread);
        if (thread) {
            unsigned one = 1;
            for (unsigned i = 0; i < 256 && !failures; ++i) {
                __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
                wake_one((void *)&word);
                while (__atomic_load_n(&word, __ATOMIC_ACQUIRE) == 1) {
                    if (!wait_address(&word, &one, sizeof(word), 5000)) {
                        CHECK(0); break;
                    }
                }
            }
            if (WaitForSingleObject(thread, 6000)) { CHECK(0); TerminateThread(thread, 1); }
            CloseHandle(thread);
        }
        STAGE("ADDRESS WAIT handshake");
        /* Multiple blocked threads, then wake-all. */
        word = ready = 0;
        void *threads[3];
        for (unsigned i = 0; i < 3; ++i) {
            threads[i] = CreateThread(0, 0, address_wait_worker, (void *)1, 0, &tid);
            CHECK(threads[i]);
        }
        before = GetTickCount();
        while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < 3 && GetTickCount() - before < 3000) Sleep(1);
        CHECK(ready == 3);
        __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
        wake_all((void *)&word);
        for (unsigned i = 0; i < 3; ++i) if (threads[i]) {
            if (WaitForSingleObject(threads[i], 6000)) { CHECK(0); TerminateThread(threads[i], 1); }
            CloseHandle(threads[i]);
        }
        STAGE("ADDRESS WAIT broadcast");
#ifdef ADDRESS_WAIT_NATIVE_CANCEL
        /* Osito's scheduler owns the wait record and must remove it on task
         * teardown. This is a native lifetime test, not a Windows contract:
         * forcibly killing a thread inside Windows' user-mode wait machinery
         * leaves its synchronization structures in an undefined state. */
        word = ready = 0;
        thread = CreateThread(0, 0, address_wait_worker, (void *)1, 0, &tid);
        CHECK(thread);
        if (thread) {
            before = GetTickCount();
            while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) && GetTickCount() - before < 3000) Sleep(1);
            Sleep(50);
            CHECK(TerminateThread(thread, 0));
            CHECK(!WaitForSingleObject(thread, 3000));
            CloseHandle(thread);
            wake_all((void *)&word);
        }
#endif
    }
    const char *text = failures ? "ADDRESS WAIT FAIL\r\n" : "ADDRESS WAIT PASS\r\n";
    unsigned long written;
    WriteFile(GetStdHandle((unsigned long)-11), text, 19, &written, 0);
    ExitProcess(failures ? 1 : 0);
}
