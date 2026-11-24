/*
 * OsitoK Windows Compatibility Layer - oleacc.dll / MSAA shim.
 */

#ifndef OLEACC_SHIM_H
#define OLEACC_SHIM_H

#include "nttypes.h"
#include "user32_shim.h"

typedef LONG HRESULT;
typedef WCHAR *BSTR;
typedef DWORD LCID;
typedef LONG DISPID;

#define OLEACC_S_OK                  ((HRESULT)0x00000000)
#define OLEACC_S_FALSE               ((HRESULT)0x00000001)
#define OLEACC_E_NOTIMPL             ((HRESULT)0x80004001)
#define OLEACC_E_NOINTERFACE         ((HRESULT)0x80004002)
#define OLEACC_E_POINTER             ((HRESULT)0x80004003)
#define OLEACC_E_OUTOFMEMORY         ((HRESULT)0x8007000E)
#define OLEACC_E_INVALIDARG          ((HRESULT)0x80070057)
#define OLEACC_CO_E_OBJNOTCONNECTED  ((HRESULT)0x800401FD)
#define OLEACC_DISP_E_MEMBERNOTFOUND ((HRESULT)0x80020003)
#define OLEACC_DISP_E_BADPARAMCOUNT  ((HRESULT)0x8002000E)
#define OLEACC_DISP_E_UNKNOWNNAME    ((HRESULT)0x80020006)

#define OLEACC_VT_EMPTY      0
#define OLEACC_VT_I4         3
#define OLEACC_VT_BSTR       8
#define OLEACC_VT_DISPATCH   9
#define OLEACC_VT_UNKNOWN    13

#define OLEACC_CHILDID_SELF  0

#define OLEACC_OBJID_WINDOW  ((LONG)0x00000000)
#define OLEACC_OBJID_CLIENT  ((LONG)0xFFFFFFFC)

typedef struct OLEACC_VARIANT {
    USHORT vt;
    USHORT reserved1;
    USHORT reserved2;
    USHORT reserved3;
    union {
        LONG lVal;
        BSTR bstrVal;
        PVOID pdispVal;
        PVOID punkVal;
        ULONG_PTR raw[2];
    } value;
} OLEACC_VARIANT;

typedef struct {
    OLEACC_VARIANT *args;
    DISPID *named_args;
    UINT arg_count;
    UINT named_arg_count;
} OLEACC_DISPPARAMS;

typedef struct OLEACC_ACCESSIBLE OLEACC_ACCESSIBLE;
typedef struct OLEACC_ENUMVARIANT OLEACC_ENUMVARIANT;

