/*
 * OsitoK Windows Compatibility Layer — ole32.dll Shim
 * OLE/COM infrastructure and built-in class activation registry.
 */

#include "ole32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern DWORD WINAPI GetCurrentProcessId(void);
extern DWORD WINAPI GetCurrentThreadId(void);

/* Use crt_malloc from msvcrt shim for CoTaskMemAlloc */
extern PVOID WINAPI crt_malloc(SIZE_T size);
extern PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size);
extern void  WINAPI crt_free(PVOID ptr);
extern SIZE_T WINAPI crt_msize(PVOID ptr);

#define OLE32_CLASS_SLOTS 32
#define OLE32_APARTMENT_SLOTS 1024

#define OLE32_S_FALSE                 ((HRESULT)0x00000001)
#define OLE32_E_INVALIDARG            ((HRESULT)0x80070057)
#define OLE32_E_OUTOFMEMORY           ((HRESULT)0x8007000E)
#define OLE32_RPC_E_CHANGED_MODE      ((HRESULT)0x80010106)
#define OLE32_COINIT_APARTMENTTHREADED 0x2u
#define OLE32_COINIT_VALID_FLAGS       0xEu

typedef struct {
    GUID clsid;
    OLE32_CLASS_ACTIVATOR activate;
} OLE32_CLASS_ENTRY;

static OLE32_CLASS_ENTRY ole32_classes[OLE32_CLASS_SLOTS];
static volatile uint32_t ole32_class_lock;

typedef struct {
    DWORD process_id;
    DWORD thread_id;
    DWORD model;
    DWORD references;
} OLE32_APARTMENT_ENTRY;

static OLE32_APARTMENT_ENTRY ole32_apartments[OLE32_APARTMENT_SLOTS];
static volatile uint32_t ole32_apartment_lock;

static void ole32_class_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&ole32_class_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static void ole32_class_lock_release(void)
{
    __sync_lock_release(&ole32_class_lock);
}

static void ole32_apartment_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&ole32_apartment_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static void ole32_apartment_lock_release(void)
{
    __sync_lock_release(&ole32_apartment_lock);
}

static BOOL ole32_guid_equal(LPCGUID left, LPCGUID right)
{
    if (!left || !right || left->Data1 != right->Data1 ||
        left->Data2 != right->Data2 || left->Data3 != right->Data3)
        return FALSE;
    for (int i = 0; i < 8; i++) {
        if (left->Data4[i] != right->Data4[i])
            return FALSE;
    }
    return TRUE;
}

static void ole32_store_pointer(PVOID output, PVOID value)
{
    if (g_compat32_mode)
        *(uint32_t *)output = (uint32_t)(ULONG_PTR)value;
    else
        *(PVOID *)output = value;
}

HRESULT ole32_register_class(LPCGUID clsid, OLE32_CLASS_ACTIVATOR activate)
{
    if (!clsid || !activate)
        return (HRESULT)0x80070057; /* E_INVALIDARG */

    ole32_class_lock_acquire();
    OLE32_CLASS_ENTRY *free_entry = NULL;
    for (int i = 0; i < OLE32_CLASS_SLOTS; i++) {
        OLE32_CLASS_ENTRY *entry = &ole32_classes[i];
        if (entry->activate && ole32_guid_equal(&entry->clsid, clsid)) {
            entry->activate = activate;
            ole32_class_lock_release();
            return S_OK;
        }
        if (!entry->activate && !free_entry)
            free_entry = entry;
    }
    if (!free_entry) {
        ole32_class_lock_release();
        return (HRESULT)0x8007000E; /* E_OUTOFMEMORY */
    }
    free_entry->clsid = *clsid;
    free_entry->activate = activate;
    ole32_class_lock_release();
    return S_OK;
}

/* ── COM initialization ────────────────────────────────────── */

HRESULT WINAPI shim_CoInitialize(PVOID reserved)
{
    return shim_CoInitializeEx(reserved,
                               OLE32_COINIT_APARTMENTTHREADED);
}

