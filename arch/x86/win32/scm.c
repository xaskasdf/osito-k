/*
 * OsitoK Windows Compatibility Layer - Service Control Manager.
 *
 * The SCM database is kernel-global, while handles and service status
 * registrations are owned by the calling Win32 process. Service programs are
 * launched as ordinary isolated PE children and attach through
 * StartServiceCtrlDispatcherW, matching the Windows process model.
 */

#include "scm.h"
#include "compat32.h"
#include "kernel32_shim.h"
#include "../fs/ositofs3.h"
#include "../kernel/smp.h"
#include "ositofs3_format.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern void WINAPI SetLastError(DWORD error);
extern DWORD WINAPI GetLastError(void);
extern BOOL WINAPI CreateProcessW(PCWSTR application_name, PWSTR command_line,
                                  PVOID process_attributes,
                                  PVOID thread_attributes,
                                  BOOL inherit_handles, DWORD creation_flags,
                                  PVOID environment, PCWSTR current_directory,
                                  PVOID startup_info,
                                  PVOID process_information);
extern DWORD win32_current_process_id(void);

#define SCM_MAX_SERVICES       32
#define SCM_MAX_HANDLES       128
#define SCM_MAX_NAME          128
#define SCM_MAX_PATH         1024
#define SCM_MAX_ACCOUNT       128
#define SCM_MAX_DEPENDENCIES  256
#define SCM_MAX_DESCRIPTION   256

#define SCM_HANDLE_PREFIX      0xA5C00000U
#define SCM_HANDLE_PREFIX_MASK 0xFFF00000U

#define SCM_KIND_MANAGER       1
#define SCM_KIND_SERVICE       2
#define SCM_KIND_STATUS        3
#define SCM_KIND_EVENT_LOG     4

#define SC_MANAGER_CONNECT             0x0001U
#define SC_MANAGER_CREATE_SERVICE      0x0002U
#define SC_MANAGER_ENUMERATE_SERVICE   0x0004U
#define SC_MANAGER_LOCK                0x0008U
#define SC_MANAGER_QUERY_LOCK_STATUS   0x0010U
#define SC_MANAGER_MODIFY_BOOT_CONFIG  0x0020U
#define SC_MANAGER_ALL_ACCESS          0x000F003FU

#define SERVICE_QUERY_CONFIG           0x0001U
#define SERVICE_CHANGE_CONFIG          0x0002U
#define SERVICE_QUERY_STATUS           0x0004U
#define SERVICE_ENUMERATE_DEPENDENTS   0x0008U
#define SERVICE_START                  0x0010U
#define SERVICE_STOP                   0x0020U
#define SERVICE_PAUSE_CONTINUE         0x0040U
#define SERVICE_INTERROGATE            0x0080U
#define SERVICE_USER_DEFINED_CONTROL   0x0100U
#define SERVICE_ALL_ACCESS             0x000F01FFU

#define DELETE_ACCESS                  0x00010000U
#define READ_CONTROL_ACCESS            0x00020000U
#define WRITE_DAC_ACCESS               0x00040000U
#define WRITE_OWNER_ACCESS             0x00080000U
#define MAXIMUM_ALLOWED_ACCESS         0x02000000U
#define GENERIC_ALL_ACCESS             0x10000000U
#define GENERIC_EXECUTE_ACCESS         0x20000000U
#define GENERIC_WRITE_ACCESS           0x40000000U
#define GENERIC_READ_ACCESS            0x80000000U

#define SERVICE_NO_CHANGE              0xFFFFFFFFU
#define SERVICE_DISABLED               0x00000004U

#define SERVICE_STOPPED                0x00000001U
#define SERVICE_START_PENDING          0x00000002U
#define SERVICE_STOP_PENDING           0x00000003U
#define SERVICE_RUNNING                0x00000004U
#define SERVICE_CONTINUE_PENDING       0x00000005U
#define SERVICE_PAUSE_PENDING          0x00000006U
#define SERVICE_PAUSED                 0x00000007U

#define SERVICE_CONTROL_STOP           0x00000001U
#define SERVICE_CONTROL_PAUSE          0x00000002U
#define SERVICE_CONTROL_CONTINUE       0x00000003U
#define SERVICE_CONTROL_INTERROGATE    0x00000004U

#define ERROR_ACCESS_DENIED                    5U
#define ERROR_INVALID_HANDLE                   6U
#define ERROR_NOT_ENOUGH_MEMORY                8U
#define ERROR_INVALID_DATA                    13U
#define ERROR_WRITE_FAULT                     29U
#define ERROR_HANDLE_EOF                      38U
#define ERROR_INVALID_PARAMETER               87U
#define ERROR_INSUFFICIENT_BUFFER            122U
#define ERROR_INVALID_NAME                   123U
#define ERROR_SERVICE_ALREADY_RUNNING       1056U
#define ERROR_SERVICE_DISABLED              1058U
#define ERROR_SERVICE_DOES_NOT_EXIST        1060U
#define ERROR_SERVICE_CANNOT_ACCEPT_CTRL    1061U
#define ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 1063U
#define ERROR_SERVICE_NOT_ACTIVE           1062U
#define ERROR_SERVICE_EXISTS               1073U
#define ERROR_SERVICE_MARKED_FOR_DELETE     1072U
#define ERROR_SERVICE_DATABASE_LOCKED       1055U
#define ERROR_RPC_SERVER_UNAVAILABLE        1722U

#define SERVICE_CONFIG_DESCRIPTION          1U
#define SERVICE_CONFIG_FAILURE_ACTIONS      2U
#define SERVICE_CONFIG_DELAYED_AUTO_START_INFO 3U
#define SERVICE_CONFIG_FAILURE_ACTIONS_FLAG 4U
#define SERVICE_CONFIG_SERVICE_SID_INFO      5U
#define SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO 6U
#define SERVICE_CONFIG_PRESHUTDOWN_INFO      7U
#define SERVICE_CONFIG_TRIGGER_INFO          8U
#define SERVICE_CONFIG_PREFERRED_NODE        9U
#define SERVICE_CONFIG_LAUNCH_PROTECTED     12U

#define SE_DACL_PRESENT                      0x0004U
#define SE_SELF_RELATIVE                     0x8000U

typedef struct {
    DWORD service_type;
    DWORD current_state;
    DWORD controls_accepted;
    DWORD win32_exit_code;
    DWORD service_specific_exit_code;
    DWORD check_point;
    DWORD wait_hint;
    DWORD process_id;
    DWORD service_flags;
} SCM_STATUS_PROCESS;

typedef struct {
    BOOL used;
    BOOL marked_for_delete;
    BOOL delayed_auto_start;
    BOOL failure_actions_on_non_crash;
    USHORT generation;
    DWORD open_handles;
    DWORD service_type;
    DWORD start_type;
    DWORD error_control;
    DWORD tag_id;
    DWORD process_id;
    ULONG_PTR handler;
    ULONG_PTR handler_context;
    BOOL handler_extended;
    BOOL handler_compat32;
    SCM_STATUS_PROCESS status;
    char name[SCM_MAX_NAME];
    char display_name[SCM_MAX_NAME];
    char binary_path[SCM_MAX_PATH];
    char load_order_group[SCM_MAX_NAME];
    char dependencies[SCM_MAX_DEPENDENCIES];
    char account[SCM_MAX_ACCOUNT];
    char description[SCM_MAX_DESCRIPTION];
} SCM_SERVICE;

typedef struct {
    BOOL used;
    BYTE kind;
    USHORT generation;
    HANDLE value;
    DWORD owner_pid;
    DWORD access;
    USHORT service_index;
    USHORT service_generation;
} SCM_HANDLE_ENTRY;

typedef struct __attribute__((packed)) {
    uint32_t process;
    uint32_t thread;
    DWORD process_id;
    DWORD thread_id;
} SCM_PROCESS_INFORMATION32;

typedef struct {
    HANDLE process;
    HANDLE thread;
    DWORD process_id;
    DWORD thread_id;
} SCM_PROCESS_INFORMATION64;

typedef struct __attribute__((packed)) {
    DWORD service_type;
    DWORD start_type;
    DWORD error_control;
    uint32_t binary_path;
    uint32_t load_order_group;
    uint32_t tag_id;
    uint32_t dependencies;
    uint32_t account;
    uint32_t display_name;
} SCM_QUERY_CONFIG32;

