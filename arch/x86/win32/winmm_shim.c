/*
 * OsitoK Windows Compatibility Layer — winmm.dll Shim
 * Multimedia timer stubs, silent audio, no joystick.
 */

#include "winmm_shim.h"

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
    if (idt_get_ticks)
        return (DWORD)(idt_get_ticks() * 10); /* 100 Hz ticks -> ms */
    return 0;
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

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT winmm_exports[] = {
    { "timeGetTime",        (PVOID)shim_timeGetTime },
    { "timeBeginPeriod",    (PVOID)shim_timeBeginPeriod },
    { "timeEndPeriod",      (PVOID)shim_timeEndPeriod },
    { "timeSetEvent",       (PVOID)shim_timeSetEvent },
    { "timeKillEvent",      (PVOID)shim_timeKillEvent },
    { "joyGetNumDevs",      (PVOID)shim_joyGetNumDevs },
    { "joyGetDevCapsA",     (PVOID)shim_joyGetDevCapsA },
    { "PlaySoundA",         (PVOID)shim_PlaySoundA },
    { "waveOutGetNumDevs",  (PVOID)shim_waveOutGetNumDevs },
    { "mciSendCommandA",    (PVOID)shim_mciSendCommandA },
    { "mciSendStringA",     (PVOID)shim_mciSendStringA },
    { NULL, NULL }
};

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