HRESULT WINAPI shim_CoInitializeEx(PVOID reserved, DWORD coinit)
{
    if (reserved || (coinit & ~OLE32_COINIT_VALID_FLAGS))
        return OLE32_E_INVALIDARG;

    DWORD process_id = GetCurrentProcessId();
    DWORD thread_id = GetCurrentThreadId();
    if (!process_id) process_id = 1;
    if (!thread_id) thread_id = 1;
    DWORD model = coinit & OLE32_COINIT_APARTMENTTHREADED;

    OLE32_APARTMENT_ENTRY *free_entry = NULL;
    ole32_apartment_lock_acquire();
    for (int i = 0; i < OLE32_APARTMENT_SLOTS; i++) {
        OLE32_APARTMENT_ENTRY *entry = &ole32_apartments[i];
        if (entry->process_id == process_id &&
            entry->thread_id == thread_id) {
            if (entry->model != model) {
                ole32_apartment_lock_release();
                serial_puts("[OLE32] apartment mode mismatch pid=");
                serial_putdec(process_id);
                serial_puts(" tid=");
                serial_putdec(thread_id);
                serial_puts("\n");
                return OLE32_RPC_E_CHANGED_MODE;
            }
            if (entry->references == UINT32_MAX) {
                ole32_apartment_lock_release();
                return OLE32_E_OUTOFMEMORY;
            }
            entry->references++;
            ole32_apartment_lock_release();
            return OLE32_S_FALSE;
        }
        if (!entry->process_id && !free_entry)
            free_entry = entry;
    }

    if (!free_entry) {
        ole32_apartment_lock_release();
        return OLE32_E_OUTOFMEMORY;
    }
    free_entry->process_id = process_id;
    free_entry->thread_id = thread_id;
    free_entry->model = model;
    free_entry->references = 1;
    ole32_apartment_lock_release();

    serial_puts("[OLE32] apartment initialized pid=");
    serial_putdec(process_id);
    serial_puts(" tid=");
    serial_putdec(thread_id);
    serial_puts(model ? " model=STA\n" : " model=MTA\n");
    return S_OK;
}

void WINAPI shim_CoUninitialize(void)
{
    DWORD process_id = GetCurrentProcessId();
    DWORD thread_id = GetCurrentThreadId();
    if (!process_id) process_id = 1;
    if (!thread_id) thread_id = 1;

    ole32_apartment_lock_acquire();
    for (int i = 0; i < OLE32_APARTMENT_SLOTS; i++) {
        OLE32_APARTMENT_ENTRY *entry = &ole32_apartments[i];
        if (entry->process_id != process_id ||
            entry->thread_id != thread_id)
            continue;
        if (entry->references > 1) {
            entry->references--;
        } else {
            entry->process_id = 0;
            entry->thread_id = 0;
            entry->model = 0;
            entry->references = 0;
        }
        break;
    }
    ole32_apartment_lock_release();
}

void ole32_release_process(DWORD process_id)
{
    if (!process_id) return;

    DWORD released = 0;
    ole32_apartment_lock_acquire();
    for (int i = 0; i < OLE32_APARTMENT_SLOTS; i++) {
        OLE32_APARTMENT_ENTRY *entry = &ole32_apartments[i];
        if (entry->process_id != process_id)
            continue;
        entry->process_id = 0;
        entry->thread_id = 0;
        entry->model = 0;
        entry->references = 0;
        released++;
    }
    ole32_apartment_lock_release();

    if (released) {
        serial_puts("[OLE32] released process apartments pid=");
        serial_putdec(process_id);
        serial_puts(" count=");
        serial_putdec(released);
        serial_puts("\n");
    }
}

/* ── CoCreateInstance ──────────────────────────────────────── */

static void ole32_log_guid(const char *label, const GUID *guid)
{
    serial_puts(label);
    if (!guid) {
        serial_puts("NULL");
        return;
    }

    serial_puthex(guid->Data1, 8);
    serial_puts("-");
    serial_puthex(guid->Data2, 4);
    serial_puts("-");
    serial_puthex(guid->Data3, 4);
    serial_puts("-");
    serial_puthex(guid->Data4[0], 2);
    serial_puthex(guid->Data4[1], 2);
    serial_puts("-");
    for (int i = 2; i < 8; i++)
        serial_puthex(guid->Data4[i], 2);
}