typedef struct {
    DWORD service_type;
    DWORD start_type;
    DWORD error_control;
    DWORD padding;
    PWSTR binary_path;
    PWSTR load_order_group;
    DWORD *tag_id;
    PWSTR dependencies;
    PWSTR account;
    PWSTR display_name;
} SCM_QUERY_CONFIG64;

typedef struct __attribute__((packed)) {
    BYTE revision;
    BYTE reserved;
    USHORT control;
    DWORD owner;
    DWORD group;
    DWORD sacl;
    DWORD dacl;
} SCM_SECURITY_DESCRIPTOR_RELATIVE;

#define SCM_STORE_MAGIC   0x314D4353U /* "SCM1" */
#define SCM_STORE_VERSION 1U
#define SCM_STORE_PATH    "System\\Registry\\services.dat"
#define SCM_STORE_TEMP    "System\\Registry\\services.dat.new"

#define SCM_STORE_FLAG_DELAYED_AUTO_START       0x00000001U
#define SCM_STORE_FLAG_FAILURE_ACTIONS_NON_CRASH 0x00000002U

typedef struct {
    DWORD service_type;
    DWORD start_type;
    DWORD error_control;
    DWORD tag_id;
    DWORD flags;
    char name[SCM_MAX_NAME];
    char display_name[SCM_MAX_NAME];
    char binary_path[SCM_MAX_PATH];
    char load_order_group[SCM_MAX_NAME];
    char dependencies[SCM_MAX_DEPENDENCIES];
    char account[SCM_MAX_ACCOUNT];
    char description[SCM_MAX_DESCRIPTION];
} SCM_STORE_RECORD;

typedef struct {
    DWORD magic;
    DWORD version;
    DWORD record_size;
    DWORD record_count;
    DWORD crc32;
    DWORD reserved[3];
    SCM_STORE_RECORD records[SCM_MAX_SERVICES];
} SCM_STORE_IMAGE;

_Static_assert(sizeof(SCM_STATUS_PROCESS) == 36,
               "SERVICE_STATUS_PROCESS layout");
_Static_assert(sizeof(SCM_QUERY_CONFIG32) == 36,
               "PE32 QUERY_SERVICE_CONFIGW layout");
_Static_assert(sizeof(SCM_QUERY_CONFIG64) == 64,
               "PE64 QUERY_SERVICE_CONFIGW layout");
_Static_assert(sizeof(SCM_SECURITY_DESCRIPTOR_RELATIVE) == 20,
               "relative security descriptor layout");

static SCM_SERVICE scm_services[SCM_MAX_SERVICES];
static SCM_HANDLE_ENTRY scm_handles[SCM_MAX_HANDLES];
static spinlock_t scm_lock = SPINLOCK_INIT;
static spinlock_t scm_store_lock = SPINLOCK_INIT;
static SCM_STORE_IMAGE scm_store_image;
static volatile DWORD scm_store_state;

static void scm_zero(void *target, SIZE_T size)
{
    BYTE *bytes = (BYTE *)target;
    while (size--) *bytes++ = 0;
}

static SIZE_T scm_strlen(const char *text)
{
    SIZE_T length = 0;
    if (text) while (text[length]) length++;
    return length;
}

static char scm_fold(char value)
{
    if (value >= 'A' && value <= 'Z') return (char)(value + 32);
    return value;
}

