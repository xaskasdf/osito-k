/*
 * DXGI 1.1 compatibility surface.
 *
 * The guest exposes one virtio-gpu adapter. Factory and adapter objects obey
 * COM identity/refcount rules and only advertise interfaces whose vtables are
 * complete. Rendering objects remain unsupported until a D3D backend exists.
 */

#include "dxgi_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern uint64_t mem_get_total(void);
extern uint64_t virtio_gpu_host_visible_bytes(void);

#define DXGI_S_OK               ((HRESULT)0x00000000)
#define DXGI_E_NOTIMPL          ((HRESULT)0x80004001)
#define DXGI_E_NOINTERFACE      ((HRESULT)0x80004002)
#define DXGI_E_POINTER          ((HRESULT)0x80004003)
#define DXGI_E_OUTOFMEMORY      ((HRESULT)0x8007000E)
#define DXGI_E_INVALIDARG       ((HRESULT)0x80070057)
#define DXGI_ERROR_NOT_FOUND    ((HRESULT)0x887A0002)
#define DXGI_ERROR_MORE_DATA    ((HRESULT)0x887A0003)
#define DXGI_ERROR_UNSUPPORTED  ((HRESULT)0x887A0004)

#define DXGI_PRIVATE_SLOTS 4
#define DXGI_PRIVATE_BYTES 128

typedef struct dxgi_factory DXGI_FACTORY;
typedef struct dxgi_adapter DXGI_ADAPTER;

typedef struct dxgi_luid {
    ULONG LowPart;
    LONG HighPart;
} DXGI_LUID;

typedef struct dxgi_adapter_desc {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    DXGI_LUID AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct dxgi_adapter_desc1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    DXGI_LUID AdapterLuid;
    UINT Flags;
} DXGI_ADAPTER_DESC1;

typedef struct dxgi_private_slot {
    BOOL valid;
    BOOL is_interface;
    GUID guid;
    UINT size;
    BYTE bytes[DXGI_PRIVATE_BYTES];
    PVOID interface;
} DXGI_PRIVATE_SLOT;

