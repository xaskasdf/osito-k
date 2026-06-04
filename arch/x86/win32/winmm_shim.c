/*
 * OsitoK Windows Compatibility Layer — winmm.dll Shim
 * Multimedia timer stubs, silent audio, no joystick.
 */

#include "winmm_shim.h"
#include "win32_abi.h"

#ifdef TEST_HARNESS
#include <time.h>
#endif

extern void serial_puts(const char *s);

/* Weak reference to kernel tick source (100 Hz timer) */
extern uint64_t idt_get_ticks(void) __attribute__((weak));

/* ── Multimedia timers ─────────────────────────────────────── */

DWORD WINAPI shim_timeGetTime(void)
{
#ifdef TEST_HARNESS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#else
    /* Use rdtsc for monotonic time — idt_get_ticks doesn't advance
     * during compat32 code because APIC timer interrupts are blocked.
     * rdtsc always increments regardless of interrupt state. */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;

    /* Assume ~3 GHz TSC → divide by 3M to get ms.
     * This is approximate but monotonic, which is what matters. */
    DWORD result = (DWORD)(tsc / 3000000ULL);

    static int tgt_log = 0;
    if (tgt_log < 3) {
        extern void serial_puts(const char *s);
        extern void serial_putdec(uint64_t val);
        serial_puts("[WINMM] timeGetTime = ");
        serial_putdec(result);
        serial_puts("ms (rdtsc)\n");
        tgt_log++;
    }
    return result;
#endif
}

UINT WINAPI shim_timeBeginPeriod(UINT period)
{
    (void)period;
    return TIMERR_NOERROR;
}

UINT WINAPI shim_timeEndPeriod(UINT period)
{
    (void)period;
    return TIMERR_NOERROR;
}

UINT WINAPI shim_timeSetEvent(UINT delay, UINT resolution, PVOID callback,
                              ULONG_PTR user, UINT flags)
{
    (void)delay; (void)resolution; (void)callback;
    (void)user; (void)flags;
    serial_puts("[WINMM] timeSetEvent (stub — fake timer ID 1)\n");
    return 1; /* fake timer ID */
}

UINT WINAPI shim_timeKillEvent(UINT timerID)
{
    (void)timerID;
    return TIMERR_NOERROR;
}

/* ── Joystick ──────────────────────────────────────────────── */

UINT WINAPI shim_joyGetNumDevs(void)
{
    return 0; /* no joysticks */
}

UINT WINAPI shim_joyGetDevCapsA(UINT id, PVOID caps, UINT size)
{
    (void)id; (void)caps; (void)size;
    return JOYERR_PARMS; /* invalid joystick ID */
}

/* ── Sound / PlaySound ─────────────────────────────────────── */

BOOL WINAPI shim_PlaySoundA(const char *sound, PVOID hmod, DWORD flags)
{
    (void)sound; (void)hmod; (void)flags;
    serial_puts("[WINMM] PlaySoundA (stub — silent)\n");
    return TRUE; /* pretend success */
}

UINT WINAPI shim_waveOutGetNumDevs(void)
{
    return 0; /* no audio devices */
}

/* ── MCI (Media Control Interface) ─────────────────────────── */

DWORD WINAPI shim_mciSendCommandA(UINT device, UINT msg, ULONG_PTR flags,
                                   ULONG_PTR param)
{
    (void)device; (void)msg; (void)flags; (void)param;
    return 0; /* success */
}

DWORD WINAPI shim_mciSendStringA(const char *cmd, char *ret, UINT retLen,
                                  PVOID hwnd)
{
    (void)cmd; (void)ret; (void)retLen; (void)hwnd;
    return 0; /* success */
}

/* ── waveOut stubs (Galaxy.dll audio) ──────────────────────── */

static UINT WINAPI shim_waveOutReset(PVOID hwo)       { (void)hwo; return 5; /* MMSYSERR_ERROR */ }
static UINT WINAPI shim_waveOutUnprepareHeader(PVOID hwo, PVOID hdr, UINT sz)
    { (void)hwo; (void)hdr; (void)sz; return 0; }
static UINT WINAPI shim_waveOutGetPosition(PVOID hwo, PVOID mmt, UINT sz)
    { (void)hwo; if (mmt) memset(mmt, 0, sz); return 0; }
static UINT WINAPI shim_waveOutGetDevCapsA(UINT dev, PVOID caps, UINT sz)
    { (void)dev; if (caps) memset(caps, 0, sz); return 5; }
static UINT WINAPI shim_waveOutOpen(PVOID *phwo, UINT dev, PVOID fmt, ULONG_PTR cb,
                                     ULONG_PTR inst, DWORD flags)
    { (void)dev; (void)fmt; (void)cb; (void)inst; (void)flags;
      if (phwo) *phwo = NULL; return 5; /* MMSYSERR_ERROR — no audio */ }
static UINT WINAPI shim_waveOutClose(PVOID hwo)       { (void)hwo; return 0; }
static UINT WINAPI shim_waveOutWrite(PVOID hwo, PVOID hdr, UINT sz)
    { (void)hwo; (void)hdr; (void)sz; return 5; }
static UINT WINAPI shim_waveOutPrepareHeader(PVOID hwo, PVOID hdr, UINT sz)
    { (void)hwo; (void)hdr; (void)sz; return 0; }