static BOOL scm_equal(const char *left, const char *right)
{
    if (!left || !right) return left == right;
    while (*left && *right) {
        if (scm_fold(*left) != scm_fold(*right)) return FALSE;
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static void scm_copy(char *destination, SIZE_T capacity, const char *source)
{
    SIZE_T index = 0;
    if (!capacity) return;
    if (source) {
        while (source[index] && index + 1 < capacity) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = 0;
}

static BOOL scm_wide_to_ascii(char *destination, SIZE_T capacity,
                              PCWSTR source)
{
    SIZE_T index = 0;
    if (!destination || !capacity) return FALSE;
    if (!source) {
        destination[0] = 0;
        return TRUE;
    }
    while (source[index]) {
        if (index + 1 >= capacity) {
            destination[0] = 0;
            return FALSE;
        }
        destination[index] = source[index] <= 0x7FU
            ? (char)source[index] : '?';
        index++;
    }
    destination[index] = 0;
    return TRUE;
}

static SIZE_T scm_ascii_to_wide(PWSTR destination, SIZE_T capacity,
                                const char *source)
{
    SIZE_T length = scm_strlen(source);
    if (destination && capacity) {
        SIZE_T copy = length < capacity - 1 ? length : capacity - 1;
        for (SIZE_T i = 0; i < copy; i++)
            destination[i] = (WCHAR)(BYTE)source[i];
        destination[copy] = 0;
    }
    return length;
}

static uint64_t scm_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&scm_lock);
    return flags;
}

static void scm_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&scm_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static DWORD scm_current_pid(void)
{
    DWORD pid = win32_current_process_id();
    return pid ? pid : 1;
}

static DWORD scm_map_manager_access(DWORD access)
{
    if (access & (MAXIMUM_ALLOWED_ACCESS | GENERIC_ALL_ACCESS))
        return SC_MANAGER_ALL_ACCESS;
    if (access & GENERIC_READ_ACCESS)
        access |= SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE |
                  SC_MANAGER_QUERY_LOCK_STATUS | READ_CONTROL_ACCESS;
    if (access & GENERIC_WRITE_ACCESS)
        access |= SC_MANAGER_CREATE_SERVICE | SC_MANAGER_MODIFY_BOOT_CONFIG |
                  READ_CONTROL_ACCESS;
    if (access & GENERIC_EXECUTE_ACCESS)
        access |= SC_MANAGER_CONNECT | SC_MANAGER_LOCK | READ_CONTROL_ACCESS;
    return access & ~(GENERIC_ALL_ACCESS | GENERIC_READ_ACCESS |
                      GENERIC_WRITE_ACCESS | GENERIC_EXECUTE_ACCESS |
                      MAXIMUM_ALLOWED_ACCESS);
}

static DWORD scm_map_service_access(DWORD access)
{
    if (access & (MAXIMUM_ALLOWED_ACCESS | GENERIC_ALL_ACCESS))
        return SERVICE_ALL_ACCESS;
    if (access & GENERIC_READ_ACCESS)
        access |= SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
                  SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS |
                  READ_CONTROL_ACCESS;
    if (access & GENERIC_WRITE_ACCESS)
        access |= SERVICE_CHANGE_CONFIG | READ_CONTROL_ACCESS;
    if (access & GENERIC_EXECUTE_ACCESS)
        access |= SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE |
                  SERVICE_USER_DEFINED_CONTROL | READ_CONTROL_ACCESS;
    return access & ~(GENERIC_ALL_ACCESS | GENERIC_READ_ACCESS |
                      GENERIC_WRITE_ACCESS | GENERIC_EXECUTE_ACCESS |
                      MAXIMUM_ALLOWED_ACCESS);
}

static int scm_handle_index(HANDLE value)
{
    DWORD raw = (DWORD)(ULONG_PTR)value;
    if ((raw & SCM_HANDLE_PREFIX_MASK) != SCM_HANDLE_PREFIX)
        return -1;
    int index = (int)(raw & 0xFFU);
    return index < SCM_MAX_HANDLES ? index : -1;
}

static SCM_HANDLE_ENTRY *scm_lookup_handle_locked(HANDLE value, BYTE kind,
                                                   DWORD required_access)
{
    int index = scm_handle_index(value);
    if (index < 0) return NULL;
    SCM_HANDLE_ENTRY *entry = &scm_handles[index];
    if (!entry->used || entry->value != value || entry->kind != kind ||
        entry->owner_pid != scm_current_pid())
        return NULL;
    if (required_access &&
        (entry->access & required_access) != required_access)
        return NULL;
    return entry;
}

static SCM_SERVICE *scm_service_from_handle_locked(SCM_HANDLE_ENTRY *entry)
{
    if (!entry || entry->service_index >= SCM_MAX_SERVICES) return NULL;
    SCM_SERVICE *service = &scm_services[entry->service_index];
    if (!service->used || service->generation != entry->service_generation)
        return NULL;
    return service;
}

static HANDLE scm_allocate_handle_locked(BYTE kind, DWORD access,
                                         int service_index)
{
    for (int i = 0; i < SCM_MAX_HANDLES; i++) {
        if (scm_handles[i].used) continue;
        USHORT generation = (USHORT)((scm_handles[i].generation + 1) & 0x0FFFU);
        if (!generation) generation = 1;
        scm_zero(&scm_handles[i], sizeof(scm_handles[i]));
        SCM_HANDLE_ENTRY *entry = &scm_handles[i];
        entry->used = TRUE;
        entry->kind = kind;
        entry->generation = generation;
        entry->owner_pid = scm_current_pid();
        entry->access = access;
        entry->service_index = service_index >= 0 ? (USHORT)service_index
                                                  : (USHORT)0xFFFFU;
        if (service_index >= 0) {
            entry->service_generation = scm_services[service_index].generation;
            if (kind == SCM_KIND_SERVICE)
                scm_services[service_index].open_handles++;
        }
        entry->value = (HANDLE)(ULONG_PTR)(SCM_HANDLE_PREFIX |
            ((DWORD)generation << 8) | (DWORD)i);
        return entry->value;
    }
    return NULL;
}

static int scm_find_service_locked(const char *name)
{
    for (int i = 0; i < SCM_MAX_SERVICES; i++)
        if (scm_services[i].used && scm_equal(scm_services[i].name, name))
            return i;
    return -1;
}

static int scm_allocate_service_locked(void)
{
    for (int i = 0; i < SCM_MAX_SERVICES; i++) {
        if (scm_services[i].used) continue;
        USHORT generation = (USHORT)(scm_services[i].generation + 1);
        if (!generation) generation = 1;
        scm_zero(&scm_services[i], sizeof(scm_services[i]));
        scm_services[i].used = TRUE;
        scm_services[i].generation = generation;
        scm_services[i].status.current_state = SERVICE_STOPPED;
        return i;
    }
    return -1;
}

static void scm_maybe_delete_service_locked(int index)
{
    if (index < 0 || index >= SCM_MAX_SERVICES) return;
    SCM_SERVICE *service = &scm_services[index];
    if (!service->used || !service->marked_for_delete ||
        service->open_handles ||
        service->status.current_state != SERVICE_STOPPED)
        return;
    USHORT generation = service->generation;
    scm_zero(service, sizeof(*service));
    service->generation = generation;
}

static BOOL scm_store_string_valid(const char *text, SIZE_T capacity)
{
    for (SIZE_T i = 0; i < capacity; i++)
        if (!text[i]) return TRUE;
    return FALSE;
}

static void scm_store_record_from_service(SCM_STORE_RECORD *record,
                                          const SCM_SERVICE *service)
{
    record->service_type = service->service_type;
    record->start_type = service->start_type;
    record->error_control = service->error_control;
    record->tag_id = service->tag_id;
    record->flags = 0;
    if (service->delayed_auto_start)
        record->flags |= SCM_STORE_FLAG_DELAYED_AUTO_START;
    if (service->failure_actions_on_non_crash)
        record->flags |= SCM_STORE_FLAG_FAILURE_ACTIONS_NON_CRASH;
    scm_copy(record->name, sizeof(record->name), service->name);
    scm_copy(record->display_name, sizeof(record->display_name),
             service->display_name);
    scm_copy(record->binary_path, sizeof(record->binary_path),
             service->binary_path);
    scm_copy(record->load_order_group, sizeof(record->load_order_group),
             service->load_order_group);
    scm_copy(record->dependencies, sizeof(record->dependencies),
             service->dependencies);
    scm_copy(record->account, sizeof(record->account), service->account);
    scm_copy(record->description, sizeof(record->description),
             service->description);
}

static void scm_store_apply_config(SCM_SERVICE *service,
                                   const SCM_STORE_RECORD *record)
{
    service->service_type = record->service_type;
    service->start_type = record->start_type;
    service->error_control = record->error_control;
    service->tag_id = record->tag_id;
    service->delayed_auto_start =
        (record->flags & SCM_STORE_FLAG_DELAYED_AUTO_START) != 0;
    service->failure_actions_on_non_crash =
        (record->flags & SCM_STORE_FLAG_FAILURE_ACTIONS_NON_CRASH) != 0;
    service->status.service_type = record->service_type;
    scm_copy(service->name, sizeof(service->name), record->name);
    scm_copy(service->display_name, sizeof(service->display_name),
             record->display_name);
    scm_copy(service->binary_path, sizeof(service->binary_path),
             record->binary_path);
    scm_copy(service->load_order_group, sizeof(service->load_order_group),
             record->load_order_group);
    scm_copy(service->dependencies, sizeof(service->dependencies),
             record->dependencies);
    scm_copy(service->account, sizeof(service->account), record->account);
    scm_copy(service->description, sizeof(service->description),
             record->description);
}

static void scm_store_service_from_record(SCM_SERVICE *service,
                                          const SCM_STORE_RECORD *record)
{
    scm_store_apply_config(service, record);
    service->status.current_state = SERVICE_STOPPED;
}

static DWORD scm_store_build_image_locked(SCM_STORE_IMAGE *image)
{
    scm_zero(image, sizeof(*image));
    image->magic = SCM_STORE_MAGIC;
    image->version = SCM_STORE_VERSION;
    image->record_size = sizeof(SCM_STORE_RECORD);
    for (int i = 0; i < SCM_MAX_SERVICES; i++) {
        const SCM_SERVICE *service = &scm_services[i];
        if (!service->used || service->marked_for_delete) continue;
        scm_store_record_from_service(&image->records[image->record_count],
                                      service);
        image->record_count++;
    }
    image->crc32 = osfs3_crc32(image, sizeof(*image));
    return image->record_count;
}

static BOOL scm_store_record_valid(const SCM_STORE_RECORD *record)
{
    const DWORD known_flags = SCM_STORE_FLAG_DELAYED_AUTO_START |
                              SCM_STORE_FLAG_FAILURE_ACTIONS_NON_CRASH;
    return record->name[0] && record->binary_path[0] &&
        !(record->flags & ~known_flags) &&
        scm_store_string_valid(record->name, sizeof(record->name)) &&
        scm_store_string_valid(record->display_name,
                               sizeof(record->display_name)) &&
        scm_store_string_valid(record->binary_path,
                               sizeof(record->binary_path)) &&
        scm_store_string_valid(record->load_order_group,
                               sizeof(record->load_order_group)) &&
        scm_store_string_valid(record->dependencies,
                               sizeof(record->dependencies)) &&
        scm_store_string_valid(record->account, sizeof(record->account)) &&
        scm_store_string_valid(record->description,
                               sizeof(record->description));
}

static BOOL scm_store_image_valid(SCM_STORE_IMAGE *image)
{
    if (image->magic != SCM_STORE_MAGIC ||
        image->version != SCM_STORE_VERSION ||
        image->record_size != sizeof(SCM_STORE_RECORD) ||
        image->record_count > SCM_MAX_SERVICES)
        return FALSE;

    DWORD stored_crc = image->crc32;
    image->crc32 = 0;
    DWORD calculated_crc = osfs3_crc32(image, sizeof(*image));
    image->crc32 = stored_crc;
    if (stored_crc != calculated_crc) return FALSE;

    for (DWORD i = 0; i < image->record_count; i++) {
        if (!scm_store_record_valid(&image->records[i])) return FALSE;
        for (DWORD j = 0; j < i; j++)
            if (scm_equal(image->records[i].name, image->records[j].name))
                return FALSE;
    }
    return TRUE;
}

static void scm_store_reset_services_locked(void)
{
    for (int i = 0; i < SCM_MAX_SERVICES; i++) {
        USHORT generation = scm_services[i].generation;
        scm_zero(&scm_services[i], sizeof(scm_services[i]));
        scm_services[i].generation = generation;
    }
}

static BOOL scm_store_load_locked(void)
{
    void *file = osfs3_find_ci(SCM_STORE_PATH);
    if (!file) {
        serial_puts("[SCM-STORE] no service database\n");
        return TRUE;
    }
    if (osfs3_file_size(file) != sizeof(scm_store_image) ||
        osfs3_read_file(file, 0, &scm_store_image,
                        sizeof(scm_store_image)) !=
            (int)sizeof(scm_store_image) ||
        !scm_store_image_valid(&scm_store_image)) {
        serial_puts("[SCM-STORE] invalid service database\n");
        return FALSE;
    }

    uint64_t flags = scm_lock_irqsave();
    scm_store_reset_services_locked();
    for (DWORD i = 0; i < scm_store_image.record_count; i++) {
        int index = scm_allocate_service_locked();
        if (index < 0) {
            scm_store_reset_services_locked();
            scm_unlock_irqrestore(flags);
            serial_puts("[SCM-STORE] service database exceeds capacity\n");
            return FALSE;
        }
        scm_store_service_from_record(&scm_services[index],
                                      &scm_store_image.records[i]);
    }
    DWORD count = scm_store_image.record_count;
    scm_unlock_irqrestore(flags);

    serial_puts("[SCM-STORE] loaded services=");
    serial_putdec(count);
    serial_puts("\n");
    return TRUE;
}

static BOOL scm_store_prepare_directories(void)
{
    if (!osfs3_directory_exists_ci("System") &&
        osfs3_mkdir("System") < 0)
        return FALSE;
    if (!osfs3_directory_exists_ci("System\\Registry") &&
        osfs3_mkdir("System\\Registry") < 0)
        return FALSE;
    return TRUE;
}

/* scm_store_lock must be held. */
static BOOL scm_store_save_locked(void)
{
    if (!osfs3_is_mounted() || !scm_store_prepare_directories())
        return FALSE;

    uint64_t flags = scm_lock_irqsave();
    DWORD count = scm_store_build_image_locked(&scm_store_image);
    scm_unlock_irqrestore(flags);

    (void)osfs3_delete(SCM_STORE_TEMP);
    void *file = osfs3_create(SCM_STORE_TEMP, sizeof(scm_store_image));
    if (!file ||
        osfs3_write(file, 0, &scm_store_image,
                    sizeof(scm_store_image)) < 0 ||
        osfs3_truncate(file, sizeof(scm_store_image)) < 0 ||
        osfs3_rename(SCM_STORE_TEMP, SCM_STORE_PATH, true) < 0) {
        (void)osfs3_delete(SCM_STORE_TEMP);
        serial_puts("[SCM-STORE] publish failed\n");
        return FALSE;
    }

    serial_puts("[SCM-STORE] saved services=");
    serial_putdec(count);
    serial_puts("\n");
    return TRUE;
}

static void scm_ensure_initialized(void)
{
    DWORD state = __atomic_load_n(&scm_store_state, __ATOMIC_ACQUIRE);
    if (state == 2 || !osfs3_is_mounted()) return;

    DWORD expected = 0;
    if (__atomic_compare_exchange_n(&scm_store_state, &expected, 1, FALSE,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        spin_lock(&scm_store_lock);
        (void)scm_store_load_locked();
        spin_unlock(&scm_store_lock);
        __atomic_store_n(&scm_store_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&scm_store_state, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile ("pause");
}

static BOOL scm_machine_is_local(PCWSTR machine_name)
{
    char machine[64];
    if (!machine_name || !*machine_name) return TRUE;
    if (!scm_wide_to_ascii(machine, sizeof(machine), machine_name))
        return FALSE;
    const char *name = machine;
    if (name[0] == '\\' && name[1] == '\\') name += 2;
    return scm_equal(name, ".") || scm_equal(name, "localhost") ||
           scm_equal(name, "ositok");
}

static BOOL scm_database_is_valid(PCWSTR database_name)
{
    char database[64];
    if (!database_name || !*database_name) return TRUE;
    if (!scm_wide_to_ascii(database, sizeof(database), database_name))
        return FALSE;
    return scm_equal(database, "ServicesActive");
}

HANDLE WINAPI OpenSCManagerW(PCWSTR machine_name, PCWSTR database_name,
                             DWORD desired_access)
{
    if (!scm_machine_is_local(machine_name)) {
        SetLastError(ERROR_RPC_SERVER_UNAVAILABLE);
        return NULL;
    }
    if (!scm_database_is_valid(database_name)) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }

    scm_ensure_initialized();
    uint64_t flags = scm_lock_irqsave();
    HANDLE handle = scm_allocate_handle_locked(
        SCM_KIND_MANAGER, scm_map_manager_access(desired_access), -1);
    scm_unlock_irqrestore(flags);
    if (!handle) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    SetLastError(0);
    return handle;
}

HANDLE WINAPI OpenServiceW(HANDLE manager, PCWSTR service_name,
                           DWORD desired_access)
{
    char name[SCM_MAX_NAME];
    if (!service_name || !*service_name ||
        !scm_wide_to_ascii(name, sizeof(name), service_name)) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }

    scm_ensure_initialized();
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *manager_entry = scm_lookup_handle_locked(
        manager, SCM_KIND_MANAGER, SC_MANAGER_CONNECT);
    if (!manager_entry) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    int index = scm_find_service_locked(name);
    if (index < 0) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
        return NULL;
    }
    if (scm_services[index].marked_for_delete) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE);
        return NULL;
    }
    HANDLE handle = scm_allocate_handle_locked(
        SCM_KIND_SERVICE, scm_map_service_access(desired_access), index);
    scm_unlock_irqrestore(flags);
    if (!handle) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    SetLastError(0);
    return handle;
}

