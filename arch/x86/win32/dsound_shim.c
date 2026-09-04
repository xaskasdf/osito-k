/*
 * OsitoK Win32 DirectSound playback shim.
 *
 * DirectSound remains a Win32-facing policy layer. Legacy PCM conversion and
 * resampling live in kernel/pcm.c so DOS Sound Blaster and Unix OSS frontends
 * can use the same mixer primitives without depending on COM or HDA details.
 */

#include "dsound_shim.h"
#include "compat32.h"
#include "kernel32_shim.h"
#include "ole32_shim.h"
#include "win32_abi.h"
#include "../include/audio_sched.h"
#include "../include/paging.h"
#include "../include/pcm.h"

extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);
extern DWORD win32_current_process_id(void);
extern uint64_t proc_current_cr3(void);

#define DS_OK                       ((HRESULT)0x00000000)
#define DSERR_UNSUPPORTED           ((HRESULT)0x80004001)
#define DSERR_NOINTERFACE           ((HRESULT)0x80004002)
#define DSERR_OUTOFMEMORY           ((HRESULT)0x8007000E)
#define DSERR_INVALIDPARAM          ((HRESULT)0x80070057)
#define DSERR_NOAGGREGATION         ((HRESULT)0x80040110)
#define DSERR_GENERIC               ((HRESULT)0x80004005)
#define DSERR_ALLOCATED             ((HRESULT)0x8878000A)
#define DSERR_CONTROLUNAVAIL        ((HRESULT)0x8878001E)
#define DSERR_INVALIDCALL           ((HRESULT)0x88780032)
#define DSERR_PRIOLEVELNEEDED       ((HRESULT)0x88780046)
#define DSERR_BADFORMAT             ((HRESULT)0x88780064)
#define DSERR_NODRIVER              ((HRESULT)0x88780078)
#define DSERR_ALREADYINITIALIZED    ((HRESULT)0x88780082)
#define DSERR_UNINITIALIZED         ((HRESULT)0x887800AA)
#define DSERR_DS8_REQUIRED          ((HRESULT)0x887800BE)

#define DSBCAPS_PRIMARYBUFFER       0x00000001U
#define DSBCAPS_STATIC              0x00000002U
#define DSBCAPS_LOCHARDWARE         0x00000004U
#define DSBCAPS_LOCSOFTWARE         0x00000008U
#define DSBCAPS_CTRL3D              0x00000010U
#define DSBCAPS_CTRLFREQUENCY       0x00000020U
#define DSBCAPS_CTRLPAN             0x00000040U
#define DSBCAPS_CTRLVOLUME          0x00000080U
#define DSBCAPS_CTRLPOSITIONNOTIFY  0x00000100U
#define DSBCAPS_CTRLFX              0x00000200U
#define DSBCAPS_STICKYFOCUS         0x00004000U
#define DSBCAPS_GLOBALFOCUS         0x00008000U
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000U
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x00020000U
#define DSBCAPS_LOCDEFER            0x00040000U

#define DSBCAPS_KNOWN_MASK (DSBCAPS_PRIMARYBUFFER | DSBCAPS_STATIC | \
    DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRL3D | \
    DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | \
    DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLFX | \
    DSBCAPS_STICKYFOCUS | DSBCAPS_GLOBALFOCUS | \
    DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE | \
    DSBCAPS_LOCDEFER)
#define DSBCAPS_UNSUPPORTED_CONTROLS DSBCAPS_CTRLPOSITIONNOTIFY

#define DSCAPS_PRIMARYMONO          0x00000001U
#define DSCAPS_PRIMARYSTEREO        0x00000002U
#define DSCAPS_PRIMARY8BIT          0x00000004U
#define DSCAPS_PRIMARY16BIT         0x00000008U
#define DSCAPS_CONTINUOUSRATE       0x00000010U
#define DSCAPS_EMULDRIVER           0x00000020U
#define DSCAPS_SECONDARYMONO        0x00000100U
#define DSCAPS_SECONDARYSTEREO      0x00000200U
#define DSCAPS_SECONDARY8BIT        0x00000400U
#define DSCAPS_SECONDARY16BIT       0x00000800U
#define DSCAPS_HARDWARE_SECONDARY_MASK (DSCAPS_SECONDARYMONO | \
    DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | \
    DSCAPS_SECONDARY16BIT)

#define DSBPLAY_LOOPING             0x00000001U
#define DSBLOCK_FROMWRITECURSOR     0x00000001U
#define DSBLOCK_ENTIREBUFFER        0x00000002U
#define DSBSTATUS_PLAYING           0x00000001U
#define DSBSTATUS_LOOPING           0x00000004U
#define DSBSTATUS_LOCSOFTWARE       0x00000010U

#define DSSCL_NORMAL                1U
#define DSSCL_PRIORITY              2U
#define DSSCL_EXCLUSIVE             3U
#define DSSCL_WRITEPRIMARY          4U
#define DSSPEAKER_STEREO            4U
#define DSSPEAKER_LAST              9U
#define DSSPEAKER_CONFIG_MASK       0x000000FFU
#define DSSPEAKER_GEOMETRY_MASK     0x00FF0000U
#define DSSPEAKER_GEOMETRY_MIN      5U
#define DSSPEAKER_GEOMETRY_NARROW   10U
#define DSSPEAKER_GEOMETRY_WIDE     20U
#define DSSPEAKER_GEOMETRY_MAX      180U

#define WAVE_FORMAT_PCM             1U
#define DSBFREQUENCY_MIN            100U
#define DSBFREQUENCY_MAX            100000U
#define DSBFREQUENCY_ORIGINAL       0U
#define DSBSIZE_MIN                 4U
#define DSBSIZE_MAX                 0x0FFFFFFFU
#define DSBVOLUME_MIN               (-10000)
#define DSBVOLUME_MAX               0
#define DSBPAN_LEFT                 (-10000)
#define DSBPAN_RIGHT                10000

#define DS3DMODE_NORMAL             0U
#define DS3DMODE_HEADRELATIVE       1U
#define DS3DMODE_DISABLE            2U
#define DS3D_IMMEDIATE              0U
#define DS3D_DEFERRED               1U
#define DS3D_MINROLLOFFFACTOR       0.0f
#define DS3D_MAXROLLOFFFACTOR       10.0f
#define DS3D_MINDOPPLERFACTOR       0.0f
#define DS3D_MAXDOPPLERFACTOR       10.0f
#define DS3D_DEFAULTDISTANCEFACTOR  1.0f
#define DS3D_DEFAULTROLLOFFFACTOR   1.0f
#define DS3D_DEFAULTDOPPLERFACTOR   1.0f
#define DS3D_DEFAULTMINDISTANCE     1.0f
#define DS3D_DEFAULTMAXDISTANCE     1000000000.0f
#define DS3D_MINCONEANGLE           0U
#define DS3D_MAXCONEANGLE           360U
#define DS3D_DEFAULTCONEANGLE       360U
#define DS3D_DEFAULTCONEOUTSIDEVOLUME DSBVOLUME_MAX
#define DS3D_SPEED_OF_SOUND         343.3f
#define DS3D_PI                     3.14159265358979323846f

#define DSOUND_PROCESS_SLOTS        64U
#define DSOUND_DEVICES_PER_PROCESS  8U
#define DSOUND_BUFFERS_PER_PROCESS  32U
#define DSOUND_MIX_FRAMES           AUDIO_OUTPUT_BLOCK_FRAMES
#define DSOUND_DS_VTBL_SLOTS        11U
#define DSOUND_DSB_VTBL_SLOTS       21U
#define DSOUND_DSL_VTBL_SLOTS       18U
#define DSOUND_DS3DB_VTBL_SLOTS     21U

#define DSOUND_DS32_VTBL_OFFSET     0U
#define DSOUND_DSB32_VTBL_OFFSET    64U
#define DSOUND_DSL32_VTBL_OFFSET    160U
#define DSOUND_DS3DB32_VTBL_OFFSET  256U
#define DSOUND_DS32_OBJECT_OFFSET   384U
#define DSOUND_DSB32_OBJECT_OFFSET  448U
#define DSOUND_DSL32_OBJECT_OFFSET  576U
#define DSOUND_DS3DB32_OBJECT_OFFSET 704U

_Static_assert(DSOUND_DS32_VTBL_OFFSET + DSOUND_DS_VTBL_SLOTS * 4U <=
                   DSOUND_DSB32_VTBL_OFFSET,
               "DirectSound proxy vtable overlap");
_Static_assert(DSOUND_DSB32_VTBL_OFFSET + DSOUND_DSB_VTBL_SLOTS * 4U <=
                   DSOUND_DSL32_VTBL_OFFSET,
               "DirectSound buffer proxy vtable overlap");
_Static_assert(DSOUND_DSL32_VTBL_OFFSET + DSOUND_DSL_VTBL_SLOTS * 4U <=
                   DSOUND_DS3DB32_VTBL_OFFSET,
               "DirectSound listener proxy vtable overlap");
_Static_assert(DSOUND_DS3DB32_VTBL_OFFSET + DSOUND_DS3DB_VTBL_SLOTS * 4U <=
                   DSOUND_DS32_OBJECT_OFFSET,
               "DirectSound 3D buffer proxy vtable overlap");
_Static_assert(DSOUND_DS3DB32_OBJECT_OFFSET +
                   DSOUND_BUFFERS_PER_PROCESS * 4U <= 4096U,
               "DirectSound proxy objects exceed one page");

#ifndef DSOUND_DIAGNOSTICS
#define DSOUND_DIAGNOSTICS          0
#endif

typedef struct __attribute__((packed)) {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
} WAVEFORMATEX_DS;

typedef struct {
    float x;
    float y;
    float z;
} D3DVECTOR_DS;

typedef struct {
    DWORD size;
    D3DVECTOR_DS position;
    D3DVECTOR_DS velocity;
    DWORD inside_cone_angle;
    DWORD outside_cone_angle;
    D3DVECTOR_DS cone_orientation;
    LONG cone_outside_volume;
    float min_distance;
    float max_distance;
    DWORD mode;
} DS3DBUFFER_DS;

typedef struct {
    DWORD size;
    D3DVECTOR_DS position;
    D3DVECTOR_DS velocity;
    D3DVECTOR_DS orient_front;
    D3DVECTOR_DS orient_top;
    float distance_factor;
    float rolloff_factor;
    float doppler_factor;
} DS3DLISTENER_DS;

_Static_assert(sizeof(D3DVECTOR_DS) == 12, "DirectSound 3D vector ABI");
_Static_assert(sizeof(DS3DBUFFER_DS) == 64, "DirectSound 3D buffer ABI");
_Static_assert(sizeof(DS3DLISTENER_DS) == 64,
               "DirectSound 3D listener ABI");

typedef struct {
    DWORD size;
    DWORD flags;
    DWORD buffer_bytes;
    DWORD reserved;
    const WAVEFORMATEX_DS *format;
} DSBUFFERDESC_VIEW;

typedef struct {
    bool in_use;
    ULONG refs;
    PVOID user_data;
    uint8_t *kernel_data;
    uint32_t bytes;
} DSOUND_ALLOCATION;

typedef struct {
    bool in_use;
    bool initialized;
    ULONG refs;
    uint32_t live_buffers;
    DWORD cooperative_level;
    PVOID object;
    DS3DLISTENER_DS listener;
    DS3DLISTENER_DS deferred_listener;
    bool listener_deferred;
} DSOUND_DEVICE;

typedef struct {
    bool in_use;
    bool primary;
    bool playing;
    bool looping;
    bool locked;
    ULONG refs;
    DWORD flags;
    PVOID object;
    DSOUND_DEVICE *device;
    DSOUND_ALLOCATION *allocation;
    pcm_format_t format;
    uint32_t original_rate;
    uint32_t playback_rate;
    uint32_t mix_rate;
    uint16_t block_align;
    LONG volume;
    LONG pan;
    uint32_t left_gain_q16;
    uint32_t right_gain_q16;
    pcm_cursor_t cursor;
    uint32_t write_cursor;
    PVOID lock_pointer1;
    PVOID lock_pointer2;
    DWORD lock_bytes1;
    DWORD lock_bytes2;
    DS3DBUFFER_DS spatial;
    DS3DBUFFER_DS deferred_spatial;
    bool spatial_deferred;
} DSOUND_BUFFER;

typedef struct {
    PVOID lpVtbl;
} DSOUND_OBJECT64;

typedef struct {
    DWORD owner_pid;
    uint64_t owner_cr3;
    bool compat32;
    BYTE *proxy_page;
    PVOID device_objects[DSOUND_DEVICES_PER_PROCESS];
    PVOID buffer_objects[DSOUND_BUFFERS_PER_PROCESS];
    PVOID listener_objects[DSOUND_BUFFERS_PER_PROCESS];
    PVOID buffer3d_objects[DSOUND_BUFFERS_PER_PROCESS];
    DSOUND_OBJECT64 device_objects64[DSOUND_DEVICES_PER_PROCESS];
    DSOUND_OBJECT64 buffer_objects64[DSOUND_BUFFERS_PER_PROCESS];
    DSOUND_OBJECT64 listener_objects64[DSOUND_BUFFERS_PER_PROCESS];
    DSOUND_OBJECT64 buffer3d_objects64[DSOUND_BUFFERS_PER_PROCESS];
    DSOUND_DEVICE devices[DSOUND_DEVICES_PER_PROCESS];
    DSOUND_BUFFER buffers[DSOUND_BUFFERS_PER_PROCESS];
    DSOUND_ALLOCATION allocations[DSOUND_BUFFERS_PER_PROCESS];
} DSOUND_PROCESS;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, LPCGUID, PVOID);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
    HRESULT (WINAPI *CreateSoundBuffer)(PVOID, PVOID, PVOID, PVOID);
    HRESULT (WINAPI *GetCaps)(PVOID, PVOID);
    HRESULT (WINAPI *DuplicateSoundBuffer)(PVOID, PVOID, PVOID);
    HRESULT (WINAPI *SetCooperativeLevel)(PVOID, PVOID, DWORD);
    HRESULT (WINAPI *Compact)(PVOID);
    HRESULT (WINAPI *GetSpeakerConfig)(PVOID, DWORD *);
    HRESULT (WINAPI *SetSpeakerConfig)(PVOID, DWORD);
    HRESULT (WINAPI *Initialize)(PVOID, LPCGUID);
} IDIRECTSOUND_VTBL;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, LPCGUID, PVOID);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
    HRESULT (WINAPI *GetCaps)(PVOID, PVOID);
    HRESULT (WINAPI *GetCurrentPosition)(PVOID, DWORD *, DWORD *);
    HRESULT (WINAPI *GetFormat)(PVOID, PVOID, DWORD, DWORD *);
    HRESULT (WINAPI *GetVolume)(PVOID, LONG *);
    HRESULT (WINAPI *GetPan)(PVOID, LONG *);
    HRESULT (WINAPI *GetFrequency)(PVOID, DWORD *);
    HRESULT (WINAPI *GetStatus)(PVOID, DWORD *);
    HRESULT (WINAPI *Initialize)(PVOID, PVOID, PVOID);
    HRESULT (WINAPI *Lock)(PVOID, DWORD, DWORD, PVOID, DWORD *, PVOID,
                           DWORD *, DWORD);
    HRESULT (WINAPI *Play)(PVOID, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *SetCurrentPosition)(PVOID, DWORD);
    HRESULT (WINAPI *SetFormat)(PVOID, PVOID);
    HRESULT (WINAPI *SetVolume)(PVOID, LONG);
    HRESULT (WINAPI *SetPan)(PVOID, LONG);
    HRESULT (WINAPI *SetFrequency)(PVOID, DWORD);
    HRESULT (WINAPI *Stop)(PVOID);
    HRESULT (WINAPI *Unlock)(PVOID, PVOID, DWORD, PVOID, DWORD);
    HRESULT (WINAPI *Restore)(PVOID);
} IDIRECTSOUNDBUFFER_VTBL;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, LPCGUID, PVOID);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
    HRESULT (WINAPI *GetAllParameters)(PVOID, DS3DLISTENER_DS *);
    HRESULT (WINAPI *GetDistanceFactor)(PVOID, float *);
    HRESULT (WINAPI *GetDopplerFactor)(PVOID, float *);
    HRESULT (WINAPI *GetOrientation)(PVOID, D3DVECTOR_DS *, D3DVECTOR_DS *);
    HRESULT (WINAPI *GetPosition)(PVOID, D3DVECTOR_DS *);
    HRESULT (WINAPI *GetRolloffFactor)(PVOID, float *);
    HRESULT (WINAPI *GetVelocity)(PVOID, D3DVECTOR_DS *);
    HRESULT (WINAPI *SetAllParameters)(PVOID, const DS3DLISTENER_DS *, DWORD);
    HRESULT (WINAPI *SetDistanceFactor)(PVOID, float, DWORD);
    HRESULT (WINAPI *SetDopplerFactor)(PVOID, float, DWORD);
    HRESULT (WINAPI *SetOrientation)(PVOID, float, float, float,
                                     float, float, float, DWORD);
    HRESULT (WINAPI *SetPosition)(PVOID, float, float, float, DWORD);
    HRESULT (WINAPI *SetRolloffFactor)(PVOID, float, DWORD);
    HRESULT (WINAPI *SetVelocity)(PVOID, float, float, float, DWORD);
    HRESULT (WINAPI *CommitDeferredSettings)(PVOID);
} IDIRECTSOUND3DLISTENER_VTBL;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(PVOID, LPCGUID, PVOID);
    ULONG (WINAPI *AddRef)(PVOID);
    ULONG (WINAPI *Release)(PVOID);
    HRESULT (WINAPI *GetAllParameters)(PVOID, DS3DBUFFER_DS *);
    HRESULT (WINAPI *GetConeAngles)(PVOID, DWORD *, DWORD *);
    HRESULT (WINAPI *GetConeOrientation)(PVOID, D3DVECTOR_DS *);
    HRESULT (WINAPI *GetConeOutsideVolume)(PVOID, LONG *);
    HRESULT (WINAPI *GetMaxDistance)(PVOID, float *);
    HRESULT (WINAPI *GetMinDistance)(PVOID, float *);
    HRESULT (WINAPI *GetMode)(PVOID, DWORD *);
    HRESULT (WINAPI *GetPosition)(PVOID, D3DVECTOR_DS *);
    HRESULT (WINAPI *GetVelocity)(PVOID, D3DVECTOR_DS *);
    HRESULT (WINAPI *SetAllParameters)(PVOID, const DS3DBUFFER_DS *, DWORD);
    HRESULT (WINAPI *SetConeAngles)(PVOID, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *SetConeOrientation)(PVOID, float, float, float, DWORD);
    HRESULT (WINAPI *SetConeOutsideVolume)(PVOID, LONG, DWORD);
    HRESULT (WINAPI *SetMaxDistance)(PVOID, float, DWORD);
    HRESULT (WINAPI *SetMinDistance)(PVOID, float, DWORD);
    HRESULT (WINAPI *SetMode)(PVOID, DWORD, DWORD);
    HRESULT (WINAPI *SetPosition)(PVOID, float, float, float, DWORD);
    HRESULT (WINAPI *SetVelocity)(PVOID, float, float, float, DWORD);
} IDIRECTSOUND3DBUFFER_VTBL;