/* ── aux/mixer stubs ──────────────────────────────────────── */

static UINT WINAPI shim_auxGetNumDevs(void)    { return 0; }
static UINT WINAPI shim_auxGetDevCapsA(UINT dev, PVOID caps, UINT sz)
    { (void)dev; if (caps) memset(caps, 0, sz); return 5; }
static UINT WINAPI shim_auxSetVolume(UINT dev, DWORD vol)
    { (void)dev; (void)vol; return 0; }

static UINT WINAPI shim_mixerGetNumDevs(void)  { return 0; }
static UINT WINAPI shim_mixerGetControlDetailsA(PVOID hmx, PVOID det, DWORD flags)
    { (void)hmx; (void)det; (void)flags; return 5; }
static UINT WINAPI shim_mixerGetDevCapsA(UINT dev, PVOID caps, UINT sz)
    { (void)dev; if (caps) memset(caps, 0, sz); return 5; }
static UINT WINAPI shim_mixerGetLineInfoA(PVOID hmx, PVOID info, DWORD flags)
    { (void)hmx; (void)info; (void)flags; return 5; }
static UINT WINAPI shim_mixerSetControlDetails(PVOID hmx, PVOID det, DWORD flags)
    { (void)hmx; (void)det; (void)flags; return 5; }

/* ── joyGetPosEx ──────────────────────────────────────────── */

static UINT WINAPI shim_joyGetPosEx(UINT id, PVOID info)
    { (void)id; (void)info; return 167; /* JOYERR_UNPLUGGED */ }

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT winmm_exports[] = {
    { "timeGetTime",              (PVOID)shim_timeGetTime,             0, CC_STDCALL },
    { "timeBeginPeriod",          (PVOID)shim_timeBeginPeriod,         1, CC_STDCALL },
    { "timeEndPeriod",            (PVOID)shim_timeEndPeriod,           1, CC_STDCALL },
    { "timeSetEvent",             (PVOID)shim_timeSetEvent,            5, CC_STDCALL },
    { "timeKillEvent",            (PVOID)shim_timeKillEvent,           1, CC_STDCALL },
    { "joyGetNumDevs",            (PVOID)shim_joyGetNumDevs,           0, CC_STDCALL },
    { "joyGetDevCapsA",           (PVOID)shim_joyGetDevCapsA,          3, CC_STDCALL },
    { "joyGetPosEx",              (PVOID)shim_joyGetPosEx,             2, CC_STDCALL },
    { "PlaySoundA",               (PVOID)shim_PlaySoundA,              3, CC_STDCALL },
    { "waveOutGetNumDevs",        (PVOID)shim_waveOutGetNumDevs,       0, CC_STDCALL },
    { "waveOutReset",             (PVOID)shim_waveOutReset,            1, CC_STDCALL },
    { "waveOutUnprepareHeader",   (PVOID)shim_waveOutUnprepareHeader,  3, CC_STDCALL },
    { "waveOutGetPosition",       (PVOID)shim_waveOutGetPosition,      3, CC_STDCALL },
    { "waveOutGetDevCapsA",       (PVOID)shim_waveOutGetDevCapsA,      3, CC_STDCALL },
    { "waveOutOpen",              (PVOID)shim_waveOutOpen,             6, CC_STDCALL },
    { "waveOutClose",             (PVOID)shim_waveOutClose,            1, CC_STDCALL },
    { "waveOutWrite",             (PVOID)shim_waveOutWrite,            3, CC_STDCALL },
    { "waveOutPrepareHeader",     (PVOID)shim_waveOutPrepareHeader,    3, CC_STDCALL },
    { "auxGetNumDevs",            (PVOID)shim_auxGetNumDevs,           0, CC_STDCALL },
    { "auxGetDevCapsA",           (PVOID)shim_auxGetDevCapsA,          3, CC_STDCALL },
    { "auxSetVolume",             (PVOID)shim_auxSetVolume,            2, CC_STDCALL },
    { "mixerGetNumDevs",          (PVOID)shim_mixerGetNumDevs,         0, CC_STDCALL },
    { "mixerGetControlDetailsA",  (PVOID)shim_mixerGetControlDetailsA, 3, CC_STDCALL },
    { "mixerGetDevCapsA",         (PVOID)shim_mixerGetDevCapsA,        3, CC_STDCALL },
    { "mixerGetLineInfoA",        (PVOID)shim_mixerGetLineInfoA,       3, CC_STDCALL },
    { "mixerSetControlDetails",   (PVOID)shim_mixerSetControlDetails,  3, CC_STDCALL },
    { "mciSendCommandA",          (PVOID)shim_mciSendCommandA,         4, CC_STDCALL },
    { "mciSendStringA",           (PVOID)shim_mciSendStringA,          4, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *winmm_abi_table(int *count) {
    *count = (int)(sizeof(winmm_exports)/sizeof(winmm_exports[0]));
    return (const WIN32_EXPORT *)winmm_exports;
}

static int wm_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID winmm_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; winmm_exports[i].name; i++) {
        if (wm_strcmp(func_name, winmm_exports[i].name) == 0)
            return winmm_exports[i].func;
    }
    return NULL;
}

void winmm_shim_init(void)
{
    serial_puts("[WINMM] winmm.dll shim initialized (silent mode)\n");
}
