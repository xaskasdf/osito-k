/* PE32+ contract test for the native TEB, PEB, loader lists, and loader lock. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef unsigned long long ULONG_PTR;
typedef unsigned int UINT;
typedef long LONG;
typedef long NTSTATUS;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI
#define NTAPI
#define DLLIMPORT __declspec(dllimport)
#define FALSE 0
#define TRUE 1

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0 0U
#define INFINITE 0xFFFFFFFFU
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER_1 ((NTSTATUS)0xC00000EFU)
#define STATUS_INVALID_PARAMETER_3 ((NTSTATUS)0xC00000F1U)

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT HANDLE WINAPI GetProcessHeap(void);
DLLIMPORT DWORD WINAPI GetCurrentThreadId(void);
DLLIMPORT HANDLE WINAPI CreateThread(PVOID attributes, ULONG_PTR stack_size,
                                     DWORD (WINAPI *start)(PVOID),
                                     PVOID parameter, DWORD flags,
                                     DWORD *thread_id);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT void WINAPI Sleep(DWORD milliseconds);
DLLIMPORT HANDLE WINAPI LoadLibraryA(const char *name);
DLLIMPORT BOOL WINAPI FreeLibrary(HANDLE module);
DLLIMPORT void WINAPI EnterCriticalSection(PVOID section);
DLLIMPORT BOOL WINAPI TryEnterCriticalSection(PVOID section);
DLLIMPORT void WINAPI LeaveCriticalSection(PVOID section);
DLLIMPORT NTSTATUS NTAPI LdrLockLoaderLock(DWORD flags, DWORD *disposition,
                                           ULONG_PTR *cookie);
DLLIMPORT NTSTATUS NTAPI LdrUnlockLoaderLock(DWORD flags, ULONG_PTR cookie);
DLLIMPORT void NTAPI RtlAcquirePebLock(void);
DLLIMPORT BOOL NTAPI RtlTryAcquirePebLock(void);
DLLIMPORT void NTAPI RtlReleasePebLock(void);

typedef struct {
    PVOID Flink;
    PVOID Blink;
} LIST_ENTRY64;

typedef struct {
    WORD Length;
    WORD MaximumLength;
    DWORD Padding;
    WORD *Buffer;
} UNICODE_STRING64;

typedef struct {
    DWORD Length;
    BYTE Initialized;
    BYTE Reserved1[3];
    PVOID SsHandle;
    LIST_ENTRY64 InLoadOrderModuleList;
    LIST_ENTRY64 InMemoryOrderModuleList;
    LIST_ENTRY64 InInitializationOrderModuleList;
    PVOID EntryInProgress;
    BYTE ShutdownInProgress;
    BYTE Reserved2[7];
    HANDLE ShutdownThreadId;
} PEB_LDR_DATA64;

typedef struct {
    PVOID DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION64;

typedef struct {
    LIST_ENTRY64 InLoadOrderLinks;
    LIST_ENTRY64 InMemoryOrderLinks;
    LIST_ENTRY64 InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    DWORD SizeOfImage;
    DWORD Reserved0;
    UNICODE_STRING64 FullDllName;
    UNICODE_STRING64 BaseDllName;
    DWORD Flags;
    WORD LoadCount;
    WORD TlsIndex;
    LIST_ENTRY64 HashLinks;
    DWORD TimeDateStamp;
    DWORD Reserved1;
    PVOID EntryPointActivationContext;
    PVOID Lock;
    PVOID DdagNode;
    LIST_ENTRY64 NodeModuleLink;
    PVOID LoadContext;
    PVOID ParentDllBase;
    PVOID SwitchBackContext;
    BYTE BaseAddressIndexNode[0x18];
    BYTE MappingInfoIndexNode[0x18];
    ULONG_PTR OriginalBase;
    long long LoadTime;
    DWORD BaseNameHashValue;
    DWORD LoadReason;
    DWORD ImplicitPathOptions;
    DWORD ReferenceCount;
    DWORD DependentLoadFlags;
    BYTE SigningLevel;
    BYTE Reserved2[3];
} LDR_DATA_TABLE_ENTRY64;

_Static_assert(sizeof(LIST_ENTRY64) == 0x10, "LIST_ENTRY64 layout");
_Static_assert(sizeof(UNICODE_STRING64) == 0x10, "UNICODE_STRING64 layout");
_Static_assert(sizeof(PEB_LDR_DATA64) == 0x58, "PEB_LDR_DATA64 layout");
_Static_assert(sizeof(RTL_CRITICAL_SECTION64) == 0x28,
               "RTL_CRITICAL_SECTION64 layout");
_Static_assert(__builtin_offsetof(LDR_DATA_TABLE_ENTRY64, DllBase) == 0x30,
               "LDR DllBase offset");
_Static_assert(__builtin_offsetof(LDR_DATA_TABLE_ENTRY64, FullDllName) == 0x48,
               "LDR FullDllName offset");
_Static_assert(__builtin_offsetof(LDR_DATA_TABLE_ENTRY64, BaseDllName) == 0x58,
               "LDR BaseDllName offset");
_Static_assert(__builtin_offsetof(LDR_DATA_TABLE_ENTRY64, ReferenceCount) ==
                   0x114,
               "LDR ReferenceCount offset");
_Static_assert(sizeof(LDR_DATA_TABLE_ENTRY64) == 0x120,
               "LDR_DATA_TABLE_ENTRY64 layout");

static RTL_CRITICAL_SECTION64 *contention_lock;
static volatile DWORD contention_ready;
static volatile DWORD contention_go;
static volatile DWORD contention_result;
static volatile BOOL contention_use_peb_api;
static volatile ULONG_PTR contention_teb;
static volatile ULONG_PTR contention_teb_self;
static volatile ULONG_PTR contention_peb;
static volatile ULONG_PTR contention_tls_vector;
static volatile ULONG_PTR contention_tls_expansion;

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
    char message[] = "PEB64 TEST FAIL 00\r\n";
    message[16] = (char)('0' + (code / 10));
    message[17] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static ULONG_PTR read_teb(void)
{
    ULONG_PTR value;
    __asm__ volatile ("movq %%gs:0x30, %0" : "=r"(value));
    return value;
}

static ULONG_PTR read_peb(void)
{
    ULONG_PTR value;
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(value));
    return value;
}

static BOOL is_user_pointer(ULONG_PTR value)
{
    return value >= 0x10000ULL && value < 0x0000800000000000ULL;
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + 32) : value;
}

static BOOL unicode_equals_ascii(const UNICODE_STRING64 *wide,
                                 const char *ascii)
{
    DWORD chars = wide->Length / sizeof(WORD);
    if (!wide->Buffer || (wide->Length & 1U) ||
        wide->MaximumLength < wide->Length + sizeof(WORD))
        return FALSE;
    DWORD length = text_length(ascii);
    if (chars != length) return FALSE;

    for (DWORD i = 0; i < chars; i++) {
        if (wide->Buffer[i] > 0x7FU ||
            ascii_lower((char)wide->Buffer[i]) != ascii_lower(ascii[i]))
            return FALSE;
    }
    return TRUE;
}

static int scan_loader_list(ULONG_PTR ldr_address, ULONG_PTR head_offset,
                            ULONG_PTR link_offset, const char *wanted_name,
                            ULONG_PTR wanted_base, DWORD *count,
                            ULONG_PTR *first_base, ULONG_PTR *found_entry)
{
    LIST_ENTRY64 *head = (LIST_ENTRY64 *)(ldr_address + head_offset);
    LIST_ENTRY64 *link = (LIST_ENTRY64 *)head->Flink;
    PVOID previous = head;
    DWORD seen = 0;
    int found = 0;

    while (link != head && seen < 2048U) {
        if (!link || (ULONG_PTR)link < link_offset) return -1;
        LDR_DATA_TABLE_ENTRY64 *entry =
            (LDR_DATA_TABLE_ENTRY64 *)((BYTE *)link - link_offset);
        if (link->Blink != previous || !link->Flink || !entry->DllBase ||
            !entry->SizeOfImage)
            return -1;
        if (!seen && first_base) *first_base = (ULONG_PTR)entry->DllBase;
        if (wanted_name &&
            unicode_equals_ascii(&entry->BaseDllName, wanted_name) &&
            (!wanted_base || (ULONG_PTR)entry->DllBase == wanted_base)) {
            found = 1;
            if (found_entry) *found_entry = (ULONG_PTR)entry;
        }
        previous = link;
        link = (LIST_ENTRY64 *)link->Flink;
        seen++;
    }

    if (link != head || head->Blink != previous) return -1;
    if (count) *count = seen;
    return found;
}

static DWORD WINAPI loader_contention_thread(PVOID parameter)
{
    (void)parameter;
    ULONG_PTR teb = read_teb();
    contention_teb = teb;
    contention_teb_self = teb ? *(ULONG_PTR *)(teb + 0x30U) : 0;
    contention_peb = read_peb();
    contention_tls_vector = teb ? *(ULONG_PTR *)(teb + 0x58U) : 0;
    contention_tls_expansion = teb ? *(ULONG_PTR *)(teb + 0x1780U) : 0;
    contention_ready = 1;
    while (!contention_go) Sleep(0);
    BOOL acquired = contention_use_peb_api
        ? RtlTryAcquirePebLock()
        : TryEnterCriticalSection(contention_lock);
    if (acquired) {
        contention_result = 2;
        if (contention_use_peb_api)
            RtlReleasePebLock();
        else
            LeaveCriticalSection(contention_lock);
    } else {
        contention_result = 1;
    }
    return 0;
}

void mainCRTStartup(void)
{
    ULONG_PTR teb = read_teb();
    ULONG_PTR peb = read_peb();
    if (!teb || !peb) fail(1);
    if (!is_user_pointer(teb) || !is_user_pointer(peb)) fail(48);
    if (*(ULONG_PTR *)(teb + 0x30U) != teb ||
        *(ULONG_PTR *)(teb + 0x60U) != peb)
        fail(2);

    ULONG_PTR image_base = *(ULONG_PTR *)(peb + 0x10U);
    ULONG_PTR parameters = *(ULONG_PTR *)(peb + 0x20U);
    ULONG_PTR process_heap = *(ULONG_PTR *)(peb + 0x30U);
    if (!image_base || !parameters || !process_heap ||
        process_heap != (ULONG_PTR)GetProcessHeap())
        fail(3);
    DWORD processors = *(DWORD *)(peb + 0xB8U);
    if (!processors || processors > 64) fail(4);
    if (*(DWORD *)(peb + 0xE8U) != 1 ||
        *(DWORD *)(peb + 0xECU) != 1)
        fail(5);
    ULONG_PTR heaps = *(ULONG_PTR *)(peb + 0xF0U);
    if (!heaps || *(ULONG_PTR *)heaps != process_heap) fail(6);
    if (!is_user_pointer(parameters) || !is_user_pointer(heaps)) fail(49);
    if (!is_user_pointer(*(ULONG_PTR *)(teb + 0x58U)) ||
        !is_user_pointer(*(ULONG_PTR *)(teb + 0x1780U)))
        fail(50);
    if (*(DWORD *)(peb + 0x118U) != 10 ||
        *(DWORD *)(peb + 0x11CU) != 0 ||
        *(WORD *)(peb + 0x120U) != 19045 ||
        *(DWORD *)(peb + 0x124U) != 2)
        fail(7);
    if (*(DWORD *)(peb + 0x128U) != 3 ||
        *(DWORD *)(peb + 0x12CU) != 6 ||
        *(DWORD *)(peb + 0x130U) != 1)
        fail(8);

    ULONG_PTR loader_address = *(ULONG_PTR *)(peb + 0x18U);
    PEB_LDR_DATA64 *loader = (PEB_LDR_DATA64 *)loader_address;
    if (!loader || loader->Length != sizeof(*loader) ||
        !loader->Initialized)
        fail(9);
    if (!is_user_pointer(loader_address)) fail(51);

    DWORD load_count = 0;
    DWORD memory_count = 0;
    DWORD init_count = 0;
    ULONG_PTR first_base = 0;
    int kernel32_found = scan_loader_list(
        loader_address, 0x10U, 0x00U, "kernel32.dll", 0,
        &load_count, &first_base, (ULONG_PTR *)0);
    int ntdll_found = scan_loader_list(
        loader_address, 0x10U, 0x00U, "ntdll.dll", 0,
        (DWORD *)0, (ULONG_PTR *)0, (ULONG_PTR *)0);
    if (kernel32_found < 0 || ntdll_found < 0 ||
        scan_loader_list(loader_address, 0x20U, 0x10U, (const char *)0, 0,
                         &memory_count, (ULONG_PTR *)0,
                         (ULONG_PTR *)0) < 0 ||
        scan_loader_list(loader_address, 0x30U, 0x20U, (const char *)0, 0,
                         &init_count, (ULONG_PTR *)0,
                         (ULONG_PTR *)0) < 0)
        fail(10);
    if (load_count < 3 || load_count != memory_count ||
        load_count != init_count)
        fail(11);
    if (first_base != image_base) fail(12);
    if (!kernel32_found || !ntdll_found) fail(13);

    ULONG_PTR lock_address = *(ULONG_PTR *)(peb + 0x110U);
    RTL_CRITICAL_SECTION64 *loader_lock =
        (RTL_CRITICAL_SECTION64 *)lock_address;
    if (!loader_lock || loader_lock->LockCount != -1 ||
        loader_lock->RecursionCount != 0 || loader_lock->OwningThread)
        fail(14);

    RTL_CRITICAL_SECTION64 *peb_lock =
        (RTL_CRITICAL_SECTION64 *)(ULONG_PTR)*(ULONG_PTR *)(peb + 0x38U);
    if (!peb_lock || peb_lock == loader_lock) fail(40);
    if (!is_user_pointer(lock_address) ||
        !is_user_pointer((ULONG_PTR)peb_lock))
        fail(52);
    if (peb_lock->LockCount != -1 || peb_lock->RecursionCount != 0 ||
        peb_lock->OwningThread)
        fail(41);

    contention_lock = peb_lock;
    contention_ready = 0;
    contention_go = 0;
    contention_result = 0;
    contention_use_peb_api = TRUE;
    DWORD peb_contention_thread_id = 0;
    HANDLE peb_contention_thread = CreateThread(
        (PVOID)0, 0, loader_contention_thread, (PVOID)0, 0,
        &peb_contention_thread_id);
    if (!peb_contention_thread || !peb_contention_thread_id) fail(42);
    while (!contention_ready) Sleep(0);
    RtlAcquirePebLock();
    contention_go = 1;
    while (!contention_result) Sleep(0);
    if (contention_result != 1 || peb_lock->LockCount != 0 ||
        peb_lock->RecursionCount != 1 ||
        (ULONG_PTR)peb_lock->OwningThread != GetCurrentThreadId())
        fail(43);
    if (!RtlTryAcquirePebLock() || peb_lock->LockCount != 1 ||
        peb_lock->RecursionCount != 2)
        fail(44);
    RtlReleasePebLock();
    if (peb_lock->LockCount != 0 || peb_lock->RecursionCount != 1)
        fail(45);
    RtlReleasePebLock();
    if (WaitForSingleObject(peb_contention_thread, INFINITE) != WAIT_OBJECT_0 ||
        !CloseHandle(peb_contention_thread))
        fail(46);
    if (peb_lock->LockCount != -1 || peb_lock->RecursionCount != 0 ||
        peb_lock->OwningThread)
        fail(47);
    if (!is_user_pointer(contention_teb) || contention_teb == teb)
        fail(53);
    if (contention_teb_self != contention_teb || contention_peb != peb)
        fail(54);
    if (!is_user_pointer(contention_tls_vector) ||
        !is_user_pointer(contention_tls_expansion))
        fail(55);

    contention_lock = loader_lock;
    contention_ready = 0;
    contention_go = 0;
    contention_result = 0;
    contention_use_peb_api = FALSE;
    DWORD contention_thread_id = 0;
    HANDLE contention_thread = CreateThread(
        (PVOID)0, 0, loader_contention_thread, (PVOID)0, 0,
        &contention_thread_id);
    if (!contention_thread || !contention_thread_id) fail(35);
    while (!contention_ready) Sleep(0);
    EnterCriticalSection(loader_lock);
    contention_go = 1;
    while (!contention_result) Sleep(0);
    if (contention_result != 1) fail(36);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        (ULONG_PTR)loader_lock->OwningThread != GetCurrentThreadId())
        fail(37);
    LeaveCriticalSection(loader_lock);
    if (WaitForSingleObject(contention_thread, INFINITE) != WAIT_OBJECT_0 ||
        !CloseHandle(contention_thread))
        fail(38);
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread)
        fail(39);

    EnterCriticalSection(loader_lock);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        (ULONG_PTR)loader_lock->OwningThread != GetCurrentThreadId())
        fail(15);
    if (!TryEnterCriticalSection(loader_lock) ||
        loader_lock->LockCount != 1 || loader_lock->RecursionCount != 2)
        fail(16);
    LeaveCriticalSection(loader_lock);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1)
        fail(17);

    HANDLE version = LoadLibraryA("version.dll");
    if (!version) fail(18);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        (ULONG_PTR)loader_lock->OwningThread != GetCurrentThreadId())
        fail(19);
    DWORD dynamic_count = 0;
    ULONG_PTR version_entry_address = 0;
    if (scan_loader_list(loader_address, 0x10U, 0x00U, "version.dll",
                         (ULONG_PTR)version, &dynamic_count,
                         (ULONG_PTR *)0, &version_entry_address) != 1 ||
        dynamic_count != load_count + 1)
        fail(20);
    LDR_DATA_TABLE_ENTRY64 *version_entry =
        (LDR_DATA_TABLE_ENTRY64 *)version_entry_address;
    if (!version_entry || version_entry->LoadCount != 1 ||
        version_entry->ReferenceCount != 1)
        fail(21);

    HANDLE version_again = LoadLibraryA("version.dll");
    if (version_again != version || version_entry->LoadCount != 2 ||
        version_entry->ReferenceCount != 2)
        fail(22);
    ULONG_PTR stable_entry_address = 0;
    if (scan_loader_list(loader_address, 0x10U, 0x00U, "version.dll",
                         (ULONG_PTR)version, &dynamic_count,
                         (ULONG_PTR *)0, &stable_entry_address) != 1 ||
        stable_entry_address != version_entry_address)
        fail(23);
    if (!FreeLibrary(version_again) || version_entry->LoadCount != 1 ||
        version_entry->ReferenceCount != 1)
        fail(24);
    if (!FreeLibrary(version)) fail(25);
    if (scan_loader_list(loader_address, 0x10U, 0x00U, "version.dll", 0,
                         &dynamic_count, (ULONG_PTR *)0,
                         (ULONG_PTR *)0) != 0 ||
        dynamic_count != load_count)
        fail(26);
    LeaveCriticalSection(loader_lock);
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread)
        fail(27);

    struct {
        ULONG_PTR cookie;
        ULONG_PTR guard;
    } lock_cookie = { 0, 0xA55A3CC35AA5C33CULL };
    DWORD disposition = 0;
    NTSTATUS status = LdrLockLoaderLock(
        0x00000002U, &disposition, &lock_cookie.cookie);
    if (status != STATUS_SUCCESS || disposition != 1 || !lock_cookie.cookie)
        fail(28);
    if (lock_cookie.guard != 0xA55A3CC35AA5C33CULL) fail(29);
    if (loader_lock->LockCount != 0 || loader_lock->RecursionCount != 1 ||
        (ULONG_PTR)loader_lock->OwningThread != GetCurrentThreadId())
        fail(30);
    if (LdrUnlockLoaderLock(0, lock_cookie.cookie) != STATUS_SUCCESS)
        fail(31);
    if (loader_lock->LockCount != -1 || loader_lock->RecursionCount != 0 ||
        loader_lock->OwningThread)
        fail(32);

    disposition = 0xFFFFFFFFU;
    lock_cookie.cookie = ~0ULL;
    status = LdrLockLoaderLock(
        0x00000004U, &disposition, &lock_cookie.cookie);
    if (status != STATUS_INVALID_PARAMETER_1 || disposition != 0 ||
        lock_cookie.cookie != 0 ||
        lock_cookie.guard != 0xA55A3CC35AA5C33CULL)
        fail(33);
    if (LdrLockLoaderLock(0, &disposition, (ULONG_PTR *)0) !=
        STATUS_INVALID_PARAMETER_3)
        fail(34);

    print("PEB64 TEST PASS\r\n");
    ExitProcess(0);
}