HRESULT WINAPI shim_CoCreateInstance(PVOID rclsid, PVOID pUnkOuter,
                                     DWORD dwClsContext, PVOID riid,
                                     PVOID output)
{
    if (!output)
        return (HRESULT)0x80004003; /* E_POINTER */
    ole32_store_pointer(output, NULL);

    serial_puts("[OLE32] CoCreateInstance ");
    ole32_log_guid("CLSID=", (const GUID *)rclsid);
    serial_puts(" ");
    ole32_log_guid("IID=", (const GUID *)riid);
    serial_puts(" clsctx=0x");
    serial_puthex(dwClsContext, 8);
    serial_puts(" outer=0x");
    serial_puthex((ULONG_PTR)pUnkOuter, 16);

    if (!rclsid || !riid || !(dwClsContext & 0x1U)) {
        serial_puts(" -> CLASS_E_CLASSNOTAVAILABLE\n");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    OLE32_CLASS_ACTIVATOR activate = NULL;
    ole32_class_lock_acquire();
    for (int i = 0; i < OLE32_CLASS_SLOTS; i++) {
        if (ole32_classes[i].activate &&
            ole32_guid_equal(&ole32_classes[i].clsid,
                             (const GUID *)rclsid)) {
            activate = ole32_classes[i].activate;
            break;
        }
    }
    ole32_class_lock_release();
    if (!activate) {
        serial_puts(" -> CLASS_E_CLASSNOTAVAILABLE\n");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    HRESULT result = activate((const GUID *)riid, pUnkOuter, output);
    serial_puts(" -> 0x");
    serial_puthex((uint32_t)result, 8);
    serial_puts("\n");
    return result;
}

/* ── Task memory ───────────────────────────────────────────── */

PVOID WINAPI shim_CoTaskMemAlloc(SIZE_T size)
{
    return crt_malloc(size);
}

void WINAPI shim_CoTaskMemFree(PVOID ptr)
{
    crt_free(ptr);
}

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID self, PVOID iid, PVOID *object);
    ULONG (WINAPI *AddRef)(PVOID self);
    ULONG (WINAPI *Release)(PVOID self);
    PVOID (WINAPI *Alloc)(PVOID self, SIZE_T size);
    PVOID (WINAPI *Realloc)(PVOID self, PVOID ptr, SIZE_T size);
    void (WINAPI *Free)(PVOID self, PVOID ptr);
    SIZE_T (WINAPI *GetSize)(PVOID self, PVOID ptr);
    int (WINAPI *DidAlloc)(PVOID self, PVOID ptr);
    void (WINAPI *HeapMinimize)(PVOID self);
} K32_IMALLOC_VTBL;

typedef struct {
    K32_IMALLOC_VTBL *lpVtbl;
} K32_IMALLOC;

static HRESULT WINAPI imalloc_QueryInterface(PVOID self, PVOID iid,
                                              PVOID *object)
{
    (void)iid;
    if (!object) return (HRESULT)0x80004003; /* E_POINTER */
    *object = self;
    return S_OK;
}

static ULONG WINAPI imalloc_AddRef(PVOID self)
{
    (void)self;
    return 2;
}

static ULONG WINAPI imalloc_Release(PVOID self)
{
    (void)self;
    return 1;
}

static PVOID WINAPI imalloc_Alloc(PVOID self, SIZE_T size)
{
    (void)self;
    return crt_malloc(size);
}

static PVOID WINAPI imalloc_Realloc(PVOID self, PVOID ptr, SIZE_T size)
{
    (void)self;
    return crt_realloc(ptr, size);
}

static void WINAPI imalloc_Free(PVOID self, PVOID ptr)
{
    (void)self;
    crt_free(ptr);
}

static SIZE_T WINAPI imalloc_GetSize(PVOID self, PVOID ptr)
{
    (void)self;
    return crt_msize(ptr);
}

static int WINAPI imalloc_DidAlloc(PVOID self, PVOID ptr)
{
    (void)self;
    return ptr ? 1 : 0;
}

static void WINAPI imalloc_HeapMinimize(PVOID self)
{
    (void)self;
}

