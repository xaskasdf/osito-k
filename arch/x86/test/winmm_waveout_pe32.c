/*
 * Freestanding PE32 integration test for the WinMM waveOut contract.
 * Build with clang/lld-link; the guest reports the failing stage as its
 * process exit code. A successful run also leaves audible PCM in QEMU's WAV
 * capture, proving that callbacks and the HDA path advanced the buffer.
 */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;
typedef PVOID HKEY;
typedef PVOID HWAVEOUT;
typedef PVOID HMIXER;
typedef unsigned long DWORD_PTR;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define MMSYSERR_NOERROR       0U
#define MMSYSERR_INVALHANDLE    5U
#define MMSYSERR_BADERRNUM      9U
#define MMSYSERR_INVALPARAM     11U
#define WAVERR_BADFORMAT      32U
#define MCIERR_MISSING_COMMAND_STRING 267U
#define MCIERR_UNSUPPORTED_FUNCTION  274U
#define MCIERR_FILE_NOT_FOUND         275U
#define MCIERR_INVALID_DEVICE_ID      257U
#define MCIERR_DEVICE_NOT_INSTALLED   306U
#define MCIERR_DUPLICATE_ALIAS        289U
#define MCIERR_INVALID_FILE           296U
#define MCIERR_NULL_PARAMETER_BLOCK   297U
#define MCI_OPEN                    0x0803U
#define MCI_CLOSE                   0x0804U
#define MCI_PLAY                    0x0806U
#define MCI_SEEK                    0x0807U
#define MCI_STOP                    0x0808U
#define MCI_PAUSE                   0x0809U
#define MCI_GETDEVCAPS              0x080BU
#define MCI_SET                     0x080DU
#define MCI_STATUS                  0x0814U
#define MCI_RESUME                  0x0855U
#define MCI_NOTIFY                  0x00000001U
#define MCI_WAIT                    0x00000002U
#define MCI_FROM                    0x00000004U
#define MCI_TO                      0x00000008U
#define MCI_OPEN_ELEMENT            0x00000200U
#define MCI_OPEN_ALIAS              0x00000400U
#define MCI_OPEN_TYPE               0x00002000U
#define MCI_SEEK_TO_START           0x00000100U
#define MCI_STATUS_ITEM             0x00000100U
#define MCI_STATUS_LENGTH           1U
#define MCI_STATUS_POSITION         2U
#define MCI_STATUS_MODE             4U
#define MCI_MODE_STOP               525U
#define MCI_MODE_PAUSE              529U
#define MCI_GETDEVCAPS_ITEM         0x00000100U
#define MCI_GETDEVCAPS_CAN_PLAY     8U
#define MCI_SET_TIME_FORMAT         0x00000400U
#define MCI_FORMAT_SAMPLES          9U
#define WAVE_MAPPER   0xFFFFFFFFU
#define WAVE_FORMAT_QUERY      1U
#define CALLBACK_FUNCTION 0x00030000U
#define WOM_OPEN          0x03BBU
#define WOM_CLOSE         0x03BCU
#define WOM_DONE          0x03BDU
#define WHDR_DONE         0x00000001U
#define WHDR_PREPARED     0x00000002U
#define TIME_BYTES        0x0004U
#define AUXCAPS_VOLUME    0x0001U
#define AUXCAPS_LRVOLUME  0x0002U
#define MIXER_OBJECTF_HMIXER              0x80000000U
#define MIXER_GETLINEINFOF_DESTINATION    0x00000000U
#define MIXER_GETLINECONTROLSF_ONEBYTYPE  0x00000002U
#define MIXERLINE_COMPONENTTYPE_DST_SPEAKERS 0x00000004U
#define MIXERCONTROL_CONTROLTYPE_VOLUME   0x50030001U
#define SND_ASYNC                         0x00000001U
#define SND_NODEFAULT                     0x00000002U
#define SND_MEMORY                        0x00000004U
#define SND_LOOP                          0x00000008U
#define SND_NOSTOP                        0x00000010U
#define SND_APPLICATION                   0x00000080U
#define SND_ALIAS                         0x00010000U
#define SND_ALIAS_ID                      0x00110000U
#define SND_FILENAME                      0x00020000U
#define GENERIC_WRITE                     0x40000000U
#define CREATE_ALWAYS                     2U
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define HKEY_CURRENT_USER ((HKEY)(DWORD_PTR)0x80000001U)
#define KEY_SET_VALUE                     0x0002U
#define REG_SZ                            1U
#define ERROR_SUCCESS                     0L

#pragma pack(push, 1)

typedef struct {
    WORD format_tag;
    WORD channels;
    DWORD samples_per_second;
    DWORD average_bytes_per_second;
    WORD block_align;
    WORD bits_per_sample;
    WORD extra_size;
} WAVEFORMATEX;

typedef struct wave_header {
    BYTE *data;
    DWORD buffer_length;
    DWORD bytes_recorded;
    DWORD_PTR user;
    DWORD flags;
    DWORD loops;
    struct wave_header *next;
    DWORD_PTR reserved;
} WAVEHDR;

typedef struct {
    UINT type;
    DWORD value;
} MMTIME;

typedef struct {
    DWORD_PTR callback;
    UINT device_id;
    const char *device_type;
    const char *element_name;
    const char *alias;
} MCI_OPEN_PARMSA;

