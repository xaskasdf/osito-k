/* PE32 contract test for Win32 byte-range file locks. */

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

typedef struct {
    DWORD Internal;
    DWORD InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED;

typedef struct {
    DWORD Status;
    DWORD Information;
} IO_STATUS_BLOCK32;

typedef union {
    struct {
        DWORD LowPart;
        long HighPart;
    } u;
    unsigned long long QuadPart;
} LARGE_INTEGER32;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define GENERIC_READ             0x80000000UL
#define GENERIC_WRITE            0x40000000UL
#define FILE_READ_ATTRIBUTES     0x00000080UL
#define FILE_SHARE_READ          0x00000001UL
#define FILE_SHARE_WRITE         0x00000002UL
#define FILE_SHARE_DELETE        0x00000004UL
#define CREATE_ALWAYS            2U
#define OPEN_EXISTING            3U
#define FILE_ATTRIBUTE_NORMAL    0x00000080UL
#define FILE_FLAG_OVERLAPPED     0x40000000UL
#define FILE_BEGIN               0U
#define LOCKFILE_FAIL_IMMEDIATELY 0x00000001UL
#define LOCKFILE_EXCLUSIVE_LOCK  0x00000002UL
#define DUPLICATE_CLOSE_SOURCE   0x00000001UL
#define DUPLICATE_SAME_ACCESS    0x00000002UL
#define ERROR_ACCESS_DENIED      5U
#define ERROR_INVALID_HANDLE     6U
#define ERROR_LOCK_VIOLATION     33U
#define ERROR_INVALID_PARAMETER  87U
#define ERROR_NOT_LOCKED         158U
#define ERROR_OPERATION_ABORTED  995U
#define ERROR_IO_PENDING         997U
#define ERROR_NOT_FOUND          1168U
#define WAIT_OBJECT_0            0U
#define WAIT_TIMEOUT             258U
#define STATUS_SUCCESS           0x00000000UL
#define STATUS_PENDING           0x00000103UL
#define STATUS_INVALID_PARAMETER 0xC000000DUL
#define STATUS_LOCK_NOT_GRANTED  0xC0000055UL
#define STATUS_RANGE_NOT_LOCKED  0xC000007EUL
#define STATUS_CANCELLED         0xC0000120UL
#define STD_OUTPUT_HANDLE        ((DWORD)-11)
#define INVALID_HANDLE_VALUE     ((HANDLE)(long)-1)
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFUL

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT HANDLE WINAPI GetCurrentProcess(void);
DLLIMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access,
                                    DWORD share, PVOID security,
                                    DWORD disposition, DWORD attributes,
                                    HANDLE template_file);
DLLIMPORT BOOL WINAPI ReadFile(HANDLE file, PVOID buffer, DWORD size,
                               DWORD *read, OVERLAPPED *overlapped);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, OVERLAPPED *overlapped);
DLLIMPORT DWORD WINAPI SetFilePointer(HANDLE file, long distance,
                                      long *distance_high, DWORD method);
DLLIMPORT BOOL WINAPI LockFile(HANDLE file, DWORD offset_low,
                               DWORD offset_high, DWORD length_low,
                               DWORD length_high);
DLLIMPORT BOOL WINAPI UnlockFile(HANDLE file, DWORD offset_low,
                                 DWORD offset_high, DWORD length_low,
                                 DWORD length_high);
DLLIMPORT BOOL WINAPI LockFileEx(HANDLE file, DWORD flags, DWORD reserved,
                                 DWORD length_low, DWORD length_high,
                                 OVERLAPPED *overlapped);
DLLIMPORT BOOL WINAPI UnlockFileEx(HANDLE file, DWORD reserved,
                                   DWORD length_low, DWORD length_high,
                                   OVERLAPPED *overlapped);
DLLIMPORT HANDLE WINAPI CreateEventA(PVOID security, BOOL manual_reset,
                                     BOOL initial_state, const char *name);
DLLIMPORT HANDLE WINAPI CreateIoCompletionPort(HANDLE file,
                                                HANDLE existing_port,
                                                DWORD completion_key,
                                                DWORD concurrent_threads);
DLLIMPORT BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, DWORD *bytes,
                                                 DWORD *completion_key,
                                                 OVERLAPPED **overlapped,
                                                 DWORD milliseconds);
