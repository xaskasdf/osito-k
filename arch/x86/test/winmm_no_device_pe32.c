/* PE32 contract test for WinMM behavior when no output device is present. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef unsigned long ULONG;
typedef long HRESULT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;
typedef PVOID HWAVEOUT;
typedef PVOID HWAVEIN;
typedef PVOID HMIXER;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define WAVE_MAPPER ((UINT)-1)
#define WAVE_FORMAT_PCM 1U
#define WAVE_FORMAT_QUERY 0x0001U
#define MMSYSERR_NOERROR 0U
#define MMSYSERR_NODRIVER 6U
#define DSERR_NODRIVER ((HRESULT)0x88780078UL)
#define DSERR_UNINITIALIZED ((HRESULT)0x887800AAUL)
#define DSBCAPS_PRIMARYBUFFER 0x00000001U
#define DSSCL_NORMAL 1U
#define DSSPEAKER_STEREO 4U
#define S_OK 0L
#define S_FALSE 1L
#define CLSCTX_INPROC_SERVER 0x00000001U
#define COINIT_APARTMENTTHREADED 0x00000002U
#define SND_NODEFAULT 0x0002U
#define SND_MEMORY    0x0004U
#define GENERIC_WRITE 0x40000000U
#define CREATE_ALWAYS 2U
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define MCIERR_DEVICE_NOT_READY 276U
#define MCI_OPEN  0x0803U
#define MCI_CLOSE 0x0804U
#define MCI_PLAY  0x0806U
#define MCI_WAIT  0x00000002U
#define MCI_OPEN_ELEMENT 0x00000200U
#define MCI_OPEN_TYPE    0x00002000U

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
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    DWORD formats;
    WORD channels;
    WORD reserved;
    DWORD support;
} WAVEOUTCAPSA;

typedef struct {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    WORD technology;
    WORD reserved;
    DWORD support;
} AUXCAPSA;

typedef struct {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    DWORD support;
    DWORD destinations;
} MIXERCAPSA;

typedef struct {
    DWORD callback;
    UINT device_id;
    const char *device_type;
    const char *element_name;
    const char *alias;
} MCI_OPEN_PARMSA;

typedef struct {
    DWORD data1;
    WORD data2;
    WORD data3;
    BYTE data4[8];
} GUID;

typedef struct IDirectSound IDirectSound;

typedef struct {
    DWORD size;
    DWORD flags;
    DWORD buffer_bytes;
    DWORD reserved;
    WAVEFORMATEX *format;
    GUID algorithm;
} DSBUFFERDESC;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(IDirectSound *, const GUID *, PVOID *);
    ULONG (WINAPI *AddRef)(IDirectSound *);
    ULONG (WINAPI *Release)(IDirectSound *);
    HRESULT (WINAPI *CreateSoundBuffer)(IDirectSound *, PVOID, PVOID *, PVOID);
    HRESULT (WINAPI *GetCaps)(IDirectSound *, PVOID);
    HRESULT (WINAPI *DuplicateSoundBuffer)(IDirectSound *, PVOID, PVOID *);
    HRESULT (WINAPI *SetCooperativeLevel)(IDirectSound *, PVOID, DWORD);
    HRESULT (WINAPI *Compact)(IDirectSound *);
    HRESULT (WINAPI *GetSpeakerConfig)(IDirectSound *, DWORD *);
    HRESULT (WINAPI *SetSpeakerConfig)(IDirectSound *, DWORD);
    HRESULT (WINAPI *Initialize)(IDirectSound *, const GUID *);
} IDirectSoundVtbl;

struct IDirectSound {
    IDirectSoundVtbl *vtable;
};

static const GUID clsid_directsound = {
    0x47D4D946UL, 0x62E8, 0x11CF,
    { 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};

static const GUID iid_idirectsound = {
    0x279AFA83UL, 0x4981, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access,
                                    DWORD share, PVOID security,
                                    DWORD creation, DWORD attributes,
                                    HANDLE template_file);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD bytes,
                                DWORD *written, PVOID overlapped);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT BOOL WINAPI PlaySoundA(const char *sound, PVOID module,
                                 DWORD flags);
DLLIMPORT DWORD WINAPI mciSendCommandA(UINT device, UINT message,
                                       DWORD flags, DWORD parameter);
DLLIMPORT HRESULT WINAPI DirectSoundCreate(PVOID device, PVOID *output,
                                           PVOID outer);
DLLIMPORT HRESULT WINAPI CoInitializeEx(PVOID reserved, DWORD model);
DLLIMPORT void WINAPI CoUninitialize(void);
DLLIMPORT HRESULT WINAPI CoCreateInstance(const GUID *class_id, PVOID outer,
                                          DWORD context, const GUID *iid,
                                          PVOID *output);
DLLIMPORT UINT WINAPI waveOutGetNumDevs(void);
DLLIMPORT UINT WINAPI waveOutGetDevCapsA(UINT device,
                                        WAVEOUTCAPSA *capabilities,
                                        UINT size);
DLLIMPORT UINT WINAPI waveOutGetErrorTextA(UINT error, char *text,
                                           UINT chars);
DLLIMPORT UINT WINAPI waveOutOpen(HWAVEOUT *output, UINT device,
                                  const WAVEFORMATEX *format,
                                  DWORD callback, DWORD instance,
                                  DWORD flags);
DLLIMPORT UINT WINAPI waveInGetNumDevs(void);
DLLIMPORT UINT WINAPI waveInGetDevCapsA(UINT device, PVOID capabilities,
                                       UINT size);
DLLIMPORT UINT WINAPI waveInOpen(HWAVEIN *input, UINT device,
                                const WAVEFORMATEX *format,
                                DWORD callback, DWORD instance, DWORD flags);
DLLIMPORT UINT WINAPI auxGetNumDevs(void);
DLLIMPORT UINT WINAPI auxGetDevCapsA(UINT device, AUXCAPSA *capabilities,
                                    UINT size);
DLLIMPORT UINT WINAPI auxSetVolume(UINT device, DWORD volume);
DLLIMPORT UINT WINAPI mixerGetNumDevs(void);
DLLIMPORT UINT WINAPI mixerGetDevCapsA(UINT device,
                                      MIXERCAPSA *capabilities, UINT size);
DLLIMPORT UINT WINAPI mixerOpen(HMIXER *mixer, UINT device, DWORD callback,
                               DWORD instance, DWORD flags);

static void fail(UINT code)
{
    ExitProcess(code);
    for (;;) { }
}

static void put_le16(BYTE *destination, WORD value)
{
    destination[0] = (BYTE)value;
    destination[1] = (BYTE)(value >> 8);
}

static void put_le32(BYTE *destination, DWORD value)
{
    destination[0] = (BYTE)value;
    destination[1] = (BYTE)(value >> 8);
    destination[2] = (BYTE)(value >> 16);
    destination[3] = (BYTE)(value >> 24);
}

static void prepare_mci_wave(void)
{
    BYTE wave[44 + 220];
    const char riff[] = "RIFF";
    const char wave_id[] = "WAVEfmt ";
    const char data_id[] = "data";
    for (UINT i = 0; i < 4U; i++) {
        wave[i] = (BYTE)riff[i];
        wave[36U + i] = (BYTE)data_id[i];
    }
    for (UINT i = 0; i < 8U; i++)
        wave[8U + i] = (BYTE)wave_id[i];
    put_le32(wave + 4, sizeof(wave) - 8U);
    put_le32(wave + 16, 16U);
    put_le16(wave + 20, WAVE_FORMAT_PCM);
    put_le16(wave + 22, 1U);
    put_le32(wave + 24, 22050U);
    put_le32(wave + 28, 22050U);
    put_le16(wave + 32, 1U);
    put_le16(wave + 34, 8U);
    put_le32(wave + 40, 220U);
    for (UINT i = 44U; i < sizeof(wave); i++)
        wave[i] = (i & 8U) ? 224U : 32U;

    HANDLE file = CreateFileA("mci-no-device.wav", GENERIC_WRITE, 0, 0,
                              CREATE_ALWAYS, 0, 0);
    DWORD written = 0;
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, wave, sizeof(wave), &written, 0) ||
        written != sizeof(wave) || !CloseHandle(file))
        fail(10);
}

void mainCRTStartup(void)
{
    WAVEOUTCAPSA capabilities = {0};
    WAVEFORMATEX format = {
        WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0,
    };
    HWAVEOUT output = (HWAVEOUT)1;
    HWAVEIN input = (HWAVEIN)1;
    HMIXER mixer = (HMIXER)1;
    PVOID direct_sound = (PVOID)1;
    IDirectSound *com_direct_sound = (IDirectSound *)0;
    PVOID sound_buffer = (PVOID)1;
    DWORD direct_sound_caps[24] = {0};
    DWORD speaker_config = 0;
    DSBUFFERDESC primary_buffer = {
        sizeof(DSBUFFERDESC), DSBCAPS_PRIMARYBUFFER, 0, 0,
        (WAVEFORMATEX *)0, {0},
    };
    AUXCAPSA aux_capabilities = {0};
    MIXERCAPSA mixer_capabilities = {0};
    char error_text[64] = {0};
    MCI_OPEN_PARMSA mci_open = {0};

    prepare_mci_wave();

    if (!PlaySoundA((const char *)0, (PVOID)0, 0) ||
        PlaySoundA((const char *)&format, (PVOID)0,
                   SND_MEMORY | SND_NODEFAULT))
        fail(9);
    if (waveOutGetNumDevs() != 0U)
        fail(1);
    if (waveOutGetDevCapsA(0, &capabilities, sizeof(capabilities)) !=
        MMSYSERR_NODRIVER)
        fail(2);
    if (waveOutOpen(&output, WAVE_MAPPER, &format, 0, 0,
                    WAVE_FORMAT_QUERY) != MMSYSERR_NODRIVER || output)
        fail(3);
    if (waveOutGetErrorTextA(MMSYSERR_NODRIVER, error_text,
                             sizeof(error_text)) != MMSYSERR_NOERROR ||
        error_text[0] != 'N')
        fail(4);
    if (DirectSoundCreate((PVOID)0, &direct_sound, (PVOID)0) !=
            DSERR_NODRIVER || direct_sound)
        fail(5);
    HRESULT com_result = CoInitializeEx((PVOID)0, COINIT_APARTMENTTHREADED);
    if (com_result != S_OK && com_result != S_FALSE)
        fail(14);
    if (CoCreateInstance(&clsid_directsound, (PVOID)0,
                         CLSCTX_INPROC_SERVER, &iid_idirectsound,
                         (PVOID *)&com_direct_sound) != S_OK ||
        !com_direct_sound)
        fail(15);
    direct_sound_caps[0] = sizeof(direct_sound_caps);
    if (com_direct_sound->vtable->CreateSoundBuffer(
            com_direct_sound, &primary_buffer, &sound_buffer, (PVOID)0) !=
            DSERR_UNINITIALIZED || sound_buffer)
        fail(18);
    if (com_direct_sound->vtable->GetCaps(
            com_direct_sound, direct_sound_caps) != DSERR_UNINITIALIZED)
        fail(19);
    if (com_direct_sound->vtable->SetCooperativeLevel(
            com_direct_sound, (PVOID)1, DSSCL_NORMAL) !=
            DSERR_UNINITIALIZED)
        fail(20);
    if (com_direct_sound->vtable->Compact(com_direct_sound) !=
            DSERR_UNINITIALIZED)
        fail(21);
    if (com_direct_sound->vtable->GetSpeakerConfig(
            com_direct_sound, &speaker_config) != DSERR_UNINITIALIZED)
        fail(22);
    if (com_direct_sound->vtable->SetSpeakerConfig(
            com_direct_sound, DSSPEAKER_STEREO) != DSERR_UNINITIALIZED)
        fail(23);
    if (com_direct_sound->vtable->Initialize(
            com_direct_sound, (const GUID *)0) != DSERR_NODRIVER)
        fail(16);
    if (com_direct_sound->vtable->Compact(com_direct_sound) !=
            DSERR_UNINITIALIZED)
        fail(24);
    if (com_direct_sound->vtable->Release(com_direct_sound) != 0U)
        fail(17);
    CoUninitialize();
    if (waveInGetNumDevs() != 0U ||
        waveInGetDevCapsA(0, &capabilities, sizeof(capabilities)) !=
            MMSYSERR_NODRIVER ||
        waveInOpen(&input, WAVE_MAPPER, &format, 0, 0,
                   WAVE_FORMAT_QUERY) != MMSYSERR_NODRIVER || input)
        fail(6);
    if (auxGetNumDevs() != 0U ||
        auxGetDevCapsA(0, &aux_capabilities, sizeof(aux_capabilities)) !=
            MMSYSERR_NODRIVER ||
        auxSetVolume(0, 0xFFFFFFFFU) != MMSYSERR_NODRIVER)
        fail(7);
    if (mixerGetNumDevs() != 0U ||
        mixerGetDevCapsA(0, &mixer_capabilities,
                         sizeof(mixer_capabilities)) != MMSYSERR_NODRIVER ||
        mixerOpen(&mixer, 0, 0, 0, 0) != MMSYSERR_NODRIVER || mixer)
        fail(8);

    mci_open.device_type = "waveaudio";
    mci_open.element_name = "mci-no-device.wav";
    if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT,
                        (DWORD)&mci_open) != 0 || !mci_open.device_id)
        fail(11);
    if (mciSendCommandA(mci_open.device_id, MCI_PLAY, MCI_WAIT, 0) !=
            MCIERR_DEVICE_NOT_READY)
        fail(12);
    if (mciSendCommandA(mci_open.device_id, MCI_CLOSE, MCI_WAIT, 0) != 0)
        fail(13);

    ExitProcess(0);
}