static HRESULT WINAPI ds_QueryInterface(PVOID self, LPCGUID iid, PVOID output);
static ULONG WINAPI ds_AddRef(PVOID self);
static ULONG WINAPI ds_Release(PVOID self);
static HRESULT WINAPI ds_CreateSoundBuffer(PVOID self, PVOID description,
                                           PVOID output, PVOID outer);
static HRESULT WINAPI ds_GetCaps(PVOID self, PVOID caps);
static HRESULT WINAPI ds_DuplicateSoundBuffer(PVOID self, PVOID original,
                                              PVOID output);
static HRESULT WINAPI ds_SetCooperativeLevel(PVOID self, PVOID window,
                                             DWORD level);
static HRESULT WINAPI ds_Compact(PVOID self);
static HRESULT WINAPI ds_GetSpeakerConfig(PVOID self, DWORD *configuration);
static HRESULT WINAPI ds_SetSpeakerConfig(PVOID self, DWORD configuration);
static HRESULT WINAPI ds_Initialize(PVOID self, LPCGUID device);

static HRESULT WINAPI dsb_QueryInterface(PVOID self, LPCGUID iid,
                                         PVOID output);
static ULONG WINAPI dsb_AddRef(PVOID self);
static ULONG WINAPI dsb_Release(PVOID self);
static HRESULT WINAPI dsb_GetCaps(PVOID self, PVOID caps);
static HRESULT WINAPI dsb_GetCurrentPosition(PVOID self, DWORD *play,
                                             DWORD *write);
static HRESULT WINAPI dsb_GetFormat(PVOID self, PVOID format, DWORD size,
                                    DWORD *written);
static HRESULT WINAPI dsb_GetVolume(PVOID self, LONG *volume);
static HRESULT WINAPI dsb_GetPan(PVOID self, LONG *pan);
static HRESULT WINAPI dsb_GetFrequency(PVOID self, DWORD *frequency);
static HRESULT WINAPI dsb_GetStatus(PVOID self, DWORD *status);
static HRESULT WINAPI dsb_Initialize(PVOID self, PVOID direct_sound,
                                     PVOID description);
static HRESULT WINAPI dsb_Lock(PVOID self, DWORD offset, DWORD bytes,
                               PVOID pointer1, DWORD *bytes1,
                               PVOID pointer2, DWORD *bytes2, DWORD flags);
static HRESULT WINAPI dsb_Play(PVOID self, DWORD reserved1, DWORD reserved2,
                               DWORD flags);
static HRESULT WINAPI dsb_SetCurrentPosition(PVOID self, DWORD position);
static HRESULT WINAPI dsb_SetFormat(PVOID self, PVOID format);
static HRESULT WINAPI dsb_SetVolume(PVOID self, LONG volume);
static HRESULT WINAPI dsb_SetPan(PVOID self, LONG pan);
static HRESULT WINAPI dsb_SetFrequency(PVOID self, DWORD frequency);
static HRESULT WINAPI dsb_Stop(PVOID self);
static HRESULT WINAPI dsb_Unlock(PVOID self, PVOID pointer1, DWORD bytes1,
                                 PVOID pointer2, DWORD bytes2);
static HRESULT WINAPI dsb_Restore(PVOID self);

static HRESULT WINAPI dsl_GetAllParameters(PVOID self,
                                            DS3DLISTENER_DS *listener);
static HRESULT WINAPI dsl_GetDistanceFactor(PVOID self, float *factor);
static HRESULT WINAPI dsl_GetDopplerFactor(PVOID self, float *factor);
static HRESULT WINAPI dsl_GetOrientation(PVOID self, D3DVECTOR_DS *front,
                                         D3DVECTOR_DS *top);
static HRESULT WINAPI dsl_GetPosition(PVOID self, D3DVECTOR_DS *position);
static HRESULT WINAPI dsl_GetRolloffFactor(PVOID self, float *factor);
static HRESULT WINAPI dsl_GetVelocity(PVOID self, D3DVECTOR_DS *velocity);
static HRESULT WINAPI dsl_SetAllParameters(
    PVOID self, const DS3DLISTENER_DS *listener, DWORD apply);
static HRESULT WINAPI dsl_SetDistanceFactor(PVOID self, float factor,
                                             DWORD apply);
static HRESULT WINAPI dsl_SetDopplerFactor(PVOID self, float factor,
                                            DWORD apply);
static HRESULT WINAPI dsl_SetOrientation(PVOID self, float front_x,
                                          float front_y, float front_z,
                                          float top_x, float top_y,
                                          float top_z, DWORD apply);
static HRESULT WINAPI dsl_SetPosition(PVOID self, float x, float y, float z,
                                      DWORD apply);
static HRESULT WINAPI dsl_SetRolloffFactor(PVOID self, float factor,
                                            DWORD apply);
static HRESULT WINAPI dsl_SetVelocity(PVOID self, float x, float y, float z,
                                      DWORD apply);
static HRESULT WINAPI dsl_CommitDeferredSettings(PVOID self);

static HRESULT WINAPI ds3db_GetAllParameters(PVOID self,
                                              DS3DBUFFER_DS *parameters);
static HRESULT WINAPI ds3db_GetConeAngles(PVOID self, DWORD *inside,
                                           DWORD *outside);
static HRESULT WINAPI ds3db_GetConeOrientation(PVOID self,
                                                D3DVECTOR_DS *orientation);
static HRESULT WINAPI ds3db_GetConeOutsideVolume(PVOID self, LONG *volume);
static HRESULT WINAPI ds3db_GetMaxDistance(PVOID self, float *distance);
static HRESULT WINAPI ds3db_GetMinDistance(PVOID self, float *distance);
static HRESULT WINAPI ds3db_GetMode(PVOID self, DWORD *mode);
static HRESULT WINAPI ds3db_GetPosition(PVOID self, D3DVECTOR_DS *position);
static HRESULT WINAPI ds3db_GetVelocity(PVOID self, D3DVECTOR_DS *velocity);
static HRESULT WINAPI ds3db_SetAllParameters(
    PVOID self, const DS3DBUFFER_DS *parameters, DWORD apply);
static HRESULT WINAPI ds3db_SetConeAngles(PVOID self, DWORD inside,
                                           DWORD outside, DWORD apply);
static HRESULT WINAPI ds3db_SetConeOrientation(PVOID self, float x, float y,
                                                float z, DWORD apply);
static HRESULT WINAPI ds3db_SetConeOutsideVolume(PVOID self, LONG volume,
                                                  DWORD apply);
static HRESULT WINAPI ds3db_SetMaxDistance(PVOID self, float distance,
                                            DWORD apply);
static HRESULT WINAPI ds3db_SetMinDistance(PVOID self, float distance,
                                            DWORD apply);
static HRESULT WINAPI ds3db_SetMode(PVOID self, DWORD mode, DWORD apply);
static HRESULT WINAPI ds3db_SetPosition(PVOID self, float x, float y, float z,
                                        DWORD apply);
static HRESULT WINAPI ds3db_SetVelocity(PVOID self, float x, float y, float z,
                                        DWORD apply);

static HRESULT WINAPI dsl32_SetDistanceFactor(PVOID self, DWORD factor,
                                               DWORD apply);
static HRESULT WINAPI dsl32_SetDopplerFactor(PVOID self, DWORD factor,
                                              DWORD apply);
static HRESULT WINAPI dsl32_SetOrientation(
    PVOID self, DWORD front_x, DWORD front_y, DWORD front_z,
    DWORD top_x, DWORD top_y, DWORD top_z, DWORD apply);
static HRESULT WINAPI dsl32_SetPosition(PVOID self, DWORD x, DWORD y,
                                        DWORD z, DWORD apply);
static HRESULT WINAPI dsl32_SetRolloffFactor(PVOID self, DWORD factor,
                                              DWORD apply);
static HRESULT WINAPI dsl32_SetVelocity(PVOID self, DWORD x, DWORD y,
                                        DWORD z, DWORD apply);
static HRESULT WINAPI ds3db32_SetConeOrientation(PVOID self, DWORD x,
                                                  DWORD y, DWORD z,
                                                  DWORD apply);
static HRESULT WINAPI ds3db32_SetMaxDistance(PVOID self, DWORD distance,
                                              DWORD apply);
static HRESULT WINAPI ds3db32_SetMinDistance(PVOID self, DWORD distance,
                                              DWORD apply);
static HRESULT WINAPI ds3db32_SetPosition(PVOID self, DWORD x, DWORD y,
                                           DWORD z, DWORD apply);
static HRESULT WINAPI ds3db32_SetVelocity(PVOID self, DWORD x, DWORD y,
                                           DWORD z, DWORD apply);

static IDIRECTSOUND_VTBL ds_vtbl64 = {
    ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer,
    ds_GetCaps, ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact,
    ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize,
};

static IDIRECTSOUNDBUFFER_VTBL dsb_vtbl64 = {
    dsb_QueryInterface, dsb_AddRef, dsb_Release, dsb_GetCaps,
    dsb_GetCurrentPosition, dsb_GetFormat, dsb_GetVolume, dsb_GetPan,
    dsb_GetFrequency, dsb_GetStatus, dsb_Initialize, dsb_Lock, dsb_Play,
    dsb_SetCurrentPosition, dsb_SetFormat, dsb_SetVolume, dsb_SetPan,
    dsb_SetFrequency, dsb_Stop, dsb_Unlock, dsb_Restore,
};

static IDIRECTSOUND3DLISTENER_VTBL dsl_vtbl64 = {
    dsb_QueryInterface, dsb_AddRef, dsb_Release,
    dsl_GetAllParameters, dsl_GetDistanceFactor, dsl_GetDopplerFactor,
    dsl_GetOrientation, dsl_GetPosition, dsl_GetRolloffFactor,
    dsl_GetVelocity, dsl_SetAllParameters, dsl_SetDistanceFactor,
    dsl_SetDopplerFactor, dsl_SetOrientation, dsl_SetPosition,
    dsl_SetRolloffFactor, dsl_SetVelocity, dsl_CommitDeferredSettings,
};

static IDIRECTSOUND3DBUFFER_VTBL ds3db_vtbl64 = {
    dsb_QueryInterface, dsb_AddRef, dsb_Release,
    ds3db_GetAllParameters, ds3db_GetConeAngles,
    ds3db_GetConeOrientation, ds3db_GetConeOutsideVolume,
    ds3db_GetMaxDistance, ds3db_GetMinDistance, ds3db_GetMode,
    ds3db_GetPosition, ds3db_GetVelocity, ds3db_SetAllParameters,
    ds3db_SetConeAngles, ds3db_SetConeOrientation,
    ds3db_SetConeOutsideVolume, ds3db_SetMaxDistance,
    ds3db_SetMinDistance, ds3db_SetMode, ds3db_SetPosition,
    ds3db_SetVelocity,
};