typedef struct {
    DWORD_PTR callback;
    UINT device_id;
    const WORD *device_type;
    const WORD *element_name;
    const WORD *alias;
} MCI_OPEN_PARMSW;

typedef struct {
    DWORD_PTR callback;
    DWORD from;
    DWORD to;
} MCI_PLAY_PARMS;

typedef struct {
    DWORD_PTR callback;
    DWORD to;
} MCI_SEEK_PARMS;

typedef struct {
    DWORD_PTR callback;
    DWORD_PTR result;
    DWORD item;
    DWORD track;
} MCI_STATUS_PARMS;

typedef struct {
    DWORD_PTR callback;
    DWORD_PTR result;
    DWORD item;
} MCI_GETDEVCAPS_PARMS;

typedef struct {
    DWORD_PTR callback;
    DWORD time_format;
    DWORD audio;
} MCI_SET_PARMS;

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
    DWORD cb_struct;
    DWORD destination;
    DWORD source;
    DWORD line_id;
    DWORD flags;
    DWORD user;
    DWORD component_type;
    DWORD channels;
    DWORD connections;
    DWORD controls;
    char short_name[16];
    char name[64];
    struct {
        DWORD type;
        DWORD device_id;
        WORD manufacturer_id;
        WORD product_id;
        DWORD driver_version;
        char product_name[32];
    } target;
} MIXERLINEA;

typedef struct {
    DWORD cb_struct;
    DWORD control_id;
    DWORD control_type;
    DWORD flags;
    DWORD multiple_items;
    char short_name[16];
    char name[64];
    DWORD bounds[6];
    DWORD metrics[6];
} MIXERCONTROLA;

typedef struct {
    DWORD cb_struct;
    DWORD line_id;
    DWORD control_id_or_type;
    DWORD controls;
    DWORD control_size;
    MIXERCONTROLA *control_array;
} MIXERLINECONTROLSA;

typedef struct {
    DWORD cb_struct;
    DWORD control_id;
    DWORD channels;
    DWORD multiple_items;
    DWORD detail_size;
    DWORD *details;
} MIXERCONTROLDETAILS;

#pragma pack(pop)

_Static_assert(sizeof(AUXCAPSA) == 48, "AUXCAPSA layout");
_Static_assert(sizeof(MIXERCAPSA) == 48, "MIXERCAPSA layout");
_Static_assert(sizeof(MIXERLINEA) == 168, "MIXERLINEA layout");
_Static_assert(sizeof(MIXERCONTROLA) == 148, "MIXERCONTROLA layout");
_Static_assert(sizeof(MIXERLINECONTROLSA) == 24,
               "MIXERLINECONTROLSA layout");
_Static_assert(sizeof(MIXERCONTROLDETAILS) == 24,
               "MIXERCONTROLDETAILS layout");

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT void WINAPI Sleep(DWORD milliseconds);
DLLIMPORT BOOL WINAPI CreateDirectoryA(const char *path, PVOID security);
DLLIMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access,
                                     DWORD share, PVOID security,
                                     DWORD disposition, DWORD attributes,
                                     HANDLE template_file);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD bytes,
                                DWORD *written, PVOID overlapped);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT BOOL WINAPI SetEnvironmentVariableA(const char *name,
                                               const char *value);
DLLIMPORT BOOL WINAPI PlaySoundA(const char *sound, PVOID module,
                                 DWORD flags);
DLLIMPORT BOOL WINAPI PlaySoundW(const WORD *sound, PVOID module,
                                 DWORD flags);
DLLIMPORT long WINAPI RegCreateKeyExA(HKEY root, const char *subkey,
                                      DWORD reserved, char *class_name,
                                      DWORD options, DWORD access,
                                      PVOID security, HKEY *result,
                                      DWORD *disposition);
DLLIMPORT long WINAPI RegSetValueExA(HKEY key, const char *value_name,
                                     DWORD reserved, DWORD type,
                                     const BYTE *data, DWORD bytes);
DLLIMPORT long WINAPI RegCloseKey(HKEY key);
DLLIMPORT BOOL WINAPI mciGetErrorStringA(DWORD error, char *text,
                                         UINT chars);
DLLIMPORT DWORD WINAPI mciSendCommandA(UINT device, UINT message,
                                       DWORD_PTR flags, DWORD_PTR parameter);
DLLIMPORT DWORD WINAPI mciSendCommandW(UINT device, UINT message,
                                       DWORD_PTR flags, DWORD_PTR parameter);
DLLIMPORT DWORD WINAPI mciSendStringA(const char *command, char *result,
                                      UINT chars, PVOID callback_window);
DLLIMPORT DWORD WINAPI mciSendStringW(const WORD *command, WORD *result,
                                      UINT chars, PVOID callback_window);
DLLIMPORT UINT WINAPI mciGetDeviceIDA(const char *name);
DLLIMPORT UINT WINAPI mciGetDeviceIDW(const WORD *name);
DLLIMPORT UINT WINAPI waveOutGetNumDevs(void);
DLLIMPORT UINT WINAPI waveOutGetDevCapsA(UINT device,
                                        WAVEOUTCAPSA *capabilities,
                                        UINT size);
DLLIMPORT UINT WINAPI waveOutGetErrorTextA(UINT error, char *text,
                                           UINT chars);
