/* PE32 contract test for DirectSound 3D COM interfaces and float ABI. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef long LONG;
typedef long HRESULT;
typedef unsigned int UINT;
typedef void *PVOID;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define DS_OK                       ((HRESULT)0x00000000UL)
#define DSERR_NOINTERFACE           ((HRESULT)0x80004002UL)
#define DSERR_INVALIDPARAM          ((HRESULT)0x80070057UL)
#define DSBCAPS_PRIMARYBUFFER       0x00000001UL
#define DSBCAPS_LOCSOFTWARE         0x00000008UL
#define DSBCAPS_CTRL3D              0x00000010UL
#define DSBCAPS_CTRLFREQUENCY       0x00000020UL
#define DSBCAPS_CTRLVOLUME          0x00000080UL
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x00020000UL
#define DSBPLAY_LOOPING             0x00000001UL
#define DSBLOCK_ENTIREBUFFER        0x00000002UL
#define DSBSTATUS_PLAYING           0x00000001UL
#define DSBSTATUS_LOOPING           0x00000004UL
#define DSBSTATUS_LOCSOFTWARE       0x00000010UL
#define DSSCL_NORMAL                1UL
#define DSSCL_EXCLUSIVE             3UL
#define DS3DMODE_NORMAL             0UL
#define DS3D_IMMEDIATE              0UL
#define DS3D_DEFERRED               1UL
#define WAVE_FORMAT_PCM             1U

int _fltused = 0;

typedef struct {
    DWORD data1;
    WORD data2;
    WORD data3;
    BYTE data4[8];
} GUID;

typedef struct {
    WORD format_tag;
    WORD channels;
    DWORD samples_per_sec;
    DWORD avg_bytes_per_sec;
    WORD block_align;
    WORD bits_per_sample;
    WORD extra_size;
} WAVEFORMATEX;

typedef struct {
    DWORD size;
    DWORD flags;
    DWORD buffer_bytes;
    DWORD reserved;
    WAVEFORMATEX *format;
} DSBUFFERDESC;

typedef struct {
    DWORD size;
    DWORD flags;
    DWORD buffer_bytes;
    DWORD unlock_transfer_rate;
    DWORD play_cpu_overhead;
} DSBCAPS;

typedef struct {
    float x;
    float y;
    float z;
} D3DVECTOR;

typedef struct {
    DWORD size;
    D3DVECTOR position;
    D3DVECTOR velocity;
    DWORD inside_cone_angle;
    DWORD outside_cone_angle;
    D3DVECTOR cone_orientation;
    LONG cone_outside_volume;
    float min_distance;
    float max_distance;
    DWORD mode;
} DS3DBUFFER;

typedef struct {
    DWORD size;
    D3DVECTOR position;
    D3DVECTOR velocity;
    D3DVECTOR orient_front;
    D3DVECTOR orient_top;
    float distance_factor;
    float rolloff_factor;
    float doppler_factor;
} DS3DLISTENER;

typedef struct IDirectSound IDirectSound;
typedef struct IDirectSoundBuffer IDirectSoundBuffer;
typedef struct IDirectSound3DListener IDirectSound3DListener;
typedef struct IDirectSound3DBuffer IDirectSound3DBuffer;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(IDirectSound *, const GUID *, PVOID *);
    ULONG (WINAPI *AddRef)(IDirectSound *);
    ULONG (WINAPI *Release)(IDirectSound *);
    HRESULT (WINAPI *CreateSoundBuffer)(IDirectSound *, const DSBUFFERDESC *,
                                        IDirectSoundBuffer **, PVOID);
    HRESULT (WINAPI *GetCaps)(IDirectSound *, PVOID);
    HRESULT (WINAPI *DuplicateSoundBuffer)(IDirectSound *,
                                            IDirectSoundBuffer *,
                                            IDirectSoundBuffer **);
    HRESULT (WINAPI *SetCooperativeLevel)(IDirectSound *, PVOID, DWORD);
    HRESULT (WINAPI *Compact)(IDirectSound *);
    HRESULT (WINAPI *GetSpeakerConfig)(IDirectSound *, DWORD *);
    HRESULT (WINAPI *SetSpeakerConfig)(IDirectSound *, DWORD);
    HRESULT (WINAPI *Initialize)(IDirectSound *, const GUID *);
} IDirectSoundVtbl;

struct IDirectSound {
    IDirectSoundVtbl *vtable;
};

typedef struct {
    HRESULT (WINAPI *QueryInterface)(IDirectSoundBuffer *, const GUID *,
                                     PVOID *);
    ULONG (WINAPI *AddRef)(IDirectSoundBuffer *);
    ULONG (WINAPI *Release)(IDirectSoundBuffer *);
    HRESULT (WINAPI *GetCaps)(IDirectSoundBuffer *, PVOID);
    HRESULT (WINAPI *GetCurrentPosition)(IDirectSoundBuffer *, DWORD *,
                                         DWORD *);
    HRESULT (WINAPI *GetFormat)(IDirectSoundBuffer *, PVOID, DWORD, DWORD *);
    HRESULT (WINAPI *GetVolume)(IDirectSoundBuffer *, LONG *);
    HRESULT (WINAPI *GetPan)(IDirectSoundBuffer *, LONG *);
    HRESULT (WINAPI *GetFrequency)(IDirectSoundBuffer *, DWORD *);
    HRESULT (WINAPI *GetStatus)(IDirectSoundBuffer *, DWORD *);
    HRESULT (WINAPI *Initialize)(IDirectSoundBuffer *, IDirectSound *, PVOID);
    HRESULT (WINAPI *Lock)(IDirectSoundBuffer *, DWORD, DWORD, PVOID *,
                            DWORD *, PVOID *, DWORD *, DWORD);
    HRESULT (WINAPI *Play)(IDirectSoundBuffer *, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *SetCurrentPosition)(IDirectSoundBuffer *, DWORD);
    HRESULT (WINAPI *SetFormat)(IDirectSoundBuffer *, PVOID);
    HRESULT (WINAPI *SetVolume)(IDirectSoundBuffer *, LONG);
    HRESULT (WINAPI *SetPan)(IDirectSoundBuffer *, LONG);
    HRESULT (WINAPI *SetFrequency)(IDirectSoundBuffer *, DWORD);
    HRESULT (WINAPI *Stop)(IDirectSoundBuffer *);
    HRESULT (WINAPI *Unlock)(IDirectSoundBuffer *, PVOID, DWORD, PVOID,
                              DWORD);
    HRESULT (WINAPI *Restore)(IDirectSoundBuffer *);
} IDirectSoundBufferVtbl;

struct IDirectSoundBuffer {
    IDirectSoundBufferVtbl *vtable;
};

typedef struct {
    HRESULT (WINAPI *QueryInterface)(IDirectSound3DListener *, const GUID *,
                                     PVOID *);
    ULONG (WINAPI *AddRef)(IDirectSound3DListener *);
    ULONG (WINAPI *Release)(IDirectSound3DListener *);
    HRESULT (WINAPI *GetAllParameters)(IDirectSound3DListener *,
                                       DS3DLISTENER *);
    HRESULT (WINAPI *GetDistanceFactor)(IDirectSound3DListener *, float *);
    HRESULT (WINAPI *GetDopplerFactor)(IDirectSound3DListener *, float *);
    HRESULT (WINAPI *GetOrientation)(IDirectSound3DListener *, D3DVECTOR *,
                                      D3DVECTOR *);
    HRESULT (WINAPI *GetPosition)(IDirectSound3DListener *, D3DVECTOR *);
    HRESULT (WINAPI *GetRolloffFactor)(IDirectSound3DListener *, float *);
    HRESULT (WINAPI *GetVelocity)(IDirectSound3DListener *, D3DVECTOR *);
    HRESULT (WINAPI *SetAllParameters)(IDirectSound3DListener *,
                                       const DS3DLISTENER *, DWORD);
    HRESULT (WINAPI *SetDistanceFactor)(IDirectSound3DListener *, float,
                                        DWORD);
    HRESULT (WINAPI *SetDopplerFactor)(IDirectSound3DListener *, float,
                                       DWORD);
    HRESULT (WINAPI *SetOrientation)(IDirectSound3DListener *, float, float,
                                     float, float, float, float, DWORD);
    HRESULT (WINAPI *SetPosition)(IDirectSound3DListener *, float, float,
                                  float, DWORD);
    HRESULT (WINAPI *SetRolloffFactor)(IDirectSound3DListener *, float,
                                       DWORD);
    HRESULT (WINAPI *SetVelocity)(IDirectSound3DListener *, float, float,
                                  float, DWORD);
    HRESULT (WINAPI *CommitDeferredSettings)(IDirectSound3DListener *);
} IDirectSound3DListenerVtbl;

struct IDirectSound3DListener {
    IDirectSound3DListenerVtbl *vtable;
};

typedef struct {
    HRESULT (WINAPI *QueryInterface)(IDirectSound3DBuffer *, const GUID *,
                                     PVOID *);
    ULONG (WINAPI *AddRef)(IDirectSound3DBuffer *);
    ULONG (WINAPI *Release)(IDirectSound3DBuffer *);
    HRESULT (WINAPI *GetAllParameters)(IDirectSound3DBuffer *, DS3DBUFFER *);
    HRESULT (WINAPI *GetConeAngles)(IDirectSound3DBuffer *, DWORD *, DWORD *);
    HRESULT (WINAPI *GetConeOrientation)(IDirectSound3DBuffer *, D3DVECTOR *);
    HRESULT (WINAPI *GetConeOutsideVolume)(IDirectSound3DBuffer *, LONG *);
    HRESULT (WINAPI *GetMaxDistance)(IDirectSound3DBuffer *, float *);
    HRESULT (WINAPI *GetMinDistance)(IDirectSound3DBuffer *, float *);
    HRESULT (WINAPI *GetMode)(IDirectSound3DBuffer *, DWORD *);
    HRESULT (WINAPI *GetPosition)(IDirectSound3DBuffer *, D3DVECTOR *);
    HRESULT (WINAPI *GetVelocity)(IDirectSound3DBuffer *, D3DVECTOR *);
    HRESULT (WINAPI *SetAllParameters)(IDirectSound3DBuffer *,
                                       const DS3DBUFFER *, DWORD);
    HRESULT (WINAPI *SetConeAngles)(IDirectSound3DBuffer *, DWORD, DWORD,
                                    DWORD);
    HRESULT (WINAPI *SetConeOrientation)(IDirectSound3DBuffer *, float, float,
                                         float, DWORD);
    HRESULT (WINAPI *SetConeOutsideVolume)(IDirectSound3DBuffer *, LONG,
                                           DWORD);
    HRESULT (WINAPI *SetMaxDistance)(IDirectSound3DBuffer *, float, DWORD);
    HRESULT (WINAPI *SetMinDistance)(IDirectSound3DBuffer *, float, DWORD);
    HRESULT (WINAPI *SetMode)(IDirectSound3DBuffer *, DWORD, DWORD);
    HRESULT (WINAPI *SetPosition)(IDirectSound3DBuffer *, float, float, float,
                                  DWORD);
    HRESULT (WINAPI *SetVelocity)(IDirectSound3DBuffer *, float, float, float,
                                  DWORD);
} IDirectSound3DBufferVtbl;

struct IDirectSound3DBuffer {
    IDirectSound3DBufferVtbl *vtable;
};

static const GUID iid_iunknown = {
    0x00000000UL, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID iid_listener = {
    0x279AFA84UL, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID iid_buffer3d = {
    0x279AFA86UL, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT void WINAPI Sleep(DWORD milliseconds);
DLLIMPORT HRESULT WINAPI DirectSoundCreate(const GUID *device,
                                            IDirectSound **output,
                                            PVOID outer);

static void fail(UINT code)
{
    ExitProcess(code);
    for (;;) { }
}

void mainCRTStartup(void)
{
    IDirectSound *direct_sound = (IDirectSound *)0;
    IDirectSoundBuffer *primary = (IDirectSoundBuffer *)0;
    IDirectSoundBuffer *secondary = (IDirectSoundBuffer *)0;
    IDirectSoundBuffer *legacy = (IDirectSoundBuffer *)0;
    IDirectSound3DListener *listener = (IDirectSound3DListener *)0;
    IDirectSound3DBuffer *buffer3d = (IDirectSound3DBuffer *)0;
    PVOID identity = (PVOID)0;
    PVOID unsupported = (PVOID)1;

    if (DirectSoundCreate((const GUID *)0,
                          (IDirectSound **)(DWORD)0x1234U,
                          (PVOID)0) != DSERR_INVALIDPARAM)
        fail(22);
    if (DirectSoundCreate((const GUID *)0,
                          (IDirectSound **)(DWORD)0x00100000U,
                          (PVOID)0) != DSERR_INVALIDPARAM)
        fail(26);
    if (DirectSoundCreate((const GUID *)0, &direct_sound, (PVOID)0) != DS_OK ||
        !direct_sound)
        fail(1);
    if (direct_sound->vtable->SetCooperativeLevel(
            direct_sound, (PVOID)0, DSSCL_NORMAL) != DS_OK)
        fail(2);

    DSBUFFERDESC primary_desc = {
        sizeof(primary_desc), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D,
        0, 0, (WAVEFORMATEX *)0,
    };
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, (const DSBUFFERDESC *)(DWORD)0x1234U,
            &primary, (PVOID)0) != DSERR_INVALIDPARAM || primary)
        fail(23);
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, &primary_desc,
            (IDirectSoundBuffer **)(DWORD)0x1234U,
            (PVOID)0) != DSERR_INVALIDPARAM)
        fail(24);
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, &primary_desc, &primary, (PVOID)0) != DS_OK ||
        !primary)
        fail(3);
    if (primary->vtable->QueryInterface(
            primary, &iid_listener, (PVOID *)&listener) != DS_OK || !listener)
        fail(4);
    if (primary->vtable->QueryInterface(
            primary, &iid_buffer3d, &unsupported) != DSERR_NOINTERFACE ||
        unsupported)
        fail(5);
    if (listener->vtable->QueryInterface(
            listener, &iid_iunknown, &identity) != DS_OK ||
        identity != (PVOID)primary)
        fail(6);
    primary->vtable->Release((IDirectSoundBuffer *)identity);

    DS3DLISTENER listener_state = {0};
    listener_state.size = sizeof(listener_state);
    if (listener->vtable->GetAllParameters(listener, &listener_state) != DS_OK ||
        listener_state.orient_front.z != 1.0f ||
        listener_state.orient_top.y != 1.0f ||
        listener_state.distance_factor != 1.0f ||
        listener_state.rolloff_factor != 1.0f ||
        listener_state.doppler_factor != 1.0f)
        fail(7);
    if (listener->vtable->SetDistanceFactor(
            listener, 2.0f, DS3D_IMMEDIATE) != DS_OK ||
        listener->vtable->SetDopplerFactor(
            listener, 0.5f, DS3D_IMMEDIATE) != DS_OK ||
        listener->vtable->SetOrientation(
            listener, 0.0f, 0.0f, 1.0f,
            0.0f, 1.0f, 0.0f, DS3D_IMMEDIATE) != DS_OK ||
        listener->vtable->SetVelocity(
            listener, 0.0f, 0.0f, 3.0f, DS3D_IMMEDIATE) != DS_OK)
        fail(8);
    float factor = 0.0f;
    D3DVECTOR vector = {0};
    if (listener->vtable->GetDistanceFactor(listener, &factor) != DS_OK ||
        factor != 2.0f ||
        listener->vtable->GetDopplerFactor(listener, &factor) != DS_OK ||
        factor != 0.5f ||
        listener->vtable->GetVelocity(listener, &vector) != DS_OK ||
        vector.z != 3.0f)
        fail(9);
    if (listener->vtable->SetRolloffFactor(
            listener, 11.0f, DS3D_IMMEDIATE) != DSERR_INVALIDPARAM)
        fail(10);

    WAVEFORMATEX format = {
        WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0,
    };
    DSBUFFERDESC secondary_desc = {
        sizeof(secondary_desc),
        DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME |
            DSBCAPS_MUTE3DATMAXDISTANCE,
        4096, 0, &format,
    };
    DSBUFFERDESC invalid_secondary_desc = secondary_desc;
    invalid_secondary_desc.format = (WAVEFORMATEX *)(DWORD)0x1234U;
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, &invalid_secondary_desc, &secondary,
            (PVOID)0) != DSERR_INVALIDPARAM || secondary)
        fail(25);
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, &secondary_desc, &secondary, (PVOID)0) != DS_OK ||
        !secondary)
        fail(11);
    if (secondary->vtable->QueryInterface(
            secondary, &iid_buffer3d, (PVOID *)&buffer3d) != DS_OK ||
        !buffer3d)
        fail(12);
    unsupported = (PVOID)1;
    if (secondary->vtable->QueryInterface(
            secondary, &iid_listener, &unsupported) != DSERR_NOINTERFACE ||
        unsupported)
        fail(13);
    if (buffer3d->vtable->QueryInterface(
            buffer3d, &iid_iunknown, &identity) != DS_OK ||
        identity != (PVOID)secondary)
        fail(14);
    secondary->vtable->Release((IDirectSoundBuffer *)identity);

    DS3DBUFFER buffer_state = {0};
    buffer_state.size = sizeof(buffer_state);
    if (buffer3d->vtable->GetAllParameters(buffer3d, &buffer_state) != DS_OK ||
        buffer_state.inside_cone_angle != 360 ||
        buffer_state.outside_cone_angle != 360 ||
        buffer_state.cone_orientation.z != 1.0f ||
        buffer_state.min_distance != 1.0f ||
        buffer_state.max_distance != 1000000000.0f ||
        buffer_state.mode != DS3DMODE_NORMAL)
        fail(15);
    if (buffer3d->vtable->SetMaxDistance(
            buffer3d, 100.0f, DS3D_IMMEDIATE) != DS_OK ||
        buffer3d->vtable->SetMinDistance(
            buffer3d, 2.0f, DS3D_IMMEDIATE) != DS_OK ||
        buffer3d->vtable->SetConeAngles(
            buffer3d, 90, 180, DS3D_IMMEDIATE) != DS_OK ||
        buffer3d->vtable->SetConeOrientation(
            buffer3d, 0.0f, 0.0f, -1.0f, DS3D_IMMEDIATE) != DS_OK ||
        buffer3d->vtable->SetConeOutsideVolume(
            buffer3d, -600, DS3D_IMMEDIATE) != DS_OK ||
        buffer3d->vtable->SetVelocity(
            buffer3d, 0.0f, 0.0f, 5.0f, DS3D_IMMEDIATE) != DS_OK)
        fail(16);
    if (buffer3d->vtable->SetPosition(
            buffer3d, 4.0f, 0.0f, 8.0f, DS3D_DEFERRED) != DS_OK ||
        listener->vtable->SetPosition(
            listener, 1.0f, 2.0f, 3.0f, DS3D_DEFERRED) != DS_OK)
        fail(17);
    if (buffer3d->vtable->GetPosition(buffer3d, &vector) != DS_OK ||
        vector.x != 0.0f || vector.y != 0.0f || vector.z != 0.0f ||
        listener->vtable->GetPosition(listener, &vector) != DS_OK ||
        vector.x != 0.0f || vector.y != 0.0f || vector.z != 0.0f)
        fail(18);
    if (listener->vtable->CommitDeferredSettings(listener) != DS_OK)
        fail(19);
    if (buffer3d->vtable->GetPosition(buffer3d, &vector) != DS_OK ||
        vector.x != 4.0f || vector.y != 0.0f || vector.z != 8.0f ||
        listener->vtable->GetPosition(listener, &vector) != DS_OK ||
        vector.x != 1.0f || vector.y != 2.0f || vector.z != 3.0f)
        fail(20);
    buffer_state.size = sizeof(buffer_state);
    if (buffer3d->vtable->GetAllParameters(buffer3d, &buffer_state) != DS_OK ||
        buffer_state.inside_cone_angle != 90 ||
        buffer_state.outside_cone_angle != 180 ||
        buffer_state.cone_orientation.z != -1.0f ||
        buffer_state.cone_outside_volume != -600 ||
        buffer_state.min_distance != 2.0f ||
        buffer_state.max_distance != 100.0f ||
        buffer_state.velocity.z != 5.0f)
        fail(21);

    if (direct_sound->vtable->SetCooperativeLevel(
            direct_sound, (PVOID)0, DSSCL_EXCLUSIVE) != DS_OK)
        fail(27);
    WAVEFORMATEX legacy_format = {
        WAVE_FORMAT_PCM, 2, 11025, 44100, 4, 16, 0,
    };
    if (primary->vtable->SetFormat(primary, &legacy_format) != DS_OK)
        fail(28);

    DSBUFFERDESC legacy_desc = {
        sizeof(legacy_desc),
        DSBCAPS_CTRLFREQUENCY | DSBCAPS_LOCSOFTWARE,
        16384, 0, &legacy_format,
    };
    if (direct_sound->vtable->CreateSoundBuffer(
            direct_sound, &legacy_desc, &legacy, (PVOID)0) != DS_OK ||
        !legacy)
        fail(29);

    DSBCAPS legacy_caps = {0};
    legacy_caps.size = sizeof(legacy_caps);
    if (legacy->vtable->GetCaps(legacy, &legacy_caps) != DS_OK ||
        legacy_caps.buffer_bytes != legacy_desc.buffer_bytes ||
        !(legacy_caps.flags & DSBCAPS_CTRLFREQUENCY) ||
        !(legacy_caps.flags & DSBCAPS_LOCSOFTWARE))
        fail(30);

    PVOID first = (PVOID)0;
    PVOID second_part = (PVOID)0;
    DWORD first_bytes = 0;
    DWORD second_bytes = 0;
    if (legacy->vtable->Lock(
            legacy, 0, 0, &first, &first_bytes, &second_part,
            &second_bytes, DSBLOCK_ENTIREBUFFER) != DS_OK ||
        !first || first_bytes != legacy_desc.buffer_bytes ||
        second_part || second_bytes)
        fail(31);
    for (DWORD i = 0; i < first_bytes; i++)
        ((BYTE *)first)[i] = (i & 32U) ? 0x60U : 0xA0U;
    if (legacy->vtable->Unlock(
            legacy, first, first_bytes, second_part, second_bytes) != DS_OK)
        fail(32);

    DWORD frequency = 0;
    if (legacy->vtable->GetFrequency(legacy, &frequency) != DS_OK ||
        frequency != legacy_format.samples_per_sec ||
        legacy->vtable->SetFrequency(legacy, 22050) != DS_OK ||
        legacy->vtable->GetFrequency(legacy, &frequency) != DS_OK ||
        frequency != 22050)
        fail(33);
    if (legacy->vtable->Play(legacy, 0, 0, DSBPLAY_LOOPING) != DS_OK)
        fail(34);
    Sleep(180);

    DWORD play_cursor = 0;
    DWORD write_cursor = 0;
    DWORD status = 0;
    if (legacy->vtable->GetCurrentPosition(
            legacy, &play_cursor, &write_cursor) != DS_OK ||
        play_cursor >= legacy_desc.buffer_bytes ||
        write_cursor >= legacy_desc.buffer_bytes ||
        (!play_cursor && !write_cursor) ||
        legacy->vtable->GetStatus(legacy, &status) != DS_OK ||
        (status & (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING |
                   DSBSTATUS_LOCSOFTWARE)) !=
            (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING |
             DSBSTATUS_LOCSOFTWARE))
        fail(35);
    if (legacy->vtable->Stop(legacy) != DS_OK ||
        legacy->vtable->GetStatus(legacy, &status) != DS_OK ||
        (status & (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING)) ||
        !(status & DSBSTATUS_LOCSOFTWARE))
        fail(36);
    if (legacy->vtable->SetCurrentPosition(legacy, 0) != DS_OK ||
        legacy->vtable->GetCurrentPosition(
            legacy, &play_cursor, &write_cursor) != DS_OK ||
        (play_cursor + legacy_format.block_align) %
                legacy_desc.buffer_bytes != write_cursor)
        fail(37);

    legacy->vtable->Release(legacy);
    buffer3d->vtable->Release(buffer3d);
    secondary->vtable->Release(secondary);
    listener->vtable->Release(listener);
    primary->vtable->Release(primary);
    direct_sound->vtable->Release(direct_sound);
    ExitProcess(0);
}