static const GUID iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID iid_idirectsound = {
    0x279AFA83, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID iid_idirectsoundbuffer = {
    0x279AFA85, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID iid_idirectsound3dlistener = {
    0x279AFA84, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID iid_idirectsound3dbuffer = {
    0x279AFA86, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID clsid_directsound = {
    0x47D4D946, 0x62E8, 0x11CF,
    { 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};
static const GUID dsdevid_default_playback = {
    0xDEF00000, 0x9C6D, 0x47ED,
    { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 }
};
static const GUID dsdevid_default_voice_playback = {
    0xDEF00002, 0x9C6D, 0x47ED,
    { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 }
};

static DSOUND_PROCESS dsound_processes[DSOUND_PROCESS_SLOTS];
static int16_t dsound_mix_output[DSOUND_MIX_FRAMES * 2U];
static int32_t dsound_mix_accumulator[DSOUND_MIX_FRAMES * 2U];
static volatile uint32_t dsound_state_lock;
static volatile uint32_t dsound_transition_lock;
static bool dsound_mixer_running;
static audio_source_id_t dsound_audio_source;
#if DSOUND_DIAGNOSTICS
static uint32_t dsound_trace_count;
static volatile uint32_t dsound_unlock_trace_count;
static volatile uint64_t dsound_mix_fill_count;
static volatile uint64_t dsound_mix_signal_count;
#endif

static void dsound_trace(const char *method)
{
#if DSOUND_DIAGNOSTICS
    uint32_t sequence = __atomic_fetch_add(&dsound_trace_count, 1,
                                           __ATOMIC_RELAXED);
    if (sequence < 192U) {
        serial_puts("[DSOUND-COM] ");
        serial_puts(method);
        serial_puts("\n");
    } else if (sequence == 192U) {
        serial_puts("[DSOUND-COM] repetitive tracing suppressed\n");
    }
#else
    (void)method;
#endif
}

static uint64_t dsound_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&dsound_state_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
    return flags;
}

static void dsound_unlock_irqrestore(uint64_t flags)
{
    __sync_lock_release(&dsound_state_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void dsound_transition_acquire(void)
{
    while (__sync_lock_test_and_set(&dsound_transition_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static void dsound_transition_release(void)
{
    __sync_lock_release(&dsound_transition_lock);
}

static bool dsound_guid_equal(LPCGUID left, LPCGUID right)
{
    if (!left || !right || left->Data1 != right->Data1 ||
        left->Data2 != right->Data2 || left->Data3 != right->Data3)
        return false;
    for (uint32_t i = 0; i < 8; i++) {
        if (left->Data4[i] != right->Data4[i])
            return false;
    }
    return true;
}

static bool dsound_playback_device_supported(LPCGUID device)
{
    return !device || dsound_guid_equal(device, &dsdevid_default_playback) ||
           dsound_guid_equal(device, &dsdevid_default_voice_playback);
}

static HRESULT dsound_validate_cooperative_level(DWORD level)
{
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY)
        return DSERR_INVALIDPARAM;
    return level == DSSCL_WRITEPRIMARY ? DSERR_UNSUPPORTED : DS_OK;
}

static bool dsound_valid_speaker_config(DWORD value)
{
    if (value & ~(DSSPEAKER_CONFIG_MASK | DSSPEAKER_GEOMETRY_MASK))
        return false;

    DWORD config = value & DSSPEAKER_CONFIG_MASK;
    DWORD geometry = (value & DSSPEAKER_GEOMETRY_MASK) >> 16;
    if (config > DSSPEAKER_LAST)
        return false;
    if (!geometry)
        return true;
    if (config != DSSPEAKER_STEREO)
        return false;
    return geometry == DSSPEAKER_GEOMETRY_MIN ||
           geometry == DSSPEAKER_GEOMETRY_NARROW ||
           geometry == DSSPEAKER_GEOMETRY_WIDE ||
           geometry == DSSPEAKER_GEOMETRY_MAX;
}

static float dsound_float_from_bits(DWORD bits)
{
    union {
        DWORD bits;
        float value;
    } conversion = { .bits = bits };
    return conversion.value;
}

static bool dsound_float_finite(float value)
{
    union {
        float value;
        DWORD bits;
    } conversion = { .value = value };
    return (conversion.bits & 0x7F800000U) != 0x7F800000U;
}

static bool dsound_vector_finite(const D3DVECTOR_DS *vector)
{
    return vector && dsound_float_finite(vector->x) &&
           dsound_float_finite(vector->y) &&
           dsound_float_finite(vector->z);
}

static float dsound_vector_length_squared(const D3DVECTOR_DS *vector)
{
    return vector->x * vector->x + vector->y * vector->y +
           vector->z * vector->z;
}

static D3DVECTOR_DS dsound_vector_cross(const D3DVECTOR_DS *left,
                                        const D3DVECTOR_DS *right)
{
    D3DVECTOR_DS result = {
        left->y * right->z - left->z * right->y,
        left->z * right->x - left->x * right->z,
        left->x * right->y - left->y * right->x,
    };
    return result;
}

static bool dsound_direction_valid(const D3DVECTOR_DS *vector)
{
    if (!dsound_vector_finite(vector))
        return false;
    float length_squared = dsound_vector_length_squared(vector);
    return dsound_float_finite(length_squared) &&
           length_squared > 0.00000001f;
}

static bool dsound_apply_valid(DWORD apply)
{
    return apply == DS3D_IMMEDIATE || apply == DS3D_DEFERRED;
}

static void dsound_listener_defaults(DS3DLISTENER_DS *listener)
{
    memset(listener, 0, sizeof(*listener));
    listener->size = sizeof(*listener);
    listener->orient_front.z = 1.0f;
    listener->orient_top.y = 1.0f;
    listener->distance_factor = DS3D_DEFAULTDISTANCEFACTOR;
    listener->rolloff_factor = DS3D_DEFAULTROLLOFFFACTOR;
    listener->doppler_factor = DS3D_DEFAULTDOPPLERFACTOR;
}

static void dsound_buffer3d_defaults(DS3DBUFFER_DS *parameters)
{
    memset(parameters, 0, sizeof(*parameters));
    parameters->size = sizeof(*parameters);
    parameters->inside_cone_angle = DS3D_DEFAULTCONEANGLE;
    parameters->outside_cone_angle = DS3D_DEFAULTCONEANGLE;
    parameters->cone_orientation.z = 1.0f;
    parameters->cone_outside_volume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
    parameters->min_distance = DS3D_DEFAULTMINDISTANCE;
    parameters->max_distance = DS3D_DEFAULTMAXDISTANCE;
    parameters->mode = DS3DMODE_NORMAL;
}

static bool dsound_listener_valid(const DS3DLISTENER_DS *listener)
{
    if (!listener || listener->size != sizeof(*listener) ||
        !dsound_vector_finite(&listener->position) ||
        !dsound_vector_finite(&listener->velocity) ||
        !dsound_direction_valid(&listener->orient_front) ||
        !dsound_direction_valid(&listener->orient_top) ||
        !dsound_float_finite(listener->distance_factor) ||
        listener->distance_factor <= 0.0f ||
        !dsound_float_finite(listener->rolloff_factor) ||
        listener->rolloff_factor < DS3D_MINROLLOFFFACTOR ||
        listener->rolloff_factor > DS3D_MAXROLLOFFFACTOR ||
        !dsound_float_finite(listener->doppler_factor) ||
        listener->doppler_factor < DS3D_MINDOPPLERFACTOR ||
        listener->doppler_factor > DS3D_MAXDOPPLERFACTOR)
        return false;

    D3DVECTOR_DS right = dsound_vector_cross(&listener->orient_top,
                                              &listener->orient_front);
    return dsound_direction_valid(&right);
}

static bool dsound_buffer3d_valid(const DS3DBUFFER_DS *parameters)
{
    return parameters && parameters->size == sizeof(*parameters) &&
           dsound_vector_finite(&parameters->position) &&
           dsound_vector_finite(&parameters->velocity) &&
           dsound_direction_valid(&parameters->cone_orientation) &&
           parameters->inside_cone_angle >= DS3D_MINCONEANGLE &&
           parameters->inside_cone_angle <= DS3D_MAXCONEANGLE &&
           parameters->outside_cone_angle >= parameters->inside_cone_angle &&
           parameters->outside_cone_angle <= DS3D_MAXCONEANGLE &&
           parameters->cone_outside_volume >= DSBVOLUME_MIN &&
           parameters->cone_outside_volume <= DSBVOLUME_MAX &&
           dsound_float_finite(parameters->min_distance) &&
           parameters->min_distance > 0.0f &&
           dsound_float_finite(parameters->max_distance) &&
           parameters->max_distance >= parameters->min_distance &&
           parameters->mode <= DS3DMODE_DISABLE;
}

static bool dsound_compat_call(PVOID self)
{
    return g_compat32_mode ||
           (self && (ULONG_PTR)self <= (ULONG_PTR)UINT32_MAX);
}

static bool dsound_guest_readable(const void *pointer, SIZE_T size,
                                  bool compat32, const char *operation)
{
    if (win32_user_range_readable(pointer, size,
                                  compat32 ? TRUE : FALSE))
        return true;
    serial_puts("[DSOUND] invalid readable range in ");
    serial_puts(operation);
    serial_puts("\n");
    return false;
}

static bool dsound_guest_writable(void *pointer, SIZE_T size,
                                  bool compat32, const char *operation)
{
    if (win32_user_range_writable(pointer, size,
                                  compat32 ? TRUE : FALSE))
        return true;
    serial_puts("[DSOUND] invalid writable range in ");
    serial_puts(operation);
    serial_puts("\n");
    return false;
}

static bool dsound_store_pointer(bool compat32, PVOID output, PVOID value)
{
    SIZE_T size = compat32 ? sizeof(uint32_t) : sizeof(PVOID);
    if (!win32_user_range_writable(output, size,
                                   compat32 ? TRUE : FALSE))
        return false;
    if (compat32)
        *(uint32_t *)output = (uint32_t)(ULONG_PTR)value;
    else
        *(PVOID *)output = value;
    return true;
}

static DSOUND_PROCESS *dsound_find_process_locked(DWORD owner_pid)
{
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        if (dsound_processes[i].owner_pid == owner_pid)
            return &dsound_processes[i];
    }
    return NULL;
}

static DSOUND_DEVICE *dsound_device_from_object_locked(
    PVOID object, DSOUND_PROCESS **owner)
{
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t d = 0; d < DSOUND_DEVICES_PER_PROCESS; d++) {
            DSOUND_DEVICE *device = &process->devices[d];
            if (device->in_use && device->object == object) {
                if (owner)
                    *owner = process;
                return device;
            }
        }
    }
    return NULL;
}

static void dsound_retire_device_locked(DSOUND_DEVICE *device)
{
    if (device && !device->refs && !device->live_buffers)
        memset(device, 0, sizeof(*device));
}

static DSOUND_DEVICE *dsound_allocate_device_locked(
    DSOUND_PROCESS *process, bool initialized)
{
    if (!process)
        return NULL;
    for (uint32_t i = 0; i < DSOUND_DEVICES_PER_PROCESS; i++) {
        DSOUND_DEVICE *device = &process->devices[i];
        if (device->in_use)
            continue;
        memset(device, 0, sizeof(*device));
        device->in_use = true;
        device->initialized = initialized;
        device->refs = 1;
        device->cooperative_level = DSSCL_NORMAL;
        device->object = process->device_objects[i];
        dsound_listener_defaults(&device->listener);
        device->deferred_listener = device->listener;
        return device;
    }
    return NULL;
}

static DSOUND_BUFFER *dsound_buffer_from_object_locked(
    PVOID object, DSOUND_PROCESS **owner)
{
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
            DSOUND_BUFFER *buffer = &process->buffers[b];
            if (buffer->in_use &&
                (buffer->object == object ||
                 process->listener_objects[b] == object ||
                 process->buffer3d_objects[b] == object)) {
                if (owner)
                    *owner = process;
                return buffer;
            }
        }
    }
    return NULL;
}

static uint32_t dsound_gain_q16(LONG hundredths_db)
{
    if (hundredths_db >= 0)
        return PCM_GAIN_UNITY;
    if (hundredths_db <= DSBVOLUME_MIN)
        return 0;

    uint32_t gain = PCM_GAIN_UNITY;
    LONG remaining = hundredths_db;
    while (remaining <= -600) {
        gain >>= 1;
        remaining += 600;
    }
    return (uint32_t)(((uint64_t)gain * (uint32_t)(1200 + remaining)) /
                      1200U);
}

static float dsound_sqrtf(float value)
{
    float result;
    __asm__ volatile ("sqrtss %1, %0" : "=x"(result) : "x"(value));
    return result;
}

static float dsound_cosf(float value)
{
    float result;
    __asm__ volatile (
        "flds %[value]\n\t"
        "fcos\n\t"
        "fstps %[result]\n\t"
        : [result] "=m"(result)
        : [value] "m"(value));
    return result;
}

static float dsound_vector_dot(const D3DVECTOR_DS *left,
                               const D3DVECTOR_DS *right)
{
    return left->x * right->x + left->y * right->y +
           left->z * right->z;
}

static bool dsound_vector_normalize(D3DVECTOR_DS *vector)
{
    float length_squared = dsound_vector_length_squared(vector);
    if (!dsound_float_finite(length_squared) ||
        length_squared <= 0.00000001f)
        return false;
    float inverse = 1.0f / dsound_sqrtf(length_squared);
    vector->x *= inverse;
    vector->y *= inverse;
    vector->z *= inverse;
    return true;
}

static float dsound_clampf(float value, float minimum, float maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static uint32_t dsound_float_gain_q16(float gain)
{
    if (!dsound_float_finite(gain) || gain <= 0.0f)
        return 0;
    if (gain >= 1.0f)
        return PCM_GAIN_UNITY;
    return (uint32_t)(gain * (float)PCM_GAIN_UNITY + 0.5f);
}

static uint32_t dsound_multiply_gain_q16(uint32_t left, uint32_t right)
{
    return (uint32_t)(((uint64_t)left * right) >> 16);
}

static void dsound_update_gains(DSOUND_BUFFER *buffer)
{
    if (!buffer)
        return;

    uint32_t base = dsound_gain_q16(buffer->volume);
    buffer->left_gain_q16 = base;
    buffer->right_gain_q16 = base;
    buffer->mix_rate = buffer->playback_rate;
    if (buffer->pan < 0) {
        buffer->right_gain_q16 = (uint32_t)(
            ((uint64_t)base * dsound_gain_q16(buffer->pan)) >> 16);
    } else if (buffer->pan > 0) {
        buffer->left_gain_q16 = (uint32_t)(
            ((uint64_t)base * dsound_gain_q16(-buffer->pan)) >> 16);
    }

    if (buffer->primary || !(buffer->flags & DSBCAPS_CTRL3D) ||
        !buffer->device || buffer->spatial.mode == DS3DMODE_DISABLE)
        return;

    const DS3DLISTENER_DS *listener = &buffer->device->listener;
    D3DVECTOR_DS relative = buffer->spatial.position;
    D3DVECTOR_DS listener_velocity = listener->velocity;
    if (buffer->spatial.mode == DS3DMODE_NORMAL) {
        relative.x -= listener->position.x;
        relative.y -= listener->position.y;
        relative.z -= listener->position.z;
    } else {
        listener_velocity.x = 0.0f;
        listener_velocity.y = 0.0f;
        listener_velocity.z = 0.0f;
    }

    float distance = dsound_sqrtf(dsound_vector_length_squared(&relative));
    D3DVECTOR_DS direction = relative;
    bool has_direction = dsound_vector_normalize(&direction);

    float attenuation = 1.0f;
    if ((buffer->flags & DSBCAPS_MUTE3DATMAXDISTANCE) &&
        distance >= buffer->spatial.max_distance) {
        attenuation = 0.0f;
    } else {
        float effective_distance = distance;
        if (effective_distance < buffer->spatial.min_distance)
            effective_distance = buffer->spatial.min_distance;
        if (effective_distance > buffer->spatial.max_distance)
            effective_distance = buffer->spatial.max_distance;
        if (effective_distance > buffer->spatial.min_distance &&
            listener->rolloff_factor > 0.0f) {
            float denominator = buffer->spatial.min_distance +
                listener->rolloff_factor *
                (effective_distance - buffer->spatial.min_distance);
            if (denominator > 0.0f)
                attenuation = buffer->spatial.min_distance / denominator;
        }
    }

    uint32_t spatial_gain = dsound_float_gain_q16(attenuation);
    buffer->left_gain_q16 = dsound_multiply_gain_q16(
        buffer->left_gain_q16, spatial_gain);
    buffer->right_gain_q16 = dsound_multiply_gain_q16(
        buffer->right_gain_q16, spatial_gain);

    if (has_direction) {
        D3DVECTOR_DS right;
        if (buffer->spatial.mode == DS3DMODE_HEADRELATIVE) {
            right.x = 1.0f;
            right.y = 0.0f;
            right.z = 0.0f;
        } else {
            right = dsound_vector_cross(&listener->orient_top,
                                         &listener->orient_front);
        }
        if (dsound_vector_normalize(&right)) {
            float pan = dsound_clampf(dsound_vector_dot(&direction, &right),
                                      -1.0f, 1.0f);
            if (pan > 0.0f) {
                buffer->left_gain_q16 = dsound_multiply_gain_q16(
                    buffer->left_gain_q16,
                    dsound_float_gain_q16(1.0f - pan));
            } else if (pan < 0.0f) {
                buffer->right_gain_q16 = dsound_multiply_gain_q16(
                    buffer->right_gain_q16,
                    dsound_float_gain_q16(1.0f + pan));
            }
        }

        if (buffer->spatial.cone_outside_volume < DSBVOLUME_MAX &&
            buffer->spatial.outside_cone_angle < DS3D_MAXCONEANGLE) {
            D3DVECTOR_DS cone = buffer->spatial.cone_orientation;
            if (dsound_vector_normalize(&cone)) {
                D3DVECTOR_DS toward_listener = {
                    -direction.x, -direction.y, -direction.z,
                };
                float dot = dsound_clampf(
                    dsound_vector_dot(&cone, &toward_listener),
                    -1.0f, 1.0f);
                float radians_per_half_degree = DS3D_PI / 360.0f;
                float inside_cos = dsound_cosf(
                    (float)buffer->spatial.inside_cone_angle *
                    radians_per_half_degree);
                float outside_cos = dsound_cosf(
                    (float)buffer->spatial.outside_cone_angle *
                    radians_per_half_degree);
                float outside_mix = 0.0f;
                if (dot <= outside_cos) {
                    outside_mix = 1.0f;
                } else if (dot < inside_cos) {
                    float range = inside_cos - outside_cos;
                    outside_mix = range > 0.000001f
                        ? (inside_cos - dot) / range : 1.0f;
                }
                LONG cone_volume = (LONG)(
                    (float)buffer->spatial.cone_outside_volume *
                    outside_mix);
                uint32_t cone_gain = dsound_gain_q16(cone_volume);
                buffer->left_gain_q16 = dsound_multiply_gain_q16(
                    buffer->left_gain_q16, cone_gain);
                buffer->right_gain_q16 = dsound_multiply_gain_q16(
                    buffer->right_gain_q16, cone_gain);
            }
        }

        if (listener->doppler_factor > 0.0f) {
            float velocity_scale = listener->distance_factor *
                                   listener->doppler_factor;
            float listener_toward = dsound_vector_dot(
                &listener_velocity, &direction) * velocity_scale;
            float source_away = dsound_vector_dot(
                &buffer->spatial.velocity, &direction) * velocity_scale;
            float minimum_term = DS3D_SPEED_OF_SOUND * 0.1f;
            float numerator = DS3D_SPEED_OF_SOUND + listener_toward;
            float denominator = DS3D_SPEED_OF_SOUND + source_away;
            if (numerator < minimum_term)
                numerator = minimum_term;
            if (denominator < minimum_term)
                denominator = minimum_term;
            float ratio = dsound_clampf(numerator / denominator,
                                        0.25f, 4.0f);
            float shifted = (float)buffer->playback_rate * ratio;
            if (shifted < (float)DSBFREQUENCY_MIN)
                shifted = (float)DSBFREQUENCY_MIN;
            if (shifted > (float)DSBFREQUENCY_MAX)
                shifted = (float)DSBFREQUENCY_MAX;
            buffer->mix_rate = (uint32_t)(shifted + 0.5f);
        }
    }
}

static void dsound_refresh_device_gains_locked(DSOUND_DEVICE *device)
{
    if (!device)
        return;
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
            DSOUND_BUFFER *buffer = &process->buffers[b];
            if (buffer->in_use && buffer->device == device)
                dsound_update_gains(buffer);
        }
    }
}

static HRESULT dsound_parse_format(const WAVEFORMATEX_DS *wave,
                                   pcm_format_t *format,
                                   uint16_t *block_align)
{
    if (!wave || !format || !block_align)
        return DSERR_INVALIDPARAM;
    if (wave->wFormatTag != WAVE_FORMAT_PCM ||
        (wave->nChannels != 1 && wave->nChannels != 2) ||
        (wave->wBitsPerSample != 8 && wave->wBitsPerSample != 16) ||
        wave->nSamplesPerSec < DSBFREQUENCY_MIN ||
        wave->nSamplesPerSec > DSBFREQUENCY_MAX)
        return DSERR_BADFORMAT;

    uint16_t expected_align =
        (uint16_t)(wave->nChannels * (wave->wBitsPerSample / 8U));
    uint32_t expected_average = wave->nSamplesPerSec * expected_align;
    if ((wave->nBlockAlign && wave->nBlockAlign != expected_align) ||
        (wave->nAvgBytesPerSec &&
         wave->nAvgBytesPerSec != expected_average))
        return DSERR_BADFORMAT;

    format->sample_rate = wave->nSamplesPerSec;
    format->channels = wave->nChannels;
    format->sample_format = wave->wBitsPerSample == 16
                          ? PCM_SAMPLE_S16_LE : PCM_SAMPLE_U8;
    *block_align = expected_align;
    return DS_OK;
}

static void dsound_write_format(const DSOUND_BUFFER *buffer,
                                WAVEFORMATEX_DS *wave)
{
    wave->wFormatTag = WAVE_FORMAT_PCM;
    wave->nChannels = buffer->format.channels;
    wave->nSamplesPerSec = buffer->format.sample_rate;
    wave->nAvgBytesPerSec = buffer->format.sample_rate * buffer->block_align;
    wave->nBlockAlign = buffer->block_align;
    wave->wBitsPerSample =
        buffer->format.sample_format == PCM_SAMPLE_S16_LE ? 16 : 8;
    wave->cbSize = 0;
}

static HRESULT dsound_read_description(PVOID description,
                                       DSBUFFERDESC_VIEW *view,
                                       bool compat32)
{
    if (!description || !view)
        return DSERR_INVALIDPARAM;

    BYTE snapshot[24];
    DWORD minimum = compat32 ? 20U : 24U;
    if (!dsound_guest_readable(description, minimum, compat32,
                               "CreateSoundBuffer description"))
        return DSERR_INVALIDPARAM;
    memcpy(snapshot, description, minimum);

    const BYTE *bytes = snapshot;
    DWORD size = *(const DWORD *)(const void *)bytes;
    if (size < minimum)
        return DSERR_INVALIDPARAM;

    view->size = size;
    view->flags = *(const DWORD *)(const void *)(bytes + 4);
    view->buffer_bytes = *(const DWORD *)(const void *)(bytes + 8);
    view->reserved = *(const DWORD *)(const void *)(bytes + 12);
    if (compat32) {
        uint32_t pointer = *(const uint32_t *)(const void *)(bytes + 16);
        view->format = (const WAVEFORMATEX_DS *)(ULONG_PTR)pointer;
    } else {
        PVOID pointer = *(PVOID const *)(const void *)(bytes + 16);
        view->format = (const WAVEFORMATEX_DS *)pointer;
    }
    return DS_OK;
}

static HRESULT dsound_validate_buffer_request(const DSBUFFERDESC_VIEW *view,
                                              DWORD *actual_flags)
{
    if (!view || !actual_flags || view->reserved)
        return DSERR_INVALIDPARAM;
    if (view->flags & ~DSBCAPS_KNOWN_MASK)
        return DSERR_INVALIDPARAM;
    if ((view->flags & DSBCAPS_LOCHARDWARE) &&
        (view->flags & DSBCAPS_LOCSOFTWARE))
        return DSERR_INVALIDPARAM;
    if (view->flags & (DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER))
        return DSERR_DS8_REQUIRED;
    if (view->flags & DSBCAPS_LOCHARDWARE)
        return DSERR_CONTROLUNAVAIL;
    if (view->flags & DSBCAPS_UNSUPPORTED_CONTROLS)
        return DSERR_CONTROLUNAVAIL;
    if ((view->flags & DSBCAPS_CTRL3D) &&
        (view->flags & DSBCAPS_CTRLPAN))
        return DSERR_INVALIDPARAM;
    if ((view->flags & DSBCAPS_MUTE3DATMAXDISTANCE) &&
        (!(view->flags & DSBCAPS_CTRL3D) ||
         (view->flags & DSBCAPS_PRIMARYBUFFER)))
        return DSERR_INVALIDPARAM;

    /* All buffers are mixed from system memory. STATIC is only a placement
     * hint, so the returned capabilities describe the actual location. */
    *actual_flags = (view->flags & ~DSBCAPS_STATIC) |
                    DSBCAPS_LOCSOFTWARE;
    return DS_OK;
}

static void dsound_log_buffer_rejection(const DSBUFFERDESC_VIEW *view,
                                        HRESULT result)
{
    serial_puts("[DSOUND] CreateSoundBuffer rejected flags=0x");
    serial_puthex(view ? view->flags : 0, 8);
    serial_puts(" hr=0x");
    serial_puthex((uint32_t)result, 8);
    serial_puts("\n");
}

static uint8_t *dsound_kernel_alias(uint64_t cr3, PVOID user_data,
                                    uint32_t bytes)
{
    if (!cr3 || !user_data || !bytes)
        return NULL;

    uint64_t user = (uint64_t)(ULONG_PTR)user_data;
    uint64_t first = paging_translate_in_cr3(cr3, user);
    if (first == UINT64_MAX)
        return NULL;

    uint64_t offset = 0;
    while (offset < bytes) {
        uint64_t physical = paging_translate_in_cr3(cr3, user + offset);
        if (physical == UINT64_MAX || physical != first + offset)
            return NULL;
        uint64_t next = 4096U - ((user + offset) & 4095U);
        if (next > (uint64_t)bytes - offset)
            next = (uint64_t)bytes - offset;
        offset += next;
    }
    return (uint8_t *)PHYS_TO_VIRT(first);
}

static bool dsound_build_proxy_page(BYTE **page_out)
{
    BYTE *page = (BYTE *)VirtualAlloc(NULL, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page || (ULONG_PTR)page > (ULONG_PTR)UINT32_MAX - 4095U) {
        if (page)
            VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    memset(page, 0, 4096);

    uint32_t *ds_vtable =
        (uint32_t *)(void *)(page + DSOUND_DS32_VTBL_OFFSET);
    uint32_t *dsb_vtable =
        (uint32_t *)(void *)(page + DSOUND_DSB32_VTBL_OFFSET);
    uint32_t *dsl_vtable =
        (uint32_t *)(void *)(page + DSOUND_DSL32_VTBL_OFFSET);
    uint32_t *ds3db_vtable =
        (uint32_t *)(void *)(page + DSOUND_DS3DB32_VTBL_OFFSET);

    const uint64_t ds_targets[DSOUND_DS_VTBL_SLOTS] = {
        (uint64_t)(ULONG_PTR)ds_QueryInterface,
        (uint64_t)(ULONG_PTR)ds_AddRef,
        (uint64_t)(ULONG_PTR)ds_Release,
        (uint64_t)(ULONG_PTR)ds_CreateSoundBuffer,
        (uint64_t)(ULONG_PTR)ds_GetCaps,
        (uint64_t)(ULONG_PTR)ds_DuplicateSoundBuffer,
        (uint64_t)(ULONG_PTR)ds_SetCooperativeLevel,
        (uint64_t)(ULONG_PTR)ds_Compact,
        (uint64_t)(ULONG_PTR)ds_GetSpeakerConfig,
        (uint64_t)(ULONG_PTR)ds_SetSpeakerConfig,
        (uint64_t)(ULONG_PTR)ds_Initialize,
    };
    static const char *const ds_names[DSOUND_DS_VTBL_SLOTS] = {
        "DS_QueryInterface", "DS_AddRef", "DS_Release",
        "DS_CreateSoundBuffer", "DS_GetCaps", "DS_DuplicateSoundBuffer",
        "DS_SetCooperativeLevel", "DS_Compact", "DS_GetSpeakerConfig",
        "DS_SetSpeakerConfig", "DS_Initialize",
    };
    static const uint8_t ds_args[DSOUND_DS_VTBL_SLOTS] = {
        3, 1, 1, 4, 2, 3, 3, 1, 2, 2, 2,
    };

    const uint64_t dsb_targets[DSOUND_DSB_VTBL_SLOTS] = {
        (uint64_t)(ULONG_PTR)dsb_QueryInterface,
        (uint64_t)(ULONG_PTR)dsb_AddRef,
        (uint64_t)(ULONG_PTR)dsb_Release,
        (uint64_t)(ULONG_PTR)dsb_GetCaps,
        (uint64_t)(ULONG_PTR)dsb_GetCurrentPosition,
        (uint64_t)(ULONG_PTR)dsb_GetFormat,
        (uint64_t)(ULONG_PTR)dsb_GetVolume,
        (uint64_t)(ULONG_PTR)dsb_GetPan,
        (uint64_t)(ULONG_PTR)dsb_GetFrequency,
        (uint64_t)(ULONG_PTR)dsb_GetStatus,
        (uint64_t)(ULONG_PTR)dsb_Initialize,
        (uint64_t)(ULONG_PTR)dsb_Lock,
        (uint64_t)(ULONG_PTR)dsb_Play,
        (uint64_t)(ULONG_PTR)dsb_SetCurrentPosition,
        (uint64_t)(ULONG_PTR)dsb_SetFormat,
        (uint64_t)(ULONG_PTR)dsb_SetVolume,
        (uint64_t)(ULONG_PTR)dsb_SetPan,
        (uint64_t)(ULONG_PTR)dsb_SetFrequency,
        (uint64_t)(ULONG_PTR)dsb_Stop,
        (uint64_t)(ULONG_PTR)dsb_Unlock,
        (uint64_t)(ULONG_PTR)dsb_Restore,
    };
    static const char *const dsb_names[DSOUND_DSB_VTBL_SLOTS] = {
        "DSB_QueryInterface", "DSB_AddRef", "DSB_Release", "DSB_GetCaps",
        "DSB_GetCurrentPosition", "DSB_GetFormat", "DSB_GetVolume",
        "DSB_GetPan", "DSB_GetFrequency", "DSB_GetStatus",
        "DSB_Initialize", "DSB_Lock", "DSB_Play",
        "DSB_SetCurrentPosition", "DSB_SetFormat", "DSB_SetVolume",
        "DSB_SetPan", "DSB_SetFrequency", "DSB_Stop", "DSB_Unlock",
        "DSB_Restore",
    };
    static const uint8_t dsb_args[DSOUND_DSB_VTBL_SLOTS] = {
        3, 1, 1, 2, 3, 4, 2, 2, 2, 2, 3, 8, 4, 2, 2, 2, 2, 2, 1, 5, 1,
    };

    const uint64_t dsl_targets[DSOUND_DSL_VTBL_SLOTS] = {
        (uint64_t)(ULONG_PTR)dsb_QueryInterface,
        (uint64_t)(ULONG_PTR)dsb_AddRef,
        (uint64_t)(ULONG_PTR)dsb_Release,
        (uint64_t)(ULONG_PTR)dsl_GetAllParameters,
        (uint64_t)(ULONG_PTR)dsl_GetDistanceFactor,
        (uint64_t)(ULONG_PTR)dsl_GetDopplerFactor,
        (uint64_t)(ULONG_PTR)dsl_GetOrientation,
        (uint64_t)(ULONG_PTR)dsl_GetPosition,
        (uint64_t)(ULONG_PTR)dsl_GetRolloffFactor,
        (uint64_t)(ULONG_PTR)dsl_GetVelocity,
        (uint64_t)(ULONG_PTR)dsl_SetAllParameters,
        (uint64_t)(ULONG_PTR)dsl32_SetDistanceFactor,
        (uint64_t)(ULONG_PTR)dsl32_SetDopplerFactor,
        (uint64_t)(ULONG_PTR)dsl32_SetOrientation,
        (uint64_t)(ULONG_PTR)dsl32_SetPosition,
        (uint64_t)(ULONG_PTR)dsl32_SetRolloffFactor,
        (uint64_t)(ULONG_PTR)dsl32_SetVelocity,
        (uint64_t)(ULONG_PTR)dsl_CommitDeferredSettings,
    };
    static const char *const dsl_names[DSOUND_DSL_VTBL_SLOTS] = {
        "DSL_QueryInterface", "DSL_AddRef", "DSL_Release",
        "DSL_GetAllParameters", "DSL_GetDistanceFactor",
        "DSL_GetDopplerFactor", "DSL_GetOrientation", "DSL_GetPosition",
        "DSL_GetRolloffFactor", "DSL_GetVelocity",
        "DSL_SetAllParameters", "DSL_SetDistanceFactor",
        "DSL_SetDopplerFactor", "DSL_SetOrientation", "DSL_SetPosition",
        "DSL_SetRolloffFactor", "DSL_SetVelocity",
        "DSL_CommitDeferredSettings",
    };
    static const uint8_t dsl_args[DSOUND_DSL_VTBL_SLOTS] = {
        3, 1, 1, 2, 2, 2, 3, 2, 2, 2, 3, 3, 3, 8, 5, 3, 5, 1,
    };

    const uint64_t ds3db_targets[DSOUND_DS3DB_VTBL_SLOTS] = {
        (uint64_t)(ULONG_PTR)dsb_QueryInterface,
        (uint64_t)(ULONG_PTR)dsb_AddRef,
        (uint64_t)(ULONG_PTR)dsb_Release,
        (uint64_t)(ULONG_PTR)ds3db_GetAllParameters,
        (uint64_t)(ULONG_PTR)ds3db_GetConeAngles,
        (uint64_t)(ULONG_PTR)ds3db_GetConeOrientation,
        (uint64_t)(ULONG_PTR)ds3db_GetConeOutsideVolume,
        (uint64_t)(ULONG_PTR)ds3db_GetMaxDistance,
        (uint64_t)(ULONG_PTR)ds3db_GetMinDistance,
        (uint64_t)(ULONG_PTR)ds3db_GetMode,
        (uint64_t)(ULONG_PTR)ds3db_GetPosition,
        (uint64_t)(ULONG_PTR)ds3db_GetVelocity,
        (uint64_t)(ULONG_PTR)ds3db_SetAllParameters,
        (uint64_t)(ULONG_PTR)ds3db_SetConeAngles,
        (uint64_t)(ULONG_PTR)ds3db32_SetConeOrientation,
        (uint64_t)(ULONG_PTR)ds3db_SetConeOutsideVolume,
        (uint64_t)(ULONG_PTR)ds3db32_SetMaxDistance,
        (uint64_t)(ULONG_PTR)ds3db32_SetMinDistance,
        (uint64_t)(ULONG_PTR)ds3db_SetMode,
        (uint64_t)(ULONG_PTR)ds3db32_SetPosition,
        (uint64_t)(ULONG_PTR)ds3db32_SetVelocity,
    };
    static const char *const ds3db_names[DSOUND_DS3DB_VTBL_SLOTS] = {
        "DS3DB_QueryInterface", "DS3DB_AddRef", "DS3DB_Release",
        "DS3DB_GetAllParameters", "DS3DB_GetConeAngles",
        "DS3DB_GetConeOrientation", "DS3DB_GetConeOutsideVolume",
        "DS3DB_GetMaxDistance", "DS3DB_GetMinDistance", "DS3DB_GetMode",
        "DS3DB_GetPosition", "DS3DB_GetVelocity",
        "DS3DB_SetAllParameters", "DS3DB_SetConeAngles",
        "DS3DB_SetConeOrientation", "DS3DB_SetConeOutsideVolume",
        "DS3DB_SetMaxDistance", "DS3DB_SetMinDistance", "DS3DB_SetMode",
        "DS3DB_SetPosition", "DS3DB_SetVelocity",
    };
    static const uint8_t ds3db_args[DSOUND_DS3DB_VTBL_SLOTS] = {
        3, 1, 1, 2, 3, 2, 2, 2, 2, 2, 2, 2, 3, 4, 5, 3, 3, 3, 3, 5, 5,
    };

    for (uint32_t i = 0; i < DSOUND_DS_VTBL_SLOTS; i++) {
        ds_vtable[i] = compat32_make_thunk_ex(
            ds_targets[i], ds_names[i], ds_args[i], CC_STDCALL);
        if (!ds_vtable[i]) {
            VirtualFree(page, 0, MEM_RELEASE);
            return false;
        }
    }
    for (uint32_t i = 0; i < DSOUND_DSB_VTBL_SLOTS; i++) {
        dsb_vtable[i] = compat32_make_thunk_ex(
            dsb_targets[i], dsb_names[i], dsb_args[i], CC_STDCALL);
        if (!dsb_vtable[i]) {
            VirtualFree(page, 0, MEM_RELEASE);
            return false;
        }
    }
    for (uint32_t i = 0; i < DSOUND_DSL_VTBL_SLOTS; i++) {
        dsl_vtable[i] = compat32_make_thunk_ex(
            dsl_targets[i], dsl_names[i], dsl_args[i], CC_STDCALL);
        if (!dsl_vtable[i]) {
            VirtualFree(page, 0, MEM_RELEASE);
            return false;
        }
    }
    for (uint32_t i = 0; i < DSOUND_DS3DB_VTBL_SLOTS; i++) {
        ds3db_vtable[i] = compat32_make_thunk_ex(
            ds3db_targets[i], ds3db_names[i], ds3db_args[i], CC_STDCALL);
        if (!ds3db_vtable[i]) {
            VirtualFree(page, 0, MEM_RELEASE);
            return false;
        }
    }

    for (uint32_t i = 0; i < DSOUND_DEVICES_PER_PROCESS; i++) {
        uint32_t *object = (uint32_t *)(void *)(
            page + DSOUND_DS32_OBJECT_OFFSET + i * sizeof(uint32_t));
        *object = (uint32_t)(ULONG_PTR)ds_vtable;
    }
    for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
        uint32_t *object = (uint32_t *)(void *)(
            page + DSOUND_DSB32_OBJECT_OFFSET + i * sizeof(uint32_t));
        *object = (uint32_t)(ULONG_PTR)dsb_vtable;
        object = (uint32_t *)(void *)(
            page + DSOUND_DSL32_OBJECT_OFFSET + i * sizeof(uint32_t));
        *object = (uint32_t)(ULONG_PTR)dsl_vtable;
        object = (uint32_t *)(void *)(
            page + DSOUND_DS3DB32_OBJECT_OFFSET + i * sizeof(uint32_t));
        *object = (uint32_t)(ULONG_PTR)ds3db_vtable;
    }

    *page_out = page;
    return true;
}

static DSOUND_PROCESS *dsound_get_or_create_process(void)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid)
        owner_pid = 1;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_PROCESS *existing = dsound_find_process_locked(owner_pid);
    dsound_unlock_irqrestore(irq_flags);
    if (existing)
        return existing;

    bool compat32 = g_compat32_mode;
    BYTE *page = NULL;
    if (compat32 && !dsound_build_proxy_page(&page))
        return NULL;

    irq_flags = dsound_lock_irqsave();
    existing = dsound_find_process_locked(owner_pid);
    DSOUND_PROCESS *process = existing;
    if (!process) {
        for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
            if (!dsound_processes[i].owner_pid) {
                process = &dsound_processes[i];
                memset(process, 0, sizeof(*process));
                process->owner_pid = owner_pid;
                process->owner_cr3 = proc_current_cr3();
                process->compat32 = compat32;
                process->proxy_page = page;
                for (uint32_t d = 0; d < DSOUND_DEVICES_PER_PROCESS; d++) {
                    process->device_objects64[d].lpVtbl = &ds_vtbl64;
                    process->device_objects[d] = compat32
                        ? (PVOID)(page + DSOUND_DS32_OBJECT_OFFSET +
                                  d * sizeof(uint32_t))
                        : (PVOID)&process->device_objects64[d];
                }
                for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
                    process->buffer_objects64[b].lpVtbl = &dsb_vtbl64;
                    process->listener_objects64[b].lpVtbl = &dsl_vtbl64;
                    process->buffer3d_objects64[b].lpVtbl = &ds3db_vtbl64;
                    process->buffer_objects[b] = compat32
                        ? (PVOID)(page + DSOUND_DSB32_OBJECT_OFFSET +
                                  b * sizeof(uint32_t))
                        : (PVOID)&process->buffer_objects64[b];
                    process->listener_objects[b] = compat32
                        ? (PVOID)(page + DSOUND_DSL32_OBJECT_OFFSET +
                                  b * sizeof(uint32_t))
                        : (PVOID)&process->listener_objects64[b];
                    process->buffer3d_objects[b] = compat32
                        ? (PVOID)(page + DSOUND_DS3DB32_OBJECT_OFFSET +
                                  b * sizeof(uint32_t))
                        : (PVOID)&process->buffer3d_objects64[b];
                }
                break;
            }
        }
    }
    dsound_unlock_irqrestore(irq_flags);

    if ((!process || existing) && page)
        VirtualFree(page, 0, MEM_RELEASE);
    if (!process)
        return NULL;

    if (!existing) {
        serial_puts("[DSOUND] process state pid=");
        serial_putdec(owner_pid);
        serial_puts(compat32 ? " PE32\n" : " PE64\n");
    }
    return process;
}