static K32_IMALLOC_VTBL imalloc_vtbl = {
    imalloc_QueryInterface,
    imalloc_AddRef,
    imalloc_Release,
    imalloc_Alloc,
    imalloc_Realloc,
    imalloc_Free,
    imalloc_GetSize,
    imalloc_DidAlloc,
    imalloc_HeapMinimize
};

static K32_IMALLOC imalloc = { &imalloc_vtbl };

HRESULT WINAPI shim_CoGetMalloc(DWORD context, PVOID *allocator)
{
    if (!allocator) return (HRESULT)0x80004003; /* E_POINTER */
    *allocator = NULL;
    if (context != 1) return (HRESULT)0x80070057; /* E_INVALIDARG */
    *allocator = &imalloc;
    return S_OK;
}

/* ── OLE initialization ───────────────────────────────────── */

HRESULT WINAPI shim_OleInitialize(PVOID reserved)
{
    return shim_CoInitializeEx(reserved,
                               OLE32_COINIT_APARTMENTTHREADED);
}

void WINAPI shim_OleUninitialize(void)
{
    shim_CoUninitialize();
}

HRESULT WINAPI shim_RegisterDragDrop(HANDLE hwnd, PVOID drop_target)
{
    (void)hwnd; (void)drop_target;
    return S_OK;
}

HRESULT WINAPI shim_RevokeDragDrop(HANDLE hwnd)
{
    (void)hwnd;
    return S_OK;
}

/* ── GUID utilities ────────────────────────────────────────── */

HRESULT WINAPI shim_CoCreateGuid(PVOID guid)
{
    if (guid) {
        BYTE *p = (BYTE *)guid;
        for (int i = 0; i < 16; i++) p[i] = 0;
    }
    return S_OK;
}

static HRESULT WINAPI shim_CLSIDFromString(PCWSTR value, PVOID clsid)
{
    if (!clsid) return (HRESULT)0x80070057; /* E_INVALIDARG */
    shim_CoCreateGuid(clsid);
    if (!value) return S_OK;

    /* ponytail: no COM registry yet; persist ProgID mappings when COM classes exist. */
    DWORD hash = 2166136261u;
    int length = 0;
    while (*value) {
        hash = (hash ^ (BYTE)*value) * 16777619u;
        hash = (hash ^ (BYTE)(*value >> 8)) * 16777619u;
        value++;
        length++;
    }
    if (!length) return (HRESULT)0x800401F3; /* CO_E_CLASSSTRING */

    GUID *guid = (GUID *)clsid;
    guid->Data1 = hash;
    guid->Data4[0] = 0xC0;
    guid->Data4[7] = 0x46;
    return S_OK;
}

int WINAPI shim_StringFromGUID2(PVOID guid, PVOID str, int max)
{
    (void)guid; (void)str; (void)max;
    return 0; /* return 0 = failure (not enough space / not implemented) */
}

/* Automation strings and variants. */

PWSTR WINAPI shim_SysAllocStringLen(PCWSTR source, UINT length)
{
    if ((SIZE_T)length > (((SIZE_T)-1 - 6) / sizeof(WCHAR))) return NULL;
    SIZE_T bytes = (SIZE_T)length * sizeof(WCHAR);
    BYTE *allocation = (BYTE *)crt_malloc(sizeof(DWORD) + bytes +
                                          sizeof(WCHAR));
    if (!allocation) return NULL;
    *(DWORD *)allocation = (DWORD)bytes;
    PWSTR string = (PWSTR)(allocation + sizeof(DWORD));
    for (UINT i = 0; i < length; i++) string[i] = source ? source[i] : 0;
    string[length] = 0;
    return string;
}

PWSTR WINAPI shim_SysAllocString(PCWSTR source)
{
    if (!source) return NULL;
    UINT length = 0;
    while (source[length]) length++;
    return shim_SysAllocStringLen(source, length);
}

void WINAPI shim_SysFreeString(PWSTR string)
{
    if (string) crt_free((BYTE *)string - sizeof(DWORD));
}