DLLIMPORT BOOL WINAPI CancelIo(HANDLE file);
DLLIMPORT BOOL WINAPI CancelIoEx(HANDLE file, OVERLAPPED *overlapped);
DLLIMPORT DWORD WINAPI NtLockFile(HANDLE file, HANDLE event,
                                  PVOID apc_routine, PVOID apc_context,
                                  IO_STATUS_BLOCK32 *iosb,
                                  LARGE_INTEGER32 *offset,
                                  LARGE_INTEGER32 *length, DWORD key,
                                  BOOL fail_immediately, BOOL exclusive);
DLLIMPORT DWORD WINAPI NtUnlockFile(HANDLE file, IO_STATUS_BLOCK32 *iosb,
                                    LARGE_INTEGER32 *offset,
                                    LARGE_INTEGER32 *length, DWORD key);
DLLIMPORT BOOL WINAPI ResetEvent(HANDLE event);
DLLIMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT BOOL WINAPI DuplicateHandle(HANDLE source_process,
                                      HANDLE source_handle,
                                      HANDLE target_process,
                                      HANDLE *target_handle,
                                      DWORD desired_access, BOOL inherit,
                                      DWORD options);
DLLIMPORT BOOL WINAPI DeleteFileA(const char *name);
DLLIMPORT DWORD WINAPI GetLastError(void);
DLLIMPORT void WINAPI SetLastError(DWORD error);

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
              &written, (OVERLAPPED *)0);
}