static bool dsound_has_playing_locked(void)
{
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
            DSOUND_BUFFER *buffer = &process->buffers[b];
            if (buffer->in_use && !buffer->primary && buffer->playing)
                return true;
        }
    }
    return false;
}

static bool dsound_audio_fill(int16_t *output, uint32_t frames)
{
    if (!output || !frames || frames > DSOUND_MIX_FRAMES)
        return false;

    memset(dsound_mix_accumulator, 0,
           frames * 2U * sizeof(dsound_mix_accumulator[0]));
    bool active = false;

    uint64_t irq_flags = dsound_lock_irqsave();
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
            DSOUND_BUFFER *buffer = &process->buffers[b];
            if (!buffer->in_use || buffer->primary || !buffer->playing)
                continue;
            active = true;
            if (buffer->locked || !buffer->allocation ||
                !buffer->allocation->kernel_data)
                continue;

            pcm_format_t playback_format = buffer->format;
            playback_format.sample_rate = buffer->mix_rate;
            bool ended = false;
            pcm_mix_circular_s16_stereo(
                dsound_mix_accumulator, frames,
                buffer->allocation->kernel_data,
                buffer->allocation->bytes, &playback_format,
                AUDIO_OUTPUT_RATE_HZ, buffer->left_gain_q16,
                buffer->right_gain_q16, buffer->looping,
                &buffer->cursor, &ended);
            if (ended && !buffer->looping)
                buffer->playing = false;
        }
    }
    if (!active)
        dsound_mixer_running = false;
    dsound_unlock_irqrestore(irq_flags);

    if (!active) {
        memset(output, 0, frames * 2U * sizeof(output[0]));
        return false;
    }

    for (uint32_t i = 0; i < frames * 2U; i++) {
        int32_t sample = dsound_mix_accumulator[i];
        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        output[i] = (int16_t)sample;
    }