UINT WINAPI shim_SysStringLen(PCWSTR string)
{
    return string ? *(const DWORD *)((const BYTE *)string - sizeof(DWORD)) /
                    sizeof(WCHAR) : 0;
}

UINT WINAPI shim_SysStringByteLen(PCWSTR string)
{
    return string ? *(const DWORD *)((const BYTE *)string - sizeof(DWORD)) : 0;
}

PWSTR WINAPI shim_SysAllocStringByteLen(PCSTR source, UINT length)
{
    BYTE *allocation = (BYTE *)crt_malloc(sizeof(DWORD) + (SIZE_T)length + 2);
    if (!allocation) return NULL;
    *(DWORD *)allocation = length;
    BYTE *string = allocation + sizeof(DWORD);
    for (UINT i = 0; i < length; i++) string[i] = source ? (BYTE)source[i] : 0;
    string[length] = 0;
    string[length + 1] = 0;
    return (PWSTR)string;
}

typedef struct {
    USHORT vt;
    USHORT reserved1;
    USHORT reserved2;
    USHORT reserved3;
    union {
        LONG lVal;
        PWSTR bstrVal;
        PVOID interface_pointer;
        ULONG_PTR raw[2];
    } value;
} OLE32_VARIANT;

_Static_assert(sizeof(OLE32_VARIANT) == 24, "Win64 VARIANT ABI");

typedef struct {
    const PVOID lpVtbl;
} OLE32_UNKNOWN;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
} OLE32_UNKNOWN_VTBL;

static void ole32_interface_addref(PVOID interface_pointer)
{
    if (!interface_pointer) return;
    OLE32_UNKNOWN *unknown = (OLE32_UNKNOWN *)interface_pointer;
    const OLE32_UNKNOWN_VTBL *vtbl =
        (const OLE32_UNKNOWN_VTBL *)unknown->lpVtbl;
    if (vtbl && vtbl->AddRef) vtbl->AddRef(interface_pointer);
}

static void ole32_interface_release(PVOID interface_pointer)
{
    if (!interface_pointer) return;
    OLE32_UNKNOWN *unknown = (OLE32_UNKNOWN *)interface_pointer;
    const OLE32_UNKNOWN_VTBL *vtbl =
        (const OLE32_UNKNOWN_VTBL *)unknown->lpVtbl;
    if (vtbl && vtbl->Release) vtbl->Release(interface_pointer);
}

void WINAPI shim_VariantInit(PVOID variant)
{
    if (variant) *(OLE32_VARIANT *)variant = (OLE32_VARIANT){0};
}

static ULONG_PTR ole32_prop_read_pointer(const BYTE *address)
{
    if (g_compat32_mode)
        return *(const uint32_t *)address;
    return *(const ULONG_PTR *)address;
}

static void ole32_prop_zero(BYTE *value)
{
    SIZE_T size = g_compat32_mode ? 16 : 24;
    for (SIZE_T i = 0; i < size; i++) value[i] = 0;
}

static void ole32_prop_release_interface(PVOID interface_pointer)
{
    if (!interface_pointer) return;
    if (!g_compat32_mode) {
        ole32_interface_release(interface_pointer);
        return;
    }

    uint32_t object = (uint32_t)(ULONG_PTR)interface_pointer;
    uint32_t vtable = *(const uint32_t *)(ULONG_PTR)object;
    if (!vtable) return;
    uint32_t release = *(const uint32_t *)(ULONG_PTR)(vtable + 8);
    if (!release) return;
    uint32_t arguments[1] = { object };
    compat32_callback_args(release, 1, arguments);
}

