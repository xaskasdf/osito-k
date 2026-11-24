/*
 * OsitoK Windows Compatibility Layer - oleacc.dll / MSAA implementation.
 *
 * Standard accessible objects are live proxies over USER32 windows. They
 * retain COM identity and enumeration state, but never retain a pointer into
 * USER32's window table; every property query takes a fresh snapshot.
 */

#include "oleacc_shim.h"
#include "ole32_shim.h"
#include "win32_abi.h"

_Static_assert(sizeof(OLEACC_VARIANT) == 24, "Win64 VARIANT ABI");

extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);
extern PVOID WINAPI crt_malloc(SIZE_T size);
extern void WINAPI crt_free(PVOID pointer);
extern DWORD WINAPI GetCurrentProcessId(void);

#define OA_ROLE_SYSTEM_WINDOW     0x09
#define OA_ROLE_SYSTEM_CLIENT     0x0A

#define OA_STATE_UNAVAILABLE      0x00000001
#define OA_STATE_FOCUSED          0x00000004
#define OA_STATE_INVISIBLE        0x00008000
#define OA_STATE_OFFSCREEN        0x00010000
#define OA_STATE_SIZEABLE         0x00020000
#define OA_STATE_MOVEABLE         0x00040000
#define OA_STATE_FOCUSABLE        0x00100000

#define OA_NAVDIR_NEXT            0x05
#define OA_NAVDIR_PREVIOUS        0x06
#define OA_NAVDIR_FIRSTCHILD      0x07
#define OA_NAVDIR_LASTCHILD       0x08

#define OA_SELFLAG_TAKEFOCUS      0x01
#define OA_SELFLAG_SELECTION_MASK 0x1E
#define OA_SELFLAG_VALID          0x1F

#define OA_DISPATCH_PROPERTYGET   0x02

#define OA_DISPID_PARENT          (-5000)
#define OA_DISPID_CHILDCOUNT      (-5001)
#define OA_DISPID_CHILD           (-5002)
#define OA_DISPID_NAME            (-5003)
#define OA_DISPID_VALUE           (-5004)
#define OA_DISPID_DESCRIPTION     (-5005)
#define OA_DISPID_ROLE            (-5006)
#define OA_DISPID_STATE           (-5007)
#define OA_DISPID_HELP            (-5008)
#define OA_DISPID_HELPTOPIC       (-5009)
#define OA_DISPID_KEYBOARDSHORTCUT (-5010)
#define OA_DISPID_FOCUS           (-5011)
#define OA_DISPID_SELECTION       (-5012)
#define OA_DISPID_DEFAULTACTION   (-5013)
#define OA_DISPID_SELECT          (-5014)
#define OA_DISPID_LOCATION        (-5015)
#define OA_DISPID_NAVIGATE        (-5016)
#define OA_DISPID_HITTEST         (-5017)
#define OA_DISPID_DODEFAULTACTION (-5018)

#define OA_MAX_WINDOWS            256
#define OA_TOKEN_SLOTS            128
#define OA_TOKEN_PREFIX           0x4A000000U

static const GUID oa_iid_unknown = {
    0x00000000, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};
static const GUID oa_iid_dispatch = {
    0x00020400, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};
static const GUID oa_iid_enumvariant = {
    0x00020404, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};
static const GUID oa_iid_accessible = {
    0x618736E0, 0x3C3D, 0x11CF,
    {0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}
};

typedef struct {
    const PVOID lpVtbl;
} OA_UNKNOWN;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
} OA_UNKNOWN_VTBL;

typedef struct {
    HWND window;
    LONG object_id;
} OA_TARGET;

typedef struct {
    OLEACC_ACCESSIBLE accessible;
    OLEACC_ENUMVARIANT enumerator;
    volatile ULONG refs;
    HWND window;
    LONG object_id;
    DWORD owner_pid;
    ULONG enum_index;
} OA_OBJECT;

typedef struct {
    BOOL used;
    ULONG generation;
    ULONG token;
    DWORD owner_pid;
    WPARAM message_param;
    GUID iid;
    PVOID object;
} OA_TOKEN;

static OA_TOKEN oa_tokens[OA_TOKEN_SLOTS];
static volatile int oa_token_lock;
static ULONG oa_next_token_generation = 1;

static const OLEACC_ACCESSIBLE_VTBL oa_accessible_vtbl;
static const OLEACC_ENUMVARIANT_VTBL oa_enumvariant_vtbl;

static int oa_guid_equal(REFIID left, REFIID right)
{
    if (!left || !right) return 0;
    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (left->Data4[i] != right->Data4[i]) return 0;
    return 1;
}

static void oa_token_acquire(void)
{
    while (__atomic_exchange_n(&oa_token_lock, 1, __ATOMIC_ACQ_REL))
        __asm__ volatile ("pause");
}

static void oa_token_release(void)
{
    __atomic_store_n(&oa_token_lock, 0, __ATOMIC_RELEASE);
}

static HRESULT oa_unknown_query(PVOID unknown, REFIID iid, PVOID *object)
{
    if (!unknown || !object) return OLEACC_E_INVALIDARG;
    OA_UNKNOWN *iface = (OA_UNKNOWN *)unknown;
    const OA_UNKNOWN_VTBL *vtbl = (const OA_UNKNOWN_VTBL *)iface->lpVtbl;
    if (!vtbl || !vtbl->QueryInterface) return OLEACC_E_NOINTERFACE;
    return vtbl->QueryInterface(unknown, iid, object);
}

static ULONG oa_unknown_release(PVOID unknown)
{
    if (!unknown) return 0;
    OA_UNKNOWN *iface = (OA_UNKNOWN *)unknown;
    const OA_UNKNOWN_VTBL *vtbl = (const OA_UNKNOWN_VTBL *)iface->lpVtbl;
    return vtbl && vtbl->Release ? vtbl->Release(unknown) : 0;
}

static void oa_variant_empty(OLEACC_VARIANT *variant)
{
    if (variant) *variant = (OLEACC_VARIANT){0};
}

static void oa_variant_i4(OLEACC_VARIANT *variant, LONG value)
{
    oa_variant_empty(variant);
    variant->vt = OLEACC_VT_I4;
    variant->value.lVal = value;
}

static OLEACC_VARIANT oa_self_variant(void)
{
    OLEACC_VARIANT value = {0};
    value.vt = OLEACC_VT_I4;
    value.value.lVal = OLEACC_CHILDID_SELF;
    return value;
}

static OA_OBJECT *oa_from_accessible(OLEACC_ACCESSIBLE *accessible)
{
    return CONTAINING_RECORD(accessible, OA_OBJECT, accessible);
}

static OA_OBJECT *oa_from_enumerator(OLEACC_ENUMVARIANT *enumerator)
{
    return CONTAINING_RECORD(enumerator, OA_OBJECT, enumerator);
}

static BOOL oa_snapshot(const OA_TARGET *target,
                        USER32_ACCESSIBLE_WINDOW_INFO *info)
{
    return target &&
           (target->object_id == OLEACC_OBJID_WINDOW ||
            target->object_id == OLEACC_OBJID_CLIENT) &&
           user32_accessibility_snapshot(target->window, info);
}