#if DSOUND_DIAGNOSTICS
    __atomic_add_fetch(&dsound_mix_fill_count, 1, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < frames * 2U; i++) {
        if (output[i]) {
            __atomic_add_fetch(&dsound_mix_signal_count, 1,
                               __ATOMIC_RELAXED);
            break;
        }
    }
#endif
    return true;
}

static audio_register_result_t dsound_refresh_mixer(void)
{
    audio_register_result_t result = AUDIO_REGISTER_OK;
    dsound_transition_acquire();

    uint64_t irq_flags = dsound_lock_irqsave();
    bool active = dsound_has_playing_locked();
    bool running = dsound_mixer_running;
    dsound_unlock_irqrestore(irq_flags);

    if (active && !running) {
        audio_source_id_t source = AUDIO_SOURCE_INVALID;
        result = audio_source_register_ex(
            dsound_audio_fill, dsound_mix_output, DSOUND_MIX_FRAMES,
            AUDIO_OUTPUT_RATE_HZ, &source);
        bool started = result == AUDIO_REGISTER_OK;
        irq_flags = dsound_lock_irqsave();
        dsound_mixer_running = started;
        dsound_audio_source = started ? source : AUDIO_SOURCE_INVALID;
        active = dsound_has_playing_locked();
        dsound_unlock_irqrestore(irq_flags);
        if (started && !active) {
            audio_source_unregister(source);
            irq_flags = dsound_lock_irqsave();
            dsound_mixer_running = false;
            dsound_audio_source = AUDIO_SOURCE_INVALID;
            dsound_unlock_irqrestore(irq_flags);
            result = AUDIO_REGISTER_OK;
        }
    } else if (!active && running) {
        audio_source_id_t source = dsound_audio_source;
        audio_source_unregister(source);
        irq_flags = dsound_lock_irqsave();
        dsound_mixer_running = false;
        dsound_audio_source = AUDIO_SOURCE_INVALID;
        dsound_unlock_irqrestore(irq_flags);
    }

    dsound_transition_release();
    return result;
}

static HRESULT dsound_registration_hresult(audio_register_result_t result)
{
    serial_puts("[DSOUND] playback rejected: ");
    serial_puts(audio_register_result_name(result));
    serial_puts("\n");

    switch (result) {
    case AUDIO_REGISTER_NO_BACKEND:
        return DSERR_NODRIVER;
    case AUDIO_REGISTER_NO_SLOTS:
        return DSERR_ALLOCATED;
    case AUDIO_REGISTER_INVALID:
        return DSERR_INVALIDPARAM;
    case AUDIO_REGISTER_SOURCE_INACTIVE:
        return DSERR_INVALIDCALL;
    case AUDIO_REGISTER_NO_CLOCK:
    default:
        return DSERR_GENERIC;
    }
}

static uint32_t dsound_play_cursor_bytes(const DSOUND_BUFFER *buffer)
{
    if (!buffer->allocation || !buffer->allocation->bytes ||
        !buffer->block_align)
        return 0;
    uint64_t frame = buffer->cursor.frame_q32 >> 32;
    return (uint32_t)((frame * buffer->block_align) %
                      buffer->allocation->bytes);
}

static uint32_t dsound_safe_write_cursor(const DSOUND_BUFFER *buffer)
{
    if (!buffer->allocation || !buffer->allocation->bytes)
        return 0;
    uint32_t lead_frames = (buffer->playback_rate * 15U + 999U) / 1000U;
    uint64_t lead_bytes = (uint64_t)lead_frames * buffer->block_align;
    return (uint32_t)((dsound_play_cursor_bytes(buffer) + lead_bytes) %
                      buffer->allocation->bytes);
}

static uint32_t dsound_reported_play_cursor(const DSOUND_BUFFER *buffer)
{
    uint32_t actual = dsound_play_cursor_bytes(buffer);
    if (!buffer->allocation ||
        (buffer->flags & DSBCAPS_GETCURRENTPOSITION2))
        return actual;

    /* The emulated DirectSound contract predates accurate play cursors. When
     * GETCURRENTPOSITION2 is absent, preserve the legacy cursor immediately
     * behind the safe write cursor. */
    uint32_t write = dsound_safe_write_cursor(buffer);
    uint32_t alignment = buffer->block_align ? buffer->block_align : 1U;
    return write >= alignment
         ? write - alignment
         : buffer->allocation->bytes - (alignment - write);
}

static HRESULT WINAPI ds_QueryInterface(PVOID self, LPCGUID iid, PVOID output)
{
    dsound_trace("IDirectSound::QueryInterface");
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;
    if (!iid || !dsound_guest_readable(iid, sizeof(*iid), compat32,
                                       "IDirectSound::QueryInterface IID") ||
        (!dsound_guid_equal(iid, &iid_iunknown) &&
                 !dsound_guid_equal(iid, &iid_idirectsound)))
        return DSERR_NOINTERFACE;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    bool valid = device && device->refs;
    if (valid)
        device->refs++;
    dsound_unlock_irqrestore(irq_flags);
    if (!valid)
        return DSERR_INVALIDCALL;
    dsound_store_pointer(compat32, output, self);
    return DS_OK;
}

static ULONG WINAPI ds_AddRef(PVOID self)
{
    dsound_trace("IDirectSound::AddRef");
    ULONG refs = 0;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    if (device && device->refs)
        refs = ++device->refs;
    dsound_unlock_irqrestore(irq_flags);
    return refs;
}

static ULONG WINAPI ds_Release(PVOID self)
{
    dsound_trace("IDirectSound::Release");
    ULONG refs = 0;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    if (device && device->refs) {
        refs = --device->refs;
        dsound_retire_device_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return refs;
}

static HRESULT WINAPI ds_CreateSoundBuffer(PVOID self, PVOID description,
                                           PVOID output, PVOID outer)
{
    dsound_trace("IDirectSound::CreateSoundBuffer");
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;
    if (outer)
        return DSERR_NOAGGREGATION;

    DSBUFFERDESC_VIEW view;
    HRESULT result = dsound_read_description(description, &view, compat32);
    if (result != DS_OK)
        return result;

    DWORD actual_flags = 0;
    result = dsound_validate_buffer_request(&view, &actual_flags);
    if (result != DS_OK) {
        dsound_log_buffer_rejection(&view, result);
        return result;
    }

    bool primary = (view.flags & DSBCAPS_PRIMARYBUFFER) != 0;
    pcm_format_t format = {
        AUDIO_OUTPUT_RATE_HZ, AUDIO_OUTPUT_CHANNELS, PCM_SAMPLE_S16_LE,
    };
    uint16_t block_align = 4;
    if (primary) {
        if (view.buffer_bytes || view.format) {
            dsound_log_buffer_rejection(&view, DSERR_INVALIDPARAM);
            return DSERR_INVALIDPARAM;
        }
    } else {
        if (view.buffer_bytes < DSBSIZE_MIN ||
            view.buffer_bytes > DSBSIZE_MAX || !view.format) {
            dsound_log_buffer_rejection(&view, DSERR_INVALIDPARAM);
            return DSERR_INVALIDPARAM;
        }
        WAVEFORMATEX_DS wave;
        if (!dsound_guest_readable(view.format, sizeof(wave), compat32,
                                   "CreateSoundBuffer format")) {
            dsound_log_buffer_rejection(&view, DSERR_INVALIDPARAM);
            return DSERR_INVALIDPARAM;
        }
        memcpy(&wave, view.format, sizeof(wave));
        result = dsound_parse_format(&wave, &format, &block_align);
        if (result != DS_OK) {
            dsound_log_buffer_rejection(&view, result);
            return result;
        }
        if (view.buffer_bytes % block_align) {
            dsound_log_buffer_rejection(&view, DSERR_INVALIDPARAM);
            return DSERR_INVALIDPARAM;
        }
    }

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_PROCESS *process = NULL;
    DSOUND_DEVICE *device =
        dsound_device_from_object_locked(self, &process);
    if (!device || !device->refs) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (!device->initialized) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_UNINITIALIZED;
    }
    if (primary) {
        for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
            DSOUND_BUFFER *existing = &process->buffers[i];
            if (existing->in_use && existing->primary &&
                existing->device == device) {
                existing->flags |= actual_flags;
                existing->refs++;
                PVOID object = existing->object;
                dsound_unlock_irqrestore(irq_flags);
                dsound_store_pointer(compat32, output, object);
                return DS_OK;
            }
        }
    }
    uint64_t owner_cr3 = process->owner_cr3;
    dsound_unlock_irqrestore(irq_flags);

    if (!audio_output_is_ready())
        return DSERR_NODRIVER;

    PVOID user_data = NULL;
    uint8_t *kernel_data = NULL;
    if (!primary) {
        user_data = VirtualAlloc(NULL, view.buffer_bytes,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!user_data)
            return DSERR_OUTOFMEMORY;
        kernel_data = dsound_kernel_alias(owner_cr3, user_data,
                                           view.buffer_bytes);
        if (!kernel_data) {
            VirtualFree(user_data, 0, MEM_RELEASE);
            return DSERR_OUTOFMEMORY;
        }
        pcm_fill_silence(kernel_data, view.buffer_bytes, &format);
    }

    irq_flags = dsound_lock_irqsave();
    process = NULL;
    device = dsound_device_from_object_locked(self, &process);
    if (!process || !device || !device->refs) {
        dsound_unlock_irqrestore(irq_flags);
        if (user_data)
            VirtualFree(user_data, 0, MEM_RELEASE);
        return DSERR_INVALIDCALL;
    }
    if (!device->initialized) {
        dsound_unlock_irqrestore(irq_flags);
        if (user_data)
            VirtualFree(user_data, 0, MEM_RELEASE);
        return DSERR_UNINITIALIZED;
    }
    if (primary) {
        for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
            DSOUND_BUFFER *existing = &process->buffers[i];
            if (existing->in_use && existing->primary &&
                existing->device == device) {
                existing->flags |= actual_flags;
                existing->refs++;
                PVOID object = existing->object;
                dsound_unlock_irqrestore(irq_flags);
                dsound_store_pointer(compat32, output, object);
                return DS_OK;
            }
        }
    }
    int buffer_slot = -1;
    int allocation_slot = -1;
    for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
        if (buffer_slot < 0 && !process->buffers[i].in_use)
            buffer_slot = (int)i;
        if (!primary && allocation_slot < 0 &&
            !process->allocations[i].in_use)
            allocation_slot = (int)i;
    }
    if (buffer_slot < 0 || (!primary && allocation_slot < 0)) {
        dsound_unlock_irqrestore(irq_flags);
        if (user_data)
            VirtualFree(user_data, 0, MEM_RELEASE);
        return DSERR_OUTOFMEMORY;
    }

    DSOUND_ALLOCATION *allocation = NULL;
    if (!primary) {
        allocation = &process->allocations[allocation_slot];
        memset(allocation, 0, sizeof(*allocation));
        allocation->in_use = true;
        allocation->refs = 1;
        allocation->user_data = user_data;
        allocation->kernel_data = kernel_data;
        allocation->bytes = view.buffer_bytes;
    }

    DSOUND_BUFFER *buffer = &process->buffers[buffer_slot];
    memset(buffer, 0, sizeof(*buffer));
    buffer->in_use = true;
    buffer->primary = primary;
    buffer->refs = 1;
    buffer->flags = actual_flags;
    buffer->object = process->buffer_objects[buffer_slot];
    buffer->device = device;
    buffer->allocation = allocation;
    buffer->format = format;
    buffer->original_rate = format.sample_rate;
    buffer->playback_rate = format.sample_rate;
    buffer->mix_rate = format.sample_rate;
    buffer->block_align = block_align;
    buffer->volume = DSBVOLUME_MAX;
    buffer->pan = 0;
    dsound_buffer3d_defaults(&buffer->spatial);
    buffer->deferred_spatial = buffer->spatial;
    dsound_update_gains(buffer);
    device->live_buffers++;
    PVOID object = buffer->object;
    dsound_unlock_irqrestore(irq_flags);

    dsound_store_pointer(compat32, output, object);
    if (primary) {
        serial_puts("[DSOUND] primary buffer flags=0x");
        serial_puthex(actual_flags, 8);
        serial_puts("\n");
    } else {
        serial_puts("[DSOUND] secondary buffer bytes=");
        serial_putdec(view.buffer_bytes);
        serial_puts(" rate=");
        serial_putdec(format.sample_rate);
        serial_puts(" channels=");
        serial_putdec(format.channels);
        serial_puts(" bits=");
        serial_putdec(format.sample_format == PCM_SAMPLE_S16_LE ? 16 : 8);
        serial_puts(" flags=0x");
        serial_puthex(actual_flags, 8);
        serial_puts("\n");
    }
    return DS_OK;
}

static void dsound_fill_device_caps(DWORD values[24])
{
    memset(values, 0, 24U * sizeof(values[0]));
    values[0] = 24U * sizeof(values[0]);
    values[1] = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO |
                DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                DSCAPS_CONTINUOUSRATE;
    values[2] = DSBFREQUENCY_MIN;
    values[3] = DSBFREQUENCY_MAX;
    values[4] = 1;
}

static HRESULT WINAPI ds_GetCaps(PVOID self, PVOID caps)
{
    dsound_trace("IDirectSound::GetCaps");
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_readable(caps, sizeof(DWORD), compat32,
                               "IDirectSound::GetCaps size") ||
        !dsound_guest_writable(caps, sizeof(DWORD), compat32,
                               "IDirectSound::GetCaps size"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    HRESULT state = !device || !device->refs ? DSERR_INVALIDCALL
                  : !device->initialized ? DSERR_UNINITIALIZED : DS_OK;
    dsound_unlock_irqrestore(irq_flags);
    if (state != DS_OK)
        return state;

    DWORD requested = *(DWORD *)caps;
    if (requested < 8)
        return DSERR_INVALIDPARAM;
    DWORD values[24];
    dsound_fill_device_caps(values);
    DWORD copy = requested < sizeof(values) ? requested : sizeof(values);
    if (!dsound_guest_writable(caps, copy, compat32,
                               "IDirectSound::GetCaps output"))
        return DSERR_INVALIDPARAM;
    memcpy(caps, values, copy);
    return DS_OK;
}

static HRESULT WINAPI ds_DuplicateSoundBuffer(PVOID self, PVOID original,
                                              PVOID output)
{
    dsound_trace("IDirectSound::DuplicateSoundBuffer");
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_PROCESS *process = NULL;
    DSOUND_DEVICE *device =
        dsound_device_from_object_locked(self, &process);
    DSOUND_PROCESS *source_process = NULL;
    DSOUND_BUFFER *source =
        dsound_buffer_from_object_locked(original, &source_process);
    if (device && device->refs && !device->initialized) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_UNINITIALIZED;
    }
    if (!device || !device->refs || !source || source_process != process ||
        source->device != device || source->primary || !source->allocation) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }

    int slot = -1;
    for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
        if (!process->buffers[i].in_use) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_OUTOFMEMORY;
    }

    DSOUND_BUFFER *duplicate = &process->buffers[slot];
    memset(duplicate, 0, sizeof(*duplicate));
    duplicate->in_use = true;
    duplicate->refs = 1;
    duplicate->flags = source->flags;
    duplicate->object = process->buffer_objects[slot];
    duplicate->device = device;
    duplicate->allocation = source->allocation;
    duplicate->allocation->refs++;
    duplicate->format = source->format;
    duplicate->original_rate = source->original_rate;
    duplicate->playback_rate = source->playback_rate;
    duplicate->mix_rate = source->mix_rate;
    duplicate->block_align = source->block_align;
    duplicate->volume = source->volume;
    duplicate->pan = source->pan;
    duplicate->spatial = source->spatial;
    duplicate->deferred_spatial = source->spatial;
    dsound_update_gains(duplicate);
    device->live_buffers++;
    PVOID object = duplicate->object;
    dsound_unlock_irqrestore(irq_flags);

    dsound_store_pointer(compat32, output, object);
    return DS_OK;
}

