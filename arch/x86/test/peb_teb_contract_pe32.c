/* PE32 contract test for the i386 TEB, PEB, and dynamic TLS layout. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef long NTSTATUS;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)
#define FALSE 0
#define TRUE 1

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_TLS_INDEX ((DWORD)-1)
#define WAIT_OBJECT_0 0U
#define INFINITE 0xFFFFFFFFU
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER_1 ((NTSTATUS)0xC00000EFU)
#define STATUS_INVALID_PARAMETER_3 ((NTSTATUS)0xC00000F1U)

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT DWORD WINAPI GetLastError(void);
DLLIMPORT void WINAPI SetLastError(DWORD error);
DLLIMPORT HANDLE WINAPI GetProcessHeap(void);
DLLIMPORT DWORD WINAPI TlsAlloc(void);
DLLIMPORT BOOL WINAPI TlsFree(DWORD index);
DLLIMPORT PVOID WINAPI TlsGetValue(DWORD index);
DLLIMPORT BOOL WINAPI TlsSetValue(DWORD index, PVOID value);
DLLIMPORT HANDLE WINAPI CreateThread(PVOID attributes, DWORD stack_size,
                                     DWORD (WINAPI *start)(PVOID),
                                     PVOID parameter, DWORD flags,
                                     DWORD *thread_id);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT HANDLE WINAPI LoadLibraryA(const char *name);
DLLIMPORT BOOL WINAPI FreeLibrary(HANDLE module);
DLLIMPORT DWORD WINAPI GetCurrentThreadId(void);
DLLIMPORT void WINAPI EnterCriticalSection(PVOID section);
DLLIMPORT BOOL WINAPI TryEnterCriticalSection(PVOID section);
DLLIMPORT void WINAPI LeaveCriticalSection(PVOID section);
DLLIMPORT NTSTATUS WINAPI LdrLockLoaderLock(DWORD flags, DWORD *disposition,
                                            DWORD *cookie);
DLLIMPORT NTSTATUS WINAPI LdrUnlockLoaderLock(DWORD flags, DWORD cookie);
DLLIMPORT void WINAPI RtlAcquirePebLock(void);
DLLIMPORT BOOL WINAPI RtlTryAcquirePebLock(void);
DLLIMPORT void WINAPI RtlReleasePebLock(void);

typedef struct __attribute__((packed)) {
    DWORD Flink;
    DWORD Blink;
} LIST_ENTRY32;

typedef struct __attribute__((packed)) {
    DWORD Length;
    BYTE Initialized;
    BYTE Reserved[3];
    DWORD SsHandle;
    LIST_ENTRY32 InLoadOrderModuleList;
    LIST_ENTRY32 InMemoryOrderModuleList;
    LIST_ENTRY32 InInitializationOrderModuleList;
    DWORD EntryInProgress;
    BYTE ShutdownInProgress;
    BYTE Reserved2[3];
    DWORD ShutdownThreadId;
} PEB_LDR_DATA32;

typedef struct __attribute__((packed)) {
    WORD Length;
    WORD MaximumLength;
    DWORD Buffer;
} UNICODE_STRING32;

typedef struct __attribute__((packed)) {
    DWORD DebugInfo;
    int LockCount;
    int RecursionCount;
    DWORD OwningThread;
    DWORD LockSemaphore;
    DWORD SpinCount;
} RTL_CRITICAL_SECTION32;

typedef struct __attribute__((packed)) {
    LIST_ENTRY32 InLoadOrderLinks;
    LIST_ENTRY32 InMemoryOrderLinks;
    LIST_ENTRY32 InInitializationOrderLinks;
    DWORD DllBase;
    DWORD EntryPoint;
    DWORD SizeOfImage;
    UNICODE_STRING32 FullDllName;
    UNICODE_STRING32 BaseDllName;
    DWORD Flags;
    WORD LoadCount;
    WORD TlsIndex;
} LDR_DATA_TABLE_ENTRY32;

static volatile DWORD low_index = INVALID_TLS_INDEX;
static volatile DWORD high_index = INVALID_TLS_INDEX;
static volatile DWORD main_teb;
static volatile DWORD main_peb;
static volatile DWORD thread_result = 0xFFFFFFFFU;

static DWORD text_length(const char *text)
{
    DWORD length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text),
              &written, (PVOID)0);
}

static void fail(UINT code)
{
    char message[] = "PEB32 TEST FAIL 00\r\n";
    message[16] = (char)('0' + (code / 10));
    message[17] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static DWORD read_teb(void)
{
    DWORD value;
    __asm__ volatile ("movl %%fs:0x18, %0" : "=r"(value));
    return value;
}

static DWORD read_peb(void)
{
    DWORD value;
    __asm__ volatile ("movl %%fs:0x30, %0" : "=r"(value));
    return value;
}

static DWORD read_last_error(void)
{
    DWORD value;
    __asm__ volatile ("movl %%fs:0x34, %0" : "=r"(value));
    return value;
}

static DWORD *teb_tls_slot(DWORD teb, DWORD index)
{
    if (index < 64)
        return (DWORD *)(teb + 0x0E10U + index * sizeof(DWORD));
    DWORD expansion = *(DWORD *)(teb + 0x0F94U);
    if (!expansion) return (DWORD *)0;
    return (DWORD *)(expansion + (index - 64) * sizeof(DWORD));
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + 32) : value;
}

static BOOL unicode_equals_ascii(const UNICODE_STRING32 *wide,
                                 const char *ascii)
{
    DWORD chars = wide->Length / sizeof(WORD);
    if (!wide->Buffer || (wide->Length & 1U) ||
        wide->MaximumLength < wide->Length + sizeof(WORD))
        return FALSE;
    DWORD length = text_length(ascii);
    if (chars != length) return FALSE;

    const WORD *buffer = (const WORD *)wide->Buffer;
    for (DWORD i = 0; i < chars; i++) {
        if (buffer[i] > 0x7FU ||
            ascii_lower((char)buffer[i]) != ascii_lower(ascii[i]))
            return FALSE;
    }
    return TRUE;
}

static int scan_loader_list(DWORD ldr_address, DWORD head_offset,
                            DWORD link_offset, const char *wanted_name,
                            DWORD wanted_base, DWORD *count,
                            DWORD *first_base, DWORD *found_entry)
{
    LIST_ENTRY32 *head = (LIST_ENTRY32 *)(ldr_address + head_offset);
    DWORD head_address = (DWORD)head;
    DWORD link = head->Flink;
    DWORD previous = head_address;
    DWORD seen = 0;
    int found = 0;

    while (link != head_address && seen < 2048U) {
        if (link < 0x10000U || link < link_offset) return -1;
        DWORD entry_address = link - link_offset;
        LDR_DATA_TABLE_ENTRY32 *entry =
            (LDR_DATA_TABLE_ENTRY32 *)entry_address;
        LIST_ENTRY32 *links = (LIST_ENTRY32 *)link;
        if (links->Blink != previous || !links->Flink ||
            !entry->DllBase || !entry->SizeOfImage)
            return -1;
        if (!seen && first_base) *first_base = entry->DllBase;
        if (wanted_name &&
            unicode_equals_ascii(&entry->BaseDllName, wanted_name) &&
            (!wanted_base || entry->DllBase == wanted_base)) {
            found = 1;
            if (found_entry) *found_entry = entry_address;
        }
        previous = link;
        link = links->Flink;
        seen++;
    }

    if (link != head_address || head->Blink != previous) return -1;
    if (count) *count = seen;
    return found;
}

static DWORD WINAPI test_thread(PVOID parameter)
{
    (void)parameter;
    DWORD teb = read_teb();
    DWORD peb = read_peb();
    DWORD *low_slot = teb_tls_slot(teb, low_index);
    DWORD *high_slot = teb_tls_slot(teb, high_index);

    if (!teb || teb == main_teb) return thread_result = 1;
    if (peb != main_peb) return thread_result = 2;
    if ((teb & 0xFFFFF000U) == (peb & 0xFFFFF000U))
        return thread_result = 3;
    if (!low_slot || !high_slot || *low_slot || *high_slot)
        return thread_result = 4;
    if (TlsGetValue(low_index) || TlsGetValue(high_index))
        return thread_result = 5;
    if (!TlsSetValue(low_index, (PVOID)0x34567000U) ||
        !TlsSetValue(high_index, (PVOID)0x45678000U))
        return thread_result = 6;
    if (*low_slot != 0x34567000U || *high_slot != 0x45678000U)
        return thread_result = 7;
    if (TlsGetValue(low_index) != (PVOID)0x34567000U ||
        TlsGetValue(high_index) != (PVOID)0x45678000U)
        return thread_result = 8;

    thread_result = 0;
    return 0;
}

void mainCRTStartup(void)
{
    DWORD indices[80];
    DWORD count = 0;
    main_teb = read_teb();
    main_peb = read_peb();

    if (!main_teb || !main_peb) fail(1);
    if (main_peb - main_teb != 0x1000U) fail(2);

    DWORD parameters = *(DWORD *)(main_peb + 0x10U);
    if (!parameters || parameters < main_peb + 0x1000U) fail(3);
    if (*(DWORD *)(main_peb + 0x18U) != (DWORD)GetProcessHeap()) fail(4);
    if (!*(DWORD *)(main_peb + 0x64U) ||
        *(DWORD *)(main_peb + 0x64U) > 32) fail(5);
    if (*(DWORD *)(main_peb + 0x88U) != 1 ||
        *(DWORD *)(main_peb + 0x8CU) != 1) fail(6);
    DWORD heaps = *(DWORD *)(main_peb + 0x90U);
    if (!heaps || *(DWORD *)heaps != *(DWORD *)(main_peb + 0x18U)) fail(7);
    if (*(DWORD *)(main_peb + 0xA4U) != 10 ||
        *(DWORD *)(main_peb + 0xA8U) != 0 ||
        *(WORD *)(main_peb + 0xACU) != 19045 ||
        *(DWORD *)(main_peb + 0xB0U) != 2) fail(8);
    if (*(DWORD *)(main_peb + 0xB4U) != 3) fail(9);
    if (*(DWORD *)(main_peb + 0xB8U) != 6 ||
        *(DWORD *)(main_peb + 0xBCU) != 0) fail(31);

    DWORD loader_lock_address = *(DWORD *)(main_peb + 0xA0U);
    if (loader_lock_address < 0x10000U) fail(46);
    RTL_CRITICAL_SECTION32 *loader_lock =
        (RTL_CRITICAL_SECTION32 *)loader_lock_address;
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread != 0)
        fail(47);

    DWORD peb_lock_address = *(DWORD *)(main_peb + 0x1CU);
    RTL_CRITICAL_SECTION32 *peb_lock =
        (RTL_CRITICAL_SECTION32 *)peb_lock_address;
    if (peb_lock_address < 0x10000U || peb_lock_address == loader_lock_address)
        fail(60);
    if (peb_lock->LockCount != -1 || peb_lock->RecursionCount != 0 ||
        peb_lock->OwningThread != 0)
        fail(61);
    RtlAcquirePebLock();
    if (peb_lock->LockCount != 0 || peb_lock->RecursionCount != 1 ||
        peb_lock->OwningThread != GetCurrentThreadId())
        fail(62);
    if (!RtlTryAcquirePebLock() || peb_lock->LockCount != 1 ||
        peb_lock->RecursionCount != 2)
        fail(63);
    RtlReleasePebLock();
    if (peb_lock->LockCount != 0 || peb_lock->RecursionCount != 1 ||
        peb_lock->OwningThread != GetCurrentThreadId())
        fail(64);
    RtlReleasePebLock();
    if (peb_lock->LockCount != -1 || peb_lock->RecursionCount != 0 ||
        peb_lock->OwningThread != 0)
        fail(65);

    EnterCriticalSection(loader_lock);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        loader_lock->OwningThread != GetCurrentThreadId())
        fail(48);
    if (!TryEnterCriticalSection(loader_lock) ||
        loader_lock->LockCount != 1 || loader_lock->RecursionCount != 2)
        fail(49);
    LeaveCriticalSection(loader_lock);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1)
        fail(50);

    DWORD loader = *(DWORD *)(main_peb + 0x0CU);
    if (loader < 0x10000U) fail(32);
    PEB_LDR_DATA32 *loader_data = (PEB_LDR_DATA32 *)loader;
    if (loader_data->Length != sizeof(*loader_data) ||
        !loader_data->Initialized)
        fail(33);

    DWORD load_count = 0;
    DWORD memory_count = 0;
    DWORD init_count = 0;
    DWORD first_base = 0;
    int kernel32_found = scan_loader_list(
        loader, 0x0CU, 0x00U, "kernel32.dll", 0,
        &load_count, &first_base, (DWORD *)0);
    if (kernel32_found < 0 ||
        scan_loader_list(loader, 0x14U, 0x08U, (const char *)0, 0,
                         &memory_count, (DWORD *)0, (DWORD *)0) < 0 ||
        scan_loader_list(loader, 0x1CU, 0x10U, (const char *)0, 0,
                         &init_count, (DWORD *)0, (DWORD *)0) < 0)
        fail(34);
    if (load_count < 2 || load_count != memory_count ||
        load_count != init_count)
        fail(35);
    if (first_base != *(DWORD *)(main_peb + 0x08U)) fail(36);
    if (!kernel32_found) fail(37);

    HANDLE version = LoadLibraryA("version.dll");
    if (!version) fail(38);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        loader_lock->OwningThread != GetCurrentThreadId())
        fail(51);
    DWORD dynamic_count = 0;
    DWORD version_entry_address = 0;
    if (scan_loader_list(loader, 0x0CU, 0x00U, "version.dll",
                         (DWORD)version, &dynamic_count,
                         (DWORD *)0, &version_entry_address) != 1 ||
        dynamic_count != load_count + 1)
        fail(39);
    LDR_DATA_TABLE_ENTRY32 *version_entry =
        (LDR_DATA_TABLE_ENTRY32 *)version_entry_address;
    if (!version_entry || version_entry->LoadCount != 1) fail(40);

    HANDLE version_again = LoadLibraryA("version.dll");
    if (version_again != version || version_entry->LoadCount != 2) fail(41);
    DWORD stable_entry_address = 0;
    if (scan_loader_list(loader, 0x0CU, 0x00U, "version.dll",
                         (DWORD)version, &dynamic_count,
                         (DWORD *)0, &stable_entry_address) != 1 ||
        dynamic_count != load_count + 1 ||
        stable_entry_address != version_entry_address)
        fail(42);
    if (!FreeLibrary(version_again) || version_entry->LoadCount != 1)
        fail(43);
    if (!FreeLibrary(version)) fail(44);
    DWORD released_count = 0;
    if (scan_loader_list(loader, 0x0CU, 0x00U, "version.dll", 0,
                         &released_count, (DWORD *)0, (DWORD *)0) != 0 ||
        released_count != load_count)
        fail(45);
    LeaveCriticalSection(loader_lock);
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread != 0)
        fail(52);

    struct {
        DWORD cookie;
        DWORD guard;
    } lock_cookie = { 0, 0xA55A3CC3U };
    DWORD lock_disposition = 0;
    NTSTATUS lock_status = LdrLockLoaderLock(
        0x00000002U, &lock_disposition, &lock_cookie.cookie);
    if (lock_status != STATUS_SUCCESS || lock_disposition != 1 ||
        !lock_cookie.cookie)
        fail(53);
    if (lock_cookie.guard != 0xA55A3CC3U) fail(54);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        loader_lock->OwningThread != GetCurrentThreadId())
        fail(55);
    if (LdrUnlockLoaderLock(0, lock_cookie.cookie) != STATUS_SUCCESS)
        fail(56);
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread != 0)
        fail(57);

    lock_disposition = 0xFFFFFFFFU;
    lock_cookie.cookie = 0xFFFFFFFFU;
    lock_status = LdrLockLoaderLock(
        0x00000004U, &lock_disposition, &lock_cookie.cookie);
    if (lock_status != STATUS_INVALID_PARAMETER_1 || lock_disposition != 0 ||
        lock_cookie.cookie != 0 || lock_cookie.guard != 0xA55A3CC3U)
        fail(58);
    if (LdrLockLoaderLock(0, &lock_disposition, (DWORD *)0) !=
        STATUS_INVALID_PARAMETER_3)
        fail(59);

    SetLastError(0x1234ABCDU);
    if (GetLastError() != 0x1234ABCDU ||
        read_last_error() != 0x1234ABCDU) fail(10);

    while (count < 80 &&
           (low_index == INVALID_TLS_INDEX ||
            high_index == INVALID_TLS_INDEX)) {
        DWORD index = TlsAlloc();
        if (index == INVALID_TLS_INDEX) fail(11);
        indices[count++] = index;
        if (index < 64 && low_index == INVALID_TLS_INDEX) low_index = index;
        if (index >= 64 && high_index == INVALID_TLS_INDEX) high_index = index;
    }
    if (low_index == INVALID_TLS_INDEX || high_index == INVALID_TLS_INDEX)
        fail(12);

    DWORD *low_slot = teb_tls_slot(main_teb, low_index);
    DWORD *high_slot = teb_tls_slot(main_teb, high_index);
    if (!low_slot || !high_slot) fail(13);
    if (!TlsSetValue(low_index, (PVOID)0x12345000U) ||
        !TlsSetValue(high_index, (PVOID)0x23456000U)) fail(14);
    if (*low_slot != 0x12345000U || *high_slot != 0x23456000U) fail(15);

    *low_slot = 0x13579000U;
    *high_slot = 0x2468A000U;
    if (TlsGetValue(low_index) != (PVOID)0x13579000U ||
        TlsGetValue(high_index) != (PVOID)0x2468A000U) fail(16);

    DWORD thread_id = 0;
    HANDLE thread = CreateThread((PVOID)0, 0, test_thread,
                                 (PVOID)0, 0, &thread_id);
    if (!thread || !thread_id) fail(17);
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0 ||
        !CloseHandle(thread)) fail(18);
    if (thread_result) fail(19 + thread_result);

    for (DWORD i = 0; i < count; i++) {
        if (!TlsFree(indices[i])) fail(30);
    }

    print("PEB32 TEST PASS\r\n");
    ExitProcess(0);
}