static HRESULT ole32_prop_clear_value(BYTE *value)
{
    USHORT type = *(USHORT *)value;
    USHORT base_type = type & 0x0FFF;
    if (type & 0x4000) { /* VT_BYREF does not own the target. */
        ole32_prop_zero(value);
        return S_OK;
    }

    if (type & 0x1000) { /* VT_VECTOR */
        DWORD count = *(DWORD *)(value + 8);
        SIZE_T pointer_offset = g_compat32_mode ? 12 : 16;
        BYTE *elements =
            (BYTE *)(ULONG_PTR)ole32_prop_read_pointer(value + pointer_offset);
        if (elements) {
            SIZE_T pointer_size = g_compat32_mode ? 4 : sizeof(ULONG_PTR);
            for (DWORD i = 0; i < count; i++) {
                if (base_type == 8) { /* VT_BSTR */
                    PWSTR string = (PWSTR)(ULONG_PTR)ole32_prop_read_pointer(
                        elements + i * pointer_size);
                    shim_SysFreeString(string);
                } else if (base_type == 30 || base_type == 31) {
                    PVOID string = (PVOID)(ULONG_PTR)ole32_prop_read_pointer(
                        elements + i * pointer_size);
                    shim_CoTaskMemFree(string);
                } else if (base_type == 9 || base_type == 13) {
                    PVOID object = (PVOID)(ULONG_PTR)ole32_prop_read_pointer(
                        elements + i * pointer_size);
                    ole32_prop_release_interface(object);
                } else if (base_type == 12) { /* VT_VARIANT */
                    SIZE_T variant_size = g_compat32_mode ? 16 : 24;
                    ole32_prop_clear_value(elements + i * variant_size);
                }
            }
            shim_CoTaskMemFree(elements);
        }
        ole32_prop_zero(value);
        return S_OK;
    }

    PVOID pointer =
        (PVOID)(ULONG_PTR)ole32_prop_read_pointer(value + 8);
    switch (base_type) {
    case 8: /* VT_BSTR */
        shim_SysFreeString((PWSTR)pointer);
        break;
    case 9:  /* VT_DISPATCH */
    case 13: /* VT_UNKNOWN */
    case 66: /* VT_STREAM */
    case 67: /* VT_STORAGE */
    case 68: /* VT_STREAMED_OBJECT */
    case 69: /* VT_STORED_OBJECT */
        ole32_prop_release_interface(pointer);
        break;
    case 30: /* VT_LPSTR */
    case 31: /* VT_LPWSTR */
    case 72: /* VT_CLSID */
        shim_CoTaskMemFree(pointer);
        break;
    case 65: { /* VT_BLOB */
        SIZE_T pointer_offset = g_compat32_mode ? 12 : 16;
        PVOID data = (PVOID)(ULONG_PTR)ole32_prop_read_pointer(
            value + pointer_offset);
        shim_CoTaskMemFree(data);
        break;
    }
    case 71: { /* VT_CF: the union owns a CLIPDATA allocation. */
        BYTE *clip_data = (BYTE *)pointer;
        if (clip_data) {
            PVOID data = (PVOID)(ULONG_PTR)ole32_prop_read_pointer(
                clip_data + 8);
            shim_CoTaskMemFree(data);
            shim_CoTaskMemFree(clip_data);
        }
        break;
    }
    case 73: { /* VT_VERSIONED_STREAM */
        BYTE *versioned_stream = (BYTE *)pointer;
        if (versioned_stream) {
            PVOID stream = (PVOID)(ULONG_PTR)ole32_prop_read_pointer(
                versioned_stream + 16);
            ole32_prop_release_interface(stream);
            shim_CoTaskMemFree(versioned_stream);
        }
        break;
    }
    default:
        break;
    }

    ole32_prop_zero(value);
    return S_OK;
}

static HRESULT WINAPI shim_PropVariantClear(PVOID prop_variant)
{
    if (!prop_variant) return (HRESULT)0x80070057; /* E_INVALIDARG */
    return ole32_prop_clear_value((BYTE *)prop_variant);
}

/* ── Export table ──────────────────────────────────────────── */