static HRESULT WINAPI ds_SetCooperativeLevel(PVOID self, PVOID window,
                                             DWORD level)
{
    dsound_trace("IDirectSound::SetCooperativeLevel");
    (void)window;
    HRESULT validation = dsound_validate_cooperative_level(level);
    if (validation != DS_OK)
        return validation;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    HRESULT result = !device || !device->refs ? DSERR_INVALIDCALL
                   : !device->initialized ? DSERR_UNINITIALIZED : DS_OK;
    if (result == DS_OK)
        device->cooperative_level = level;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI ds_Compact(PVOID self)
{
    dsound_trace("IDirectSound::Compact");
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    HRESULT result = !device || !device->refs ? DSERR_INVALIDCALL
                   : !device->initialized ? DSERR_UNINITIALIZED
                   : device->cooperative_level < DSSCL_PRIORITY
                   ? DSERR_PRIOLEVELNEEDED : DS_OK;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI ds_GetSpeakerConfig(PVOID self, DWORD *configuration)
{
    dsound_trace("IDirectSound::GetSpeakerConfig");
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(configuration, sizeof(*configuration),
                               compat32,
                               "IDirectSound::GetSpeakerConfig"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    HRESULT result = !device || !device->refs ? DSERR_INVALIDCALL
                   : !device->initialized ? DSERR_UNINITIALIZED : DS_OK;
    if (result == DS_OK)
        *configuration = DSSPEAKER_STEREO;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI ds_SetSpeakerConfig(PVOID self, DWORD configuration)
{
    dsound_trace("IDirectSound::SetSpeakerConfig");
    if (!dsound_valid_speaker_config(configuration))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_device_from_object_locked(self, NULL);
    HRESULT result = !device || !device->refs ? DSERR_INVALIDCALL
                   : !device->initialized ? DSERR_UNINITIALIZED : DS_OK;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI ds_Initialize(PVOID self, LPCGUID device)
{
    dsound_trace("IDirectSound::Initialize");
    bool compat32 = dsound_compat_call(self);
    if (device && !dsound_guest_readable(device, sizeof(*device), compat32,
                                         "IDirectSound::Initialize device"))
        return DSERR_INVALIDPARAM;
    if (!dsound_playback_device_supported(device)) {
        serial_puts("[DSOUND] Initialize rejected unknown playback device\n");
        return DSERR_NODRIVER;
    }
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device_state =
        dsound_device_from_object_locked(self, NULL);
    HRESULT result;
    if (!device_state || !device_state->refs) {
        result = DSERR_INVALIDCALL;
    } else if (device_state->initialized) {
        result = DSERR_ALREADYINITIALIZED;
    } else if (!audio_output_is_ready()) {
        result = DSERR_NODRIVER;
    } else {
        device_state->initialized = true;
        result = DS_OK;
    }
    dsound_unlock_irqrestore(irq_flags);
    if (result == DSERR_NODRIVER)
        serial_puts("[DSOUND] Initialize failed: no playback device\n");
    return result;
}

static HRESULT WINAPI dsb_QueryInterface(PVOID self, LPCGUID iid,
                                          PVOID output)
{
    dsound_trace("IDirectSoundBuffer/3D::QueryInterface");
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;
    if (!iid || !dsound_guest_readable(
            iid, sizeof(*iid), compat32,
            "IDirectSoundBuffer::QueryInterface IID"))
        return DSERR_NOINTERFACE;

    PVOID result_object = NULL;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_PROCESS *process = NULL;
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, &process);
    if (!buffer || !buffer->refs || !process) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }

    uint32_t slot = (uint32_t)(buffer - process->buffers);
    if (dsound_guid_equal(iid, &iid_iunknown) ||
        dsound_guid_equal(iid, &iid_idirectsoundbuffer)) {
        result_object = process->buffer_objects[slot];
    } else if (dsound_guid_equal(iid, &iid_idirectsound3dlistener) &&
               buffer->primary && (buffer->flags & DSBCAPS_CTRL3D)) {
        result_object = process->listener_objects[slot];
    } else if (dsound_guid_equal(iid, &iid_idirectsound3dbuffer) &&
               !buffer->primary && (buffer->flags & DSBCAPS_CTRL3D)) {
        result_object = process->buffer3d_objects[slot];
    }
    if (result_object)
        buffer->refs++;
    dsound_unlock_irqrestore(irq_flags);

    if (!result_object)
        return DSERR_NOINTERFACE;
    dsound_store_pointer(compat32, output, result_object);
    return DS_OK;
}

static ULONG WINAPI dsb_AddRef(PVOID self)
{
    dsound_trace("IDirectSoundBuffer::AddRef");
    ULONG refs = 0;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (buffer)
        refs = ++buffer->refs;
    dsound_unlock_irqrestore(irq_flags);
    return refs;
}

static ULONG WINAPI dsb_Release(PVOID self)
{
    dsound_trace("IDirectSoundBuffer::Release");
    ULONG refs = 0;
    PVOID free_data = NULL;
    bool refresh = false;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (buffer && buffer->refs) {
        refs = --buffer->refs;
        if (!refs) {
            refresh = buffer->playing;
            DSOUND_ALLOCATION *allocation = buffer->allocation;
            DSOUND_DEVICE *device = buffer->device;
            memset(buffer, 0, sizeof(*buffer));
            if (allocation && allocation->refs && !--allocation->refs) {
                free_data = allocation->user_data;
                memset(allocation, 0, sizeof(*allocation));
            }
            if (device && device->live_buffers)
                device->live_buffers--;
            dsound_retire_device_locked(device);
        }
    }
    dsound_unlock_irqrestore(irq_flags);
    if (free_data)
        VirtualFree(free_data, 0, MEM_RELEASE);
    if (refresh)
        dsound_refresh_mixer();
    return refs;
}

static HRESULT WINAPI dsb_GetCaps(PVOID self, PVOID caps)
{
    dsound_trace("IDirectSoundBuffer::GetCaps");
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_readable(caps, sizeof(DWORD), compat32,
                               "IDirectSoundBuffer::GetCaps size") ||
        !dsound_guest_writable(caps, 20U, compat32,
                               "IDirectSoundBuffer::GetCaps output") ||
        *(DWORD *)caps < 20U)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DWORD values[5] = {
        sizeof(values), buffer->flags,
        buffer->allocation ? buffer->allocation->bytes : 0,
        0, 0,
    };
    dsound_unlock_irqrestore(irq_flags);
    memcpy(caps, values, sizeof(values));
    return DS_OK;
}

static HRESULT WINAPI dsb_GetCurrentPosition(PVOID self, DWORD *play,
                                             DWORD *write)
{
    if (!play && !write)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    if ((play && !dsound_guest_writable(
            play, sizeof(*play), compat32,
            "IDirectSoundBuffer::GetCurrentPosition play")) ||
        (write && !dsound_guest_writable(
            write, sizeof(*write), compat32,
            "IDirectSoundBuffer::GetCurrentPosition write")))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (play)
        *play = dsound_reported_play_cursor(buffer);
    if (write)
        *write = dsound_safe_write_cursor(buffer);
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFormat(PVOID self, PVOID format, DWORD size,
                                    DWORD *written)
{
    dsound_trace("IDirectSoundBuffer::GetFormat");
    if (!format && !written)
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    DWORD copy = format && size < sizeof(WAVEFORMATEX_DS)
               ? size : sizeof(WAVEFORMATEX_DS);
    if ((written && !dsound_guest_writable(
            written, sizeof(*written), compat32,
            "IDirectSoundBuffer::GetFormat written")) ||
        (format && copy && !dsound_guest_writable(
            format, copy, compat32,
            "IDirectSoundBuffer::GetFormat output")))
        return DSERR_INVALIDPARAM;
    WAVEFORMATEX_DS wave;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (buffer)
        dsound_write_format(buffer, &wave);
    dsound_unlock_irqrestore(irq_flags);
    if (!buffer)
        return DSERR_INVALIDCALL;

    if (written)
        *written = sizeof(wave);
    if (format) {
        memcpy(format, &wave, copy);
    }
    return DS_OK;
}

static HRESULT WINAPI dsb_GetVolume(PVOID self, LONG *volume)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(volume, sizeof(*volume), compat32,
                               "IDirectSoundBuffer::GetVolume"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLVOLUME)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK)
        *volume = buffer->volume;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_GetPan(PVOID self, LONG *pan)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(pan, sizeof(*pan), compat32,
                               "IDirectSoundBuffer::GetPan"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLPAN)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK)
        *pan = buffer->pan;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_GetFrequency(PVOID self, DWORD *frequency)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(frequency, sizeof(*frequency), compat32,
                               "IDirectSoundBuffer::GetFrequency"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLFREQUENCY)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK)
        *frequency = buffer->playback_rate;
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_GetStatus(PVOID self, DWORD *status)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(status, sizeof(*status), compat32,
                               "IDirectSoundBuffer::GetStatus"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    *status = buffer->playing ? DSBSTATUS_PLAYING : 0;
    if (buffer->playing && buffer->looping)
        *status |= DSBSTATUS_LOOPING;
    *status |= DSBSTATUS_LOCSOFTWARE;
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsb_Initialize(PVOID self, PVOID direct_sound,
                                     PVOID description)
{
    (void)direct_sound;
    (void)description;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DSERR_ALREADYINITIALIZED : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsb_Lock(PVOID self, DWORD offset, DWORD bytes,
                               PVOID pointer1, DWORD *bytes1,
                               PVOID pointer2, DWORD *bytes2, DWORD flags)
{
    dsound_trace("IDirectSoundBuffer::Lock");
    if (!pointer1 || !bytes1 || (flags & ~(DSBLOCK_FROMWRITECURSOR |
                                           DSBLOCK_ENTIREBUFFER)))
        return DSERR_INVALIDPARAM;
    bool compat32 = dsound_compat_call(self);
    SIZE_T pointer_size = compat32 ? sizeof(uint32_t) : sizeof(PVOID);
    if (!dsound_guest_writable(pointer1, pointer_size, compat32,
                               "IDirectSoundBuffer::Lock pointer1") ||
        !dsound_guest_writable(bytes1, sizeof(*bytes1), compat32,
                               "IDirectSoundBuffer::Lock bytes1") ||
        (pointer2 && !dsound_guest_writable(
            pointer2, pointer_size, compat32,
            "IDirectSoundBuffer::Lock pointer2")) ||
        (bytes2 && !dsound_guest_writable(
            bytes2, sizeof(*bytes2), compat32,
            "IDirectSoundBuffer::Lock bytes2")))
        return DSERR_INVALIDPARAM;
    dsound_store_pointer(compat32, pointer1, NULL);
    *bytes1 = 0;
    if (pointer2)
        dsound_store_pointer(compat32, pointer2, NULL);
    if (bytes2)
        *bytes2 = 0;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer || buffer->primary || !buffer->allocation) {
        dsound_unlock_irqrestore(irq_flags);
        return buffer ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM;
    }
    if (buffer->locked) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }

    uint32_t size = buffer->allocation->bytes;
    if (flags & DSBLOCK_ENTIREBUFFER) {
        offset = 0;
        bytes = size;
    } else if (flags & DSBLOCK_FROMWRITECURSOR) {
        offset = dsound_safe_write_cursor(buffer);
    }
    if (bytes > size || offset >= size) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }

    DWORD first = size - offset;
    if (first > bytes)
        first = bytes;
    DWORD second = bytes - first;
    if (second && (!pointer2 || !bytes2)) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }

    PVOID first_pointer = (BYTE *)buffer->allocation->user_data + offset;
    PVOID second_pointer = second ? buffer->allocation->user_data : NULL;
    buffer->locked = true;
    buffer->lock_pointer1 = first_pointer;
    buffer->lock_pointer2 = second_pointer;
    buffer->lock_bytes1 = first;
    buffer->lock_bytes2 = second;
    dsound_unlock_irqrestore(irq_flags);

    dsound_store_pointer(compat32, pointer1, first_pointer);
    *bytes1 = first;
    if (pointer2)
        dsound_store_pointer(compat32, pointer2, second_pointer);
    if (bytes2)
        *bytes2 = second;
    return DS_OK;
}

static HRESULT WINAPI dsb_Play(PVOID self, DWORD reserved1, DWORD reserved2,
                               DWORD flags)
{
    dsound_trace("IDirectSoundBuffer::Play");
    if (reserved1 || reserved2 || (flags & ~DSBPLAY_LOOPING))
        return DSERR_INVALIDPARAM;
    if (!audio_output_is_ready())
        return DSERR_NODRIVER;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (buffer->primary) {
        dsound_unlock_irqrestore(irq_flags);
        return (flags & DSBPLAY_LOOPING) ? DS_OK : DSERR_INVALIDPARAM;
    }
    buffer->playing = true;
    buffer->looping = (flags & DSBPLAY_LOOPING) != 0;
    dsound_unlock_irqrestore(irq_flags);

    audio_register_result_t registration = dsound_refresh_mixer();
    if (registration != AUDIO_REGISTER_OK) {
        irq_flags = dsound_lock_irqsave();
        buffer = dsound_buffer_from_object_locked(self, NULL);
        if (buffer)
            buffer->playing = false;
        dsound_unlock_irqrestore(irq_flags);
        return dsound_registration_hresult(registration);
    }
    return DS_OK;
}