HANDLE WINAPI CreateServiceW(HANDLE manager, PCWSTR service_name,
                             PCWSTR display_name, DWORD desired_access,
                             DWORD service_type, DWORD start_type,
                             DWORD error_control, PCWSTR binary_path,
                             PCWSTR load_order_group, DWORD *tag_id,
                             PCWSTR dependencies, PCWSTR service_start_name,
                             PCWSTR password)
{
    char name[SCM_MAX_NAME];
    char path[SCM_MAX_PATH];
    char display[SCM_MAX_NAME];
    char group[SCM_MAX_NAME];
    char deps[SCM_MAX_DEPENDENCIES];
    char account[SCM_MAX_ACCOUNT];
    (void)password;

    if (!service_name || !*service_name || !binary_path || !*binary_path ||
        !scm_wide_to_ascii(name, sizeof(name), service_name) ||
        !scm_wide_to_ascii(path, sizeof(path), binary_path) ||
        !scm_wide_to_ascii(display, sizeof(display),
                           display_name ? display_name : service_name) ||
        !scm_wide_to_ascii(group, sizeof(group), load_order_group) ||
        !scm_wide_to_ascii(deps, sizeof(deps), dependencies) ||
        !scm_wide_to_ascii(account, sizeof(account), service_start_name)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!account[0]) scm_copy(account, sizeof(account), "LocalSystem");

    scm_ensure_initialized();
    spin_lock(&scm_store_lock);
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *manager_entry = scm_lookup_handle_locked(
        manager, SCM_KIND_MANAGER, SC_MANAGER_CREATE_SERVICE);
    if (!manager_entry) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    int existing = scm_find_service_locked(name);
    if (existing >= 0) {
        DWORD error = scm_services[existing].marked_for_delete
            ? ERROR_SERVICE_MARKED_FOR_DELETE : ERROR_SERVICE_EXISTS;
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(error);
        return NULL;
    }
    int index = scm_allocate_service_locked();
    if (index < 0) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_SERVICE_DATABASE_LOCKED);
        return NULL;
    }

    SCM_SERVICE *service = &scm_services[index];
    service->service_type = service_type;
    service->start_type = start_type;
    service->error_control = error_control;
    service->tag_id = 0;
    service->status.service_type = service_type;
    scm_copy(service->name, sizeof(service->name), name);
    scm_copy(service->display_name, sizeof(service->display_name), display);
    scm_copy(service->binary_path, sizeof(service->binary_path), path);
    scm_copy(service->load_order_group, sizeof(service->load_order_group), group);
    scm_copy(service->dependencies, sizeof(service->dependencies), deps);
    scm_copy(service->account, sizeof(service->account), account);
    HANDLE handle = scm_allocate_handle_locked(
        SCM_KIND_SERVICE, scm_map_service_access(desired_access), index);
    if (!handle) {
        USHORT generation = service->generation;
        scm_zero(service, sizeof(*service));
        service->generation = generation;
    }
    scm_unlock_irqrestore(flags);

    if (!handle) {
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!scm_store_save_locked()) {
        flags = scm_lock_irqsave();
        int handle_index = scm_handle_index(handle);
        if (handle_index >= 0 && scm_handles[handle_index].used &&
            scm_handles[handle_index].value == handle) {
            USHORT handle_generation = scm_handles[handle_index].generation;
            scm_zero(&scm_handles[handle_index],
                     sizeof(scm_handles[handle_index]));
            scm_handles[handle_index].generation = handle_generation;
        }
        service = &scm_services[index];
        USHORT service_generation = service->generation;
        scm_zero(service, sizeof(*service));
        service->generation = service_generation;
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_WRITE_FAULT);
        return NULL;
    }
    spin_unlock(&scm_store_lock);
    if (tag_id) *tag_id = 0;
    serial_puts("[SCM] created service '");
    serial_puts(name);
    serial_puts("' image='");
    serial_puts(path);
    serial_puts("'\n");
    SetLastError(0);
    return handle;
}