DLLIMPORT UINT WINAPI waveOutOpen(HWAVEOUT *output, UINT device,
                                  const WAVEFORMATEX *format,
                                  DWORD_PTR callback, DWORD_PTR instance,
                                  DWORD flags);
DLLIMPORT UINT WINAPI waveOutPrepareHeader(HWAVEOUT output, WAVEHDR *header,
                                           UINT size);
DLLIMPORT UINT WINAPI waveOutUnprepareHeader(HWAVEOUT output,
                                             WAVEHDR *header, UINT size);
DLLIMPORT UINT WINAPI waveOutWrite(HWAVEOUT output, WAVEHDR *header,
                                   UINT size);
DLLIMPORT UINT WINAPI waveOutPause(HWAVEOUT output);
DLLIMPORT UINT WINAPI waveOutRestart(HWAVEOUT output);
DLLIMPORT UINT WINAPI waveOutReset(HWAVEOUT output);
DLLIMPORT UINT WINAPI waveOutGetPosition(HWAVEOUT output, MMTIME *position,
                                         UINT size);
DLLIMPORT UINT WINAPI waveOutSetVolume(HWAVEOUT output, DWORD volume);
DLLIMPORT UINT WINAPI waveOutGetVolume(HWAVEOUT output, DWORD *volume);
DLLIMPORT UINT WINAPI waveOutGetID(HWAVEOUT output, UINT *device_id);
DLLIMPORT UINT WINAPI waveOutClose(HWAVEOUT output);
DLLIMPORT UINT WINAPI auxGetNumDevs(void);
DLLIMPORT UINT WINAPI auxGetDevCapsA(UINT device, AUXCAPSA *capabilities,
                                    UINT size);
DLLIMPORT UINT WINAPI auxSetVolume(UINT device, DWORD volume);
DLLIMPORT UINT WINAPI auxGetVolume(UINT device, DWORD *volume);
DLLIMPORT UINT WINAPI mixerGetNumDevs(void);
DLLIMPORT UINT WINAPI mixerGetDevCapsA(DWORD_PTR mixer_id,
                                      MIXERCAPSA *capabilities, UINT size);
DLLIMPORT UINT WINAPI mixerOpen(HMIXER *mixer, UINT device,
                               DWORD_PTR callback, DWORD_PTR instance,
                               DWORD flags);
DLLIMPORT UINT WINAPI mixerClose(HMIXER mixer);
DLLIMPORT UINT WINAPI mixerGetID(PVOID object, UINT *mixer_id, DWORD flags);
DLLIMPORT UINT WINAPI mixerGetLineInfoA(PVOID object, MIXERLINEA *line,
                                       DWORD flags);
DLLIMPORT UINT WINAPI mixerGetLineControlsA(PVOID object,
                                           MIXERLINECONTROLSA *controls,
                                           DWORD flags);
DLLIMPORT UINT WINAPI mixerGetControlDetailsA(PVOID object,
                                              MIXERCONTROLDETAILS *details,
                                              DWORD flags);
DLLIMPORT UINT WINAPI mixerSetControlDetails(PVOID object,
                                             MIXERCONTROLDETAILS *details,
                                             DWORD flags);

static volatile DWORD open_callbacks;
static volatile DWORD done_callbacks;
static volatile DWORD close_callbacks;
static BYTE tone[4410];
static BYTE reset_tone[22050];
static BYTE play_wave[44 + 2205];
static const BYTE invalid_wave[12] = {
    'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'
};
static const WORD play_wave_name_w[] = {
    'p', 'l', 'a', 'y', 's', 'o', 'u', 'n', 'd', '.', 'w', 'a', 'v', 0
};
static const WORD waveaudio_w[] = {
    'w', 'a', 'v', 'e', 'a', 'u', 'd', 'i', 'o', 0
};
static const WORD mci_alias_w[] = {
    'm', 'c', 'i', 'w', 'i', 'd', 'e', 0
};
static const WORD mci_status_w[] = {
    's', 't', 'a', 't', 'u', 's', ' ', 'm', 'c', 'i', 'w', 'i', 'd', 'e',
    ' ', 'l', 'e', 'n', 'g', 't', 'h', ' ', 'w', 'a', 'i', 't', 0
};
static const WORD mci_close_w[] = {
    'c', 'l', 'o', 's', 'e', ' ', 'm', 'c', 'i', 'w', 'i', 'd', 'e', ' ',
    'w', 'a', 'i', 't', 0
};

void *memset(void *destination, int value, unsigned int bytes)
{
    BYTE *output = (BYTE *)destination;
    for (unsigned int i = 0; i < bytes; i++)
        output[i] = (BYTE)value;
    return destination;
}

static void WINAPI wave_callback(HWAVEOUT output, UINT message,
                                 DWORD_PTR instance, DWORD_PTR parameter1,
                                 DWORD_PTR parameter2)
{
    (void)output;
    (void)instance;
    (void)parameter1;
    (void)parameter2;
    if (message == WOM_OPEN)
        open_callbacks++;
    else if (message == WOM_DONE)
        done_callbacks++;
    else if (message == WOM_CLOSE)
        close_callbacks++;
}

static void fail(UINT stage)
{
    ExitProcess(stage);
}