static HRESULT WINAPI dsb_SetCurrentPosition(PVOID self, DWORD position)
{
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer || buffer->primary || !buffer->allocation) {
        dsound_unlock_irqrestore(irq_flags);
        return buffer ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM;
    }
    if (position >= buffer->allocation->bytes) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }
    uint32_t frame = position / buffer->block_align;
    buffer->cursor.frame_q32 = (uint64_t)frame << 32;
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFormat(PVOID self, PVOID format_pointer)
{
    dsound_trace("IDirectSoundBuffer::SetFormat");
    bool compat32 = dsound_compat_call(self);
    WAVEFORMATEX_DS wave;
    if (!dsound_guest_readable(format_pointer, sizeof(wave), compat32,
                               "IDirectSoundBuffer::SetFormat"))
        return DSERR_INVALIDPARAM;
    memcpy(&wave, format_pointer, sizeof(wave));

    pcm_format_t format;
    uint16_t block_align;
    HRESULT result = dsound_parse_format(&wave, &format, &block_align);
    if (result != DS_OK)
        return result;

    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer || !buffer->primary) {
        result = buffer ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM;
    } else if (!buffer->device ||
               buffer->device->cooperative_level < DSSCL_PRIORITY) {
        result = DSERR_PRIOLEVELNEEDED;
    } else {
        buffer->format = format;
        buffer->original_rate = format.sample_rate;
        buffer->playback_rate = format.sample_rate;
        buffer->mix_rate = format.sample_rate;
        buffer->block_align = block_align;
        dsound_update_gains(buffer);
        result = DS_OK;
    }
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_SetVolume(PVOID self, LONG volume)
{
    if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLVOLUME)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK) {
        buffer->volume = volume;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_SetPan(PVOID self, LONG pan)
{
    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLPAN)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK) {
        buffer->pan = pan;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_SetFrequency(PVOID self, DWORD frequency)
{
    if (frequency != DSBFREQUENCY_ORIGINAL &&
        (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    HRESULT result = !buffer ? DSERR_INVALIDCALL
                   : !(buffer->flags & DSBCAPS_CTRLFREQUENCY)
                   ? DSERR_CONTROLUNAVAIL : DS_OK;
    if (result == DS_OK) {
        buffer->playback_rate = frequency ? frequency : buffer->original_rate;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return result;
}

static HRESULT WINAPI dsb_Stop(PVOID self)
{
    dsound_trace("IDirectSoundBuffer::Stop");
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    buffer->playing = false;
    buffer->looping = false;
    dsound_unlock_irqrestore(irq_flags);
    dsound_refresh_mixer();
    return DS_OK;
}

static HRESULT WINAPI dsb_Unlock(PVOID self, PVOID pointer1, DWORD bytes1,
                                 PVOID pointer2, DWORD bytes2)
{
    dsound_trace("IDirectSoundBuffer::Unlock");
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    if (!buffer || !buffer->locked) {
        dsound_unlock_irqrestore(irq_flags);
        return buffer ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM;
    }
    if (pointer1 != buffer->lock_pointer1 || pointer2 != buffer->lock_pointer2 ||
        bytes1 != buffer->lock_bytes1 || bytes2 != buffer->lock_bytes2) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }
    uint64_t end = (uint64_t)((BYTE *)pointer1 -
                              (BYTE *)buffer->allocation->user_data) +
                   bytes1 + bytes2;
#if DSOUND_DIAGNOSTICS
    uint32_t trace = __atomic_fetch_add(&dsound_unlock_trace_count, 1,
                                        __ATOMIC_RELAXED);
    uint32_t nonzero_user = 0;
    uint32_t nonzero_kernel = 0;
    uint32_t alias_mismatch = 0;
    uint32_t first_offset = (uint32_t)((BYTE *)pointer1 -
                                      (BYTE *)buffer->allocation->user_data);
    if (trace < 24U) {
        const BYTE *user_regions[2] = {
            (const BYTE *)pointer1, (const BYTE *)pointer2,
        };
        const BYTE *kernel_regions[2] = {
            buffer->allocation->kernel_data + first_offset,
            buffer->allocation->kernel_data,
        };
        DWORD region_bytes[2] = { bytes1, bytes2 };
        for (uint32_t region = 0; region < 2; region++) {
            if (!user_regions[region] || !region_bytes[region])
                continue;
            for (DWORD i = 0; i < region_bytes[region]; i++) {
                BYTE user_byte = user_regions[region][i];
                BYTE kernel_byte = kernel_regions[region][i];
                if (user_byte)
                    nonzero_user++;
                if (kernel_byte)
                    nonzero_kernel++;
                if (user_byte != kernel_byte)
                    alias_mismatch++;
            }
        }
    }
#endif
    buffer->write_cursor = (uint32_t)(end % buffer->allocation->bytes);
    buffer->locked = false;
    buffer->lock_pointer1 = NULL;
    buffer->lock_pointer2 = NULL;
    buffer->lock_bytes1 = 0;
    buffer->lock_bytes2 = 0;
    dsound_unlock_irqrestore(irq_flags);

#if DSOUND_DIAGNOSTICS
    if (trace < 24U) {
        serial_puts("[DSOUND-DATA] unlock offset=");
        serial_putdec(first_offset);
        serial_puts(" bytes=");
        serial_putdec((uint64_t)bytes1 + bytes2);
        serial_puts(" user_nonzero=");
        serial_putdec(nonzero_user);
        serial_puts(" kernel_nonzero=");
        serial_putdec(nonzero_kernel);
        serial_puts(" alias_mismatch=");
        serial_putdec(alias_mismatch);
        serial_puts(" mix_fills=");
        serial_putdec(__atomic_load_n(&dsound_mix_fill_count,
                                      __ATOMIC_RELAXED));
        serial_puts(" signal_fills=");
        serial_putdec(__atomic_load_n(&dsound_mix_signal_count,
                                      __ATOMIC_RELAXED));
        serial_puts("\n");
    }
#endif
    return DS_OK;
}

static HRESULT WINAPI dsb_Restore(PVOID self)
{
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(self, NULL);
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static DSOUND_BUFFER *dsound_listener_from_object_locked(PVOID object)
{
    DSOUND_PROCESS *process = NULL;
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(object,
                                                              &process);
    if (!buffer || !process || !buffer->primary ||
        !(buffer->flags & DSBCAPS_CTRL3D))
        return NULL;
    uint32_t slot = (uint32_t)(buffer - process->buffers);
    return process->listener_objects[slot] == object ? buffer : NULL;
}

static DSOUND_BUFFER *dsound_buffer3d_from_object_locked(PVOID object)
{
    DSOUND_PROCESS *process = NULL;
    DSOUND_BUFFER *buffer = dsound_buffer_from_object_locked(object,
                                                              &process);
    if (!buffer || !process || buffer->primary ||
        !(buffer->flags & DSBCAPS_CTRL3D))
        return NULL;
    uint32_t slot = (uint32_t)(buffer - process->buffers);
    return process->buffer3d_objects[slot] == object ? buffer : NULL;
}

static HRESULT WINAPI dsl_GetAllParameters(PVOID self,
                                            DS3DLISTENER_DS *listener)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_readable(listener, sizeof(listener->size), compat32,
                               "IDirectSound3DListener::GetAllParameters size") ||
        !dsound_guest_writable(listener, sizeof(*listener), compat32,
                               "IDirectSound3DListener::GetAllParameters") ||
        listener->size != sizeof(*listener))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *listener = buffer->device->listener;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetDistanceFactor(PVOID self, float *factor)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(factor, sizeof(*factor), compat32,
                               "IDirectSound3DListener::GetDistanceFactor"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *factor = buffer->device->listener.distance_factor;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetDopplerFactor(PVOID self, float *factor)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(factor, sizeof(*factor), compat32,
                               "IDirectSound3DListener::GetDopplerFactor"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *factor = buffer->device->listener.doppler_factor;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetOrientation(PVOID self, D3DVECTOR_DS *front,
                                         D3DVECTOR_DS *top)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(front, sizeof(*front), compat32,
                               "IDirectSound3DListener::GetOrientation front") ||
        !dsound_guest_writable(top, sizeof(*top), compat32,
                               "IDirectSound3DListener::GetOrientation top"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer) {
        *front = buffer->device->listener.orient_front;
        *top = buffer->device->listener.orient_top;
    }
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetPosition(PVOID self, D3DVECTOR_DS *position)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(position, sizeof(*position), compat32,
                               "IDirectSound3DListener::GetPosition"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *position = buffer->device->listener.position;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetRolloffFactor(PVOID self, float *factor)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(factor, sizeof(*factor), compat32,
                               "IDirectSound3DListener::GetRolloffFactor"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *factor = buffer->device->listener.rolloff_factor;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_GetVelocity(PVOID self, D3DVECTOR_DS *velocity)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(velocity, sizeof(*velocity), compat32,
                               "IDirectSound3DListener::GetVelocity"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (buffer)
        *velocity = buffer->device->listener.velocity;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI dsl_SetAllParameters(
    PVOID self, const DS3DLISTENER_DS *listener, DWORD apply)
{
    bool compat32 = dsound_compat_call(self);
    DS3DLISTENER_DS snapshot;
    if (!dsound_guest_readable(listener, sizeof(snapshot), compat32,
                               "IDirectSound3DListener::SetAllParameters"))
        return DSERR_INVALIDPARAM;
    memcpy(&snapshot, listener, sizeof(snapshot));
    if (!dsound_apply_valid(apply) || !dsound_listener_valid(&snapshot))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener = snapshot;
        device->listener_deferred = true;
    } else {
        device->listener = snapshot;
        device->deferred_listener = snapshot;
        device->listener_deferred = false;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetDistanceFactor(PVOID self, float factor,
                                             DWORD apply)
{
    if (!dsound_apply_valid(apply) || !dsound_float_finite(factor) ||
        factor <= 0.0f)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.distance_factor = factor;
        device->listener_deferred = true;
    } else {
        device->listener.distance_factor = factor;
        device->deferred_listener.distance_factor = factor;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetDopplerFactor(PVOID self, float factor,
                                            DWORD apply)
{
    if (!dsound_apply_valid(apply) || !dsound_float_finite(factor) ||
        factor < DS3D_MINDOPPLERFACTOR ||
        factor > DS3D_MAXDOPPLERFACTOR)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.doppler_factor = factor;
        device->listener_deferred = true;
    } else {
        device->listener.doppler_factor = factor;
        device->deferred_listener.doppler_factor = factor;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetOrientation(PVOID self, float front_x,
                                          float front_y, float front_z,
                                          float top_x, float top_y,
                                          float top_z, DWORD apply)
{
    D3DVECTOR_DS front = { front_x, front_y, front_z };
    D3DVECTOR_DS top = { top_x, top_y, top_z };
    D3DVECTOR_DS right = dsound_vector_cross(&top, &front);
    if (!dsound_apply_valid(apply) || !dsound_direction_valid(&front) ||
        !dsound_direction_valid(&top) ||
        !dsound_direction_valid(&right))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.orient_front = front;
        device->deferred_listener.orient_top = top;
        device->listener_deferred = true;
    } else {
        device->listener.orient_front = front;
        device->listener.orient_top = top;
        device->deferred_listener.orient_front = front;
        device->deferred_listener.orient_top = top;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetPosition(PVOID self, float x, float y, float z,
                                      DWORD apply)
{
    D3DVECTOR_DS position = { x, y, z };
    if (!dsound_apply_valid(apply) || !dsound_vector_finite(&position))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.position = position;
        device->listener_deferred = true;
    } else {
        device->listener.position = position;
        device->deferred_listener.position = position;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetRolloffFactor(PVOID self, float factor,
                                            DWORD apply)
{
    if (!dsound_apply_valid(apply) || !dsound_float_finite(factor) ||
        factor < DS3D_MINROLLOFFFACTOR || factor > DS3D_MAXROLLOFFFACTOR)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.rolloff_factor = factor;
        device->listener_deferred = true;
    } else {
        device->listener.rolloff_factor = factor;
        device->deferred_listener.rolloff_factor = factor;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_SetVelocity(PVOID self, float x, float y, float z,
                                      DWORD apply)
{
    D3DVECTOR_DS velocity = { x, y, z };
    if (!dsound_apply_valid(apply) || !dsound_vector_finite(&velocity))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_listener_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DSOUND_DEVICE *device = buffer->device;
    if (apply == DS3D_DEFERRED) {
        device->deferred_listener.velocity = velocity;
        device->listener_deferred = true;
    } else {
        device->listener.velocity = velocity;
        device->deferred_listener.velocity = velocity;
        dsound_refresh_device_gains_locked(device);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl_CommitDeferredSettings(PVOID self)
{
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *listener_buffer =
        dsound_listener_from_object_locked(self);
    if (!listener_buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }

    DSOUND_DEVICE *device = listener_buffer->device;
    if (device->listener_deferred) {
        device->listener = device->deferred_listener;
        device->listener_deferred = false;
    }
    for (uint32_t i = 0; i < DSOUND_PROCESS_SLOTS; i++) {
        DSOUND_PROCESS *process = &dsound_processes[i];
        if (!process->owner_pid)
            continue;
        for (uint32_t b = 0; b < DSOUND_BUFFERS_PER_PROCESS; b++) {
            DSOUND_BUFFER *buffer = &process->buffers[b];
            if (buffer->in_use && buffer->device == device &&
                buffer->spatial_deferred) {
                buffer->spatial = buffer->deferred_spatial;
                buffer->spatial_deferred = false;
            }
        }
    }
    dsound_refresh_device_gains_locked(device);
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_GetAllParameters(PVOID self,
                                              DS3DBUFFER_DS *parameters)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_readable(parameters, sizeof(parameters->size), compat32,
                               "IDirectSound3DBuffer::GetAllParameters size") ||
        !dsound_guest_writable(parameters, sizeof(*parameters), compat32,
                               "IDirectSound3DBuffer::GetAllParameters") ||
        parameters->size != sizeof(*parameters))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *parameters = buffer->spatial;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetConeAngles(PVOID self, DWORD *inside,
                                           DWORD *outside)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(inside, sizeof(*inside), compat32,
                               "IDirectSound3DBuffer::GetConeAngles inside") ||
        !dsound_guest_writable(outside, sizeof(*outside), compat32,
                               "IDirectSound3DBuffer::GetConeAngles outside"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer) {
        *inside = buffer->spatial.inside_cone_angle;
        *outside = buffer->spatial.outside_cone_angle;
    }
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetConeOrientation(PVOID self,
                                                D3DVECTOR_DS *orientation)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(
            orientation, sizeof(*orientation), compat32,
            "IDirectSound3DBuffer::GetConeOrientation"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *orientation = buffer->spatial.cone_orientation;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetConeOutsideVolume(PVOID self, LONG *volume)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(
            volume, sizeof(*volume), compat32,
            "IDirectSound3DBuffer::GetConeOutsideVolume"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *volume = buffer->spatial.cone_outside_volume;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetMaxDistance(PVOID self, float *distance)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(distance, sizeof(*distance), compat32,
                               "IDirectSound3DBuffer::GetMaxDistance"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *distance = buffer->spatial.max_distance;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetMinDistance(PVOID self, float *distance)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(distance, sizeof(*distance), compat32,
                               "IDirectSound3DBuffer::GetMinDistance"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *distance = buffer->spatial.min_distance;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetMode(PVOID self, DWORD *mode)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(mode, sizeof(*mode), compat32,
                               "IDirectSound3DBuffer::GetMode"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *mode = buffer->spatial.mode;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetPosition(PVOID self, D3DVECTOR_DS *position)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(position, sizeof(*position), compat32,
                               "IDirectSound3DBuffer::GetPosition"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *position = buffer->spatial.position;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_GetVelocity(PVOID self, D3DVECTOR_DS *velocity)
{
    bool compat32 = dsound_compat_call(self);
    if (!dsound_guest_writable(velocity, sizeof(*velocity), compat32,
                               "IDirectSound3DBuffer::GetVelocity"))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (buffer)
        *velocity = buffer->spatial.velocity;
    dsound_unlock_irqrestore(irq_flags);
    return buffer ? DS_OK : DSERR_INVALIDCALL;
}

static HRESULT WINAPI ds3db_SetAllParameters(
    PVOID self, const DS3DBUFFER_DS *parameters, DWORD apply)
{
    bool compat32 = dsound_compat_call(self);
    DS3DBUFFER_DS snapshot;
    if (!dsound_guest_readable(parameters, sizeof(snapshot), compat32,
                               "IDirectSound3DBuffer::SetAllParameters"))
        return DSERR_INVALIDPARAM;
    memcpy(&snapshot, parameters, sizeof(snapshot));
    if (!dsound_apply_valid(apply) || !dsound_buffer3d_valid(&snapshot))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial = snapshot;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial = snapshot;
        buffer->deferred_spatial = snapshot;
        buffer->spatial_deferred = false;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetConeAngles(PVOID self, DWORD inside,
                                           DWORD outside, DWORD apply)
{
    if (!dsound_apply_valid(apply) || inside > DS3D_MAXCONEANGLE ||
        outside < inside || outside > DS3D_MAXCONEANGLE)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.inside_cone_angle = inside;
        buffer->deferred_spatial.outside_cone_angle = outside;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.inside_cone_angle = inside;
        buffer->spatial.outside_cone_angle = outside;
        buffer->deferred_spatial.inside_cone_angle = inside;
        buffer->deferred_spatial.outside_cone_angle = outside;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetConeOrientation(PVOID self, float x, float y,
                                                float z, DWORD apply)
{
    D3DVECTOR_DS orientation = { x, y, z };
    if (!dsound_apply_valid(apply) ||
        !dsound_direction_valid(&orientation))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.cone_orientation = orientation;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.cone_orientation = orientation;
        buffer->deferred_spatial.cone_orientation = orientation;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetConeOutsideVolume(PVOID self, LONG volume,
                                                  DWORD apply)
{
    if (!dsound_apply_valid(apply) || volume < DSBVOLUME_MIN ||
        volume > DSBVOLUME_MAX)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.cone_outside_volume = volume;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.cone_outside_volume = volume;
        buffer->deferred_spatial.cone_outside_volume = volume;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetMaxDistance(PVOID self, float distance,
                                            DWORD apply)
{
    if (!dsound_apply_valid(apply) || !dsound_float_finite(distance) ||
        distance <= 0.0f)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DS3DBUFFER_DS *target = apply == DS3D_DEFERRED
                          ? &buffer->deferred_spatial : &buffer->spatial;
    if (distance < target->min_distance) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }
    target->max_distance = distance;
    if (apply == DS3D_DEFERRED) {
        buffer->spatial_deferred = true;
    } else {
        buffer->deferred_spatial.max_distance = distance;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetMinDistance(PVOID self, float distance,
                                            DWORD apply)
{
    if (!dsound_apply_valid(apply) || !dsound_float_finite(distance) ||
        distance <= 0.0f)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    DS3DBUFFER_DS *target = apply == DS3D_DEFERRED
                          ? &buffer->deferred_spatial : &buffer->spatial;
    if (distance > target->max_distance) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDPARAM;
    }
    target->min_distance = distance;
    if (apply == DS3D_DEFERRED) {
        buffer->spatial_deferred = true;
    } else {
        buffer->deferred_spatial.min_distance = distance;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetMode(PVOID self, DWORD mode, DWORD apply)
{
    if (!dsound_apply_valid(apply) || mode > DS3DMODE_DISABLE)
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.mode = mode;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.mode = mode;
        buffer->deferred_spatial.mode = mode;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetPosition(PVOID self, float x, float y, float z,
                                        DWORD apply)
{
    D3DVECTOR_DS position = { x, y, z };
    if (!dsound_apply_valid(apply) || !dsound_vector_finite(&position))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.position = position;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.position = position;
        buffer->deferred_spatial.position = position;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI ds3db_SetVelocity(PVOID self, float x, float y, float z,
                                        DWORD apply)
{
    D3DVECTOR_DS velocity = { x, y, z };
    if (!dsound_apply_valid(apply) || !dsound_vector_finite(&velocity))
        return DSERR_INVALIDPARAM;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_BUFFER *buffer = dsound_buffer3d_from_object_locked(self);
    if (!buffer) {
        dsound_unlock_irqrestore(irq_flags);
        return DSERR_INVALIDCALL;
    }
    if (apply == DS3D_DEFERRED) {
        buffer->deferred_spatial.velocity = velocity;
        buffer->spatial_deferred = true;
    } else {
        buffer->spatial.velocity = velocity;
        buffer->deferred_spatial.velocity = velocity;
        dsound_update_gains(buffer);
    }
    dsound_unlock_irqrestore(irq_flags);
    return DS_OK;
}

static HRESULT WINAPI dsl32_SetDistanceFactor(PVOID self, DWORD factor,
                                               DWORD apply)
{
    return dsl_SetDistanceFactor(self, dsound_float_from_bits(factor), apply);
}

static HRESULT WINAPI dsl32_SetDopplerFactor(PVOID self, DWORD factor,
                                              DWORD apply)
{
    return dsl_SetDopplerFactor(self, dsound_float_from_bits(factor), apply);
}

static HRESULT WINAPI dsl32_SetOrientation(
    PVOID self, DWORD front_x, DWORD front_y, DWORD front_z,
    DWORD top_x, DWORD top_y, DWORD top_z, DWORD apply)
{
    return dsl_SetOrientation(self,
        dsound_float_from_bits(front_x), dsound_float_from_bits(front_y),
        dsound_float_from_bits(front_z), dsound_float_from_bits(top_x),
        dsound_float_from_bits(top_y), dsound_float_from_bits(top_z), apply);
}

static HRESULT WINAPI dsl32_SetPosition(PVOID self, DWORD x, DWORD y,
                                        DWORD z, DWORD apply)
{
    return dsl_SetPosition(self, dsound_float_from_bits(x),
                           dsound_float_from_bits(y),
                           dsound_float_from_bits(z), apply);
}

static HRESULT WINAPI dsl32_SetRolloffFactor(PVOID self, DWORD factor,
                                              DWORD apply)
{
    return dsl_SetRolloffFactor(self, dsound_float_from_bits(factor), apply);
}

static HRESULT WINAPI dsl32_SetVelocity(PVOID self, DWORD x, DWORD y,
                                        DWORD z, DWORD apply)
{
    return dsl_SetVelocity(self, dsound_float_from_bits(x),
                           dsound_float_from_bits(y),
                           dsound_float_from_bits(z), apply);
}

static HRESULT WINAPI ds3db32_SetConeOrientation(PVOID self, DWORD x,
                                                  DWORD y, DWORD z,
                                                  DWORD apply)
{
    return ds3db_SetConeOrientation(self, dsound_float_from_bits(x),
                                    dsound_float_from_bits(y),
                                    dsound_float_from_bits(z), apply);
}

static HRESULT WINAPI ds3db32_SetMaxDistance(PVOID self, DWORD distance,
                                              DWORD apply)
{
    return ds3db_SetMaxDistance(self, dsound_float_from_bits(distance),
                                apply);
}

static HRESULT WINAPI ds3db32_SetMinDistance(PVOID self, DWORD distance,
                                              DWORD apply)
{
    return ds3db_SetMinDistance(self, dsound_float_from_bits(distance),
                                apply);
}

static HRESULT WINAPI ds3db32_SetPosition(PVOID self, DWORD x, DWORD y,
                                           DWORD z, DWORD apply)
{
    return ds3db_SetPosition(self, dsound_float_from_bits(x),
                             dsound_float_from_bits(y),
                             dsound_float_from_bits(z), apply);
}

static HRESULT WINAPI ds3db32_SetVelocity(PVOID self, DWORD x, DWORD y,
                                           DWORD z, DWORD apply)
{
    return ds3db_SetVelocity(self, dsound_float_from_bits(x),
                             dsound_float_from_bits(y),
                             dsound_float_from_bits(z), apply);
}

HRESULT WINAPI DirectSoundCreate(LPCGUID device, PVOID output, PVOID outer)
{
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = g_compat32_mode;
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;
    if (outer)
        return DSERR_NOAGGREGATION;
    if (device && !dsound_guest_readable(device, sizeof(*device), compat32,
                                         "DirectSoundCreate device"))
        return DSERR_INVALIDPARAM;
    if (!dsound_playback_device_supported(device)) {
        serial_puts("[DSOUND] DirectSoundCreate rejected unknown playback device\n");
        return DSERR_NODRIVER;
    }
    if (!audio_output_is_ready()) {
        serial_puts("[DSOUND] no playback device\n");
        return DSERR_NODRIVER;
    }

    DSOUND_PROCESS *process = dsound_get_or_create_process();
    if (!process)
        return DSERR_OUTOFMEMORY;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device_state =
        dsound_allocate_device_locked(process, true);
    PVOID object = device_state ? device_state->object : NULL;
    dsound_unlock_irqrestore(irq_flags);
    if (!device_state)
        return DSERR_OUTOFMEMORY;
    dsound_store_pointer(compat32, output, object);
    serial_puts("[DSOUND] DirectSoundCreate object=0x");
    serial_puthex((ULONG_PTR)object, compat32 ? 8 : 16);
    serial_puts("\n");
    return DS_OK;
}

static HRESULT dsound_com_activate(LPCGUID iid, PVOID outer, PVOID output)
{
    if (!output)
        return DSERR_INVALIDPARAM;
    bool compat32 = g_compat32_mode;
    if (!dsound_store_pointer(compat32, output, NULL))
        return DSERR_INVALIDPARAM;
    if (!iid || !dsound_guest_readable(iid, sizeof(*iid), compat32,
                                       "DirectSound COM activation IID") ||
        (!dsound_guid_equal(iid, &iid_iunknown) &&
                 !dsound_guid_equal(iid, &iid_idirectsound)))
        return DSERR_NOINTERFACE;
    if (outer)
        return DSERR_NOAGGREGATION;

    DSOUND_PROCESS *process = dsound_get_or_create_process();
    if (!process)
        return DSERR_OUTOFMEMORY;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_DEVICE *device = dsound_allocate_device_locked(process, false);
    PVOID object = device ? device->object : NULL;
    dsound_unlock_irqrestore(irq_flags);
    if (!device)
        return DSERR_OUTOFMEMORY;
    dsound_store_pointer(compat32, output, object);
    serial_puts("[DSOUND] COM object=0x");
    serial_puthex((ULONG_PTR)object, compat32 ? 8 : 16);
    serial_puts(" (pending Initialize)\n");
    return DS_OK;
}

typedef BOOL (WINAPI *DSENUMCALLBACKA)(LPGUID, PCSTR, PCSTR, PVOID);
typedef BOOL (WINAPI *DSENUMCALLBACKW)(LPGUID, PCWSTR, PCWSTR, PVOID);

HRESULT WINAPI DirectSoundEnumerateA(PVOID callback, PVOID context)
{
    if (!callback)
        return DSERR_INVALIDPARAM;
    if (!audio_output_is_ready())
        return DS_OK;

    static const char description64[] = "Primary Sound Driver";
    static const char module64[] = "dsound.dll";
    if (!g_compat32_mode) {
        ((DSENUMCALLBACKA)callback)(NULL, description64, module64, context);
        return DS_OK;
    }

    BYTE *strings = (BYTE *)VirtualAlloc(NULL, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!strings)
        return DSERR_OUTOFMEMORY;
    char *description = (char *)strings;
    char *module = (char *)(strings + 64);
    memcpy(description, description64, sizeof(description64));
    memcpy(module, module64, sizeof(module64));
    uint32_t args[4] = {
        0, (uint32_t)(ULONG_PTR)description, (uint32_t)(ULONG_PTR)module,
        (uint32_t)(ULONG_PTR)context,
    };
    compat32_callback_args((uint32_t)(ULONG_PTR)callback, 4, args);
    VirtualFree(strings, 0, MEM_RELEASE);
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateW(PVOID callback, PVOID context)
{
    if (!callback)
        return DSERR_INVALIDPARAM;
    if (!audio_output_is_ready())
        return DS_OK;

    static const WCHAR description64[] = {
        'P','r','i','m','a','r','y',' ','S','o','u','n','d',' ','D','r','i','v','e','r',0
    };
    static const WCHAR module64[] = {
        'd','s','o','u','n','d','.','d','l','l',0
    };
    if (!g_compat32_mode) {
        ((DSENUMCALLBACKW)callback)(NULL, description64, module64, context);
        return DS_OK;
    }

    BYTE *strings = (BYTE *)VirtualAlloc(NULL, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!strings)
        return DSERR_OUTOFMEMORY;
    WCHAR *description = (WCHAR *)(void *)strings;
    WCHAR *module = (WCHAR *)(void *)(strings + 128);
    memcpy(description, description64, sizeof(description64));
    memcpy(module, module64, sizeof(module64));
    uint32_t args[4] = {
        0, (uint32_t)(ULONG_PTR)description, (uint32_t)(ULONG_PTR)module,
        (uint32_t)(ULONG_PTR)context,
    };
    compat32_callback_args((uint32_t)(ULONG_PTR)callback, 4, args);
    VirtualFree(strings, 0, MEM_RELEASE);
    return DS_OK;
}

HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID device, PVOID output,
                                        PVOID outer)
{
    (void)device;
    if (!output)
        return DSERR_INVALIDPARAM;
    if (!dsound_store_pointer(g_compat32_mode, output, NULL))
        return DSERR_INVALIDPARAM;
    if (outer)
        return DSERR_NOAGGREGATION;
    return DSERR_NODRIVER;
}

HRESULT WINAPI DirectSoundCaptureEnumerateA(PVOID callback, PVOID context)
{
    (void)context;
    return callback ? DS_OK : DSERR_INVALIDPARAM;
}

HRESULT WINAPI DirectSoundCaptureEnumerateW(PVOID callback, PVOID context)
{
    return DirectSoundCaptureEnumerateA(callback, context);
}

void dsound_release_process(DWORD process_id)
{
    if (!process_id)
        return;

    bool removed = false;
    uint64_t irq_flags = dsound_lock_irqsave();
    DSOUND_PROCESS *process = dsound_find_process_locked(process_id);
    if (process) {
        for (uint32_t i = 0; i < DSOUND_BUFFERS_PER_PROCESS; i++) {
            process->buffers[i].playing = false;
            process->buffers[i].in_use = false;
            process->buffers[i].allocation = NULL;
            process->allocations[i].kernel_data = NULL;
            process->allocations[i].user_data = NULL;
            process->allocations[i].in_use = false;
        }
        memset(process, 0, sizeof(*process));
        removed = true;
    }
    dsound_unlock_irqrestore(irq_flags);
    if (removed)
        dsound_refresh_mixer();
}

int dsound_selftest(void)
{
    int failures = 0;
    WAVEFORMATEX_DS wave = {
        WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0,
    };
    pcm_format_t format;
    uint16_t block_align = 0;
    if (dsound_parse_format(&wave, &format, &block_align) != DS_OK ||
        block_align != 4 || format.sample_rate != 22050 ||
        format.channels != 2 || format.sample_format != PCM_SAMPLE_S16_LE)
        failures++;
    wave.nChannels = 3;
    if (dsound_parse_format(&wave, &format, &block_align) != DSERR_BADFORMAT)
        failures++;
    if (dsound_gain_q16(0) != PCM_GAIN_UNITY ||
        dsound_gain_q16(DSBVOLUME_MIN) != 0)
        failures++;
    if (dsound_float_from_bits(0x3F800000U) != 1.0f)
        failures++;

    DS3DLISTENER_DS listener;
    dsound_listener_defaults(&listener);
    if (!dsound_listener_valid(&listener) ||
        listener.orient_front.z != 1.0f ||
        listener.orient_top.y != 1.0f ||
        listener.distance_factor != DS3D_DEFAULTDISTANCEFACTOR ||
        listener.rolloff_factor != DS3D_DEFAULTROLLOFFFACTOR ||
        listener.doppler_factor != DS3D_DEFAULTDOPPLERFACTOR)
        failures++;
    listener.orient_top = listener.orient_front;
    if (dsound_listener_valid(&listener))
        failures++;

    DS3DBUFFER_DS spatial;
    dsound_buffer3d_defaults(&spatial);
    if (!dsound_buffer3d_valid(&spatial) ||
        spatial.inside_cone_angle != DS3D_DEFAULTCONEANGLE ||
        spatial.outside_cone_angle != DS3D_DEFAULTCONEANGLE ||
        spatial.min_distance != DS3D_DEFAULTMINDISTANCE ||
        spatial.max_distance != DS3D_DEFAULTMAXDISTANCE)
        failures++;
    spatial.max_distance = 0.5f;
    if (dsound_buffer3d_valid(&spatial))
        failures++;

    DSBUFFERDESC_VIEW request = {
        .size = 20,
        .flags = DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC,
        .buffer_bytes = 4096,
        .reserved = 0,
        .format = &wave,
    };
    DWORD actual_flags = 0;
    if (dsound_validate_buffer_request(&request, &actual_flags) != DS_OK ||
        !(actual_flags & DSBCAPS_LOCSOFTWARE) ||
        !(actual_flags & DSBCAPS_CTRLVOLUME) ||
        (actual_flags & (DSBCAPS_LOCHARDWARE | DSBCAPS_STATIC)))
        failures++;

    request.flags = DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;
    request.flags = DSBCAPS_LOCHARDWARE;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_CONTROLUNAVAIL)
        failures++;
    request.flags = DSBCAPS_CTRL3D;
    if (dsound_validate_buffer_request(&request, &actual_flags) != DS_OK ||
        !(actual_flags & DSBCAPS_CTRL3D) ||
        !(actual_flags & DSBCAPS_LOCSOFTWARE))
        failures++;
    request.flags = DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;
    request.flags = DSBCAPS_MUTE3DATMAXDISTANCE;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;
    request.flags = DSBCAPS_CTRL3D | DSBCAPS_MUTE3DATMAXDISTANCE;
    if (dsound_validate_buffer_request(&request, &actual_flags) != DS_OK)
        failures++;
    request.flags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D |
                    DSBCAPS_MUTE3DATMAXDISTANCE;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;
    request.flags = DSBCAPS_CTRLFX;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_DS8_REQUIRED)
        failures++;
    request.flags = 0x80000000U;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;
    request.flags = 0;
    request.reserved = 1;
    if (dsound_validate_buffer_request(&request, &actual_flags) !=
        DSERR_INVALIDPARAM)
        failures++;

    DWORD caps[24];
    dsound_fill_device_caps(caps);
    if ((caps[1] & (DSCAPS_EMULDRIVER |
                    DSCAPS_HARDWARE_SECONDARY_MASK)) || caps[4] != 1 ||
        caps[5] || caps[6] || caps[7] || caps[8] || caps[9] || caps[10])
        failures++;

    DSOUND_ALLOCATION allocation = { .bytes = 4000 };
    DSOUND_BUFFER cursor_buffer = {
        .flags = DSBCAPS_GETCURRENTPOSITION2,
        .allocation = &allocation,
        .playback_rate = 1000,
        .block_align = 4,
        .cursor = { .frame_q32 = 10ULL << 32 },
    };
    if (dsound_reported_play_cursor(&cursor_buffer) != 40 ||
        dsound_safe_write_cursor(&cursor_buffer) != 100)
        failures++;
    cursor_buffer.flags = 0;
    if (dsound_reported_play_cursor(&cursor_buffer) != 96)
        failures++;

    DSOUND_DEVICE spatial_device;
    memset(&spatial_device, 0, sizeof(spatial_device));
    dsound_listener_defaults(&spatial_device.listener);
    spatial_device.deferred_listener = spatial_device.listener;
    DSOUND_BUFFER spatial_buffer;
    memset(&spatial_buffer, 0, sizeof(spatial_buffer));
    spatial_buffer.flags = DSBCAPS_CTRL3D;
    spatial_buffer.device = &spatial_device;
    spatial_buffer.volume = DSBVOLUME_MAX;
    spatial_buffer.playback_rate = 1000;
    dsound_buffer3d_defaults(&spatial_buffer.spatial);
    spatial_buffer.deferred_spatial = spatial_buffer.spatial;
    spatial_buffer.spatial.position.x = 1.0f;
    dsound_update_gains(&spatial_buffer);
    if (spatial_buffer.left_gain_q16 != 0 ||
        spatial_buffer.right_gain_q16 != PCM_GAIN_UNITY ||
        spatial_buffer.mix_rate != 1000)
        failures++;
    spatial_device.listener.orient_front.x = 1.0f;
    spatial_device.listener.orient_front.z = 0.0f;
    spatial_buffer.spatial.mode = DS3DMODE_HEADRELATIVE;
    dsound_update_gains(&spatial_buffer);
    if (spatial_buffer.left_gain_q16 != 0 ||
        spatial_buffer.right_gain_q16 != PCM_GAIN_UNITY)
        failures++;
    dsound_listener_defaults(&spatial_device.listener);
    spatial_buffer.spatial.mode = DS3DMODE_NORMAL;
    spatial_buffer.spatial.position.x = 0.0f;
    spatial_buffer.spatial.position.z = 2.0f;
    dsound_update_gains(&spatial_buffer);
    if (spatial_buffer.left_gain_q16 < 32760U ||
        spatial_buffer.left_gain_q16 > 32776U ||
        spatial_buffer.right_gain_q16 != spatial_buffer.left_gain_q16)
        failures++;
    spatial_buffer.spatial.velocity.z = 100.0f;
    dsound_update_gains(&spatial_buffer);
    if (spatial_buffer.mix_rate >= spatial_buffer.playback_rate ||
        spatial_buffer.mix_rate < DSBFREQUENCY_MIN)
        failures++;
    spatial_buffer.spatial.mode = DS3DMODE_DISABLE;
    dsound_update_gains(&spatial_buffer);
    if (spatial_buffer.left_gain_q16 != PCM_GAIN_UNITY ||
        spatial_buffer.right_gain_q16 != PCM_GAIN_UNITY ||
        spatial_buffer.mix_rate != spatial_buffer.playback_rate)
        failures++;

    DSOUND_PROCESS process;
    memset(&process, 0, sizeof(process));
    process.device_objects[0] = (PVOID)(ULONG_PTR)0x1000U;
    process.device_objects[1] = (PVOID)(ULONG_PTR)0x1004U;
    DSOUND_DEVICE *first = dsound_allocate_device_locked(&process, false);
    DSOUND_DEVICE *second = dsound_allocate_device_locked(&process, false);
    if (!first || !second || first == second || first->object == second->object ||
        first->initialized || second->initialized ||
        first->listener.orient_front.z != 1.0f ||
        first->listener.orient_top.y != 1.0f)
        failures++;
    if (first) {
        first->initialized = true;
        first->refs = 0;
        first->live_buffers = 1;
        dsound_retire_device_locked(first);
        if (!first->in_use || !first->initialized)
            failures++;
        first->live_buffers = 0;
        dsound_retire_device_locked(first);
        if (first->in_use)
            failures++;
    }
    if (second && second->initialized)
        failures++;
    if (dsound_validate_cooperative_level(DSSCL_NORMAL) != DS_OK ||
        dsound_validate_cooperative_level(DSSCL_PRIORITY) != DS_OK ||
        dsound_validate_cooperative_level(DSSCL_EXCLUSIVE) != DS_OK ||
        dsound_validate_cooperative_level(DSSCL_WRITEPRIMARY) !=
            DSERR_UNSUPPORTED ||
        dsound_validate_cooperative_level(0) != DSERR_INVALIDPARAM)
        failures++;
    if (!dsound_valid_speaker_config(DSSPEAKER_STEREO) ||
        !dsound_valid_speaker_config(
            DSSPEAKER_STEREO | (DSSPEAKER_GEOMETRY_WIDE << 16)) ||
        dsound_valid_speaker_config(10U) ||
        dsound_valid_speaker_config(
            1U | (DSSPEAKER_GEOMETRY_WIDE << 16)) ||
        dsound_valid_speaker_config(0x80000000U))
        failures++;

    if (pcm_selftest())
        failures++;
    return failures;
}

typedef struct {
    const char *name;
    PVOID func;
    uint8_t argc;
    uint8_t cc;
} SHIM_EXPORT;

static const SHIM_EXPORT dsound_exports[] = {
    { "DirectSoundCreate", (PVOID)DirectSoundCreate, 3, CC_STDCALL },
    { "DirectSoundEnumerateA", (PVOID)DirectSoundEnumerateA, 2, CC_STDCALL },
    { "DirectSoundEnumerateW", (PVOID)DirectSoundEnumerateW, 2, CC_STDCALL },
    { "DirectSoundCaptureCreate", (PVOID)DirectSoundCaptureCreate, 3, CC_STDCALL },
    { "DirectSoundCaptureEnumerateA", (PVOID)DirectSoundCaptureEnumerateA, 2, CC_STDCALL },
    { "DirectSoundCaptureEnumerateW", (PVOID)DirectSoundCaptureEnumerateW, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL },
};

const WIN32_EXPORT *dsound_abi_table(int *count)
{
    *count = (int)(sizeof(dsound_exports) / sizeof(dsound_exports[0]));
    return (const WIN32_EXPORT *)dsound_exports;
}

static int dsound_strcmp(const char *left, const char *right)
{
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

PVOID dsound_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name)
        return NULL;
    for (uint32_t i = 0; dsound_exports[i].name; i++) {
        if (dsound_strcmp(func_name, dsound_exports[i].name) == 0)
            return dsound_exports[i].func;
    }
    return NULL;
}

PVOID dsound_shim_init(void)
{
#if DSOUND_DIAGNOSTICS
    __atomic_store_n(&dsound_trace_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dsound_unlock_trace_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dsound_mix_fill_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dsound_mix_signal_count, 0, __ATOMIC_RELEASE);
#endif
    HRESULT result = ole32_register_class(&clsid_directsound,
                                          dsound_com_activate);
    if (result != DS_OK) {
        serial_puts("[DSOUND] failed to register COM class: 0x");
        serial_puthex((uint32_t)result, 8);
        serial_puts("\n");
    }
    return (PVOID)dsound_exports;
}