BOOL WINAPI ChangeServiceConfigW(HANDLE service_handle, DWORD service_type,
                                 DWORD start_type, DWORD error_control,
                                 PCWSTR binary_path, PCWSTR load_order_group,
                                 DWORD *tag_id, PCWSTR dependencies,
                                 PCWSTR service_start_name, PCWSTR password,
                                 PCWSTR display_name)
{
    char path[SCM_MAX_PATH], group[SCM_MAX_NAME];
    char deps[SCM_MAX_DEPENDENCIES], account[SCM_MAX_ACCOUNT];
    char display[SCM_MAX_NAME];
    (void)password;
    if ((binary_path && !scm_wide_to_ascii(path, sizeof(path), binary_path)) ||
        (load_order_group &&
         !scm_wide_to_ascii(group, sizeof(group), load_order_group)) ||
        (dependencies && !scm_wide_to_ascii(deps, sizeof(deps), dependencies)) ||
        (service_start_name &&
         !scm_wide_to_ascii(account, sizeof(account), service_start_name)) ||
        (display_name &&
         !scm_wide_to_ascii(display, sizeof(display), display_name))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    scm_ensure_initialized();
    spin_lock(&scm_store_lock);
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_CHANGE_CONFIG);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service || service->marked_for_delete) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(service ? ERROR_SERVICE_MARKED_FOR_DELETE
                             : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    int service_index = (int)(service - scm_services);
    USHORT service_generation = service->generation;
    SCM_STORE_RECORD original;
    scm_zero(&original, sizeof(original));
    scm_store_record_from_service(&original, service);
    if (service_type != SERVICE_NO_CHANGE) {
        service->service_type = service_type;
        service->status.service_type = service_type;
    }
    if (start_type != SERVICE_NO_CHANGE) service->start_type = start_type;
    if (error_control != SERVICE_NO_CHANGE)
        service->error_control = error_control;
    if (binary_path) scm_copy(service->binary_path,
                              sizeof(service->binary_path), path);
    if (load_order_group) scm_copy(service->load_order_group,
                                   sizeof(service->load_order_group), group);
    if (dependencies) scm_copy(service->dependencies,
                               sizeof(service->dependencies), deps);
    if (service_start_name) scm_copy(service->account,
                                     sizeof(service->account), account);
    if (display_name) scm_copy(service->display_name,
                               sizeof(service->display_name), display);
    DWORD tag = service->tag_id;
    scm_unlock_irqrestore(flags);
    if (!scm_store_save_locked()) {
        flags = scm_lock_irqsave();
        service = &scm_services[service_index];
        if (service->used && service->generation == service_generation)
            scm_store_apply_config(service, &original);
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    spin_unlock(&scm_store_lock);
    if (tag_id) *tag_id = tag;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI ChangeServiceConfig2W(HANDLE service_handle, DWORD info_level,
                                  PVOID info)
{
    if (!info) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    scm_ensure_initialized();
    spin_lock(&scm_store_lock);
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_CHANGE_CONFIG);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service || service->marked_for_delete) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(service ? ERROR_SERVICE_MARKED_FOR_DELETE
                             : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    int service_index = (int)(service - scm_services);
    USHORT service_generation = service->generation;
    SCM_STORE_RECORD original;
    scm_zero(&original, sizeof(original));
    scm_store_record_from_service(&original, service);

    switch (info_level) {
    case SERVICE_CONFIG_DESCRIPTION: {
        PCWSTR description = g_compat32_mode
            ? (PCWSTR)(ULONG_PTR)*(uint32_t *)info
            : *(PCWSTR *)info;
        char ascii[SCM_MAX_DESCRIPTION];
        if (!scm_wide_to_ascii(ascii, sizeof(ascii), description)) {
            scm_unlock_irqrestore(flags);
            spin_unlock(&scm_store_lock);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        scm_copy(service->description, sizeof(service->description), ascii);
        break;
    }
    case SERVICE_CONFIG_DELAYED_AUTO_START_INFO:
        service->delayed_auto_start = *(BOOL *)info != FALSE;
        break;
    case SERVICE_CONFIG_FAILURE_ACTIONS_FLAG:
        service->failure_actions_on_non_crash = *(BOOL *)info != FALSE;
        break;
    case SERVICE_CONFIG_FAILURE_ACTIONS:
    case SERVICE_CONFIG_SERVICE_SID_INFO:
    case SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO:
    case SERVICE_CONFIG_PRESHUTDOWN_INFO:
    case SERVICE_CONFIG_TRIGGER_INFO:
    case SERVICE_CONFIG_PREFERRED_NODE:
    case SERVICE_CONFIG_LAUNCH_PROTECTED:
        /* These records are accepted and retained by Windows even when the
         * corresponding policy is not exercised during this boot. */
        break;
    default:
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    scm_unlock_irqrestore(flags);
    if (!scm_store_save_locked()) {
        flags = scm_lock_irqsave();
        service = &scm_services[service_index];
        if (service->used && service->generation == service_generation)
            scm_store_apply_config(service, &original);
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    spin_unlock(&scm_store_lock);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI DeleteService(HANDLE service_handle)
{
    scm_ensure_initialized();
    spin_lock(&scm_store_lock);
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, DELETE_ACCESS);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (service->marked_for_delete) {
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE);
        return FALSE;
    }
    int service_index = (int)(service - scm_services);
    USHORT service_generation = service->generation;
    service->marked_for_delete = TRUE;
    scm_unlock_irqrestore(flags);
    if (!scm_store_save_locked()) {
        flags = scm_lock_irqsave();
        service = &scm_services[service_index];
        if (service->used && service->generation == service_generation)
            service->marked_for_delete = FALSE;
        scm_unlock_irqrestore(flags);
        spin_unlock(&scm_store_lock);
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    spin_unlock(&scm_store_lock);
    SetLastError(0);
    return TRUE;
}

static BOOL scm_append_wide(PWSTR command, SIZE_T capacity, SIZE_T *length,
                            PCWSTR text, BOOL quote)
{
    SIZE_T at = *length;
    if (at && at + 1 < capacity) command[at++] = ' ';
    else if (at) return FALSE;
    if (quote) {
        if (at + 1 >= capacity) return FALSE;
        command[at++] = '"';
    }
    for (SIZE_T i = 0; text && text[i]; i++) {
        if (at + 1 >= capacity) return FALSE;
        command[at++] = text[i];
    }
    if (quote) {
        if (at + 1 >= capacity) return FALSE;
        command[at++] = '"';
    }
    command[at] = 0;
    *length = at;
    return TRUE;
}

BOOL WINAPI StartServiceW(HANDLE service_handle, DWORD argc, PVOID argv)
{
    char binary_path[SCM_MAX_PATH];
    int service_index;
    USHORT service_generation;

    if (argc && !argv) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    scm_ensure_initialized();
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_START);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (service->marked_for_delete) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE);
        return FALSE;
    }
    if (service->start_type == SERVICE_DISABLED) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_DISABLED);
        return FALSE;
    }
    if (service->status.current_state != SERVICE_STOPPED) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_ALREADY_RUNNING);
        return FALSE;
    }
    if (!service->binary_path[0]) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    service_index = (int)(service - scm_services);
    service_generation = service->generation;
    scm_copy(binary_path, sizeof(binary_path), service->binary_path);
    scm_unlock_irqrestore(flags);

    WCHAR command[2048];
    SIZE_T command_length = scm_ascii_to_wide(command,
                                               sizeof(command) / sizeof(command[0]),
                                               binary_path);
    if (command_length >= sizeof(command) / sizeof(command[0]) - 1) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (DWORD i = 0; i < argc; i++) {
        PCWSTR argument = g_compat32_mode
            ? (PCWSTR)(ULONG_PTR)((uint32_t *)argv)[i]
            : ((PCWSTR *)argv)[i];
        BOOL quote = FALSE;
        for (SIZE_T j = 0; argument && argument[j]; j++)
            if (argument[j] == ' ' || argument[j] == '\t') quote = TRUE;
        if (!argument || !scm_append_wide(command,
                sizeof(command) / sizeof(command[0]), &command_length,
                argument, quote)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    flags = scm_lock_irqsave();
    service = service_index >= 0 && service_index < SCM_MAX_SERVICES
        ? &scm_services[service_index] : NULL;
    if (!service || !service->used ||
        service->generation != service_generation) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
        return FALSE;
    }
    if (service->marked_for_delete) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE);
        return FALSE;
    }
    if (service->status.current_state != SERVICE_STOPPED) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_ALREADY_RUNNING);
        return FALSE;
    }
    service->status.current_state = SERVICE_START_PENDING;
    service->status.win32_exit_code = 0;
    service->status.service_specific_exit_code = 0;
    service->status.check_point = 1;
    service->status.wait_hint = 30000;
    service->status.process_id = 0;
    service->process_id = 0;
    scm_unlock_irqrestore(flags);

    BOOL caller32 = g_compat32_mode;
    SCM_PROCESS_INFORMATION32 pi32;
    SCM_PROCESS_INFORMATION64 pi64;
    scm_zero(&pi32, sizeof(pi32));
    scm_zero(&pi64, sizeof(pi64));
    PVOID process_info = caller32 ? (PVOID)&pi32 : (PVOID)&pi64;
    BOOL started = CreateProcessW(NULL, command, NULL, NULL, FALSE, 0,
                                  NULL, NULL, NULL, process_info);
    DWORD error = started ? 0 : GetLastError();
    DWORD process_id = caller32 ? pi32.process_id : pi64.process_id;
    HANDLE process = caller32 ? (HANDLE)(ULONG_PTR)pi32.process : pi64.process;
    HANDLE thread = caller32 ? (HANDLE)(ULONG_PTR)pi32.thread : pi64.thread;
    if (process) CloseHandle(process);
    if (thread) CloseHandle(thread);

    flags = scm_lock_irqsave();
    service = service_index >= 0 && service_index < SCM_MAX_SERVICES
        ? &scm_services[service_index] : NULL;
    if (service && service->used &&
        service->generation == service_generation) {
        if (!started) {
            service->status.current_state = SERVICE_STOPPED;
            service->status.win32_exit_code = error;
            service->status.process_id = 0;
            service->process_id = 0;
        } else if (service->status.current_state == SERVICE_START_PENDING) {
            service->process_id = process_id;
            service->status.process_id = process_id;
        }
    }
    scm_unlock_irqrestore(flags);

    if (!started) {
        serial_puts("[SCM] start failed image='");
        serial_puts(binary_path);
        serial_puts("' error=");
        serial_putdec(error);
        serial_puts("\n");
        SetLastError(error);
        return FALSE;
    }
    serial_puts("[SCM] start image='");
    serial_puts(binary_path);
    serial_puts("' pid=");
    serial_putdec(process_id);
    serial_puts("\n");
    SetLastError(0);
    return TRUE;
}