typedef struct OLEACC_ACCESSIBLE_VTBL {
    HRESULT (WINAPI *QueryInterface)(OLEACC_ACCESSIBLE *, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(OLEACC_ACCESSIBLE *);
    ULONG (WINAPI *Release)(OLEACC_ACCESSIBLE *);
    HRESULT (WINAPI *GetTypeInfoCount)(OLEACC_ACCESSIBLE *, UINT *);
    HRESULT (WINAPI *GetTypeInfo)(OLEACC_ACCESSIBLE *, UINT, LCID, PVOID *);
    HRESULT (WINAPI *GetIDsOfNames)(OLEACC_ACCESSIBLE *, REFIID, PWSTR *,
                                    UINT, LCID, DISPID *);
    HRESULT (WINAPI *Invoke)(OLEACC_ACCESSIBLE *, DISPID, REFIID, LCID, WORD,
                             OLEACC_DISPPARAMS *, OLEACC_VARIANT *, PVOID,
                             UINT *);
    HRESULT (WINAPI *get_accParent)(OLEACC_ACCESSIBLE *, PVOID *);
    HRESULT (WINAPI *get_accChildCount)(OLEACC_ACCESSIBLE *, LONG *);
    HRESULT (WINAPI *get_accChild)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                   PVOID *);
    HRESULT (WINAPI *get_accName)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                  BSTR *);
    HRESULT (WINAPI *get_accValue)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                   BSTR *);
    HRESULT (WINAPI *get_accDescription)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                         BSTR *);
    HRESULT (WINAPI *get_accRole)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                  OLEACC_VARIANT *);
    HRESULT (WINAPI *get_accState)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                   OLEACC_VARIANT *);
    HRESULT (WINAPI *get_accHelp)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT,
                                  BSTR *);
    HRESULT (WINAPI *get_accHelpTopic)(OLEACC_ACCESSIBLE *, BSTR *,
                                       OLEACC_VARIANT, LONG *);
    HRESULT (WINAPI *get_accKeyboardShortcut)(OLEACC_ACCESSIBLE *,
                                              OLEACC_VARIANT, BSTR *);
    HRESULT (WINAPI *get_accFocus)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT *);
    HRESULT (WINAPI *get_accSelection)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT *);
    HRESULT (WINAPI *get_accDefaultAction)(OLEACC_ACCESSIBLE *,
                                           OLEACC_VARIANT, BSTR *);
    HRESULT (WINAPI *accSelect)(OLEACC_ACCESSIBLE *, LONG, OLEACC_VARIANT);
    HRESULT (WINAPI *accLocation)(OLEACC_ACCESSIBLE *, LONG *, LONG *, LONG *,
                                  LONG *, OLEACC_VARIANT);
    HRESULT (WINAPI *accNavigate)(OLEACC_ACCESSIBLE *, LONG, OLEACC_VARIANT,
                                  OLEACC_VARIANT *);
    HRESULT (WINAPI *accHitTest)(OLEACC_ACCESSIBLE *, LONG, LONG,
                                 OLEACC_VARIANT *);
    HRESULT (WINAPI *accDoDefaultAction)(OLEACC_ACCESSIBLE *,
                                         OLEACC_VARIANT);
    HRESULT (WINAPI *put_accName)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT, BSTR);
    HRESULT (WINAPI *put_accValue)(OLEACC_ACCESSIBLE *, OLEACC_VARIANT, BSTR);
} OLEACC_ACCESSIBLE_VTBL;

struct OLEACC_ACCESSIBLE {
    const OLEACC_ACCESSIBLE_VTBL *lpVtbl;
};

typedef struct OLEACC_ENUMVARIANT_VTBL {
    HRESULT (WINAPI *QueryInterface)(OLEACC_ENUMVARIANT *, REFIID, PVOID *);
    ULONG (WINAPI *AddRef)(OLEACC_ENUMVARIANT *);
    ULONG (WINAPI *Release)(OLEACC_ENUMVARIANT *);
    HRESULT (WINAPI *Next)(OLEACC_ENUMVARIANT *, ULONG, OLEACC_VARIANT *,
                           ULONG *);
    HRESULT (WINAPI *Skip)(OLEACC_ENUMVARIANT *, ULONG);
    HRESULT (WINAPI *Reset)(OLEACC_ENUMVARIANT *);
    HRESULT (WINAPI *Clone)(OLEACC_ENUMVARIANT *, OLEACC_ENUMVARIANT **);
} OLEACC_ENUMVARIANT_VTBL;

struct OLEACC_ENUMVARIANT {
    const OLEACC_ENUMVARIANT_VTBL *lpVtbl;
};

HRESULT WINAPI oleacc_CreateStdAccessibleObject(HWND window, LONG object_id,
                                                REFIID iid, PVOID *object);
HRESULT WINAPI oleacc_AccessibleObjectFromWindow(HWND window, DWORD object_id,
                                                 REFIID iid, PVOID *object);
HRESULT WINAPI oleacc_AccessibleChildren(OLEACC_ACCESSIBLE *container,
                                         LONG child_start, LONG child_count,
                                         OLEACC_VARIANT *children,
                                         LONG *obtained);
LRESULT WINAPI oleacc_LresultFromObject(REFIID iid, WPARAM message_param,
                                        PVOID unknown);
HRESULT WINAPI oleacc_ObjectFromLresult(LRESULT result, REFIID iid,
                                        WPARAM message_param, PVOID *object);
HRESULT WINAPI oleacc_WindowFromAccessibleObject(OLEACC_ACCESSIBLE *accessible,
                                                 HWND *window);

void oleacc_release_process(DWORD pid);
int oleacc_selftest(void);
PVOID oleacc_shim_init(void);
PVOID oleacc_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* OLEACC_SHIM_H */
