/* Concurrent hive writes; run again with "verify" after a cold VM reset. */
typedef unsigned int DWORD;
typedef int LONG;
typedef void *HANDLE;
#ifdef _WIN64
typedef unsigned long long UINT_PTR;
#else
typedef unsigned int UINT_PTR;
#endif
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define WORKERS 4
#define ROUNDS 8
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API char *CALL GetCommandLineA(void);
API HANDLE CALL CreateThread(void *, UINT_PTR, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API int CALL CloseHandle(HANDLE);
API void CALL Sleep(DWORD);
API void CALL ExitProcess(DWORD);
typedef LONG (CALL *create_fn)(HANDLE, const char *, DWORD, char *, DWORD, DWORD, void *, HANDLE *, DWORD *);
typedef LONG (CALL *set_fn)(HANDLE, const char *, DWORD, DWORD, const void *, DWORD);
typedef LONG (CALL *query_fn)(HANDLE, const char *, DWORD *, DWORD *, void *, DWORD *);
typedef LONG (CALL *key_fn)(HANDLE);
static create_fn create_key;
static set_fn set_value;
static query_fn query_value;
static key_fn flush_key, close_key;
static volatile DWORD start, ready[WORKERS], errors[WORKERS];
static const char path[] = "Software\\OsitoTests\\RegistryConcurrent";

static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}

static HANDLE open_key(void)
{
    HANDLE key = 0;
    DWORD disposition;
    if (create_key((HANDLE)(UINT_PTR)(long long)-2147483647, path, 0, 0,
                   0, 0x2001f, 0, &key, &disposition)) return 0;
    return key;
}

static DWORD CALL worker(void *arg)
{
    unsigned id = (unsigned)(UINT_PTR)arg;
    __atomic_store_n(&ready[id], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE)) Sleep(1);
    HANDLE key = open_key();
    if (!key) { errors[id] = 1; return 1; }
    char name[] = "worker0";
    name[6] += id;
    for (DWORD i = 1; i <= ROUNDS; i++) {
        DWORD value = (id + 1) * 1000 + i;
        if (set_value(key, name, 0, 4, &value, sizeof(value)) || flush_key(key)) {
            errors[id] = 2;
            break;
        }
        Sleep(1);
    }
    if (close_key(key)) errors[id] = 3;
    return errors[id];
}

void mainCRTStartup(void)
{
    void *module = LoadLibraryA("advapi32.dll");
    create_key = GetProcAddress(module, "RegCreateKeyExA");
    set_value = GetProcAddress(module, "RegSetValueExA");
    query_value = GetProcAddress(module, "RegQueryValueExA");
    flush_key = GetProcAddress(module, "RegFlushKey");
    close_key = GetProcAddress(module, "RegCloseKey");
    if (!create_key || !set_value || !query_value || !flush_key || !close_key) {
        report("REGISTRY-CONCURRENT MISSING EXPORT\n");
        ExitProcess(2);
    }
    const char *cmd = GetCommandLineA();
    int verify = 0;
    for (; *cmd; cmd++) {
        if (cmd[0] == 'v' && cmd[1] == 'e' && cmd[2] == 'r' &&
            cmd[3] == 'i' && cmd[4] == 'f' && cmd[5] == 'y') verify = 1;
    }
    unsigned failed = 0;
    if (!verify) {
        HANDLE threads[WORKERS];
        for (unsigned id = 0; id < WORKERS; id++) {
            threads[id] = CreateThread(0, 0, worker, (void *)(UINT_PTR)id, 0, 0);
            if (!threads[id]) {
                report("REGISTRY-CONCURRENT THREAD FAIL\n");
                ExitProcess(3);
            }
        }
        for (unsigned id = 0; id < WORKERS; id++) {
            unsigned tries = 0;
            while (!__atomic_load_n(&ready[id], __ATOMIC_ACQUIRE) && tries++ < 5000) Sleep(1);
            if (!ready[id]) {
                report("REGISTRY-CONCURRENT BARRIER FAIL\n");
                ExitProcess(4);
            }
        }
        report("REGISTRY-CONCURRENT BEGIN\n");
        __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
        for (unsigned id = 0; id < WORKERS; id++) {
            if (WaitForSingleObject(threads[id], 60000) || errors[id]) failed++;
            CloseHandle(threads[id]);
        }
    }
    HANDLE key = open_key();
    if (!key) failed++;
    else {
        for (unsigned id = 0; id < WORKERS; id++) {
            char name[] = "worker0";
            name[6] += id;
            DWORD value = 0, type = 0, size = sizeof(value);
            if (query_value(key, name, 0, &type, &value, &size) || type != 4 ||
                size != sizeof(value) || value != (id + 1) * 1000 + ROUNDS) failed++;
        }
        close_key(key);
    }
    report(failed ? "REGISTRY-CONCURRENT FAIL\n" :
           verify ? "REGISTRY-CONCURRENT PERSISTED\n" : "REGISTRY-CONCURRENT PASS\n");
    ExitProcess(failed ? 1 : 0);
}
