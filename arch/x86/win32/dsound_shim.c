/*
 * OsitoK Windows Compatibility Layer — dsound.dll Shim
 * Silent audio stubs. UT99 can run without sound.
 */

#include "dsound_shim.h"

extern void serial_puts(const char *s);

/* ── IDirectSound COM stub ─────────────────────────────────── */

#define DS_OK       ((HRESULT)0)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define DSERR_GENERIC ((HRESULT)0x80004005)

/* Minimal IDirectSound vtable — just enough to not crash */
typedef struct IDirectSoundVtbl {
    HRESULT (WINAPI *QueryInterface)(PVOID self, LPCGUID iid, PVOID *ppv);
    ULONG   (WINAPI *AddRef)(PVOID self);
    ULONG   (WINAPI *Release)(PVOID self);
    HRESULT (WINAPI *CreateSoundBuffer)(PVOID self, PVOID desc, PVOID *ppDSB, PVOID pUnk);
    HRESULT (WINAPI *GetCaps)(PVOID self, PVOID pDSCaps);
    PVOID   _pad5; /* DuplicateSoundBuffer */
    HRESULT (WINAPI *SetCooperativeLevel)(PVOID self, PVOID hwnd, DWORD dwLevel);
    PVOID   _pad7; /* Compact */
    PVOID   _pad8; /* GetSpeakerConfig */
    PVOID   _pad9; /* SetSpeakerConfig */
    PVOID   _pad10; /* Initialize */
} IDirectSoundVtbl;

typedef struct { IDirectSoundVtbl *lpVtbl; } IDirectSound;

static HRESULT WINAPI ds_QueryInterface(PVOID self, LPCGUID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_NOINTERFACE;
    *ppv = self;
    return DS_OK;
}

static ULONG WINAPI ds_AddRef(PVOID self)  { (void)self; return 2; }
static ULONG WINAPI ds_Release(PVOID self) { (void)self; return 1; }

static HRESULT WINAPI ds_CreateSoundBuffer(PVOID self, PVOID desc, PVOID *ppDSB, PVOID pUnk)
{
    (void)self; (void)desc; (void)pUnk;
    serial_puts("[DSOUND] CreateSoundBuffer (stub — silent)\n");
    if (ppDSB) *ppDSB = NULL;
    return DSERR_GENERIC; /* fail gracefully — UT99 falls back to no sound */
}

static HRESULT WINAPI ds_GetCaps(PVOID self, PVOID pDSCaps)
{
    (void)self; (void)pDSCaps;
    return DSERR_GENERIC;
}

static HRESULT WINAPI ds_SetCooperativeLevel(PVOID self, PVOID hwnd, DWORD dwLevel)
{
    (void)self; (void)hwnd; (void)dwLevel;
    serial_puts("[DSOUND] SetCooperativeLevel (stub)\n");
    return DS_OK;
}

static IDirectSoundVtbl ds_vtbl = {
    ds_QueryInterface,
    ds_AddRef,
    ds_Release,
    ds_CreateSoundBuffer,
    ds_GetCaps,
    NULL, /* DuplicateSoundBuffer */
    ds_SetCooperativeLevel,
    NULL, NULL, NULL, NULL
};

static IDirectSound g_dsound = { &ds_vtbl };

/* ── Entry points ──────────────────────────────────────────── */

HRESULT WINAPI DirectSoundCreate(LPCGUID lpcGuidDevice, PVOID *ppDS, PVOID pUnkOuter)
{
    (void)lpcGuidDevice; (void)pUnkOuter;
    serial_puts("[DSOUND] DirectSoundCreate (stub)\n");
    if (!ppDS) return DSERR_GENERIC;
    *ppDS = &g_dsound;
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateA(PVOID lpDSEnumCallback, PVOID lpContext)
{
    (void)lpDSEnumCallback; (void)lpContext;
    /* Don't enumerate any devices — UT99 will try to use default */
    return DS_OK;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT dsound_exports[] = {
    { "DirectSoundCreate",     (PVOID)DirectSoundCreate },
    { "DirectSoundEnumerateA", (PVOID)DirectSoundEnumerateA },
    { NULL, NULL }
};

static int ds_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID dsound_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; dsound_exports[i].name; i++) {
        if (ds_strcmp(func_name, dsound_exports[i].name) == 0)
            return dsound_exports[i].func;
    }
    return NULL;
}

PVOID dsound_shim_init(void) { return (PVOID)dsound_exports; }