static void fail(UINT code)
{
    char message[] = "FILE LOCK TEST FAIL 000\r\n";
    message[20] = (char)('0' + ((code / 100) % 10));
    message[21] = (char)('0' + ((code / 10) % 10));
    message[22] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static void zero_overlapped(OVERLAPPED *overlapped, HANDLE event)
{
    overlapped->Internal = 0xAAAAAAAAUL;
    overlapped->InternalHigh = 0xBBBBBBBBUL;
    overlapped->Offset = 0;
    overlapped->OffsetHigh = 0;
    overlapped->hEvent = event;
}

static void seek(HANDLE file, DWORD offset, UINT failure)
{
    SetLastError(0);
    if (SetFilePointer(file, (long)offset, (long *)0, FILE_BEGIN) ==
            INVALID_SET_FILE_POINTER &&
        GetLastError() != 0)
        fail(failure);
}

void mainCRTStartup(void)
{
    static const char file_name[] = "file-lock-contract.tmp";
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    BYTE data[8] = { 'l', 'o', 'c', 'k', 't', 'e', 's', 't' };
    BYTE value = 0;
    DWORD transferred = 0;

    DeleteFileA(file_name);
    HANDLE first = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE,
                               share, (PVOID)0, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    HANDLE peer = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE,
                              share, (PVOID)0, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (first == INVALID_HANDLE_VALUE || peer == INVALID_HANDLE_VALUE) fail(1);
    if (!WriteFile(first, data, sizeof(data), &transferred, (OVERLAPPED *)0) ||
        transferred != sizeof(data))
        fail(2);

    if (!LockFile(first, 0, 0, 4, 0)) fail(3);
    SetLastError(0);
    if (LockFile(peer, 0, 0, 4, 0) ||
        GetLastError() != ERROR_LOCK_VIOLATION)
        fail(4);
    seek(peer, 0, 5);
    if (ReadFile(peer, &value, 1, &transferred, (OVERLAPPED *)0) ||
        GetLastError() != ERROR_LOCK_VIOLATION)
        fail(6);
    seek(peer, 0, 7);
    if (WriteFile(peer, &value, 1, &transferred, (OVERLAPPED *)0) ||
        GetLastError() != ERROR_LOCK_VIOLATION)
        fail(8);
    seek(first, 0, 9);
    if (!ReadFile(first, &value, 1, &transferred, (OVERLAPPED *)0)) fail(10);
    seek(first, 0, 11);
    if (!WriteFile(first, &value, 1, &transferred, (OVERLAPPED *)0)) fail(12);
    seek(peer, 4, 13);
    if (!WriteFile(peer, &value, 1, &transferred, (OVERLAPPED *)0)) fail(14);
    if (UnlockFile(first, 0, 0, 3, 0) ||
        GetLastError() != ERROR_NOT_LOCKED)
        fail(15);
    if (!UnlockFile(first, 0, 0, 4, 0)) fail(16);

    HANDLE event = CreateEventA((PVOID)0, 1, 0, (const char *)0);
    if (!event) fail(17);
    OVERLAPPED first_lock;
    zero_overlapped(&first_lock, event);
    SetLastError(203);
    if (!LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 0, 4, 0,
                    &first_lock))
        fail(18);
    if (first_lock.Internal != STATUS_SUCCESS ||
        first_lock.InternalHigh != 0 || GetLastError() != 203 ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        fail(19);

    OVERLAPPED peer_lock;
    zero_overlapped(&peer_lock, (HANDLE)0);
    if (!LockFileEx(peer, LOCKFILE_FAIL_IMMEDIATELY, 0, 4, 0,
                    &peer_lock))
        fail(20);
    seek(peer, 0, 21);
    if (!ReadFile(peer, &value, 1, &transferred, (OVERLAPPED *)0)) fail(22);
    seek(peer, 0, 23);
    if (WriteFile(peer, &value, 1, &transferred, (OVERLAPPED *)0) ||
        GetLastError() != ERROR_LOCK_VIOLATION)
        fail(24);

    if (!ResetEvent(event)) fail(25);
    peer_lock.hEvent = event;
    if (!UnlockFileEx(peer, 0, 4, 0, &peer_lock) ||
        peer_lock.Internal != STATUS_SUCCESS ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
        fail(26);
    if (!UnlockFileEx(first, 0, 4, 0, &first_lock)) fail(27);
    if (UnlockFileEx(first, 0, 4, 0, &first_lock) ||
        GetLastError() != ERROR_NOT_LOCKED ||
        first_lock.Internal != STATUS_RANGE_NOT_LOCKED)
        fail(28);

    zero_overlapped(&first_lock, event);
    if (!LockFile(first, 0, 0, 4, 0)) fail(29);
    if (!ResetEvent(event)) fail(30);
    if (LockFileEx(peer,
                   LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK,
                   0, 4, 0, &first_lock) ||
        GetLastError() != ERROR_LOCK_VIOLATION ||
        first_lock.Internal != STATUS_LOCK_NOT_GRANTED ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        fail(31);
    if (!UnlockFile(first, 0, 0, 4, 0)) fail(32);

    zero_overlapped(&first_lock, (HANDLE)0);
    if (LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 1, 1, 0,
                   &first_lock) ||
        GetLastError() != ERROR_INVALID_PARAMETER ||
        first_lock.Internal != STATUS_INVALID_PARAMETER)
        fail(33);
    if (LockFileEx(first, 4, 0, 1, 0, &first_lock) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        fail(34);
    if (!LockFile(first, 0, 0, 0, 0) ||
        !UnlockFile(first, 0, 0, 0, 0))
        fail(35);

    if (!LockFile(first, 0, 0, 4, 0)) fail(36);
    if (!CloseHandle(first)) fail(37);
    first = INVALID_HANDLE_VALUE;
    if (!LockFile(peer, 0, 0, 4, 0)) fail(38);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(39);

    HANDLE attributes = CreateFileA(file_name, FILE_READ_ATTRIBUTES, share,
                                    (PVOID)0, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (attributes == INVALID_HANDLE_VALUE) fail(40);
    if (LockFile(attributes, 0, 0, 1, 0) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        fail(41);
    if (LockFile(INVALID_HANDLE_VALUE, 0, 0, 1, 0) ||
        GetLastError() != ERROR_INVALID_HANDLE)
        fail(42);

    HANDLE process = GetCurrentProcess();
    HANDLE source = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE,
                                share, (PVOID)0, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    HANDLE duplicate = (HANDLE)0;
    if (source == INVALID_HANDLE_VALUE ||
        !LockFile(source, 0, 0, 4, 0))
        fail(43);
    if (!DuplicateHandle(process, source, process, &duplicate, 0, 0,
                         DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS) ||
        !duplicate)
        fail(44);
    SetLastError(0);
    if (CloseHandle(source) || GetLastError() != ERROR_INVALID_HANDLE)
        fail(45);
    seek(peer, 0, 46);
    if (ReadFile(peer, &value, 1, &transferred, (OVERLAPPED *)0) ||
        GetLastError() != ERROR_LOCK_VIOLATION)
        fail(47);
    if (!CloseHandle(duplicate)) fail(48);
    seek(peer, 0, 49);
    if (!ReadFile(peer, &value, 1, &transferred, (OVERLAPPED *)0)) fail(50);

    source = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE, share,
                         (PVOID)0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         (HANDLE)0);
    if (source == INVALID_HANDLE_VALUE ||
        !LockFile(source, 0, 0, 4, 0))
        fail(51);
    if (!DuplicateHandle(process, source, (HANDLE)0, (HANDLE *)0, 0, 0,
                         DUPLICATE_CLOSE_SOURCE))
        fail(52);
    SetLastError(0);
    if (CloseHandle(source) || GetLastError() != ERROR_INVALID_HANDLE)
        fail(53);
    if (!LockFile(peer, 0, 0, 4, 0) ||
        !UnlockFile(peer, 0, 0, 4, 0))
        fail(54);

    source = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE, share,
                         (PVOID)0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         (HANDLE)0);
    if (source == INVALID_HANDLE_VALUE) fail(55);
    SetLastError(0);
    if (DuplicateHandle(process, source, (HANDLE)(long)0x7FFFFFFC,
                        &duplicate, 0, 0, DUPLICATE_CLOSE_SOURCE) ||
        GetLastError() != ERROR_INVALID_HANDLE)
        fail(56);
    SetLastError(0);
    if (CloseHandle(source) || GetLastError() != ERROR_INVALID_HANDLE)
        fail(57);

    HANDLE async_file = CreateFileA(
        file_name, GENERIC_READ | GENERIC_WRITE, share, (PVOID)0,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        (HANDLE)0);
    if (async_file == INVALID_HANDLE_VALUE) fail(58);
    HANDLE completion_port = CreateIoCompletionPort(
        async_file, (HANDLE)0, 0x4C4BUL, 1);
    if (!completion_port) fail(59);

    OVERLAPPED pending_lock;
    DWORD completion_bytes = 0;
    DWORD completion_key = 0;
    OVERLAPPED *completion_overlapped = (OVERLAPPED *)0;
    if (!LockFile(peer, 0, 0, 4, 0)) fail(60);
    if (!ResetEvent(event)) fail(61);
    zero_overlapped(&pending_lock, event);
    SetLastError(0);
    if (LockFileEx(async_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &pending_lock) ||
        GetLastError() != ERROR_IO_PENDING ||
        pending_lock.Internal != STATUS_PENDING ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
        fail(62);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(63);
    if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0 ||
        pending_lock.Internal != STATUS_SUCCESS ||
        pending_lock.InternalHigh != 0)
        fail(64);
    if (!GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) || completion_bytes != 0 ||
        completion_key != 0x4C4BUL ||
        completion_overlapped != &pending_lock)
        fail(65);
    if (!UnlockFileEx(async_file, 0, 4, 0, &pending_lock)) fail(66);

    if (!LockFile(peer, 0, 0, 4, 0)) fail(67);
    if (!ResetEvent(event)) fail(68);
    zero_overlapped(&pending_lock, event);
    SetLastError(0);
    if (LockFileEx(async_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &pending_lock) ||
        GetLastError() != ERROR_IO_PENDING ||
        pending_lock.Internal != STATUS_PENDING)
        fail(69);
    if (!CancelIoEx(async_file, &pending_lock) ||
        pending_lock.Internal != STATUS_CANCELLED ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        fail(70);
    completion_bytes = 99;
    completion_key = 0;
    completion_overlapped = (OVERLAPPED *)0;
    SetLastError(0);
    if (GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) ||
        GetLastError() != ERROR_OPERATION_ABORTED || completion_bytes != 0 ||
        completion_key != 0x4C4BUL ||
        completion_overlapped != &pending_lock)
        fail(71);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(72);
    if (!LockFile(peer, 0, 0, 4, 0) ||
        !UnlockFile(peer, 0, 0, 4, 0))
        fail(73);
    SetLastError(0);
    if (CancelIoEx(async_file, &pending_lock) ||
        GetLastError() != ERROR_NOT_FOUND)
        fail(74);

    if (!LockFile(peer, 0, 0, 4, 0)) fail(75);
    if (!ResetEvent(event)) fail(76);
    zero_overlapped(&pending_lock, event);
    SetLastError(0);
    if (LockFileEx(async_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &pending_lock) ||
        GetLastError() != ERROR_IO_PENDING)
        fail(77);
    if (!CancelIo(async_file) || pending_lock.Internal != STATUS_CANCELLED ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        fail(78);
    completion_bytes = 99;
    completion_key = 0;
    completion_overlapped = (OVERLAPPED *)0;
    SetLastError(0);
    if (GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) ||
        GetLastError() != ERROR_OPERATION_ABORTED || completion_bytes != 0 ||
        completion_key != 0x4C4BUL ||
        completion_overlapped != &pending_lock)
        fail(79);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(80);

    if (!LockFile(peer, 0, 0, 4, 0)) fail(81);
    if (!ResetEvent(event)) fail(82);
    zero_overlapped(&pending_lock, event);
    SetLastError(0);
    if (LockFileEx(async_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &pending_lock) ||
        GetLastError() != ERROR_IO_PENDING)
        fail(83);
    if (!CloseHandle(async_file)) fail(84);
    async_file = INVALID_HANDLE_VALUE;
    if (pending_lock.Internal != STATUS_CANCELLED ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        fail(85);
    completion_bytes = 99;
    completion_key = 0;
    completion_overlapped = (OVERLAPPED *)0;
    SetLastError(0);
    if (GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) ||
        GetLastError() != ERROR_OPERATION_ABORTED || completion_bytes != 0 ||
        completion_key != 0x4C4BUL ||
        completion_overlapped != &pending_lock)
        fail(86);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(87);
    if (!LockFile(peer, 0, 0, 4, 0) ||
        !UnlockFile(peer, 0, 0, 4, 0))
        fail(88);

    HANDLE native_file = CreateFileA(
        file_name, GENERIC_READ | GENERIC_WRITE, share, (PVOID)0,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        (HANDLE)0);
    if (native_file == INVALID_HANDLE_VALUE) fail(89);
    if (!LockFile(peer, 0, 0, 4, 0)) fail(90);
    if (!ResetEvent(event)) fail(91);
    IO_STATUS_BLOCK32 native_iosb = { 0xAAAAAAAAUL, 0xBBBBBBBBUL };
    LARGE_INTEGER32 native_offset;
    LARGE_INTEGER32 native_length;
    native_offset.QuadPart = 0;
    native_length.QuadPart = 4;
    DWORD native_status = NtLockFile(
        native_file, event, (PVOID)0, (PVOID)0, &native_iosb,
        &native_offset, &native_length, 0, 0, 1);
    if (native_status != STATUS_PENDING ||
        native_iosb.Status != STATUS_PENDING || native_iosb.Information != 0 ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
        fail(92);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(93);
    if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0 ||
        native_iosb.Status != STATUS_SUCCESS || native_iosb.Information != 0)
        fail(94);
    native_iosb.Status = 0xAAAAAAAAUL;
    native_iosb.Information = 0xBBBBBBBBUL;
    native_status = NtUnlockFile(
        native_file, &native_iosb, &native_offset, &native_length, 0);
    if (native_status != STATUS_SUCCESS ||
        native_iosb.Status != STATUS_SUCCESS || native_iosb.Information != 0)
        fail(95);

    if (CreateIoCompletionPort(native_file, completion_port, 0xF1F0UL, 1) !=
        completion_port)
        fail(96);
    HANDLE second_event = CreateEventA((PVOID)0, 1, 0, (const char *)0);
    if (!second_event) fail(97);
    OVERLAPPED first_waiter;
    OVERLAPPED second_waiter;
    zero_overlapped(&first_waiter, event);
    zero_overlapped(&second_waiter, second_event);
    if (!ResetEvent(event) || !LockFile(peer, 0, 0, 4, 0)) fail(98);
    SetLastError(0);
    if (LockFileEx(native_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &first_waiter) || GetLastError() != ERROR_IO_PENDING)
        fail(99);
    SetLastError(0);
    if (LockFileEx(native_file, LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0,
                   &second_waiter) || GetLastError() != ERROR_IO_PENDING)
        fail(100);
    if (!UnlockFile(peer, 0, 0, 4, 0)) fail(101);
    if (first_waiter.Internal != STATUS_SUCCESS ||
        WaitForSingleObject(event, 0) != WAIT_OBJECT_0 ||
        second_waiter.Internal != STATUS_PENDING ||
        WaitForSingleObject(second_event, 0) != WAIT_TIMEOUT)
        fail(102);
    completion_bytes = 99;
    completion_key = 0;
    completion_overlapped = (OVERLAPPED *)0;
    if (!GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) || completion_bytes != 0 ||
        completion_key != 0xF1F0UL ||
        completion_overlapped != &first_waiter)
        fail(103);
    if (!UnlockFileEx(native_file, 0, 4, 0, &first_waiter) ||
        second_waiter.Internal != STATUS_SUCCESS ||
        WaitForSingleObject(second_event, 0) != WAIT_OBJECT_0)
        fail(104);
    completion_bytes = 99;
    completion_key = 0;
    completion_overlapped = (OVERLAPPED *)0;
    if (!GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0) || completion_bytes != 0 ||
        completion_key != 0xF1F0UL ||
        completion_overlapped != &second_waiter)
        fail(105);
    if (!UnlockFileEx(native_file, 0, 4, 0, &second_waiter)) fail(106);
    CloseHandle(second_event);
    CloseHandle(native_file);

    CloseHandle(completion_port);
    CloseHandle(attributes);
    CloseHandle(event);
    CloseHandle(peer);
    DeleteFileA(file_name);
    print("FILE LOCK TEST PASS\r\n");
    ExitProcess(0);
    for (;;) { }
}