static void ole32_test_expect(BOOL condition, const char *name,
                              int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[OLE32-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int ole32_apartment_selftest(void)
{
    int checks = 0;
    int failures = 0;
    DWORD process_id = GetCurrentProcessId();
    if (!process_id) process_id = 1;

    ole32_release_process(process_id);
    ole32_test_expect(
        shim_CoInitializeEx((PVOID)(ULONG_PTR)1,
                            OLE32_COINIT_APARTMENTTHREADED) ==
            OLE32_E_INVALIDARG,
        "reserved pointer rejected", &checks, &failures);
    ole32_test_expect(shim_CoInitializeEx(NULL, 1) ==
                          OLE32_E_INVALIDARG,
                      "unknown COINIT flag rejected", &checks, &failures);
    ole32_test_expect(shim_CoInitialize(NULL) == S_OK,
                      "first STA initialization", &checks, &failures);
    ole32_test_expect(
        shim_CoInitializeEx(NULL,
                            OLE32_COINIT_APARTMENTTHREADED | 0x4U) ==
            OLE32_S_FALSE,
        "balanced repeated STA initialization", &checks, &failures);
    ole32_test_expect(shim_CoInitializeEx(NULL, 0) ==
                          OLE32_RPC_E_CHANGED_MODE,
                      "STA to MTA mismatch", &checks, &failures);
    shim_CoUninitialize();
    ole32_test_expect(shim_CoInitializeEx(NULL, 0) ==
                          OLE32_RPC_E_CHANGED_MODE,
                      "one STA reference remains", &checks, &failures);
    shim_CoUninitialize();
    ole32_test_expect(shim_CoInitializeEx(NULL, 0) == S_OK,
                      "MTA allowed after balanced teardown",
                      &checks, &failures);
    ole32_test_expect(shim_OleInitialize(NULL) ==
                          OLE32_RPC_E_CHANGED_MODE,
                      "OLE requires STA", &checks, &failures);
    shim_CoUninitialize();
    ole32_test_expect(shim_OleInitialize(NULL) == S_OK,
                      "OLE initializes STA", &checks, &failures);
    ole32_test_expect(shim_CoInitialize(NULL) == OLE32_S_FALSE,
                      "OLE and COM share the apartment count",
                      &checks, &failures);
    shim_OleUninitialize();
    shim_CoUninitialize();
    shim_CoUninitialize();
    ole32_test_expect(shim_CoInitializeEx(NULL, 0) == S_OK,
                      "unmatched teardown does not poison state",
                      &checks, &failures);
    shim_CoUninitialize();
    ole32_release_process(process_id);

    serial_puts("[OLE32-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT ole32_exports[] = {
    { "CoInitialize",      (PVOID)shim_CoInitialize,     1, CC_STDCALL },
    { "CoInitializeEx",    (PVOID)shim_CoInitializeEx,   2, CC_STDCALL },
    { "CoUninitialize",    (PVOID)shim_CoUninitialize,   0, CC_STDCALL },
    { "CoCreateInstance",  (PVOID)shim_CoCreateInstance, 5, CC_STDCALL },
    { "CoGetMalloc",       (PVOID)shim_CoGetMalloc,      2, CC_STDCALL },
    { "CoTaskMemAlloc",    (PVOID)shim_CoTaskMemAlloc,   1, CC_STDCALL },
    { "CoTaskMemFree",     (PVOID)shim_CoTaskMemFree,    1, CC_STDCALL },
    { "PropVariantClear",  (PVOID)shim_PropVariantClear, 1, CC_STDCALL },
    { "OleInitialize",     (PVOID)shim_OleInitialize,    1, CC_STDCALL },
    { "OleUninitialize",   (PVOID)shim_OleUninitialize,  0, CC_STDCALL },
    { "RegisterDragDrop",  (PVOID)shim_RegisterDragDrop, 2, CC_STDCALL },
    { "RevokeDragDrop",    (PVOID)shim_RevokeDragDrop,   1, CC_STDCALL },
    { "CoCreateGuid",      (PVOID)shim_CoCreateGuid,     1, CC_STDCALL },
    { "CLSIDFromString",   (PVOID)shim_CLSIDFromString,  2, CC_STDCALL },
    { "StringFromGUID2",   (PVOID)shim_StringFromGUID2,  3, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

HRESULT WINAPI shim_VariantClear(PVOID variant)
{
    if (!variant) return (HRESULT)0x80070057; /* E_INVALIDARG */
    OLE32_VARIANT *value = (OLE32_VARIANT *)variant;
    USHORT type = value->vt;
    if (!(type & 0x4000)) { /* VT_BYREF does not own the referenced value. */
        switch (type & 0x0FFF) {
        case 8:  /* VT_BSTR */
            shim_SysFreeString(value->value.bstrVal);
            break;
        case 9:  /* VT_DISPATCH */
        case 13: /* VT_UNKNOWN */
            ole32_interface_release(value->value.interface_pointer);
            break;
        default:
            if (type & 0x2000) return (HRESULT)0x80004001;
            break;
        }
    }
    *value = (OLE32_VARIANT){0};
    return S_OK;
}

HRESULT WINAPI shim_VariantCopy(PVOID destination, PCVOID source)
{
    if (!destination || !source) return (HRESULT)0x80070057;
    if (destination == source) return S_OK;
    HRESULT status = shim_VariantClear(destination);
    if (status != S_OK) return status;

    OLE32_VARIANT *out = (OLE32_VARIANT *)destination;
    const OLE32_VARIANT *in = (const OLE32_VARIANT *)source;
    *out = *in;
    if (!(in->vt & 0x4000)) {
        switch (in->vt & 0x0FFF) {
        case 8: {
            UINT length = shim_SysStringLen(in->value.bstrVal);
            out->value.bstrVal = in->value.bstrVal
                ? shim_SysAllocStringLen(in->value.bstrVal, length) : NULL;
            if (in->value.bstrVal && !out->value.bstrVal) {
                *out = (OLE32_VARIANT){0};
                return (HRESULT)0x8007000E;
            }
            break;
        }
        case 9:
        case 13:
            ole32_interface_addref(out->value.interface_pointer);
            break;
        }
    }
    return S_OK;
}

static const SHIM_EXPORT oleaut32_exports[] = {
    { "SysAllocString", (PVOID)shim_SysAllocString, 1, CC_STDCALL },
    { "SysAllocStringLen", (PVOID)shim_SysAllocStringLen, 2, CC_STDCALL },
    { "SysFreeString", (PVOID)shim_SysFreeString, 1, CC_STDCALL },
    { "SysStringLen", (PVOID)shim_SysStringLen, 1, CC_STDCALL },
    { "SysStringByteLen", (PVOID)shim_SysStringByteLen, 1, CC_STDCALL },
    { "SysAllocStringByteLen", (PVOID)shim_SysAllocStringByteLen, 2,
      CC_STDCALL },
    { "VariantInit", (PVOID)shim_VariantInit, 1, CC_STDCALL },
    { "VariantClear", (PVOID)shim_VariantClear, 1, CC_STDCALL },
    { "VariantCopy", (PVOID)shim_VariantCopy, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *ole32_abi_table(int *count) {
    *count = (int)(sizeof(ole32_exports)/sizeof(ole32_exports[0]));
    return (const WIN32_EXPORT *)ole32_exports;
}

const WIN32_EXPORT *oleaut32_abi_table(int *count) {
    *count = (int)(sizeof(oleaut32_exports)/sizeof(oleaut32_exports[0]));
    return (const WIN32_EXPORT *)oleaut32_exports;
}

static int ole_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID ole32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; ole32_exports[i].name; i++) {
        if (ole_strcmp(func_name, ole32_exports[i].name) == 0)
            return ole32_exports[i].func;
    }
    return NULL;
}

PVOID oleaut32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        switch (ordinal) {
        case 2: return (PVOID)shim_SysAllocString;
        case 4: return (PVOID)shim_SysAllocStringLen;
        case 6: return (PVOID)shim_SysFreeString;
        case 7: return (PVOID)shim_SysStringLen;
        case 8: return (PVOID)shim_VariantInit;
        case 9: return (PVOID)shim_VariantClear;
        case 10: return (PVOID)shim_VariantCopy;
        case 149: return (PVOID)shim_SysStringByteLen;
        case 150: return (PVOID)shim_SysAllocStringByteLen;
        default: return NULL;
        }
    }
    if (!func_name) return NULL;
    for (int i = 0; oleaut32_exports[i].name; i++) {
        if (ole_strcmp(func_name, oleaut32_exports[i].name) == 0)
            return oleaut32_exports[i].func;
    }
    return NULL;
}

PVOID ole32_shim_init(void)
{
    serial_puts("[OLE32] ole32.dll shim initialized\n");
    return (PVOID)ole32_exports;
}