static void scm_copy_basic_status(PVOID destination,
                                  const SCM_STATUS_PROCESS *status)
{
    DWORD *output = (DWORD *)destination;
    output[0] = status->service_type;
    output[1] = status->current_state;
    output[2] = status->controls_accepted;
    output[3] = status->win32_exit_code;
    output[4] = status->service_specific_exit_code;
    output[5] = status->check_point;
    output[6] = status->wait_hint;
}

BOOL WINAPI QueryServiceStatus(HANDLE service_handle, PVOID status)
{
    if (!status) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_QUERY_STATUS);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    SCM_STATUS_PROCESS snapshot = service->status;
    scm_unlock_irqrestore(flags);
    scm_copy_basic_status(status, &snapshot);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI QueryServiceStatusEx(HANDLE service_handle, DWORD info_level,
                                 BYTE *buffer, DWORD buffer_size,
                                 DWORD *bytes_needed)
{
    if (!bytes_needed || info_level != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *bytes_needed = sizeof(SCM_STATUS_PROCESS);
    if (!buffer || buffer_size < sizeof(SCM_STATUS_PROCESS)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_QUERY_STATUS);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *(SCM_STATUS_PROCESS *)buffer = service->status;
    scm_unlock_irqrestore(flags);
    SetLastError(0);
    return TRUE;
}

static DWORD scm_config_string_bytes(const char *text, BOOL multi_string)
{
    DWORD chars = (DWORD)scm_strlen(text) + 1;
    if (multi_string) chars++;
    return chars * sizeof(WCHAR);
}

static PWSTR scm_config_write_string(BYTE *buffer, DWORD *offset,
                                     const char *text, BOOL multi_string)
{
    PWSTR destination = (PWSTR)(void *)(buffer + *offset);
    DWORD length = (DWORD)scm_strlen(text);
    for (DWORD i = 0; i < length; i++)
        destination[i] = (WCHAR)(BYTE)text[i];
    destination[length] = 0;
    if (multi_string) destination[length + 1] = 0;
    *offset += (length + 1 + (multi_string ? 1 : 0)) * sizeof(WCHAR);
    return destination;
}

BOOL WINAPI QueryServiceConfigW(HANDLE service_handle, BYTE *buffer,
                                DWORD buffer_size, DWORD *bytes_needed)
{
    if (!bytes_needed) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, SERVICE_QUERY_CONFIG);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    SCM_SERVICE snapshot = *service;
    scm_unlock_irqrestore(flags);

    DWORD header_size = g_compat32_mode ? sizeof(SCM_QUERY_CONFIG32)
                                        : sizeof(SCM_QUERY_CONFIG64);
    DWORD required = header_size + sizeof(DWORD) +
        scm_config_string_bytes(snapshot.binary_path, FALSE) +
        scm_config_string_bytes(snapshot.load_order_group, FALSE) +
        scm_config_string_bytes(snapshot.dependencies, TRUE) +
        scm_config_string_bytes(snapshot.account, FALSE) +
        scm_config_string_bytes(snapshot.display_name, FALSE);
    *bytes_needed = required;
    if (!buffer || buffer_size < required) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    scm_zero(buffer, required);
    DWORD offset = header_size;
    DWORD *tag = (DWORD *)(void *)(buffer + offset);
    *tag = snapshot.tag_id;
    offset += sizeof(DWORD);
    PWSTR binary = scm_config_write_string(buffer, &offset,
                                           snapshot.binary_path, FALSE);
    PWSTR group = scm_config_write_string(buffer, &offset,
                                          snapshot.load_order_group, FALSE);
    PWSTR dependencies = scm_config_write_string(buffer, &offset,
                                                 snapshot.dependencies, TRUE);
    PWSTR account = scm_config_write_string(buffer, &offset,
                                            snapshot.account, FALSE);
    PWSTR display = scm_config_write_string(buffer, &offset,
                                            snapshot.display_name, FALSE);
    if (g_compat32_mode) {
        SCM_QUERY_CONFIG32 *config = (SCM_QUERY_CONFIG32 *)buffer;
        config->service_type = snapshot.service_type;
        config->start_type = snapshot.start_type;
        config->error_control = snapshot.error_control;
        config->binary_path = (uint32_t)(ULONG_PTR)binary;
        config->load_order_group = (uint32_t)(ULONG_PTR)group;
        config->tag_id = (uint32_t)(ULONG_PTR)tag;
        config->dependencies = (uint32_t)(ULONG_PTR)dependencies;
        config->account = (uint32_t)(ULONG_PTR)account;
        config->display_name = (uint32_t)(ULONG_PTR)display;
    } else {
        SCM_QUERY_CONFIG64 *config = (SCM_QUERY_CONFIG64 *)buffer;
        config->service_type = snapshot.service_type;
        config->start_type = snapshot.start_type;
        config->error_control = snapshot.error_control;
        config->binary_path = binary;
        config->load_order_group = group;
        config->tag_id = tag;
        config->dependencies = dependencies;
        config->account = account;
        config->display_name = display;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI CloseServiceHandle(HANDLE object)
{
    int index = scm_handle_index(object);
    if (index < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = &scm_handles[index];
    if (!entry->used || entry->value != object ||
        entry->owner_pid != scm_current_pid() ||
        (entry->kind != SCM_KIND_MANAGER && entry->kind != SCM_KIND_SERVICE)) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    int service_index = -1;
    if (entry->kind == SCM_KIND_SERVICE &&
        entry->service_index < SCM_MAX_SERVICES) {
        service_index = entry->service_index;
        SCM_SERVICE *service = scm_service_from_handle_locked(entry);
        if (service && service->open_handles) service->open_handles--;
    }
    USHORT generation = entry->generation;
    scm_zero(entry, sizeof(*entry));
    entry->generation = generation;
    scm_maybe_delete_service_locked(service_index);
    scm_unlock_irqrestore(flags);
    SetLastError(0);
    return TRUE;
}

static int scm_dispatch_service_index_locked(const char *name, DWORD pid)
{
    if (name && *name) {
        int index = scm_find_service_locked(name);
        if (index >= 0 &&
            (scm_services[index].process_id == 0 ||
             scm_services[index].process_id == pid))
            return index;
    }
    int candidate = -1;
    for (int i = 0; i < SCM_MAX_SERVICES; i++) {
        SCM_SERVICE *service = &scm_services[i];
        if (!service->used || service->marked_for_delete) continue;
        if (service->process_id == pid) return i;
        if (service->status.current_state == SERVICE_START_PENDING &&
            service->process_id == 0) {
            if (candidate >= 0) return -1;
            candidate = i;
        }
    }
    return candidate;
}

BOOL WINAPI StartServiceCtrlDispatcherW(PVOID service_table)
{
    if (!service_table) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    scm_ensure_initialized();
    BOOL compat32 = g_compat32_mode;
    DWORD pid = scm_current_pid();
    BOOL dispatched = FALSE;

    for (DWORD table_index = 0; table_index < SCM_MAX_SERVICES; table_index++) {
        PCWSTR wide_name;
        ULONG_PTR procedure;
        if (compat32) {
            const uint32_t *entry = (const uint32_t *)service_table +
                                    table_index * 2;
            wide_name = (PCWSTR)(ULONG_PTR)entry[0];
            procedure = entry[1];
        } else {
            const ULONG_PTR *entry = (const ULONG_PTR *)service_table +
                                     table_index * 2;
            wide_name = (PCWSTR)entry[0];
            procedure = entry[1];
        }
        if (!procedure) break;

        char name[SCM_MAX_NAME];
        if (!scm_wide_to_ascii(name, sizeof(name), wide_name)) {
            SetLastError(ERROR_INVALID_NAME);
            return FALSE;
        }
        uint64_t flags = scm_lock_irqsave();
        int service_index = scm_dispatch_service_index_locked(name, pid);
        if (service_index >= 0) {
            SCM_SERVICE *service = &scm_services[service_index];
            service->process_id = pid;
            service->status.process_id = pid;
            if (service->status.current_state == SERVICE_STOPPED)
                service->status.current_state = SERVICE_START_PENDING;
        }
        scm_unlock_irqrestore(flags);
        if (service_index < 0) continue;

        serial_puts("[SCM] dispatcher attached service='");
        serial_puts(name);
        serial_puts("' pid=");
        serial_putdec(pid);
        serial_puts(" arch=");
        serial_puts(compat32 ? "32\n" : "64\n");

        if (compat32) {
            uint32_t arguments[2] = {0, 0};
            void *wire = VirtualAlloc(NULL, 4096,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
            if (!wire || (ULONG_PTR)wire > 0xFFFFFFFFULL) {
                if (wire) VirtualFree(wire, 0, MEM_RELEASE);
                flags = scm_lock_irqsave();
                SCM_SERVICE *service = &scm_services[service_index];
                if (service->used && service->process_id == pid) {
                    service->status.current_state = SERVICE_STOPPED;
                    service->status.win32_exit_code = ERROR_NOT_ENOUGH_MEMORY;
                    service->status.process_id = 0;
                    service->process_id = 0;
                }
                scm_unlock_irqrestore(flags);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }
            uint32_t *argv32 = (uint32_t *)wire;
            WCHAR *name32 = (WCHAR *)(argv32 + 2);
            scm_ascii_to_wide(name32, 256, name);
            argv32[0] = (uint32_t)(ULONG_PTR)name32;
            argv32[1] = 0;
            arguments[0] = 1;
            arguments[1] = (uint32_t)(ULONG_PTR)argv32;
            compat32_callback_args((uint32_t)procedure, 2, arguments);
            VirtualFree(wire, 0, MEM_RELEASE);
        } else {
            typedef void (WINAPI *SCM_SERVICE_MAIN64)(DWORD, PWSTR *);
            WCHAR service_name[SCM_MAX_NAME];
            PWSTR arguments[2];
            scm_ascii_to_wide(service_name, SCM_MAX_NAME, name);
            arguments[0] = service_name;
            arguments[1] = NULL;
            ((SCM_SERVICE_MAIN64)(ULONG_PTR)procedure)(1, arguments);
        }
        dispatched = TRUE;

        flags = scm_lock_irqsave();
        SCM_SERVICE *service = &scm_services[service_index];
        if (service->used && service->process_id == pid &&
            service->status.current_state != SERVICE_STOPPED) {
            service->status.current_state = SERVICE_STOPPED;
            service->status.controls_accepted = 0;
            service->status.process_id = 0;
            service->process_id = 0;
        }
        scm_unlock_irqrestore(flags);
    }

    if (!dispatched) {
        SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static HANDLE scm_register_handler(PCWSTR service_name, PVOID handler,
                                   PVOID context, BOOL extended)
{
    char name[SCM_MAX_NAME];
    if (!handler || !scm_wide_to_ascii(name, sizeof(name), service_name)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    DWORD pid = scm_current_pid();
    uint64_t flags = scm_lock_irqsave();
    int service_index = scm_dispatch_service_index_locked(name, pid);
    if (service_index < 0 || scm_services[service_index].process_id != pid) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
        return NULL;
    }
    SCM_SERVICE *service = &scm_services[service_index];
    service->handler = (ULONG_PTR)handler;
    service->handler_context = (ULONG_PTR)context;
    service->handler_extended = extended;
    service->handler_compat32 = g_compat32_mode;
    HANDLE status_handle = scm_allocate_handle_locked(
        SCM_KIND_STATUS, 0, service_index);
    scm_unlock_irqrestore(flags);
    if (!status_handle) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    serial_puts("[SCM] handler registered service='");
    serial_puts(name);
    serial_puts("' handle=0x");
    serial_puthex((ULONG_PTR)status_handle, 16);
    serial_puts(" callback=0x");
    serial_puthex((ULONG_PTR)handler, g_compat32_mode ? 8 : 16);
    serial_puts(extended ? " extended\n" : "\n");
    SetLastError(0);
    return status_handle;
}

HANDLE WINAPI RegisterServiceCtrlHandlerW(PCWSTR service_name, PVOID handler)
{
    return scm_register_handler(service_name, handler, NULL, FALSE);
}

HANDLE WINAPI RegisterServiceCtrlHandlerExW(PCWSTR service_name,
                                            PVOID handler, PVOID context)
{
    return scm_register_handler(service_name, handler, context, TRUE);
}

BOOL WINAPI SetServiceStatus(HANDLE status_handle, PVOID status_buffer)
{
    if (!status_buffer) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD *input = (DWORD *)status_buffer;
    if (input[1] < SERVICE_STOPPED || input[1] > SERVICE_PAUSED) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        status_handle, SCM_KIND_STATUS, 0);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    service->status.service_type = input[0];
    service->status.current_state = input[1];
    service->status.controls_accepted = input[2];
    service->status.win32_exit_code = input[3];
    service->status.service_specific_exit_code = input[4];
    service->status.check_point = input[5];
    service->status.wait_hint = input[6];
    if (input[1] == SERVICE_STOPPED) {
        service->status.process_id = 0;
        service->process_id = 0;
    } else {
        service->process_id = scm_current_pid();
        service->status.process_id = service->process_id;
    }
    int service_index = (int)(service - scm_services);
    char service_name[SCM_MAX_NAME];
    scm_copy(service_name, sizeof(service_name), service->name);
    scm_maybe_delete_service_locked(service_index);
    scm_unlock_irqrestore(flags);
    serial_puts("[SCM] status service='");
    serial_puts(service_name);
    serial_puts("' state=");
    serial_putdec(input[1]);
    serial_puts(" win32_error=");
    serial_putdec(input[3]);
    serial_puts(" service_error=");
    serial_putdec(input[4]);
    serial_puts(" checkpoint=");
    serial_putdec(input[5]);
    serial_puts(" wait_hint=");
    serial_putdec(input[6]);
    serial_puts("\n");
    if (input[1] == SERVICE_STOPPED && g_compat32_mode) {
        extern void compat32_dump_recent_calls(void);
        serial_puts("[SCM] PE32 service stopped; capturing pre-stop calls\n");
        compat32_dump_recent_calls();
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI ControlService(HANDLE service_handle, DWORD control, PVOID status)
{
    DWORD access = SERVICE_USER_DEFINED_CONTROL;
    if (control == SERVICE_CONTROL_STOP) access = SERVICE_STOP;
    else if (control == SERVICE_CONTROL_PAUSE ||
             control == SERVICE_CONTROL_CONTINUE)
        access = SERVICE_PAUSE_CONTINUE;
    else if (control == SERVICE_CONTROL_INTERROGATE)
        access = SERVICE_INTERROGATE;

    ULONG_PTR handler = 0, context = 0;
    BOOL extended = FALSE, compat32 = FALSE;
    DWORD owner_pid = 0;
    SCM_STATUS_PROCESS snapshot;
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, access);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    if (!service) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (service->status.current_state == SERVICE_STOPPED) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_SERVICE_NOT_ACTIVE);
        return FALSE;
    }
    snapshot = service->status;
    handler = service->handler;
    context = service->handler_context;
    extended = service->handler_extended;
    compat32 = service->handler_compat32;
    owner_pid = service->process_id;
    scm_unlock_irqrestore(flags);

    if (control != SERVICE_CONTROL_INTERROGATE) {
        if (!handler || owner_pid != scm_current_pid()) {
            SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
            return FALSE;
        }
        if (compat32) {
            uint32_t args[4] = {control, 0, 0, (uint32_t)context};
            compat32_callback_args((uint32_t)handler,
                                   extended ? 4 : 1, args);
        } else if (extended) {
            typedef DWORD (WINAPI *SCM_HANDLER_EX64)(DWORD, DWORD,
                                                      PVOID, PVOID);
            ((SCM_HANDLER_EX64)handler)(control, 0, NULL, (PVOID)context);
        } else {
            typedef void (WINAPI *SCM_HANDLER64)(DWORD);
            ((SCM_HANDLER64)handler)(control);
        }
        flags = scm_lock_irqsave();
        entry = scm_lookup_handle_locked(service_handle, SCM_KIND_SERVICE, 0);
        service = scm_service_from_handle_locked(entry);
        if (service) snapshot = service->status;
        scm_unlock_irqrestore(flags);
    }
    if (status) scm_copy_basic_status(status, &snapshot);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI QueryServiceObjectSecurity(HANDLE service_handle,
                                       DWORD security_info,
                                       PVOID descriptor, DWORD buffer_size,
                                       DWORD *bytes_needed)
{
    (void)security_info;
    if (!bytes_needed) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *bytes_needed = sizeof(SCM_SECURITY_DESCRIPTOR_RELATIVE);
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, READ_CONTROL_ACCESS);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    BOOL service_found = service != NULL;
    scm_unlock_irqrestore(flags);
    if (!service_found) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!descriptor || buffer_size < sizeof(SCM_SECURITY_DESCRIPTOR_RELATIVE)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    SCM_SECURITY_DESCRIPTOR_RELATIVE *security =
        (SCM_SECURITY_DESCRIPTOR_RELATIVE *)descriptor;
    scm_zero(security, sizeof(*security));
    security->revision = 1;
    security->control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetServiceObjectSecurity(HANDLE service_handle,
                                     DWORD security_info, PVOID descriptor)
{
    DWORD required = (security_info & 0x4U) ? WRITE_DAC_ACCESS
                                            : WRITE_OWNER_ACCESS;
    if (!descriptor || *(BYTE *)descriptor != 1) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        service_handle, SCM_KIND_SERVICE, required);
    SCM_SERVICE *service = scm_service_from_handle_locked(entry);
    scm_unlock_irqrestore(flags);
    if (!service) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

HANDLE WINAPI RegisterEventSourceW_scm(PCWSTR server_name, PCWSTR source_name)
{
    (void)server_name;
    if (!source_name || !*source_name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    uint64_t flags = scm_lock_irqsave();
    HANDLE handle = scm_allocate_handle_locked(SCM_KIND_EVENT_LOG, 0, -1);
    scm_unlock_irqrestore(flags);
    if (!handle) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    SetLastError(0);
    return handle;
}

BOOL WINAPI DeregisterEventSource_scm(HANDLE event_log)
{
    return CloseEventLog_scm(event_log);
}

BOOL WINAPI ReportEventW_scm(HANDLE event_log, WORD type, WORD category,
                             DWORD event_id, PVOID user_sid,
                             WORD string_count, DWORD data_size,
                             PVOID strings, PVOID raw_data)
{
    (void)type;
    (void)category;
    (void)event_id;
    (void)user_sid;
    (void)string_count;
    (void)data_size;
    (void)strings;
    (void)raw_data;
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        event_log, SCM_KIND_EVENT_LOG, 0);
    scm_unlock_irqrestore(flags);
    if (!entry) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

HANDLE WINAPI OpenEventLogA_scm(PCSTR server_name, PCSTR source_name)
{
    (void)server_name;
    if (!source_name || !*source_name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    uint64_t flags = scm_lock_irqsave();
    HANDLE handle = scm_allocate_handle_locked(SCM_KIND_EVENT_LOG, 0, -1);
    scm_unlock_irqrestore(flags);
    if (!handle) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    else SetLastError(0);
    return handle;
}

BOOL WINAPI ReadEventLogW_scm(HANDLE event_log, DWORD read_flags,
                              DWORD record_offset, PVOID buffer,
                              DWORD bytes_to_read, DWORD *bytes_read,
                              DWORD *minimum_bytes_needed)
{
    (void)read_flags;
    (void)record_offset;
    (void)buffer;
    (void)bytes_to_read;
    if (bytes_read) *bytes_read = 0;
    if (minimum_bytes_needed) *minimum_bytes_needed = 0;
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = scm_lookup_handle_locked(
        event_log, SCM_KIND_EVENT_LOG, 0);
    scm_unlock_irqrestore(flags);
    if (!entry) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    SetLastError(ERROR_HANDLE_EOF);
    return FALSE;
}

BOOL WINAPI CloseEventLog_scm(HANDLE event_log)
{
    int index = scm_handle_index(event_log);
    if (index < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    uint64_t flags = scm_lock_irqsave();
    SCM_HANDLE_ENTRY *entry = &scm_handles[index];
    if (!entry->used || entry->value != event_log ||
        entry->owner_pid != scm_current_pid() ||
        entry->kind != SCM_KIND_EVENT_LOG) {
        scm_unlock_irqrestore(flags);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    USHORT generation = entry->generation;
    scm_zero(entry, sizeof(*entry));
    entry->generation = generation;
    scm_unlock_irqrestore(flags);
    SetLastError(0);
    return TRUE;
}

DWORD advapi32_service_release_process(DWORD process_id)
{
    if (!process_id) return 0;
    DWORD released = 0;
    uint64_t flags = scm_lock_irqsave();
    for (int i = 0; i < SCM_MAX_HANDLES; i++) {
        SCM_HANDLE_ENTRY *entry = &scm_handles[i];
        if (!entry->used || entry->owner_pid != process_id) continue;
        if (entry->kind == SCM_KIND_SERVICE) {
            SCM_SERVICE *service = scm_service_from_handle_locked(entry);
            if (service && service->open_handles) service->open_handles--;
        }
        USHORT generation = entry->generation;
        scm_zero(entry, sizeof(*entry));
        entry->generation = generation;
        released++;
    }
    for (int i = 0; i < SCM_MAX_SERVICES; i++) {
        SCM_SERVICE *service = &scm_services[i];
        if (!service->used || service->process_id != process_id) continue;
        service->process_id = 0;
        service->handler = 0;
        service->handler_context = 0;
        service->status.current_state = SERVICE_STOPPED;
        service->status.controls_accepted = 0;
        service->status.process_id = 0;
        scm_maybe_delete_service_locked(i);
    }
    scm_unlock_irqrestore(flags);
    if (released) {
        serial_puts("[SCM] process cleanup pid=");
        serial_putdec(process_id);
        serial_puts(" handles=");
        serial_putdec(released);
        serial_puts("\n");
    }
    return released;
}