static BOOL text_equal(const char *left, const char *right)
{
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static void fill_square(BYTE *buffer, DWORD bytes, DWORD period)
{
    for (DWORD i = 0; i < bytes; i++)
        buffer[i] = (i % period) < (period / 2U) ? 224U : 32U;
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

static void build_play_wave(void)
{
    play_wave[0] = 'R'; play_wave[1] = 'I';
    play_wave[2] = 'F'; play_wave[3] = 'F';
    put_le32(play_wave + 4, sizeof(play_wave) - 8U);
    play_wave[8] = 'W'; play_wave[9] = 'A';
    play_wave[10] = 'V'; play_wave[11] = 'E';
    play_wave[12] = 'f'; play_wave[13] = 'm';
    play_wave[14] = 't'; play_wave[15] = ' ';
    put_le32(play_wave + 16, 16U);
    put_le16(play_wave + 20, 1U);
    put_le16(play_wave + 22, 1U);
    put_le32(play_wave + 24, 22050U);
    put_le32(play_wave + 28, 22050U);
    put_le16(play_wave + 32, 1U);
    put_le16(play_wave + 34, 8U);
    play_wave[36] = 'd'; play_wave[37] = 'a';
    play_wave[38] = 't'; play_wave[39] = 'a';
    put_le32(play_wave + 40, sizeof(play_wave) - 44U);
    fill_square(play_wave + 44, sizeof(play_wave) - 44U, 32U);
}

static void register_sound_alias(const char *path, UINT fail_stage)
{
    static const char file_name[] = "playsound.wav";
    HKEY key = 0;
    DWORD disposition = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, 0, 0,
                        KEY_SET_VALUE, 0, &key, &disposition) != ERROR_SUCCESS ||
        !key ||
        RegSetValueExA(key, 0, 0, REG_SZ, (const BYTE *)file_name,
                       sizeof(file_name)) != ERROR_SUCCESS ||
        RegCloseKey(key) != ERROR_SUCCESS)
        fail(fail_stage);
}

static void prepare_play_sound_sources(void)
{
    build_play_wave();
    HANDLE file = CreateFileA("playsound.wav", GENERIC_WRITE, 0, 0,
                              CREATE_ALWAYS, 0, 0);
    DWORD written = 0;
    if (!file || file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, play_wave, sizeof(play_wave), &written, 0) ||
        written != sizeof(play_wave) || !CloseHandle(file))
        fail(47);

    file = CreateFileA("invalid.wav", GENERIC_WRITE, 0, 0,
                       CREATE_ALWAYS, 0, 0);
    written = 0;
    if (!file || file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, invalid_wave, sizeof(invalid_wave), &written, 0) ||
        written != sizeof(invalid_wave) || !CloseHandle(file))
        fail(90);

    if (!CreateDirectoryA("soundpath", 0))
        fail(57);
    file = CreateFileA("soundpath\\pathsound.wav", GENERIC_WRITE, 0, 0,
                       CREATE_ALWAYS, 0, 0);
    written = 0;
    if (!file || file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, play_wave, sizeof(play_wave), &written, 0) ||
        written != sizeof(play_wave) || !CloseHandle(file) ||
        !SetEnvironmentVariableA("PATH", "missing;soundpath"))
        fail(58);

    static const char alias_path[] =
        "AppEvents\\Schemes\\Apps\\.Default\\OsitoContract\\.Current";
    static const char predefined_path[] =
        "AppEvents\\Schemes\\Apps\\.Default\\SystemAsterisk\\.Current";
    static const char application_path[] =
        "AppEvents\\Schemes\\Apps\\winmm_waveout_pe32\\"
        "OsitoApplication\\.Current";
    static const char isolated_global_path[] =
        "AppEvents\\Schemes\\Apps\\.Default\\ApplicationIsolation\\"
        ".Current";
    register_sound_alias(alias_path, 48);
    register_sound_alias(predefined_path, 48);
    register_sound_alias(application_path, 59);
    register_sound_alias(isolated_global_path, 59);
}

static HWAVEOUT open_output(const WAVEFORMATEX *format, UINT fail_stage)
{
    HWAVEOUT output = (HWAVEOUT)0;
    UINT result = waveOutOpen(&output, WAVE_MAPPER, format,
                              (DWORD_PTR)wave_callback, 0,
                              CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR || !output)
        fail(fail_stage);
    return output;
}