static BOOL oa_object_snapshot(OA_OBJECT *object,
                               USER32_ACCESSIBLE_WINDOW_INFO *info)
{
    OA_TARGET target = { object->window, object->object_id };
    if (!oa_snapshot(&target, info)) return FALSE;
    return info->owner_pid == object->owner_pid;
}

static LONG oa_target_child_count(const OA_TARGET *target)
{
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(target, &info)) return -1;
    if (target->object_id == OLEACC_OBJID_WINDOW) return 1;
    return (LONG)user32_accessibility_children(target->window, NULL, 0);
}

static BOOL oa_target_child_at(const OA_TARGET *target, LONG index,
                               OA_TARGET *child)
{
    LONG count = oa_target_child_count(target);
    if (!child || index < 0 || index >= count) return FALSE;

    if (target->object_id == OLEACC_OBJID_WINDOW) {
        child->window = target->window;
        child->object_id = OLEACC_OBJID_CLIENT;
        return TRUE;
    }

    HWND windows[OA_MAX_WINDOWS];
    UINT total = user32_accessibility_children(target->window, windows,
                                               OA_MAX_WINDOWS);
    if ((UINT)index >= total || (UINT)index >= OA_MAX_WINDOWS) return FALSE;
    child->window = windows[index];
    child->object_id = OLEACC_OBJID_WINDOW;
    return TRUE;
}

static BOOL oa_target_equal(const OA_TARGET *left, const OA_TARGET *right)
{
    return left && right && left->window == right->window &&
           left->object_id == right->object_id;
}

static BOOL oa_target_parent(const OA_TARGET *target, OA_TARGET *parent)
{
    if (!target || !parent) return FALSE;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(target, &info)) return FALSE;

    if (target->object_id == OLEACC_OBJID_CLIENT) {
        parent->window = target->window;
        parent->object_id = OLEACC_OBJID_WINDOW;
        return TRUE;
    }

    HWND parent_window = info.parent ? info.parent : info.owner;
    if (!parent_window) return FALSE;
    USER32_ACCESSIBLE_WINDOW_INFO parent_info;
    if (!user32_accessibility_snapshot(parent_window, &parent_info))
        return FALSE;
    parent->window = parent_window;
    parent->object_id = OLEACC_OBJID_CLIENT;
    return TRUE;
}

static BOOL oa_resolve_variant_target(OA_OBJECT *object,
                                      OLEACC_VARIANT child_variant,
                                      OA_TARGET *target)
{
    if (!object || !target || child_variant.vt != OLEACC_VT_I4)
        return FALSE;
    if (child_variant.value.lVal == OLEACC_CHILDID_SELF) {
        target->window = object->window;
        target->object_id = object->object_id;
        return TRUE;
    }
    if (child_variant.value.lVal < 1) return FALSE;
    OA_TARGET self = { object->window, object->object_id };
    return oa_target_child_at(&self, child_variant.value.lVal - 1, target);
}

static HRESULT oa_create_target(const OA_TARGET *target, REFIID iid,
                                PVOID *result);

static ULONG oa_object_addref(OA_OBJECT *object)
{
    return __atomic_add_fetch(&object->refs, 1, __ATOMIC_RELAXED);
}

static ULONG oa_object_release(OA_OBJECT *object)
{
    ULONG refs = __atomic_sub_fetch(&object->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) crt_free(object);
    return refs;
}

