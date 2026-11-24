/* Minimal DirectInput 8 COM implementation for Win32 applications. */

#include "dinput8_shim.h"
#include "compat32.h"
#include "kernel32_shim.h"
#include "win32_abi.h"

#define DI_OK                   ((HRESULT)0x00000000)
#define DIERR_UNSUPPORTED       ((HRESULT)0x80004001)
#define DIERR_NOINTERFACE       ((HRESULT)0x80004002)
#define DIERR_OUTOFMEMORY       ((HRESULT)0x8007000E)
#define DIERR_NOTFOUND          ((HRESULT)0x80070002)
#define DIERR_INVALIDPARAM      ((HRESULT)0x80070057)
#define CLASS_E_NOAGGREGATION   ((HRESULT)0x80040110)

#define DIRECTINPUT_VERSION     0x0800u
#define DINPUT8_VTBL_SLOTS      11
#define DINPUT8_PROCESS_SLOTS   128

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);
extern DWORD win32_current_process_id(void);

static const GUID iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID iid_idirectinput8a = {
    0xBF798030, 0x483A, 0x4DA2,
    { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 }
};
static const GUID iid_idirectinput8w = {
    0xBF798031, 0x483A, 0x4DA2,
    { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 }
};

typedef struct {
    PVOID lpVtbl;
} DINPUT8_OBJECT;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID self, REFIID iid, PVOID output);
    ULONG (WINAPI *AddRef)(PVOID self);
    ULONG (WINAPI *Release)(PVOID self);
    HRESULT (WINAPI *CreateDevice)(PVOID self, REFIID guid, PVOID output,
                                   PVOID outer);
    HRESULT (WINAPI *EnumDevices)(PVOID self, DWORD type, PVOID callback,
                                  PVOID context, DWORD flags);
    HRESULT (WINAPI *GetDeviceStatus)(PVOID self, REFIID guid);
    HRESULT (WINAPI *RunControlPanel)(PVOID self, HANDLE owner, DWORD flags);
    HRESULT (WINAPI *Initialize)(PVOID self, HANDLE instance, DWORD version);
    HRESULT (WINAPI *FindDevice)(PVOID self, REFIID class_guid,
                                 PCSTR name, PVOID instance_guid);
    HRESULT (WINAPI *EnumDevicesBySemantics)(PVOID self, PCSTR user,
                                              PVOID action_format,
                                              PVOID callback, PVOID context,
                                              DWORD flags);
    HRESULT (WINAPI *ConfigureDevices)(PVOID self, PVOID callback,
                                       PVOID params, DWORD flags,
                                       PVOID context);
} DINPUT8_VTBL;

typedef struct {
    DWORD owner_pid;
    uint32_t *vtbl32;
    uint32_t *object32;
    ULONG refs;
} DINPUT8_PROCESS_STATE;

static ULONG dinput_refs64;
static DINPUT8_PROCESS_STATE dinput_process_states[DINPUT8_PROCESS_SLOTS];
static volatile uint32_t dinput_state_lock;

static void dinput_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&dinput_state_lock, 1))
        __asm__ volatile ("pause");
}

static void dinput_lock_release(void)
{
    __sync_lock_release(&dinput_state_lock);
}

static DINPUT8_PROCESS_STATE *dinput_find_state_locked(DWORD owner_pid,
                                                        PVOID object)
{
    for (uint32_t i = 0; i < DINPUT8_PROCESS_SLOTS; i++) {
        DINPUT8_PROCESS_STATE *state = &dinput_process_states[i];
        if (owner_pid && state->owner_pid == owner_pid)
            return state;
        if (object && state->object32 == (uint32_t *)object)
            return state;
    }
    return NULL;
}