typedef struct dxgi_factory_vtbl {
    HRESULT (WINAPI *QueryInterface)(DXGI_FACTORY *, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(DXGI_FACTORY *);
    ULONG (WINAPI *Release)(DXGI_FACTORY *);
    HRESULT (WINAPI *SetPrivateData)(DXGI_FACTORY *, LPCGUID, UINT, PCVOID);
    HRESULT (WINAPI *SetPrivateDataInterface)(DXGI_FACTORY *, LPCGUID, PVOID);
    HRESULT (WINAPI *GetPrivateData)(DXGI_FACTORY *, LPCGUID, UINT *, PVOID);
    HRESULT (WINAPI *GetParent)(DXGI_FACTORY *, REFIID, PVOID *);
    HRESULT (WINAPI *EnumAdapters)(DXGI_FACTORY *, UINT, PVOID *);
    HRESULT (WINAPI *MakeWindowAssociation)(DXGI_FACTORY *, HANDLE, UINT);
    HRESULT (WINAPI *GetWindowAssociation)(DXGI_FACTORY *, HANDLE *);
    HRESULT (WINAPI *CreateSwapChain)(DXGI_FACTORY *, PVOID, PVOID, PVOID *);
    HRESULT (WINAPI *CreateSoftwareAdapter)(DXGI_FACTORY *, HANDLE, PVOID *);
    HRESULT (WINAPI *EnumAdapters1)(DXGI_FACTORY *, UINT, PVOID *);
    BOOL (WINAPI *IsCurrent)(DXGI_FACTORY *);
} DXGI_FACTORY_VTBL;

typedef struct dxgi_adapter_vtbl {
    HRESULT (WINAPI *QueryInterface)(DXGI_ADAPTER *, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(DXGI_ADAPTER *);
    ULONG (WINAPI *Release)(DXGI_ADAPTER *);
    HRESULT (WINAPI *SetPrivateData)(DXGI_ADAPTER *, LPCGUID, UINT, PCVOID);
    HRESULT (WINAPI *SetPrivateDataInterface)(DXGI_ADAPTER *, LPCGUID, PVOID);
    HRESULT (WINAPI *GetPrivateData)(DXGI_ADAPTER *, LPCGUID, UINT *, PVOID);
    HRESULT (WINAPI *GetParent)(DXGI_ADAPTER *, REFIID, PVOID *);
    HRESULT (WINAPI *EnumOutputs)(DXGI_ADAPTER *, UINT, PVOID *);
    HRESULT (WINAPI *GetDesc)(DXGI_ADAPTER *, DXGI_ADAPTER_DESC *);
    HRESULT (WINAPI *CheckInterfaceSupport)(DXGI_ADAPTER *, LPCGUID,
                                             LARGE_INTEGER *);
    HRESULT (WINAPI *GetDesc1)(DXGI_ADAPTER *, DXGI_ADAPTER_DESC1 *);
} DXGI_ADAPTER_VTBL;

struct dxgi_adapter {
    const DXGI_ADAPTER_VTBL *lpVtbl;
    volatile ULONG refs;
    DXGI_FACTORY *parent;
    DXGI_PRIVATE_SLOT private_data[DXGI_PRIVATE_SLOTS];
};

struct dxgi_factory {
    const DXGI_FACTORY_VTBL *lpVtbl;
    volatile ULONG refs;
    HANDLE associated_window;
    UINT association_flags;
    DXGI_PRIVATE_SLOT private_data[DXGI_PRIVATE_SLOTS];
    DXGI_ADAPTER adapter;
};

static const GUID dxgi_iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID dxgi_iid_object = {
    0xAEC22FB8, 0x76F3, 0x4639,
    { 0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E }
};
static const GUID dxgi_iid_factory = {
    0x7B7166EC, 0x21C7, 0x44AE,
    { 0xB2, 0x1A, 0xC9, 0xAE, 0x32, 0x1A, 0xE3, 0x69 }
};
static const GUID dxgi_iid_factory1 = {
    0x770AAE78, 0xF26F, 0x4DBA,
    { 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87 }
};
static const GUID dxgi_iid_adapter = {
    0x2411E7E1, 0x12AC, 0x4CCF,
    { 0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0 }
};
static const GUID dxgi_iid_adapter1 = {
    0x29038F61, 0x3839, 0x4626,
    { 0x91, 0xFD, 0x08, 0x68, 0x79, 0x01, 0x1A, 0x05 }
};

static const DXGI_FACTORY_VTBL dxgi_factory_vtbl;
static const DXGI_ADAPTER_VTBL dxgi_adapter_vtbl;

static void dxgi_zero(PVOID memory, SIZE_T size)
{
    BYTE *out = (BYTE *)memory;
    while (size--) *out++ = 0;
}

static void dxgi_copy(PVOID destination, PCVOID source, SIZE_T size)
{
    BYTE *out = (BYTE *)destination;
    const BYTE *in = (const BYTE *)source;
    while (size--) *out++ = *in++;
}

static BOOL dxgi_guid_equal(LPCGUID left, LPCGUID right)
{
    if (!left || !right) return FALSE;
    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3)
        return FALSE;
    for (int i = 0; i < 8; i++)
        if (left->Data4[i] != right->Data4[i]) return FALSE;
    return TRUE;
}

static void dxgi_trace_guid(LPCGUID guid)
{
    if (!guid) {
        serial_puts("<null>");
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

static ULONG dxgi_unknown_addref(PVOID object)
{
    if (!object) return 0;
    PVOID *table = *(PVOID **)object;
    if (!table || !table[1]) return 0;
    return ((ULONG (WINAPI *)(PVOID))table[1])(object);
}

static ULONG dxgi_unknown_release(PVOID object)
{
    if (!object) return 0;
    PVOID *table = *(PVOID **)object;
    if (!table || !table[2]) return 0;
    return ((ULONG (WINAPI *)(PVOID))table[2])(object);
}

static DXGI_PRIVATE_SLOT *dxgi_private_find(DXGI_PRIVATE_SLOT *slots,
                                             LPCGUID guid)
{
    for (int i = 0; i < DXGI_PRIVATE_SLOTS; i++)
        if (slots[i].valid && dxgi_guid_equal(&slots[i].guid, guid))
            return &slots[i];
    return NULL;
}

static DXGI_PRIVATE_SLOT *dxgi_private_reserve(DXGI_PRIVATE_SLOT *slots,
                                                LPCGUID guid)
{
    DXGI_PRIVATE_SLOT *slot = dxgi_private_find(slots, guid);
    if (slot) return slot;
    for (int i = 0; i < DXGI_PRIVATE_SLOTS; i++)
        if (!slots[i].valid) return &slots[i];
    return NULL;
}

static void dxgi_private_clear_slot(DXGI_PRIVATE_SLOT *slot)
{
    PVOID old_interface = slot->is_interface ? slot->interface : NULL;
    dxgi_zero(slot, sizeof(*slot));
    if (old_interface) dxgi_unknown_release(old_interface);
}

static void dxgi_private_clear_all(DXGI_PRIVATE_SLOT *slots)
{
    for (int i = 0; i < DXGI_PRIVATE_SLOTS; i++)
        if (slots[i].valid) dxgi_private_clear_slot(&slots[i]);
}

static HRESULT dxgi_private_set(DXGI_PRIVATE_SLOT *slots, LPCGUID guid,
                                UINT size, PCVOID data)
{
    if (!guid || (size && !data)) return DXGI_E_INVALIDARG;
    DXGI_PRIVATE_SLOT *slot = dxgi_private_find(slots, guid);
    if (!size) {
        if (slot) dxgi_private_clear_slot(slot);
        return DXGI_S_OK;
    }
    if (size > DXGI_PRIVATE_BYTES) return DXGI_E_OUTOFMEMORY;
    if (!slot) slot = dxgi_private_reserve(slots, guid);
    if (!slot) return DXGI_E_OUTOFMEMORY;
    if (slot->valid) dxgi_private_clear_slot(slot);
    slot->valid = TRUE;
    slot->guid = *guid;
    slot->size = size;
    dxgi_copy(slot->bytes, data, size);
    return DXGI_S_OK;
}

static HRESULT dxgi_private_set_interface(DXGI_PRIVATE_SLOT *slots,
                                          LPCGUID guid, PVOID object)
{
    if (!guid) return DXGI_E_INVALIDARG;
    DXGI_PRIVATE_SLOT *slot = dxgi_private_find(slots, guid);
    if (!object) {
        if (slot) dxgi_private_clear_slot(slot);
        return DXGI_S_OK;
    }
    if (!slot) slot = dxgi_private_reserve(slots, guid);
    if (!slot) return DXGI_E_OUTOFMEMORY;
    dxgi_unknown_addref(object);
    if (slot->valid) dxgi_private_clear_slot(slot);
    slot->valid = TRUE;
    slot->is_interface = TRUE;
    slot->guid = *guid;
    slot->size = sizeof(PVOID);
    slot->interface = object;
    return DXGI_S_OK;
}

static HRESULT dxgi_private_get(DXGI_PRIVATE_SLOT *slots, LPCGUID guid,
                                UINT *size, PVOID data)
{
    if (!guid || !size) return DXGI_E_INVALIDARG;
    DXGI_PRIVATE_SLOT *slot = dxgi_private_find(slots, guid);
    if (!slot) return DXGI_ERROR_NOT_FOUND;
    UINT required = slot->size;
    if (!data || *size < required) {
        *size = required;
        return DXGI_ERROR_MORE_DATA;
    }
    if (slot->is_interface) {
        *(PVOID *)data = slot->interface;
        dxgi_unknown_addref(slot->interface);
    } else {
        dxgi_copy(data, slot->bytes, required);
    }
    *size = required;
    return DXGI_S_OK;
}

static BOOL dxgi_factory_iid_supported(REFIID iid)
{
    return dxgi_guid_equal(iid, &dxgi_iid_iunknown) ||
           dxgi_guid_equal(iid, &dxgi_iid_object) ||
           dxgi_guid_equal(iid, &dxgi_iid_factory) ||
           dxgi_guid_equal(iid, &dxgi_iid_factory1);
}

static BOOL dxgi_adapter_iid_supported(REFIID iid)
{
    return dxgi_guid_equal(iid, &dxgi_iid_iunknown) ||
           dxgi_guid_equal(iid, &dxgi_iid_object) ||
           dxgi_guid_equal(iid, &dxgi_iid_adapter) ||
           dxgi_guid_equal(iid, &dxgi_iid_adapter1);
}

static ULONG WINAPI dxgi_factory_AddRef(DXGI_FACTORY *self)
{
    if (!self) return 0;
    return __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dxgi_factory_Release(DXGI_FACTORY *self)
{
    if (!self) return 0;
    ULONG current = __atomic_load_n(&self->refs, __ATOMIC_ACQUIRE);
    while (current) {
        ULONG next = current - 1;
        if (__atomic_compare_exchange_n(&self->refs, &current, next, FALSE,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (!next) {
                dxgi_private_clear_all(self->private_data);
                kfree(self);
            }
            return next;
        }
    }
    return 0;
}

static HRESULT WINAPI dxgi_factory_QueryInterface(DXGI_FACTORY *self,
                                                   REFIID iid, PVOID *object)
{
    if (!object) return DXGI_E_POINTER;
    *object = NULL;
    if (!self || !dxgi_factory_iid_supported(iid))
        return DXGI_E_NOINTERFACE;
    dxgi_factory_AddRef(self);
    *object = self;
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_factory_SetPrivateData(DXGI_FACTORY *self,
                                                   LPCGUID guid, UINT size,
                                                   PCVOID data)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_set(self->private_data, guid, size, data);
}

static HRESULT WINAPI dxgi_factory_SetPrivateDataInterface(
    DXGI_FACTORY *self, LPCGUID guid, PVOID object)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_set_interface(self->private_data, guid, object);
}

static HRESULT WINAPI dxgi_factory_GetPrivateData(DXGI_FACTORY *self,
                                                   LPCGUID guid, UINT *size,
                                                   PVOID data)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_get(self->private_data, guid, size, data);
}

static HRESULT WINAPI dxgi_factory_GetParent(DXGI_FACTORY *self, REFIID iid,
                                              PVOID *parent)
{
    (void)self;
    (void)iid;
    if (!parent) return DXGI_E_POINTER;
    *parent = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static ULONG WINAPI dxgi_adapter_AddRef(DXGI_ADAPTER *self)
{
    if (!self || !self->parent) return 0;
    dxgi_factory_AddRef(self->parent);
    return __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dxgi_adapter_Release(DXGI_ADAPTER *self)
{
    if (!self || !self->parent) return 0;
    ULONG current = __atomic_load_n(&self->refs, __ATOMIC_ACQUIRE);
    while (current) {
        ULONG next = current - 1;
        if (__atomic_compare_exchange_n(&self->refs, &current, next, FALSE,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            DXGI_FACTORY *parent = self->parent;
            if (!next) dxgi_private_clear_all(self->private_data);
            dxgi_factory_Release(parent);
            return next;
        }
    }
    return 0;
}

static HRESULT WINAPI dxgi_adapter_QueryInterface(DXGI_ADAPTER *self,
                                                   REFIID iid, PVOID *object)
{
    if (!object) return DXGI_E_POINTER;
    *object = NULL;
    if (!self || !dxgi_adapter_iid_supported(iid))
        return DXGI_E_NOINTERFACE;
    dxgi_adapter_AddRef(self);
    *object = self;
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_adapter_SetPrivateData(DXGI_ADAPTER *self,
                                                   LPCGUID guid, UINT size,
                                                   PCVOID data)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_set(self->private_data, guid, size, data);
}

static HRESULT WINAPI dxgi_adapter_SetPrivateDataInterface(
    DXGI_ADAPTER *self, LPCGUID guid, PVOID object)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_set_interface(self->private_data, guid, object);
}

static HRESULT WINAPI dxgi_adapter_GetPrivateData(DXGI_ADAPTER *self,
                                                   LPCGUID guid, UINT *size,
                                                   PVOID data)
{
    if (!self) return DXGI_E_INVALIDARG;
    return dxgi_private_get(self->private_data, guid, size, data);
}

static HRESULT WINAPI dxgi_adapter_GetParent(DXGI_ADAPTER *self, REFIID iid,
                                              PVOID *parent)
{
    if (!parent) return DXGI_E_POINTER;
    *parent = NULL;
    if (!self || !self->parent) return DXGI_E_INVALIDARG;
    return dxgi_factory_QueryInterface(self->parent, iid, parent);
}

static HRESULT WINAPI dxgi_factory_EnumAdapters(DXGI_FACTORY *self,
                                                 UINT index, PVOID *adapter)
{
    if (!adapter) return DXGI_E_POINTER;
    *adapter = NULL;
    if (!self) return DXGI_E_INVALIDARG;
    if (index != 0) return DXGI_ERROR_NOT_FOUND;
    dxgi_adapter_AddRef(&self->adapter);
    *adapter = &self->adapter;
    serial_puts("[DXGI] EnumAdapters(0) -> virtio-gpu\n");
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_factory_MakeWindowAssociation(DXGI_FACTORY *self,
                                                          HANDLE window,
                                                          UINT flags)
{
    if (!self || (flags & ~7U)) return DXGI_E_INVALIDARG;
    self->associated_window = window;
    self->association_flags = flags;
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_factory_GetWindowAssociation(DXGI_FACTORY *self,
                                                         HANDLE *window)
{
    if (!window) return DXGI_E_POINTER;
    if (!self) {
        *window = NULL;
        return DXGI_E_INVALIDARG;
    }
    *window = self->associated_window;
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_factory_CreateSwapChain(DXGI_FACTORY *self,
                                                    PVOID device, PVOID desc,
                                                    PVOID *swapchain)
{
    (void)self;
    (void)device;
    (void)desc;
    if (!swapchain) return DXGI_E_POINTER;
    *swapchain = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT WINAPI dxgi_factory_CreateSoftwareAdapter(DXGI_FACTORY *self,
                                                          HANDLE module,
                                                          PVOID *adapter)
{
    (void)self;
    (void)module;
    if (!adapter) return DXGI_E_POINTER;
    *adapter = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT WINAPI dxgi_factory_EnumAdapters1(DXGI_FACTORY *self,
                                                  UINT index, PVOID *adapter)
{
    return dxgi_factory_EnumAdapters(self, index, adapter);
}

static BOOL WINAPI dxgi_factory_IsCurrent(DXGI_FACTORY *self)
{
    return self ? TRUE : FALSE;
}

static HRESULT WINAPI dxgi_adapter_EnumOutputs(DXGI_ADAPTER *self, UINT index,
                                               PVOID *output)
{
    (void)self;
    (void)index;
    if (!output) return DXGI_E_POINTER;
    *output = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static void dxgi_fill_desc1(DXGI_ADAPTER_DESC1 *desc)
{
    static const char name[] = "Osito-K Virtio GPU (Venus)";
    dxgi_zero(desc, sizeof(*desc));
    for (UINT i = 0; name[i] && i < 127; i++)
        desc->Description[i] = (WCHAR)(BYTE)name[i];
    desc->VendorId = 0x1AF4;
    desc->DeviceId = 0x1050;
    desc->Revision = 1;
    desc->DedicatedVideoMemory = virtio_gpu_host_visible_bytes();
    if (!desc->DedicatedVideoMemory)
        desc->DedicatedVideoMemory = 256ULL * 1024ULL * 1024ULL;
    desc->SharedSystemMemory = mem_get_total() / 2;
    desc->AdapterLuid.LowPart = 0x4F534954;
    desc->AdapterLuid.HighPart = 1;
    desc->Flags = 0;
}

static HRESULT WINAPI dxgi_adapter_GetDesc(DXGI_ADAPTER *self,
                                            DXGI_ADAPTER_DESC *desc)
{
    if (!self || !desc) return DXGI_E_INVALIDARG;
    DXGI_ADAPTER_DESC1 full;
    dxgi_fill_desc1(&full);
    dxgi_copy(desc, &full, sizeof(*desc));
    serial_puts("[DXGI] GetDesc -> virtio-gpu, host-visible MB=");
    serial_putdec(full.DedicatedVideoMemory >> 20);
    serial_puts("\n");
    return DXGI_S_OK;
}

static HRESULT WINAPI dxgi_adapter_CheckInterfaceSupport(
    DXGI_ADAPTER *self, LPCGUID guid, LARGE_INTEGER *version)
{
    (void)self;
    (void)guid;
    if (version) version->QuadPart = 0;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT WINAPI dxgi_adapter_GetDesc1(DXGI_ADAPTER *self,
                                             DXGI_ADAPTER_DESC1 *desc)
{
    if (!self || !desc) return DXGI_E_INVALIDARG;
    dxgi_fill_desc1(desc);
    serial_puts("[DXGI] GetDesc1 -> virtio-gpu, host-visible MB=");
    serial_putdec(desc->DedicatedVideoMemory >> 20);
    serial_puts("\n");
    return DXGI_S_OK;
}

static const DXGI_FACTORY_VTBL dxgi_factory_vtbl = {
    dxgi_factory_QueryInterface,
    dxgi_factory_AddRef,
    dxgi_factory_Release,
    dxgi_factory_SetPrivateData,
    dxgi_factory_SetPrivateDataInterface,
    dxgi_factory_GetPrivateData,
    dxgi_factory_GetParent,
    dxgi_factory_EnumAdapters,
    dxgi_factory_MakeWindowAssociation,
    dxgi_factory_GetWindowAssociation,
    dxgi_factory_CreateSwapChain,
    dxgi_factory_CreateSoftwareAdapter,
    dxgi_factory_EnumAdapters1,
    dxgi_factory_IsCurrent
};

static const DXGI_ADAPTER_VTBL dxgi_adapter_vtbl = {
    dxgi_adapter_QueryInterface,
    dxgi_adapter_AddRef,
    dxgi_adapter_Release,
    dxgi_adapter_SetPrivateData,
    dxgi_adapter_SetPrivateDataInterface,
    dxgi_adapter_GetPrivateData,
    dxgi_adapter_GetParent,
    dxgi_adapter_EnumOutputs,
    dxgi_adapter_GetDesc,
    dxgi_adapter_CheckInterfaceSupport,
    dxgi_adapter_GetDesc1
};

static HRESULT dxgi_create_factory(REFIID iid, PVOID *object,
                                   const char *entrypoint)
{
    if (!object) return DXGI_E_POINTER;
    *object = NULL;
    if (!iid || !dxgi_factory_iid_supported(iid)) {
        serial_puts("[DXGI] ");
        serial_puts(entrypoint);
        serial_puts(" iid=");
        dxgi_trace_guid(iid);
        serial_puts(" -> E_NOINTERFACE\n");
        return DXGI_E_NOINTERFACE;
    }
    DXGI_FACTORY *factory = (DXGI_FACTORY *)kmalloc(sizeof(*factory));
    if (!factory) return DXGI_E_OUTOFMEMORY;
    dxgi_zero(factory, sizeof(*factory));
    factory->lpVtbl = &dxgi_factory_vtbl;
    factory->refs = 1;
    factory->adapter.lpVtbl = &dxgi_adapter_vtbl;
    factory->adapter.parent = factory;
    *object = factory;
    serial_puts("[DXGI] ");
    serial_puts(entrypoint);
    serial_puts(" -> factory 1.1\n");
    return DXGI_S_OK;
}

HRESULT WINAPI shim_CreateDXGIFactory(PCVOID riid, PVOID *factory)
{
    return dxgi_create_factory((REFIID)riid, factory, "CreateDXGIFactory");
}

HRESULT WINAPI shim_CreateDXGIFactory1(PCVOID riid, PVOID *factory)
{
    return dxgi_create_factory((REFIID)riid, factory, "CreateDXGIFactory1");
}

HRESULT WINAPI shim_CreateDXGIFactory2(UINT flags, PCVOID riid, PVOID *factory)
{
    (void)flags;
    return dxgi_create_factory((REFIID)riid, factory, "CreateDXGIFactory2");
}

HRESULT WINAPI shim_DXGIGetDebugInterface1(UINT flags, PCVOID riid,
                                           PVOID *debug)
{
    (void)flags;
    (void)riid;
    if (!debug) return DXGI_E_POINTER;
    *debug = NULL;
    return DXGI_E_NOINTERFACE;
}

HRESULT WINAPI shim_DXGIDeclareAdapterRemovalSupport(void)
{
    return DXGI_S_OK;
}

int dxgi_selftest(void)
{
    int checks = 0;
    int failures = 0;
#define DXGI_CHECK(expression) do { checks++; if (!(expression)) failures++; } while (0)

    PVOID raw_factory = NULL;
    HRESULT status = shim_CreateDXGIFactory1(&dxgi_iid_factory1, &raw_factory);
    DXGI_CHECK(status == DXGI_S_OK && raw_factory != NULL);
    DXGI_FACTORY *factory = (DXGI_FACTORY *)raw_factory;

    PVOID old_factory = NULL;
    if (factory) {
        status = factory->lpVtbl->QueryInterface(factory, &dxgi_iid_factory,
                                                  &old_factory);
        DXGI_CHECK(status == DXGI_S_OK && old_factory == factory);
        if (old_factory) factory->lpVtbl->Release((DXGI_FACTORY *)old_factory);
    }

    PVOID raw_adapter = NULL;
    if (factory) {
        status = factory->lpVtbl->EnumAdapters1(factory, 0, &raw_adapter);
        DXGI_CHECK(status == DXGI_S_OK && raw_adapter != NULL);
    }
    DXGI_ADAPTER *adapter = (DXGI_ADAPTER *)raw_adapter;
    DXGI_ADAPTER_DESC1 desc;
    dxgi_zero(&desc, sizeof(desc));
    if (adapter) {
        status = adapter->lpVtbl->GetDesc1(adapter, &desc);
        DXGI_CHECK(status == DXGI_S_OK);
        DXGI_CHECK(desc.Description[0] != 0);
        DXGI_CHECK(desc.VendorId == 0x1AF4 && desc.DeviceId == 0x1050);
        DXGI_CHECK(desc.DedicatedVideoMemory != 0);

        PVOID queried = NULL;
        status = adapter->lpVtbl->QueryInterface(adapter, &dxgi_iid_adapter,
                                                  &queried);
        DXGI_CHECK(status == DXGI_S_OK && queried == adapter);
        if (queried) adapter->lpVtbl->Release((DXGI_ADAPTER *)queried);

        PVOID parent = NULL;
        status = adapter->lpVtbl->GetParent(adapter, &dxgi_iid_factory1,
                                             &parent);
        DXGI_CHECK(status == DXGI_S_OK && parent == factory);
        if (parent) factory->lpVtbl->Release((DXGI_FACTORY *)parent);

        PVOID output = (PVOID)1;
        status = adapter->lpVtbl->EnumOutputs(adapter, 0, &output);
        DXGI_CHECK(status == DXGI_ERROR_NOT_FOUND && output == NULL);
    }

    if (factory) {
        PVOID extra = (PVOID)1;
        status = factory->lpVtbl->EnumAdapters1(factory, 1, &extra);
        DXGI_CHECK(status == DXGI_ERROR_NOT_FOUND && extra == NULL);

        static const GUID test_key = {
            0x7A5B91C1, 0x4089, 0x4D0F,
            { 0xA4, 0x06, 0x65, 0xEA, 0xA0, 0x35, 0x76, 0x21 }
        };
        UINT value = 0xC001D00D;
        status = factory->lpVtbl->SetPrivateData(factory, &test_key,
                                                  sizeof(value), &value);
        DXGI_CHECK(status == DXGI_S_OK);
        UINT size = 0;
        status = factory->lpVtbl->GetPrivateData(factory, &test_key, &size,
                                                  NULL);
        DXGI_CHECK(status == DXGI_ERROR_MORE_DATA && size == sizeof(value));
        UINT readback = 0;
        status = factory->lpVtbl->GetPrivateData(factory, &test_key, &size,
                                                  &readback);
        DXGI_CHECK(status == DXGI_S_OK && readback == value);
    }

    if (adapter) adapter->lpVtbl->Release(adapter);
    if (factory) factory->lpVtbl->Release(factory);

    serial_puts("[DXGITEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef DXGI_CHECK
    return failures;
}

static const WIN32_EXPORT dxgi_exports[] = {
    { "CreateDXGIFactory", (PVOID)shim_CreateDXGIFactory, 2, CC_STDCALL },
    { "CreateDXGIFactory1", (PVOID)shim_CreateDXGIFactory1, 2, CC_STDCALL },
    { "CreateDXGIFactory2", (PVOID)shim_CreateDXGIFactory2, 3, CC_STDCALL },
    { "DXGIGetDebugInterface1", (PVOID)shim_DXGIGetDebugInterface1, 3,
      CC_STDCALL },
    { "DXGIDeclareAdapterRemovalSupport",
      (PVOID)shim_DXGIDeclareAdapterRemovalSupport, 0, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *dxgi_abi_table(int *count)
{
    *count = (int)(sizeof(dxgi_exports) / sizeof(dxgi_exports[0]));
    return dxgi_exports;
}

static int dxgi_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID dxgi_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; dxgi_exports[i].name; i++) {
        if (dxgi_strcmp(func_name, dxgi_exports[i].name) == 0)
            return dxgi_exports[i].func;
    }
    return NULL;
}

PVOID dxgi_shim_init(void)
{
    return (PVOID)dxgi_exports;
}