static HRESULT oa_object_query(OA_OBJECT *object, REFIID iid, PVOID *result)
{
    if (!result) return OLEACC_E_POINTER;
    *result = NULL;
    if (!iid) return OLEACC_E_NOINTERFACE;

    if (oa_guid_equal(iid, &oa_iid_unknown) ||
        oa_guid_equal(iid, &oa_iid_dispatch) ||
        oa_guid_equal(iid, &oa_iid_accessible)) {
        *result = &object->accessible;
    } else if (oa_guid_equal(iid, &oa_iid_enumvariant)) {
        *result = &object->enumerator;
    } else {
        return OLEACC_E_NOINTERFACE;
    }
    oa_object_addref(object);
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_acc_query(OLEACC_ACCESSIBLE *self, REFIID iid,
                                   PVOID *result)
{
    return oa_object_query(oa_from_accessible(self), iid, result);
}

static ULONG WINAPI oa_acc_addref(OLEACC_ACCESSIBLE *self)
{
    return oa_object_addref(oa_from_accessible(self));
}

static ULONG WINAPI oa_acc_release(OLEACC_ACCESSIBLE *self)
{
    return oa_object_release(oa_from_accessible(self));
}

static HRESULT WINAPI oa_enum_query(OLEACC_ENUMVARIANT *self, REFIID iid,
                                    PVOID *result)
{
    return oa_object_query(oa_from_enumerator(self), iid, result);
}

static ULONG WINAPI oa_enum_addref(OLEACC_ENUMVARIANT *self)
{
    return oa_object_addref(oa_from_enumerator(self));
}

static ULONG WINAPI oa_enum_release(OLEACC_ENUMVARIANT *self)
{
    return oa_object_release(oa_from_enumerator(self));
}

static HRESULT oa_connected(OA_OBJECT *object)
{
    USER32_ACCESSIBLE_WINDOW_INFO info;
    return oa_object_snapshot(object, &info) ? OLEACC_S_OK
                                             : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_get_type_info_count(OLEACC_ACCESSIBLE *self,
                                              UINT *count)
{
    (void)self;
    if (!count) return OLEACC_E_POINTER;
    *count = 0;
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_get_type_info(OLEACC_ACCESSIBLE *self, UINT index,
                                       LCID locale, PVOID *info)
{
    (void)self; (void)index; (void)locale;
    if (info) *info = NULL;
    return OLEACC_E_NOTIMPL;
}

static int oa_wide_equal_ascii(PCWSTR wide, const char *ascii)
{
    if (!wide || !ascii) return 0;
    while (*wide && *ascii) {
        WCHAR wc = *wide++;
        char ac = *ascii++;
        if (wc >= 'A' && wc <= 'Z') wc = (WCHAR)(wc + ('a' - 'A'));
        if (ac >= 'A' && ac <= 'Z') ac = (char)(ac + ('a' - 'A'));
        if (wc != (WCHAR)(BYTE)ac) return 0;
    }
    return *wide == 0 && *ascii == 0;
}

typedef struct { const char *name; DISPID id; } OA_DISPATCH_NAME;

static const OA_DISPATCH_NAME oa_dispatch_names[] = {
    { "accParent", OA_DISPID_PARENT },
    { "accChildCount", OA_DISPID_CHILDCOUNT },
    { "accChild", OA_DISPID_CHILD },
    { "accName", OA_DISPID_NAME },
    { "accValue", OA_DISPID_VALUE },
    { "accDescription", OA_DISPID_DESCRIPTION },
    { "accRole", OA_DISPID_ROLE },
    { "accState", OA_DISPID_STATE },
    { "accHelp", OA_DISPID_HELP },
    { "accHelpTopic", OA_DISPID_HELPTOPIC },
    { "accKeyboardShortcut", OA_DISPID_KEYBOARDSHORTCUT },
    { "accFocus", OA_DISPID_FOCUS },
    { "accSelection", OA_DISPID_SELECTION },
    { "accDefaultAction", OA_DISPID_DEFAULTACTION },
    { "accSelect", OA_DISPID_SELECT },
    { "accLocation", OA_DISPID_LOCATION },
    { "accNavigate", OA_DISPID_NAVIGATE },
    { "accHitTest", OA_DISPID_HITTEST },
    { "accDoDefaultAction", OA_DISPID_DODEFAULTACTION },
    { NULL, 0 }
};

static HRESULT WINAPI oa_get_ids_of_names(OLEACC_ACCESSIBLE *self, REFIID iid,
                                           PWSTR *names, UINT count,
                                           LCID locale, DISPID *ids)
{
    (void)self; (void)locale;
    if (!names || !ids || !count) return OLEACC_E_INVALIDARG;
    if (iid && !oa_guid_equal(iid, &oa_iid_unknown))
        return OLEACC_E_INVALIDARG;

    HRESULT status = OLEACC_S_OK;
    for (UINT i = 0; i < count; i++) {
        ids[i] = -1;
        for (int j = 0; oa_dispatch_names[j].name; j++)
            if (oa_wide_equal_ascii(names[i], oa_dispatch_names[j].name)) {
                ids[i] = oa_dispatch_names[j].id;
                break;
            }
        if (ids[i] == -1) status = OLEACC_DISP_E_UNKNOWNNAME;
    }
    return status;
}

static HRESULT WINAPI oa_get_parent(OLEACC_ACCESSIBLE *self, PVOID *parent)
{
    if (!parent) return OLEACC_E_POINTER;
    *parent = NULL;
    OA_OBJECT *object = oa_from_accessible(self);
    HRESULT connected = oa_connected(object);
    if (connected != OLEACC_S_OK) return connected;

    OA_TARGET target = { object->window, object->object_id };
    OA_TARGET parent_target;
    if (!oa_target_parent(&target, &parent_target)) return OLEACC_S_FALSE;
    return oa_create_target(&parent_target, &oa_iid_dispatch, parent);
}

static HRESULT WINAPI oa_get_child_count(OLEACC_ACCESSIBLE *self, LONG *count)
{
    if (!count) return OLEACC_E_POINTER;
    *count = 0;
    OA_OBJECT *object = oa_from_accessible(self);
    HRESULT connected = oa_connected(object);
    if (connected != OLEACC_S_OK) return connected;
    OA_TARGET target = { object->window, object->object_id };
    LONG found = oa_target_child_count(&target);
    if (found < 0) return OLEACC_CO_E_OBJNOTCONNECTED;
    *count = found;
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_get_child(OLEACC_ACCESSIBLE *self,
                                   OLEACC_VARIANT child_variant,
                                   PVOID *child)
{
    if (!child) return OLEACC_E_POINTER;
    *child = NULL;
    OA_OBJECT *object = oa_from_accessible(self);
    if (oa_connected(object) != OLEACC_S_OK)
        return OLEACC_CO_E_OBJNOTCONNECTED;
    if (child_variant.vt != OLEACC_VT_I4 ||
        child_variant.value.lVal <= OLEACC_CHILDID_SELF)
        return OLEACC_E_INVALIDARG;

    OA_TARGET target = { object->window, object->object_id };
    OA_TARGET child_target;
    if (!oa_target_child_at(&target, child_variant.value.lVal - 1,
                            &child_target))
        return OLEACC_E_INVALIDARG;
    return oa_create_target(&child_target, &oa_iid_dispatch, child);
}

static HRESULT oa_target_name(const OA_TARGET *target, BSTR *name)
{
    if (!name) return OLEACC_E_POINTER;
    *name = NULL;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(target, &info)) return OLEACC_CO_E_OBJNOTCONNECTED;

    UINT length = 0;
    while (length < USER32_ACCESSIBILITY_TITLE_CAP && info.title[length])
        length++;
    if (!length) return OLEACC_S_FALSE;
    *name = shim_SysAllocStringLen(info.title, length);
    return *name ? OLEACC_S_OK : OLEACC_E_OUTOFMEMORY;
}

static HRESULT WINAPI oa_get_name(OLEACC_ACCESSIBLE *self,
                                  OLEACC_VARIANT child_variant, BSTR *name)
{
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET target;
    if (!oa_resolve_variant_target(object, child_variant, &target)) {
        if (name) *name = NULL;
        return OLEACC_E_INVALIDARG;
    }
    return oa_target_name(&target, name);
}

static HRESULT oa_empty_bstr_property(OA_OBJECT *object,
                                      OLEACC_VARIANT child_variant,
                                      BSTR *value)
{
    if (!value) return OLEACC_E_POINTER;
    *value = NULL;
    OA_TARGET target;
    if (!oa_resolve_variant_target(object, child_variant, &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    return oa_snapshot(&target, &info) ? OLEACC_S_FALSE
                                       : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_get_value(OLEACC_ACCESSIBLE *self,
                                   OLEACC_VARIANT child_variant, BSTR *value)
{
    return oa_empty_bstr_property(oa_from_accessible(self), child_variant,
                                  value);
}

static HRESULT WINAPI oa_get_description(OLEACC_ACCESSIBLE *self,
                                         OLEACC_VARIANT child_variant,
                                         BSTR *description)
{
    return oa_empty_bstr_property(oa_from_accessible(self), child_variant,
                                  description);
}

static HRESULT WINAPI oa_get_role(OLEACC_ACCESSIBLE *self,
                                  OLEACC_VARIANT child_variant,
                                  OLEACC_VARIANT *role)
{
    if (!role) return OLEACC_E_POINTER;
    oa_variant_empty(role);
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET target;
    if (!oa_resolve_variant_target(object, child_variant, &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(&target, &info)) return OLEACC_CO_E_OBJNOTCONNECTED;
    oa_variant_i4(role, target.object_id == OLEACC_OBJID_WINDOW
                            ? OA_ROLE_SYSTEM_WINDOW : OA_ROLE_SYSTEM_CLIENT);
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_get_state(OLEACC_ACCESSIBLE *self,
                                   OLEACC_VARIANT child_variant,
                                   OLEACC_VARIANT *state_variant)
{
    if (!state_variant) return OLEACC_E_POINTER;
    oa_variant_empty(state_variant);
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET target;
    if (!oa_resolve_variant_target(object, child_variant, &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(&target, &info)) return OLEACC_CO_E_OBJNOTCONNECTED;

    LONG state = OA_STATE_FOCUSABLE;
    if (!info.enabled) state |= OA_STATE_UNAVAILABLE;
    if (!info.visible) state |= OA_STATE_INVISIBLE | OA_STATE_OFFSCREEN;
    if (GetFocus() == target.window) state |= OA_STATE_FOCUSED;
    if (target.object_id == OLEACC_OBJID_WINDOW && !(info.style & WS_CHILD))
        state |= OA_STATE_MOVEABLE;
    if (target.object_id == OLEACC_OBJID_WINDOW &&
        (info.style & 0x00040000U))
        state |= OA_STATE_SIZEABLE;
    oa_variant_i4(state_variant, state);
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_get_help(OLEACC_ACCESSIBLE *self,
                                  OLEACC_VARIANT child_variant, BSTR *help)
{
    return oa_empty_bstr_property(oa_from_accessible(self), child_variant,
                                  help);
}

static HRESULT WINAPI oa_get_help_topic(OLEACC_ACCESSIBLE *self,
                                        BSTR *help_file,
                                        OLEACC_VARIANT child_variant,
                                        LONG *topic)
{
    if (!help_file || !topic) return OLEACC_E_POINTER;
    *help_file = NULL;
    *topic = -1;
    OA_TARGET target;
    if (!oa_resolve_variant_target(oa_from_accessible(self), child_variant,
                                   &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    return oa_snapshot(&target, &info) ? OLEACC_S_FALSE
                                       : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_get_keyboard_shortcut(OLEACC_ACCESSIBLE *self,
                                                OLEACC_VARIANT child_variant,
                                                BSTR *shortcut)
{
    return oa_empty_bstr_property(oa_from_accessible(self), child_variant,
                                  shortcut);
}

static HRESULT oa_variant_dispatch_target(const OA_TARGET *target,
                                          OLEACC_VARIANT *variant)
{
    if (!variant) return OLEACC_E_POINTER;
    oa_variant_empty(variant);
    PVOID dispatch = NULL;
    HRESULT status = oa_create_target(target, &oa_iid_dispatch, &dispatch);
    if (status != OLEACC_S_OK) return status;
    variant->vt = OLEACC_VT_DISPATCH;
    variant->value.pdispVal = dispatch;
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_get_focus(OLEACC_ACCESSIBLE *self,
                                   OLEACC_VARIANT *focused)
{
    if (!focused) return OLEACC_E_POINTER;
    oa_variant_empty(focused);
    OA_OBJECT *object = oa_from_accessible(self);
    if (oa_connected(object) != OLEACC_S_OK)
        return OLEACC_CO_E_OBJNOTCONNECTED;
    HWND focus = GetFocus();
    if (!focus) return OLEACC_S_FALSE;
    if (focus == object->window) {
        oa_variant_i4(focused, OLEACC_CHILDID_SELF);
        return OLEACC_S_OK;
    }

    OA_TARGET self_target = { object->window, object->object_id };
    LONG count = oa_target_child_count(&self_target);
    for (LONG i = 0; i < count; i++) {
        OA_TARGET child;
        if (oa_target_child_at(&self_target, i, &child) &&
            (child.window == focus || IsChild(child.window, focus)))
            return oa_variant_dispatch_target(&child, focused);
    }
    return OLEACC_S_FALSE;
}

static HRESULT WINAPI oa_get_selection(OLEACC_ACCESSIBLE *self,
                                       OLEACC_VARIANT *selection)
{
    if (!selection) return OLEACC_E_POINTER;
    oa_variant_empty(selection);
    return oa_connected(oa_from_accessible(self)) == OLEACC_S_OK
        ? OLEACC_S_FALSE : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_get_default_action(OLEACC_ACCESSIBLE *self,
                                             OLEACC_VARIANT child_variant,
                                             BSTR *action)
{
    return oa_empty_bstr_property(oa_from_accessible(self), child_variant,
                                  action);
}

static HRESULT WINAPI oa_select(OLEACC_ACCESSIBLE *self, LONG flags,
                                OLEACC_VARIANT child_variant)
{
    if (!flags || (flags & ~OA_SELFLAG_VALID)) return OLEACC_E_INVALIDARG;
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET target;
    if (!oa_resolve_variant_target(object, child_variant, &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(&target, &info)) return OLEACC_CO_E_OBJNOTCONNECTED;
    if (flags & OA_SELFLAG_SELECTION_MASK) return OLEACC_E_NOTIMPL;
    if ((flags & OA_SELFLAG_TAKEFOCUS) && SetFocus(target.window) != target.window &&
        GetFocus() != target.window)
        return OLEACC_E_INVALIDARG;
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_location(OLEACC_ACCESSIBLE *self, LONG *left,
                                  LONG *top, LONG *width, LONG *height,
                                  OLEACC_VARIANT child_variant)
{
    if (!left || !top || !width || !height) return OLEACC_E_POINTER;
    *left = *top = *width = *height = 0;
    OA_TARGET target;
    if (!oa_resolve_variant_target(oa_from_accessible(self), child_variant,
                                   &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(&target, &info)) return OLEACC_CO_E_OBJNOTCONNECTED;
    RECT rect = target.object_id == OLEACC_OBJID_CLIENT
        ? info.client_rect : info.window_rect;
    *left = rect.left;
    *top = rect.top;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return OLEACC_S_OK;
}

static HRESULT oa_find_sibling(const OA_TARGET *target, int delta,
                               OA_TARGET *sibling)
{
    OA_TARGET parent;
    if (!oa_target_parent(target, &parent)) return OLEACC_S_FALSE;
    LONG count = oa_target_child_count(&parent);
    for (LONG i = 0; i < count; i++) {
        OA_TARGET candidate;
        if (!oa_target_child_at(&parent, i, &candidate)) continue;
        if (oa_target_equal(&candidate, target)) {
            LONG next = i + delta;
            return next >= 0 && next < count &&
                   oa_target_child_at(&parent, next, sibling)
                ? OLEACC_S_OK : OLEACC_S_FALSE;
        }
    }
    return OLEACC_S_FALSE;
}

static HRESULT WINAPI oa_navigate(OLEACC_ACCESSIBLE *self, LONG direction,
                                  OLEACC_VARIANT start,
                                  OLEACC_VARIANT *destination)
{
    if (!destination) return OLEACC_E_POINTER;
    oa_variant_empty(destination);
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET start_target;
    if (!oa_resolve_variant_target(object, start, &start_target))
        return OLEACC_E_INVALIDARG;

    OA_TARGET destination_target;
    HRESULT status = OLEACC_S_FALSE;
    if (direction == OA_NAVDIR_FIRSTCHILD || direction == OA_NAVDIR_LASTCHILD) {
        LONG count = oa_target_child_count(&start_target);
        LONG index = direction == OA_NAVDIR_FIRSTCHILD ? 0 : count - 1;
        if (count > 0 && oa_target_child_at(&start_target, index,
                                            &destination_target))
            status = OLEACC_S_OK;
    } else if (direction == OA_NAVDIR_NEXT ||
               direction == OA_NAVDIR_PREVIOUS) {
        status = oa_find_sibling(&start_target,
                                 direction == OA_NAVDIR_NEXT ? 1 : -1,
                                 &destination_target);
    } else {
        return OLEACC_E_NOTIMPL;
    }

    return status == OLEACC_S_OK
        ? oa_variant_dispatch_target(&destination_target, destination)
        : status;
}

static int oa_point_in_rect(LONG x, LONG y, const RECT *rect)
{
    return rect && x >= rect->left && y >= rect->top &&
           x < rect->right && y < rect->bottom;
}

static HRESULT WINAPI oa_hit_test(OLEACC_ACCESSIBLE *self, LONG x, LONG y,
                                  OLEACC_VARIANT *hit)
{
    if (!hit) return OLEACC_E_POINTER;
    oa_variant_empty(hit);
    OA_OBJECT *object = oa_from_accessible(self);
    OA_TARGET self_target = { object->window, object->object_id };
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(&self_target, &info))
        return OLEACC_CO_E_OBJNOTCONNECTED;
    const RECT *self_rect = object->object_id == OLEACC_OBJID_CLIENT
        ? &info.client_rect : &info.window_rect;
    if (!oa_point_in_rect(x, y, self_rect)) return OLEACC_S_FALSE;

    LONG count = oa_target_child_count(&self_target);
    for (LONG i = 0; i < count; i++) {
        OA_TARGET child;
        USER32_ACCESSIBLE_WINDOW_INFO child_info;
        if (!oa_target_child_at(&self_target, i, &child) ||
            !oa_snapshot(&child, &child_info))
            continue;
        const RECT *rect = child.object_id == OLEACC_OBJID_CLIENT
            ? &child_info.client_rect : &child_info.window_rect;
        if (oa_point_in_rect(x, y, rect))
            return oa_variant_dispatch_target(&child, hit);
    }
    oa_variant_i4(hit, OLEACC_CHILDID_SELF);
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_do_default_action(OLEACC_ACCESSIBLE *self,
                                            OLEACC_VARIANT child_variant)
{
    OA_TARGET target;
    if (!oa_resolve_variant_target(oa_from_accessible(self), child_variant,
                                   &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    return oa_snapshot(&target, &info) ? OLEACC_DISP_E_MEMBERNOTFOUND
                                       : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_put_name(OLEACC_ACCESSIBLE *self,
                                  OLEACC_VARIANT child_variant, BSTR name)
{
    (void)name;
    OA_TARGET target;
    if (!oa_resolve_variant_target(oa_from_accessible(self), child_variant,
                                   &target))
        return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    return oa_snapshot(&target, &info) ? OLEACC_E_NOTIMPL
                                       : OLEACC_CO_E_OBJNOTCONNECTED;
}

static HRESULT WINAPI oa_put_value(OLEACC_ACCESSIBLE *self,
                                   OLEACC_VARIANT child_variant, BSTR value)
{
    return oa_put_name(self, child_variant, value);
}

static HRESULT oa_dispatch_optional_child(OLEACC_DISPPARAMS *parameters,
                                          OLEACC_VARIANT *child)
{
    if (!child) return OLEACC_E_POINTER;
    *child = oa_self_variant();
    if (!parameters || !parameters->arg_count) return OLEACC_S_OK;
    if (parameters->arg_count != 1 || !parameters->args)
        return OLEACC_DISP_E_BADPARAMCOUNT;
    *child = parameters->args[0];
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_invoke(OLEACC_ACCESSIBLE *self, DISPID member,
                                REFIID iid, LCID locale, WORD flags,
                                OLEACC_DISPPARAMS *parameters,
                                OLEACC_VARIANT *result, PVOID exception,
                                UINT *argument_error)
{
    (void)locale; (void)exception;
    if (argument_error) *argument_error = 0;
    if (iid && !oa_guid_equal(iid, &oa_iid_unknown))
        return OLEACC_E_INVALIDARG;
    if (result) oa_variant_empty(result);

    if (member == OA_DISPID_PARENT) {
        if (!result || (parameters && parameters->arg_count))
            return result ? OLEACC_DISP_E_BADPARAMCOUNT : OLEACC_E_POINTER;
        PVOID parent = NULL;
        HRESULT status = oa_get_parent(self, &parent);
        if (status == OLEACC_S_OK) {
            result->vt = OLEACC_VT_DISPATCH;
            result->value.pdispVal = parent;
        }
        return status;
    }
    if (member == OA_DISPID_CHILDCOUNT) {
        if (!result || (parameters && parameters->arg_count))
            return result ? OLEACC_DISP_E_BADPARAMCOUNT : OLEACC_E_POINTER;
        LONG count;
        HRESULT status = oa_get_child_count(self, &count);
        if (status == OLEACC_S_OK) oa_variant_i4(result, count);
        return status;
    }
    if (member == OA_DISPID_FOCUS || member == OA_DISPID_SELECTION) {
        if (!result || (parameters && parameters->arg_count))
            return result ? OLEACC_DISP_E_BADPARAMCOUNT : OLEACC_E_POINTER;
        return member == OA_DISPID_FOCUS ? oa_get_focus(self, result)
                                         : oa_get_selection(self, result);
    }

    OLEACC_VARIANT child;
    HRESULT status = oa_dispatch_optional_child(parameters, &child);
    if (status != OLEACC_S_OK) return status;

    if (!(flags & OA_DISPATCH_PROPERTYGET) &&
        member <= OA_DISPID_PARENT && member >= OA_DISPID_DEFAULTACTION)
        return OLEACC_DISP_E_MEMBERNOTFOUND;

    BSTR text = NULL;
    switch (member) {
    case OA_DISPID_CHILD: {
        if (!result) return OLEACC_E_POINTER;
        PVOID dispatch = NULL;
        status = oa_get_child(self, child, &dispatch);
        if (status == OLEACC_S_OK) {
            result->vt = OLEACC_VT_DISPATCH;
            result->value.pdispVal = dispatch;
        }
        return status;
    }
    case OA_DISPID_NAME:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_name(self, child, &text);
        break;
    case OA_DISPID_VALUE:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_value(self, child, &text);
        break;
    case OA_DISPID_DESCRIPTION:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_description(self, child, &text);
        break;
    case OA_DISPID_HELP:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_help(self, child, &text);
        break;
    case OA_DISPID_KEYBOARDSHORTCUT:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_keyboard_shortcut(self, child, &text);
        break;
    case OA_DISPID_DEFAULTACTION:
        if (!result) return OLEACC_E_POINTER;
        status = oa_get_default_action(self, child, &text);
        break;
    case OA_DISPID_ROLE:
        return result ? oa_get_role(self, child, result) : OLEACC_E_POINTER;
    case OA_DISPID_STATE:
        return result ? oa_get_state(self, child, result) : OLEACC_E_POINTER;
    case OA_DISPID_DODEFAULTACTION:
        return oa_do_default_action(self, child);
    default:
        return OLEACC_DISP_E_MEMBERNOTFOUND;
    }
    if (status == OLEACC_S_OK) {
        result->vt = OLEACC_VT_BSTR;
        result->value.bstrVal = text;
    }
    return status;
}

static const OLEACC_ACCESSIBLE_VTBL oa_accessible_vtbl = {
    oa_acc_query,
    oa_acc_addref,
    oa_acc_release,
    oa_get_type_info_count,
    oa_get_type_info,
    oa_get_ids_of_names,
    oa_invoke,
    oa_get_parent,
    oa_get_child_count,
    oa_get_child,
    oa_get_name,
    oa_get_value,
    oa_get_description,
    oa_get_role,
    oa_get_state,
    oa_get_help,
    oa_get_help_topic,
    oa_get_keyboard_shortcut,
    oa_get_focus,
    oa_get_selection,
    oa_get_default_action,
    oa_select,
    oa_location,
    oa_navigate,
    oa_hit_test,
    oa_do_default_action,
    oa_put_name,
    oa_put_value
};

static HRESULT WINAPI oa_enum_next(OLEACC_ENUMVARIANT *self, ULONG requested,
                                   OLEACC_VARIANT *items, ULONG *fetched)
{
    OA_OBJECT *object = oa_from_enumerator(self);
    ULONG local_fetched = 0;
    if (!fetched && requested != 1) return OLEACC_E_POINTER;
    if (fetched) *fetched = 0;
    if (requested && !items) return OLEACC_E_POINTER;
    if (oa_connected(object) != OLEACC_S_OK)
        return OLEACC_CO_E_OBJNOTCONNECTED;
    if (!requested) return OLEACC_S_OK;

    OA_TARGET target = { object->window, object->object_id };
    LONG count = oa_target_child_count(&target);
    while (local_fetched < requested && object->enum_index < (ULONG)count) {
        OA_TARGET child;
        OLEACC_VARIANT *item = &items[local_fetched];
        oa_variant_empty(item);
        if (!oa_target_child_at(&target, (LONG)object->enum_index, &child))
            break;
        HRESULT status = oa_variant_dispatch_target(&child, item);
        if (status != OLEACC_S_OK) {
            if (!local_fetched) return status;
            break;
        }
        object->enum_index++;
        local_fetched++;
    }
    if (fetched) *fetched = local_fetched;
    return local_fetched == requested ? OLEACC_S_OK : OLEACC_S_FALSE;
}

static HRESULT WINAPI oa_enum_skip(OLEACC_ENUMVARIANT *self, ULONG requested)
{
    OA_OBJECT *object = oa_from_enumerator(self);
    if (oa_connected(object) != OLEACC_S_OK)
        return OLEACC_CO_E_OBJNOTCONNECTED;
    OA_TARGET target = { object->window, object->object_id };
    ULONG count = (ULONG)oa_target_child_count(&target);
    ULONG available = object->enum_index < count ? count - object->enum_index : 0;
    ULONG skipped = requested < available ? requested : available;
    object->enum_index += skipped;
    return skipped == requested ? OLEACC_S_OK : OLEACC_S_FALSE;
}

static HRESULT WINAPI oa_enum_reset(OLEACC_ENUMVARIANT *self)
{
    OA_OBJECT *object = oa_from_enumerator(self);
    if (oa_connected(object) != OLEACC_S_OK)
        return OLEACC_CO_E_OBJNOTCONNECTED;
    object->enum_index = 0;
    return OLEACC_S_OK;
}

static HRESULT WINAPI oa_enum_clone(OLEACC_ENUMVARIANT *self,
                                    OLEACC_ENUMVARIANT **clone)
{
    if (!clone) return OLEACC_E_POINTER;
    *clone = NULL;
    OA_OBJECT *object = oa_from_enumerator(self);
    OA_TARGET target = { object->window, object->object_id };
    PVOID result = NULL;
    HRESULT status = oa_create_target(&target, &oa_iid_enumvariant, &result);
    if (status != OLEACC_S_OK) return status;
    OA_OBJECT *copy = oa_from_enumerator((OLEACC_ENUMVARIANT *)result);
    copy->enum_index = object->enum_index;
    *clone = (OLEACC_ENUMVARIANT *)result;
    return OLEACC_S_OK;
}

static const OLEACC_ENUMVARIANT_VTBL oa_enumvariant_vtbl = {
    oa_enum_query,
    oa_enum_addref,
    oa_enum_release,
    oa_enum_next,
    oa_enum_skip,
    oa_enum_reset,
    oa_enum_clone
};

static HRESULT oa_create_target(const OA_TARGET *target, REFIID iid,
                                PVOID *result)
{
    if (!result) return OLEACC_E_POINTER;
    *result = NULL;
    if (!target || !iid) return OLEACC_E_INVALIDARG;
    USER32_ACCESSIBLE_WINDOW_INFO info;
    if (!oa_snapshot(target, &info) || info.message_only)
        return OLEACC_E_INVALIDARG;

    OA_OBJECT *object = (OA_OBJECT *)crt_malloc(sizeof(*object));
    if (!object) return OLEACC_E_OUTOFMEMORY;
    *object = (OA_OBJECT){0};
    object->accessible.lpVtbl = &oa_accessible_vtbl;
    object->enumerator.lpVtbl = &oa_enumvariant_vtbl;
    object->refs = 1;
    object->window = target->window;
    object->object_id = target->object_id;
    object->owner_pid = info.owner_pid;

    HRESULT status = oa_object_query(object, iid, result);
    oa_object_release(object);
    return status;
}

HRESULT WINAPI oleacc_CreateStdAccessibleObject(HWND window, LONG object_id,
                                                REFIID iid, PVOID *object)
{
    OA_TARGET target = { window, object_id };
    HRESULT status = oa_create_target(&target, iid, object);
    static ULONG trace_count;
    if (__atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED) < 64) {
        serial_puts("[OLEACC] CreateStdAccessibleObject object=");
        serial_putdec((uint64_t)(uint32_t)object_id);
        serial_puts(status == OLEACC_S_OK ? " ok\n" : " failed\n");
    }
    return status;
}

LRESULT WINAPI oleacc_LresultFromObject(REFIID iid, WPARAM message_param,
                                        PVOID unknown)
{
    if (!iid || !unknown) return (LRESULT)OLEACC_E_INVALIDARG;
    PVOID retained = NULL;
    HRESULT status = oa_unknown_query(unknown, iid, &retained);
    if (status != OLEACC_S_OK || !retained)
        return (LRESULT)(status == OLEACC_S_OK ? OLEACC_E_NOINTERFACE : status);

    oa_token_acquire();
    int slot = -1;
    for (int i = 0; i < OA_TOKEN_SLOTS; i++)
        if (!oa_tokens[i].used) { slot = i; break; }
    if (slot < 0) {
        oa_token_release();
        oa_unknown_release(retained);
        return (LRESULT)OLEACC_E_OUTOFMEMORY;
    }

    ULONG generation = oa_next_token_generation++ & 0xFFFFU;
    if (!generation) generation = oa_next_token_generation++ & 0xFFFFU;
    ULONG token = OA_TOKEN_PREFIX | (generation << 8) | (ULONG)(slot + 1);
    oa_tokens[slot].used = TRUE;
    oa_tokens[slot].generation = generation;
    oa_tokens[slot].token = token;
    oa_tokens[slot].owner_pid = GetCurrentProcessId();
    oa_tokens[slot].message_param = message_param;
    oa_tokens[slot].iid = *iid;
    oa_tokens[slot].object = retained;
    oa_token_release();
    return (LRESULT)(LONG_PTR)token;
}

HRESULT WINAPI oleacc_ObjectFromLresult(LRESULT result, REFIID iid,
                                        WPARAM message_param, PVOID *object)
{
    if (!object) return OLEACC_E_POINTER;
    *object = NULL;
    if (!iid || result <= 0) return OLEACC_E_INVALIDARG;
    ULONG token = (ULONG)(ULONG_PTR)result;
    int slot = (int)(token & 0xFFU) - 1;
    if ((token & 0xFF000000U) != OA_TOKEN_PREFIX ||
        slot < 0 || slot >= OA_TOKEN_SLOTS)
        return OLEACC_E_INVALIDARG;

    OA_TOKEN entry = {0};
    oa_token_acquire();
    if (oa_tokens[slot].used && oa_tokens[slot].token == token) {
        entry = oa_tokens[slot];
        oa_tokens[slot] = (OA_TOKEN){0};
    }
    oa_token_release();
    if (!entry.used) return OLEACC_E_INVALIDARG;
    if (entry.message_param != message_param) {
        oa_unknown_release(entry.object);
        return OLEACC_E_INVALIDARG;
    }

    if (oa_guid_equal(iid, &entry.iid)) {
        *object = entry.object;
        return OLEACC_S_OK;
    }
    HRESULT status = oa_unknown_query(entry.object, iid, object);
    oa_unknown_release(entry.object);
    return status;
}

HRESULT WINAPI oleacc_AccessibleObjectFromWindow(HWND window, DWORD object_id,
                                                 REFIID iid, PVOID *object)
{
    if (!object) return OLEACC_E_POINTER;
    *object = NULL;
    if (!iid || !IsWindow(window)) return OLEACC_E_INVALIDARG;

    LRESULT result = SendMessageW(window, WM_GETOBJECT, 0,
                                 (LPARAM)(LONG)object_id);
    if (result)
        return oleacc_ObjectFromLresult(result, iid, 0, object);
    return oleacc_CreateStdAccessibleObject(window, (LONG)object_id, iid,
                                           object);
}

HRESULT WINAPI oleacc_AccessibleChildren(OLEACC_ACCESSIBLE *container,
                                         LONG child_start, LONG child_count,
                                         OLEACC_VARIANT *children,
                                         LONG *obtained)
{
    if (!container || !obtained || child_start < 0 || child_count < 0 ||
        (child_count && !children))
        return OLEACC_E_INVALIDARG;
    *obtained = 0;
    if (!child_count) return OLEACC_S_OK;
    for (LONG i = 0; i < child_count; i++) oa_variant_empty(&children[i]);

    PVOID enum_pointer = NULL;
    HRESULT status = container->lpVtbl->QueryInterface(
        container, &oa_iid_enumvariant, &enum_pointer);
    if (status == OLEACC_S_OK && enum_pointer) {
        OLEACC_ENUMVARIANT *enumerator = (OLEACC_ENUMVARIANT *)enum_pointer;
        status = enumerator->lpVtbl->Reset(enumerator);
        if (status == OLEACC_S_OK && child_start)
            status = enumerator->lpVtbl->Skip(enumerator, (ULONG)child_start);
        ULONG fetched = 0;
        if (status == OLEACC_S_OK)
            status = enumerator->lpVtbl->Next(enumerator, (ULONG)child_count,
                                              children, &fetched);
        *obtained = (LONG)fetched;
        enumerator->lpVtbl->Release(enumerator);
        return status;
    }

    LONG count = 0;
    status = container->lpVtbl->get_accChildCount(container, &count);
    if (status != OLEACC_S_OK) return status;
    for (LONG i = child_start; i < count && *obtained < child_count; i++) {
        OLEACC_VARIANT child_id = {0};
        oa_variant_i4(&child_id, i + 1);
        PVOID dispatch = NULL;
        HRESULT child_status = container->lpVtbl->get_accChild(
            container, child_id, &dispatch);
        OLEACC_VARIANT *output = &children[*obtained];
        if (child_status == OLEACC_S_OK && dispatch) {
            output->vt = OLEACC_VT_DISPATCH;
            output->value.pdispVal = dispatch;
        } else {
            oa_variant_i4(output, i + 1);
        }
        (*obtained)++;
    }
    return *obtained == child_count ? OLEACC_S_OK : OLEACC_S_FALSE;
}

HRESULT WINAPI oleacc_WindowFromAccessibleObject(OLEACC_ACCESSIBLE *accessible,
                                                 HWND *window)
{
    if (!accessible || !window) return OLEACC_E_INVALIDARG;
    *window = NULL;
    OLEACC_ACCESSIBLE *current = accessible;
    BOOL current_owned = FALSE;

    for (int depth = 0; current && depth < OA_MAX_WINDOWS; depth++) {
        if (current->lpVtbl == &oa_accessible_vtbl) {
            OA_OBJECT *object = oa_from_accessible(current);
            *window = object->window;
            if (current_owned) current->lpVtbl->Release(current);
            return OLEACC_S_OK;
        }

        PVOID parent = NULL;
        HRESULT status = current->lpVtbl->get_accParent(current, &parent);
        if (current_owned) current->lpVtbl->Release(current);
        current_owned = FALSE;
        if (status != OLEACC_S_OK || !parent) break;

        PVOID next = NULL;
        status = oa_unknown_query(parent, &oa_iid_accessible, &next);
        oa_unknown_release(parent);
        if (status != OLEACC_S_OK || !next) break;
        current = (OLEACC_ACCESSIBLE *)next;
        current_owned = TRUE;
    }
    if (current_owned && current) current->lpVtbl->Release(current);
    return OLEACC_S_OK;
}

void oleacc_release_process(DWORD pid)
{
    PVOID release_list[OA_TOKEN_SLOTS];
    int release_count = 0;
    oa_token_acquire();
    for (int i = 0; i < OA_TOKEN_SLOTS; i++) {
        if (oa_tokens[i].used && oa_tokens[i].owner_pid == pid) {
            release_list[release_count++] = oa_tokens[i].object;
            oa_tokens[i] = (OA_TOKEN){0};
        }
    }
    oa_token_release();
    for (int i = 0; i < release_count; i++)
        oa_unknown_release(release_list[i]);
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; }
    OA_SHIM_EXPORT;

static const OA_SHIM_EXPORT oa_exports[] = {
    { "AccessibleChildren", (PVOID)oleacc_AccessibleChildren, 5, CC_STDCALL },
    { "AccessibleObjectFromWindow", (PVOID)oleacc_AccessibleObjectFromWindow,
      4, CC_STDCALL },
    { "CreateStdAccessibleObject", (PVOID)oleacc_CreateStdAccessibleObject,
      4, CC_STDCALL },
    { "LresultFromObject", (PVOID)oleacc_LresultFromObject, 3, CC_STDCALL },
    { "ObjectFromLresult", (PVOID)oleacc_ObjectFromLresult, 4, CC_STDCALL },
    { "WindowFromAccessibleObject",
      (PVOID)oleacc_WindowFromAccessibleObject, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *oleacc_abi_table(int *count)
{
    *count = (int)(sizeof(oa_exports) / sizeof(oa_exports[0]));
    return (const WIN32_EXPORT *)oa_exports;
}

static int oa_string_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right) { left++; right++; }
    return (BYTE)*left - (BYTE)*right;
}

PVOID oleacc_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        switch (ordinal) {
        case 6: return (PVOID)oleacc_AccessibleChildren;
        case 9: return (PVOID)oleacc_AccessibleObjectFromWindow;
        case 11: return (PVOID)oleacc_CreateStdAccessibleObject;
        case 25: return (PVOID)oleacc_LresultFromObject;
        case 26: return (PVOID)oleacc_ObjectFromLresult;
        case 28: return (PVOID)oleacc_WindowFromAccessibleObject;
        default: return NULL;
        }
    }
    if (!func_name) return NULL;
    for (int i = 0; oa_exports[i].name; i++)
        if (oa_string_equal(func_name, oa_exports[i].name) == 0)
            return oa_exports[i].func;
    return NULL;
}

PVOID oleacc_shim_init(void)
{
    serial_puts("[OLEACC] oleacc.dll MSAA shim initialized\n");
    return (PVOID)oa_exports;
}

static int oa_test_checks;
static int oa_test_failures;

static void oa_test_check(BOOL condition, const char *name)
{
    oa_test_checks++;
    if (condition) return;
    oa_test_failures++;
    serial_puts("[OLEACC-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int oleacc_selftest(void)
{
    oa_test_checks = 0;
    oa_test_failures = 0;
    user32_shim_init();

    WNDCLASSA window_class = {0};
    window_class.lpfnWndProc = DefWindowProcA;
    window_class.lpszClassName = "OsitoOleaccTest";
    oa_test_check(RegisterClassA(&window_class) != 0, "register class");

    HWND parent = CreateWindowExA(0, "OsitoOleaccTest", "Accessible Parent",
                                  WS_VISIBLE, 10, 20, 320, 200, NULL, NULL,
                                  NULL, NULL);
    HWND child = CreateWindowExA(0, "OsitoOleaccTest", "Accessible Child",
                                 WS_CHILD | WS_VISIBLE, 30, 40, 100, 60,
                                 parent, NULL, NULL, NULL);
    oa_test_check(parent != NULL && child != NULL, "create window hierarchy");
    SetFocus(child);

    OLEACC_ACCESSIBLE *accessible = NULL;
    HRESULT status = oleacc_CreateStdAccessibleObject(
        parent, OLEACC_OBJID_CLIENT, &oa_iid_accessible, (PVOID *)&accessible);
    oa_test_check(status == OLEACC_S_OK && accessible != NULL,
                  "create client accessible");

    if (accessible) {
        OLEACC_VARIANT self = oa_self_variant();
        BSTR name = NULL;
        status = accessible->lpVtbl->get_accName(accessible, self, &name);
        oa_test_check(status == OLEACC_S_OK && name && name[0] == 'A',
                      "window title BSTR");
        shim_SysFreeString(name);

        OLEACC_DISPPARAMS no_args = {0};
        OLEACC_VARIANT dispatch_result;
        status = accessible->lpVtbl->Invoke(
            accessible, OA_DISPID_NAME, &oa_iid_unknown, 0,
            OA_DISPATCH_PROPERTYGET, &no_args, &dispatch_result, NULL, NULL);
        oa_test_check(status == OLEACC_S_OK &&
                      dispatch_result.vt == OLEACC_VT_BSTR &&
                      dispatch_result.value.bstrVal &&
                      dispatch_result.value.bstrVal[0] == 'A',
                      "IDispatch property getter");
        shim_VariantClear(&dispatch_result);

        status = accessible->lpVtbl->Invoke(
            accessible, OA_DISPID_NAME, &oa_iid_unknown, 0, 0,
            &no_args, &dispatch_result, NULL, NULL);
        oa_test_check(status == OLEACC_DISP_E_MEMBERNOTFOUND,
                      "IDispatch rejects property without getter flag");

        OLEACC_VARIANT role;
        status = accessible->lpVtbl->get_accRole(accessible, self, &role);
        oa_test_check(status == OLEACC_S_OK && role.vt == OLEACC_VT_I4 &&
                      role.value.lVal == OA_ROLE_SYSTEM_CLIENT, "client role");

        OLEACC_VARIANT state;
        status = accessible->lpVtbl->get_accState(accessible, self, &state);
        oa_test_check(status == OLEACC_S_OK && state.vt == OLEACC_VT_I4 &&
                      !(state.value.lVal & OA_STATE_INVISIBLE), "visible state");

        LONG count = -1;
        status = accessible->lpVtbl->get_accChildCount(accessible, &count);
        oa_test_check(status == OLEACC_S_OK && count == 1, "direct child count");

        OLEACC_VARIANT children[2];
        LONG obtained = -1;
        status = oleacc_AccessibleChildren(accessible, 0, 2, children,
                                            &obtained);
        oa_test_check(status == OLEACC_S_FALSE && obtained == 1 &&
                      children[0].vt == OLEACC_VT_DISPATCH,
                      "enumerate direct children");
        if (obtained == 1 && children[0].vt == OLEACC_VT_DISPATCH) {
            HWND child_window = NULL;
            status = oleacc_WindowFromAccessibleObject(
                (OLEACC_ACCESSIBLE *)children[0].value.pdispVal,
                &child_window);
            oa_test_check(status == OLEACC_S_OK && child_window == child,
                          "child maps to HWND");
            oa_unknown_release(children[0].value.pdispVal);
        }

        LONG left, top, width, height;
        status = accessible->lpVtbl->accLocation(accessible, &left, &top,
                                                  &width, &height, self);
        oa_test_check(status == OLEACC_S_OK && left == 10 && top == 20 &&
                      width == 320 && height == 200, "client location");

        PVOID unknown = NULL;
        status = accessible->lpVtbl->QueryInterface(
            accessible, &oa_iid_unknown, &unknown);
        oa_test_check(status == OLEACC_S_OK && unknown == accessible,
                      "COM identity");
        if (unknown) oa_unknown_release(unknown);

        LRESULT token = oleacc_LresultFromObject(&oa_iid_accessible, 0x1234,
                                                 accessible);
        oa_test_check(token > 0, "LresultFromObject token");
        PVOID round_trip = NULL;
        status = oleacc_ObjectFromLresult(token, &oa_iid_accessible, 0x1234,
                                          &round_trip);
        oa_test_check(status == OLEACC_S_OK && round_trip != NULL,
                      "ObjectFromLresult round trip");
        if (round_trip) oa_unknown_release(round_trip);
        round_trip = NULL;
        status = oleacc_ObjectFromLresult(token, &oa_iid_accessible, 0x1234,
                                          &round_trip);
        oa_test_check(status == OLEACC_E_INVALIDARG && !round_trip,
                      "LRESULT token is one-shot");

        oa_test_check(DestroyWindow(parent), "destroy window hierarchy");
        name = NULL;
        status = accessible->lpVtbl->get_accName(accessible, self, &name);
        oa_test_check(status == OLEACC_CO_E_OBJNOTCONNECTED && !name,
                      "destroyed HWND disconnects proxy");
        accessible->lpVtbl->Release(accessible);
    }

    PVOID invalid = NULL;
    status = oleacc_CreateStdAccessibleObject((HWND)(ULONG_PTR)0xDEADBEEF,
                                              OLEACC_OBJID_CLIENT,
                                              &oa_iid_accessible, &invalid);
    oa_test_check(status == OLEACC_E_INVALIDARG && !invalid,
                  "invalid HWND rejected");

    serial_puts("[OLEACC-TEST] checks=");
    serial_putdec((uint64_t)oa_test_checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)oa_test_failures);
    serial_puts("\n");
    return oa_test_failures;
}