static ULONG dinput_add_ref(PVOID self)
{
    if ((ULONG_PTR)self <= UINT32_MAX) {
        ULONG refs = 0;
        dinput_lock_acquire();
        DINPUT8_PROCESS_STATE *state = dinput_find_state_locked(0, self);
        if (state)
            refs = ++state->refs;
        dinput_lock_release();
        if (state)
            return refs;
    }
    return __sync_add_and_fetch(&dinput_refs64, 1);
}

static ULONG dinput_release_ref(PVOID self)
{
    if ((ULONG_PTR)self <= UINT32_MAX) {
        ULONG refs = 0;
        dinput_lock_acquire();
        DINPUT8_PROCESS_STATE *state = dinput_find_state_locked(0, self);
        if (state && state->refs)
            refs = --state->refs;
        dinput_lock_release();
        if (state)
            return refs;
    }

    ULONG refs;
    do {
        refs = __atomic_load_n(&dinput_refs64, __ATOMIC_ACQUIRE);
        if (!refs) return 0;
    } while (!__atomic_compare_exchange_n(&dinput_refs64, &refs, refs - 1,
                                          FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    return refs - 1;
}

static BOOL dinput_guid_equal(REFIID left, REFIID right)
{
    if (!left || !right || left->Data1 != right->Data1 ||
        left->Data2 != right->Data2 || left->Data3 != right->Data3)
        return FALSE;
    for (int i = 0; i < 8; i++) {
        if (left->Data4[i] != right->Data4[i]) return FALSE;
    }
    return TRUE;
}

static BOOL dinput_supported_iid(REFIID iid)
{
    return dinput_guid_equal(iid, &iid_iunknown) ||
           dinput_guid_equal(iid, &iid_idirectinput8a) ||
           dinput_guid_equal(iid, &iid_idirectinput8w);
}

static void dinput_store_pointer(PVOID output, PVOID value)
{
    if (g_compat32_mode)
        *(uint32_t *)output = (uint32_t)(ULONG_PTR)value;
    else
        *(PVOID *)output = value;
}

static HRESULT WINAPI di_QueryInterface(PVOID self, REFIID iid, PVOID output)
{
    if (!output) return DIERR_INVALIDPARAM;
    dinput_store_pointer(output, NULL);
    if (!dinput_supported_iid(iid)) return DIERR_NOINTERFACE;
    dinput_store_pointer(output, self);
    dinput_add_ref(self);
    return DI_OK;
}

static ULONG WINAPI di_AddRef(PVOID self)
{
    return dinput_add_ref(self);
}

static ULONG WINAPI di_Release(PVOID self)
{
    return dinput_release_ref(self);
}

static HRESULT WINAPI di_CreateDevice(PVOID self, REFIID guid, PVOID output,
                                      PVOID outer)
{
    (void)self;
    (void)guid;
    (void)outer;
    if (!output) return DIERR_INVALIDPARAM;
    dinput_store_pointer(output, NULL);
    return DIERR_NOTFOUND;
}

static HRESULT WINAPI di_EnumDevices(PVOID self, DWORD type, PVOID callback,
                                     PVOID context, DWORD flags)
{
    (void)self;
    (void)context;
    if (!callback) return DIERR_INVALIDPARAM;
    serial_puts("[DINPUT8] EnumDevices type=0x");
    serial_puthex(type, 8);
    serial_puts(" flags=0x");
    serial_puthex(flags, 8);
    serial_puts(" -> 0 devices\n");
    return DI_OK;
}

static HRESULT WINAPI di_GetDeviceStatus(PVOID self, REFIID guid)
{
    (void)self;
    (void)guid;
    return DIERR_NOTFOUND;
}

static HRESULT WINAPI di_RunControlPanel(PVOID self, HANDLE owner, DWORD flags)
{
    (void)self;
    (void)owner;
    (void)flags;
    return DI_OK;
}

static HRESULT WINAPI di_Initialize(PVOID self, HANDLE instance, DWORD version)
{
    (void)self;
    (void)instance;
    return version >= DIRECTINPUT_VERSION ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI di_FindDevice(PVOID self, REFIID class_guid,
                                    PCSTR name, PVOID instance_guid)
{
    (void)self;
    (void)class_guid;
    (void)name;
    if (!instance_guid) return DIERR_INVALIDPARAM;
    BYTE *bytes = (BYTE *)instance_guid;
    for (int i = 0; i < 16; i++) bytes[i] = 0;
    return DIERR_NOTFOUND;
}

static HRESULT WINAPI di_EnumDevicesBySemantics(PVOID self, PCSTR user,
                                                 PVOID action_format,
                                                 PVOID callback,
                                                 PVOID context, DWORD flags)
{
    (void)self;
    (void)user;
    (void)action_format;
    (void)context;
    (void)flags;
    return callback ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI di_ConfigureDevices(PVOID self, PVOID callback,
                                          PVOID params, DWORD flags,
                                          PVOID context)
{
    (void)self;
    (void)callback;
    (void)params;
    (void)flags;
    (void)context;
    return DIERR_UNSUPPORTED;
}

static DINPUT8_VTBL dinput_vtbl64 = {
    di_QueryInterface,
    di_AddRef,
    di_Release,
    di_CreateDevice,
    di_EnumDevices,
    di_GetDeviceStatus,
    di_RunControlPanel,
    di_Initialize,
    di_FindDevice,
    di_EnumDevicesBySemantics,
    di_ConfigureDevices,
};
static DINPUT8_OBJECT dinput_object64 = { &dinput_vtbl64 };

static PVOID dinput_create_com32(void)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    dinput_lock_acquire();
    DINPUT8_PROCESS_STATE *state =
        dinput_find_state_locked(owner_pid, NULL);
    dinput_lock_release();
    if (state)
        return state->object32;

    BYTE *page = (BYTE *)VirtualAlloc(NULL, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page || (ULONG_PTR)page > (ULONG_PTR)UINT32_MAX - 4095) {
        if (page) VirtualFree(page, 0, MEM_RELEASE);
        return NULL;
    }
    for (int i = 0; i < 4096; i++) page[i] = 0;

    uint32_t *vtbl32 = (uint32_t *)page;
    uint32_t *object32 = (uint32_t *)(page + 64);
    const uint64_t targets[DINPUT8_VTBL_SLOTS] = {
        (uint64_t)(ULONG_PTR)di_QueryInterface,
        (uint64_t)(ULONG_PTR)di_AddRef,
        (uint64_t)(ULONG_PTR)di_Release,
        (uint64_t)(ULONG_PTR)di_CreateDevice,
        (uint64_t)(ULONG_PTR)di_EnumDevices,
        (uint64_t)(ULONG_PTR)di_GetDeviceStatus,
        (uint64_t)(ULONG_PTR)di_RunControlPanel,
        (uint64_t)(ULONG_PTR)di_Initialize,
        (uint64_t)(ULONG_PTR)di_FindDevice,
        (uint64_t)(ULONG_PTR)di_EnumDevicesBySemantics,
        (uint64_t)(ULONG_PTR)di_ConfigureDevices,
    };
    static const char *const names[DINPUT8_VTBL_SLOTS] = {
        "DI8_QueryInterface", "DI8_AddRef", "DI8_Release",
        "DI8_CreateDevice", "DI8_EnumDevices", "DI8_GetDeviceStatus",
        "DI8_RunControlPanel", "DI8_Initialize", "DI8_FindDevice",
        "DI8_EnumBySemantics", "DI8_ConfigureDevices",
    };
    static const uint8_t args[DINPUT8_VTBL_SLOTS] = {
        3, 1, 1, 4, 5, 2, 3, 3, 4, 6, 5,
    };

    for (int i = 0; i < DINPUT8_VTBL_SLOTS; i++) {
        vtbl32[i] = compat32_make_thunk_ex(
            targets[i], names[i], args[i], CC_STDCALL);
        if (!vtbl32[i]) {
            VirtualFree(page, 0, MEM_RELEASE);
            return NULL;
        }
    }
    object32[0] = (uint32_t)(ULONG_PTR)vtbl32;

    DINPUT8_PROCESS_STATE *published = NULL;
    dinput_lock_acquire();
    state = dinput_find_state_locked(owner_pid, NULL);
    if (state) {
        published = state;
    } else {
        for (uint32_t i = 0; i < DINPUT8_PROCESS_SLOTS; i++) {
            if (!dinput_process_states[i].owner_pid) {
                published = &dinput_process_states[i];
                published->owner_pid = owner_pid;
                published->vtbl32 = vtbl32;
                published->object32 = object32;
                published->refs = 0;
                break;
            }
        }
    }
    dinput_lock_release();

    if (!published || published->object32 != object32) {
        VirtualFree(page, 0, MEM_RELEASE);
        return published ? published->object32 : NULL;
    }

    serial_puts("[DINPUT8] COM32 proxy pid=");
    serial_putdec(owner_pid);
    serial_puts(" va=0x");
    serial_puthex((uint64_t)(ULONG_PTR)object32, 8);
    serial_puts("\n");
    return object32;
}

HRESULT WINAPI DirectInput8Create(HANDLE instance, DWORD version, REFIID iid,
                                  PVOID output, PVOID outer)
{
    (void)instance;
    if (!output || !iid) return DIERR_INVALIDPARAM;
    dinput_store_pointer(output, NULL);
    if (outer) return CLASS_E_NOAGGREGATION;
    if (version < DIRECTINPUT_VERSION) return DIERR_INVALIDPARAM;
    if (!dinput_supported_iid(iid)) return DIERR_NOINTERFACE;

    PVOID object = &dinput_object64;
    if (g_compat32_mode) {
        object = dinput_create_com32();
        if (!object) return DIERR_OUTOFMEMORY;
    }
    dinput_store_pointer(output, object);
    dinput_add_ref(object);
    serial_puts("[DINPUT8] DirectInput8Create version=0x");
    serial_puthex(version, 4);
    serial_puts("\n");
    return DI_OK;
}

typedef struct {
    const char *name;
    PVOID func;
    uint8_t argc;
    uint8_t cc;
} DINPUT8_EXPORT;

static const DINPUT8_EXPORT dinput8_exports[] = {
    { "DirectInput8Create", (PVOID)DirectInput8Create, 5, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL },
};

const WIN32_EXPORT *dinput8_abi_table(int *count)
{
    *count = (int)(sizeof(dinput8_exports) / sizeof(dinput8_exports[0]));
    return (const WIN32_EXPORT *)dinput8_exports;
}

static int dinput_strcmp(const char *left, const char *right)
{
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

PVOID dinput8_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    for (int i = 0; dinput8_exports[i].name; i++) {
        if (dinput_strcmp(func_name, dinput8_exports[i].name) == 0)
            return dinput8_exports[i].func;
    }
    return NULL;
}

void dinput8_release_process(DWORD process_id)
{
    if (!process_id) return;

    dinput_lock_acquire();
    for (uint32_t i = 0; i < DINPUT8_PROCESS_SLOTS; i++) {
        DINPUT8_PROCESS_STATE *state = &dinput_process_states[i];
        if (state->owner_pid != process_id)
            continue;
        state->owner_pid = 0;
        state->vtbl32 = NULL;
        state->object32 = NULL;
        state->refs = 0;
    }
    dinput_lock_release();
}

PVOID dinput8_shim_init(void)
{
    dinput_refs64 = 0;
    dinput_lock_acquire();
    for (uint32_t i = 0; i < DINPUT8_PROCESS_SLOTS; i++) {
        dinput_process_states[i].owner_pid = 0;
        dinput_process_states[i].vtbl32 = NULL;
        dinput_process_states[i].object32 = NULL;
        dinput_process_states[i].refs = 0;
    }
    dinput_lock_release();
    return (PVOID)dinput8_exports;
}