void mainCRTStartup(void)
{
    WAVEFORMATEX format = {
        1, 1, 22050, 22050, 1, 8, 0
    };
    WAVEFORMATEX invalid_format = {
        1, 1, 22050, 66150, 3, 24, 0
    };
    HWAVEOUT output;
    WAVEHDR header = {0};
    MMTIME position = { TIME_BYTES, 0 };
    DWORD paused_position;
    DWORD volume = 0;
    UINT device_id = ~0U;
    WAVEOUTCAPSA capabilities = {0};
    AUXCAPSA aux_capabilities = {0};
    MIXERCAPSA mixer_capabilities = {0};
    MIXERLINEA mixer_line = {0};
    MIXERCONTROLA mixer_control = {0};
    MIXERLINECONTROLSA line_controls = {0};
    MIXERCONTROLDETAILS control_details = {0};
    HMIXER mixer = (HMIXER)0;
    UINT mixer_id = ~0U;
    DWORD master_volume = 0;
    DWORD mixer_values[2] = {0, 0};
    char error_text[80] = {0};
    char mci_text[80] = {0};
    char mci_result[16] = {'x', 0};
    WORD mci_result_w[8] = {'x', 0};
    MCI_OPEN_PARMSA mci_open = {0};
    MCI_OPEN_PARMSW mci_open_w = {0};
    MCI_PLAY_PARMS mci_play = {0};
    MCI_SEEK_PARMS mci_seek = {0};
    MCI_STATUS_PARMS mci_status = {0};
    MCI_GETDEVCAPS_PARMS mci_caps = {0};
    MCI_SET_PARMS mci_set = {0};

    prepare_play_sound_sources();
    if (!PlaySoundA((const char *)0, (PVOID)0, 0) ||
        PlaySoundA("missing.wav", (PVOID)0, 0))
        fail(32);
    if (PlaySoundA((const char *)invalid_wave, 0,
                   SND_MEMORY | SND_NODEFAULT) ||
        PlaySoundA((const char *)play_wave, 0, SND_MEMORY | SND_LOOP))
        fail(49);
    if (!PlaySoundA((const char *)play_wave, 0,
                    SND_MEMORY | SND_NODEFAULT))
        fail(50);
    if (!PlaySoundW((const WORD *)play_wave, 0,
                    SND_MEMORY | SND_NODEFAULT))
        fail(51);
    if (!PlaySoundA("playsound.wav", 0,
                    SND_FILENAME | SND_NODEFAULT) ||
        !PlaySoundW(play_wave_name_w, 0,
                    SND_FILENAME | SND_NODEFAULT) ||
        !PlaySoundA("playsound.wav", 0, SND_NODEFAULT) ||
        !PlaySoundA("pathsound.wav", 0,
                    SND_FILENAME | SND_NODEFAULT))
        fail(52);
    if (!PlaySoundA("OsitoContract", 0,
                    SND_ALIAS | SND_NODEFAULT) ||
        !PlaySoundA((const char *)(DWORD_PTR)0x2A53U, 0,
                    SND_ALIAS_ID | SND_NODEFAULT) ||
        !PlaySoundA("OsitoApplication", 0,
                    SND_ALIAS | SND_APPLICATION | SND_NODEFAULT) ||
        PlaySoundA("ApplicationIsolation", 0,
                   SND_ALIAS | SND_APPLICATION | SND_NODEFAULT))
        fail(53);
    static const char default_path[] =
        "AppEvents\\Schemes\\Apps\\.Default\\.Default\\.Current";
    register_sound_alias(default_path, 60);
    if (!PlaySoundA("MissingAlias", 0, SND_ALIAS) ||
        !PlaySoundA("SystemDefault", 0,
                    SND_ALIAS | SND_NODEFAULT) ||
        !PlaySoundA((const char *)(DWORD_PTR)0x4453U, 0,
                    SND_ALIAS_ID | SND_NODEFAULT))
        fail(61);
    if (!PlaySoundA((const char *)play_wave, 0,
                    SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT) ||
        PlaySoundA((const char *)play_wave, 0,
                   SND_MEMORY | SND_ASYNC | SND_NOSTOP | SND_NODEFAULT) ||
        !PlaySoundA(0, 0, 0))
        fail(54);
    if (!PlaySoundA((const char *)play_wave, 0,
                    SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        fail(55);
    Sleep(200);
    if (!PlaySoundA((const char *)play_wave, 0,
                    SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NOSTOP |
                        SND_NODEFAULT) ||
        !PlaySoundA(0, 0, 0))
        fail(56);
    if (waveOutGetErrorTextA(WAVERR_BADFORMAT, error_text,
                             sizeof(error_text)) != MMSYSERR_NOERROR ||
        error_text[0] != 'T')
        fail(33);
    error_text[0] = 'x';
    if (waveOutGetErrorTextA(0xFFFFU, error_text, sizeof(error_text)) !=
            MMSYSERR_BADERRNUM || error_text[0] != 0)
        fail(34);
    if (mciSendStringA((const char *)0, mci_result, sizeof(mci_result),
                       (PVOID)0) != MCIERR_MISSING_COMMAND_STRING ||
        mci_result[0] != 0)
        fail(35);
    DWORD missing_mci_result = mciSendStringA(
        "open missing.wav", mci_result, sizeof(mci_result), (PVOID)0);
    if (missing_mci_result != MCIERR_FILE_NOT_FOUND)
        fail(36);
    if (mci_result[0] != 0)
        fail(88);
    if (mciSendStringA("open invalid.wav", mci_result, sizeof(mci_result),
                       (PVOID)0) != MCIERR_INVALID_FILE)
        fail(91);
    if (mciSendCommandA(0, MCI_OPEN, 0, 0) !=
            MCIERR_NULL_PARAMETER_BLOCK)
        fail(89);
    if (!mciGetErrorStringA(MCIERR_UNSUPPORTED_FUNCTION, mci_text,
                            sizeof(mci_text)) || mci_text[0] != 'T')
        fail(37);

    mci_open.device_type = "waveaudio";
    mci_open.element_name = "playsound.wav";
    mci_open.alias = "mcitest";
    if (mciSendCommandA(0, MCI_OPEN,
                        MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS |
                            MCI_WAIT,
                        (DWORD_PTR)&mci_open) != 0 || !mci_open.device_id)
        fail(62);
    if (mciSendCommandA(0, MCI_OPEN,
                        MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS,
                        (DWORD_PTR)&mci_open) != MCIERR_DUPLICATE_ALIAS)
        fail(63);

    MCI_OPEN_PARMSA mci_cd = {0};
    mci_cd.device_type = "cdaudio";
    if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_TYPE,
                        (DWORD_PTR)&mci_cd) != MCIERR_DEVICE_NOT_INSTALLED)
        fail(64);

    mci_status.item = MCI_STATUS_LENGTH;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS,
                        MCI_STATUS_ITEM | MCI_WAIT,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != 100U)
        fail(65);
    mci_status.item = MCI_STATUS_MODE;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != MCI_MODE_STOP)
        fail(66);
    if (mciSendCommandA(mci_open.device_id, MCI_PAUSE, MCI_WAIT, 0) != 0 ||
        mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != MCI_MODE_STOP)
        fail(92);
    if (mciSendCommandA(mci_open.device_id, MCI_STOP, MCI_NOTIFY, 0) !=
            MCIERR_NULL_PARAMETER_BLOCK)
        fail(93);

    mci_caps.item = MCI_GETDEVCAPS_CAN_PLAY;
    if (mciSendCommandA(mci_open.device_id, MCI_GETDEVCAPS,
                        MCI_GETDEVCAPS_ITEM, (DWORD_PTR)&mci_caps) != 0 ||
        mci_caps.result != 1U)
        fail(67);
    mci_set.time_format = MCI_FORMAT_SAMPLES;
    if (mciSendCommandA(mci_open.device_id, MCI_SET,
                        MCI_SET_TIME_FORMAT | MCI_WAIT,
                        (DWORD_PTR)&mci_set) != 0)
        fail(68);
    mci_status.item = MCI_STATUS_LENGTH;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != 2205U)
        fail(69);

    if (mciSendCommandA(mci_open.device_id, MCI_SEEK,
                        MCI_SEEK_TO_START | MCI_WAIT,
                        (DWORD_PTR)&mci_seek) != 0)
        fail(70);
    mci_play.from = 0;
    mci_play.to = 1102U;
    if (mciSendCommandA(mci_open.device_id, MCI_PLAY,
                        MCI_FROM | MCI_TO | MCI_WAIT,
                        (DWORD_PTR)&mci_play) != 0)
        fail(71);
    mci_status.item = MCI_STATUS_POSITION;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != 1102U)
        fail(72);

    if (mciSendCommandA(mci_open.device_id, MCI_SEEK,
                        MCI_SEEK_TO_START, (DWORD_PTR)&mci_seek) != 0 ||
        mciSendCommandA(mci_open.device_id, MCI_PLAY, 0,
                        (DWORD_PTR)&mci_play) != 0)
        fail(73);
    Sleep(30);
    if (mciSendCommandA(mci_open.device_id, MCI_PAUSE, MCI_WAIT, 0) != 0)
        fail(74);
    mci_status.item = MCI_STATUS_MODE;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != MCI_MODE_PAUSE)
        fail(75);
    mci_status.item = MCI_STATUS_POSITION;
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        !mci_status.result || mci_status.result >= 2205U)
        fail(76);
    DWORD paused_mci_position = (DWORD)mci_status.result;
    Sleep(30);
    if (mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != 0 ||
        mci_status.result != paused_mci_position ||
        mciSendCommandA(mci_open.device_id, MCI_RESUME, MCI_WAIT, 0) != 0)
        fail(77);
    for (UINT waited = 0; waited < 1000U; waited += 5U) {
        mci_status.item = MCI_STATUS_MODE;
        if (mciSendCommandA(mci_open.device_id, MCI_STATUS,
                            MCI_STATUS_ITEM, (DWORD_PTR)&mci_status) != 0)
            fail(78);
        if (mci_status.result == MCI_MODE_STOP)
            break;
        Sleep(5);
    }
    if (mci_status.result != MCI_MODE_STOP ||
        mciSendCommandA(mci_open.device_id, MCI_CLOSE, MCI_WAIT, 0) != 0 ||
        mciSendCommandA(mci_open.device_id, MCI_STATUS, MCI_STATUS_ITEM,
                        (DWORD_PTR)&mci_status) != MCIERR_INVALID_DEVICE_ID)
        fail(79);

    mci_open_w.device_type = waveaudio_w;
    mci_open_w.element_name = play_wave_name_w;
    mci_open_w.alias = mci_alias_w;
    if (mciSendCommandW(0, MCI_OPEN,
                        MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS,
                        (DWORD_PTR)&mci_open_w) != 0 ||
        !mci_open_w.device_id ||
        mciGetDeviceIDW(mci_alias_w) != mci_open_w.device_id ||
        mciSendStringW(mci_status_w, mci_result_w, 8U, 0) != 0 ||
        mci_result_w[0] != '1' || mci_result_w[1] != '0' ||
        mci_result_w[2] != '0' || mci_result_w[3] != 0 ||
        mciSendStringW(mci_close_w, mci_result_w, 8U, 0) != 0 ||
        mciGetDeviceIDW(mci_alias_w) != 0)
        fail(80);

    if (mciSendStringA(
            "open \"playsound.wav\" type waveaudio alias stringmci wait",
            mci_result, sizeof(mci_result), 0) != 0)
        fail(81);
    UINT string_mci_id = mciGetDeviceIDA("stringmci");
    if (!string_mci_id ||
        mciSendStringA("set stringmci time format samples wait", mci_result,
                       sizeof(mci_result), 0) != 0)
        fail(82);
    if (mciSendStringA("status stringmci length wait", mci_result,
                       sizeof(mci_result), 0) != 0 ||
        !text_equal(mci_result, "2205"))
        fail(83);
    if (mciSendStringA("seek stringmci to start wait", mci_result,
                       sizeof(mci_result), 0) != 0 ||
        mciSendStringA("play stringmci from 0 to 551 wait", mci_result,
                       sizeof(mci_result), 0) != 0)
        fail(84);
    if (mciSendStringA("status stringmci position wait", mci_result,
                       sizeof(mci_result), 0) != 0 ||
        !text_equal(mci_result, "551"))
        fail(85);
    if (mciSendStringA("close stringmci wait", mci_result,
                       sizeof(mci_result), 0) != 0 ||
        mciGetDeviceIDA("stringmci") != 0)
        fail(86);
    if (mciSendStringA("open cdaudio", mci_result, sizeof(mci_result), 0) !=
            MCIERR_DEVICE_NOT_INSTALLED)
        fail(87);

    if (waveOutGetNumDevs() != 1U)
        fail(10);
    if (waveOutGetDevCapsA(0, &capabilities, sizeof(capabilities)) !=
            MMSYSERR_NOERROR || capabilities.channels != 2U ||
        capabilities.support != 3U || capabilities.product_name[0] != 'O')
        fail(31);

    if (auxGetNumDevs() != 1U ||
        auxGetDevCapsA(0, &aux_capabilities, sizeof(aux_capabilities)) !=
            MMSYSERR_NOERROR || aux_capabilities.technology != 2U ||
        aux_capabilities.support !=
            (AUXCAPS_VOLUME | AUXCAPS_LRVOLUME) ||
        aux_capabilities.product_name[0] != 'O')
        fail(38);
    if (mixerGetNumDevs() != 1U ||
        mixerGetDevCapsA(0, &mixer_capabilities,
                         sizeof(mixer_capabilities)) != MMSYSERR_NOERROR ||
        mixer_capabilities.destinations != 1U ||
        mixer_capabilities.product_name[0] != 'O')
        fail(39);
    if (mixerOpen(&mixer, 0, 0, 0, 0) != MMSYSERR_NOERROR || !mixer ||
        mixerGetDevCapsA((DWORD_PTR)mixer, &mixer_capabilities,
                         sizeof(mixer_capabilities)) != MMSYSERR_NOERROR ||
        mixerGetID(mixer, &mixer_id, MIXER_OBJECTF_HMIXER) !=
            MMSYSERR_NOERROR || mixer_id != 0U)
        fail(40);

    mixer_line.cb_struct = sizeof(mixer_line);
    mixer_line.destination = 0;
    if (mixerGetLineInfoA((PVOID)0, &mixer_line,
                          MIXER_GETLINEINFOF_DESTINATION) !=
            MMSYSERR_NOERROR ||
        mixer_line.line_id == 0 || mixer_line.channels != 2U ||
        mixer_line.controls != 1U ||
        mixer_line.component_type !=
            MIXERLINE_COMPONENTTYPE_DST_SPEAKERS ||
        mixer_line.name[0] != 'O')
        fail(41);

    line_controls.cb_struct = sizeof(line_controls);
    line_controls.line_id = mixer_line.line_id;
    line_controls.control_id_or_type =
        MIXERCONTROL_CONTROLTYPE_VOLUME;
    line_controls.controls = 1;
    line_controls.control_size = sizeof(mixer_control);
    line_controls.control_array = &mixer_control;
    if (mixerGetLineControlsA(
            mixer, &line_controls,
            MIXER_OBJECTF_HMIXER |
                MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR ||
        mixer_control.control_id == 0 ||
        mixer_control.control_type != MIXERCONTROL_CONTROLTYPE_VOLUME ||
        mixer_control.bounds[1] != 65535U ||
        mixer_control.name[0] != 'M')
        fail(42);

    if (auxSetVolume(0, 0x60004000U) != MMSYSERR_NOERROR)
        fail(43);
    control_details.cb_struct = sizeof(control_details);
    control_details.control_id = mixer_control.control_id;
    control_details.channels = 2;
    control_details.detail_size = sizeof(DWORD);
    control_details.details = mixer_values;
    if (mixerGetControlDetailsA(mixer, &control_details,
                                MIXER_OBJECTF_HMIXER) !=
            MMSYSERR_NOERROR ||
        mixer_values[0] != 0x4000U || mixer_values[1] != 0x6000U)
        fail(44);

    mixer_values[0] = 0x2000U;
    mixer_values[1] = 0x7000U;
    if (mixerSetControlDetails(mixer, &control_details,
                               MIXER_OBJECTF_HMIXER) != MMSYSERR_NOERROR ||
        auxGetVolume(0, &master_volume) != MMSYSERR_NOERROR ||
        master_volume != 0x70002000U)
        fail(45);
    if (mixerClose(mixer) != MMSYSERR_NOERROR ||
        mixerClose(mixer) != MMSYSERR_INVALHANDLE ||
        auxSetVolume(0, 0xFFFFFFFFU) != MMSYSERR_NOERROR)
        fail(46);

    if (waveOutOpen((HWAVEOUT *)0, WAVE_MAPPER, &format, 0, 0,
                    WAVE_FORMAT_QUERY) != MMSYSERR_NOERROR)
        fail(11);
    if (waveOutOpen((HWAVEOUT *)0, WAVE_MAPPER, &invalid_format, 0, 0,
                    WAVE_FORMAT_QUERY) != WAVERR_BADFORMAT)
        fail(12);
    if (waveOutOpen((HWAVEOUT *)(DWORD_PTR)0x1234U, WAVE_MAPPER,
                    &format, 0, 0, WAVE_FORMAT_QUERY) !=
            MMSYSERR_INVALPARAM)
        fail(94);
    if (waveOutOpen((HWAVEOUT *)(DWORD_PTR)0x00100000U, WAVE_MAPPER,
                    &format, 0, 0, WAVE_FORMAT_QUERY) !=
            MMSYSERR_INVALPARAM)
        fail(98);
    output = (HWAVEOUT)1;
    if (waveOutOpen(&output, WAVE_MAPPER,
                    (const WAVEFORMATEX *)(DWORD_PTR)0x1234U,
                    0, 0, WAVE_FORMAT_QUERY) != MMSYSERR_INVALPARAM || output)
        fail(95);

    fill_square(tone, sizeof(tone), 50);
    output = open_output(&format, 13);
    if (open_callbacks != 1U)
        fail(14);
    if (waveOutSetVolume(output, 0x60004000U) != MMSYSERR_NOERROR ||
        waveOutGetVolume(output, &volume) != MMSYSERR_NOERROR ||
        volume != 0x60004000U)
        fail(15);
    if (waveOutGetID(output, &device_id) != MMSYSERR_NOERROR ||
        device_id != 0U)
        fail(16);
    if (waveOutPrepareHeader(
            output, (WAVEHDR *)(DWORD_PTR)0x1234U, sizeof(WAVEHDR)) !=
            MMSYSERR_INVALPARAM)
        fail(96);

    WAVEHDR invalid_header = {0};
    invalid_header.data = (BYTE *)(DWORD_PTR)0x1234U;
    invalid_header.buffer_length = 256U;
    if (waveOutPrepareHeader(output, &invalid_header,
                             sizeof(invalid_header)) != MMSYSERR_NOERROR ||
        waveOutWrite(output, &invalid_header, sizeof(invalid_header)) !=
            MMSYSERR_INVALPARAM ||
        waveOutUnprepareHeader(output, &invalid_header,
                               sizeof(invalid_header)) != MMSYSERR_NOERROR)
        fail(97);

    header.data = tone;
    header.buffer_length = sizeof(tone);
    if (waveOutPrepareHeader(output, &header, sizeof(header)) !=
        MMSYSERR_NOERROR || !(header.flags & WHDR_PREPARED))
        fail(17);
    if (waveOutWrite(output, &header, sizeof(header)) != MMSYSERR_NOERROR)
        fail(18);
    Sleep(30);
    if (waveOutPause(output) != MMSYSERR_NOERROR)
        fail(19);
    if (waveOutGetPosition(output, &position, sizeof(position)) !=
        MMSYSERR_NOERROR)
        fail(20);
    paused_position = position.value;
    Sleep(50);
    position.type = TIME_BYTES;
    if (waveOutGetPosition(output, &position, sizeof(position)) !=
            MMSYSERR_NOERROR || position.value != paused_position)
        fail(21);
    if (waveOutRestart(output) != MMSYSERR_NOERROR)
        fail(22);

    for (UINT waited = 0; waited < 3000U && done_callbacks != 1U;
         waited += 5U)
        Sleep(5);
    if (done_callbacks != 1U || !(header.flags & WHDR_DONE))
        fail(23);
    position.type = TIME_BYTES;
    if (waveOutGetPosition(output, &position, sizeof(position)) !=
            MMSYSERR_NOERROR || position.value != sizeof(tone))
        fail(24);
    if (waveOutUnprepareHeader(output, &header, sizeof(header)) !=
            MMSYSERR_NOERROR || (header.flags & WHDR_PREPARED))
        fail(25);
    if (waveOutClose(output) != MMSYSERR_NOERROR || close_callbacks != 1U)
        fail(26);

    fill_square(reset_tone, sizeof(reset_tone), 75);
    header.data = reset_tone;
    header.buffer_length = sizeof(reset_tone);
    header.flags = 0;
    output = open_output(&format, 27);
    if (waveOutPrepareHeader(output, &header, sizeof(header)) !=
            MMSYSERR_NOERROR ||
        waveOutWrite(output, &header, sizeof(header)) != MMSYSERR_NOERROR)
        fail(28);
    Sleep(20);
    if (waveOutReset(output) != MMSYSERR_NOERROR ||
        done_callbacks != 2U || !(header.flags & WHDR_DONE))
        fail(29);
    if (waveOutUnprepareHeader(output, &header, sizeof(header)) !=
            MMSYSERR_NOERROR ||
        waveOutClose(output) != MMSYSERR_NOERROR || close_callbacks != 2U)
        fail(30);

    ExitProcess(0);
}
