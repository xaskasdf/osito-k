/*
 * OsitoK Windows Compatibility Layer — winmm.dll Shim
 * Multimedia timers, legacy PCM playback, and mixer/device contracts.
 */

#include "winmm_shim.h"
#include "advapi32_shim.h"
#include "compat32.h"
#include "kernel32_shim.h"
#include "user32_shim.h"
#include "win32_abi.h"
#include "../include/audio_sched.h"
#include "../include/pcm.h"

#ifdef TEST_HARNESS
#include <time.h>
#endif

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

/* Weak reference to kernel tick source (100 Hz timer) */
extern uint64_t idt_get_ticks(void) __attribute__((weak));
extern uint64_t idt_get_tsc_freq(void);

/* ── Multimedia timers ─────────────────────────────────────── */

#define WINMM_TIMER_SLOTS       128
#define WINMM_DISPATCHER_SLOTS   64
#define WINMM_PERIOD_SLOTS       64
#define WINMM_WAVE_OUT_SLOTS     32
#define WINMM_PLAY_SOUND_SLOTS   64
#define WINMM_MCI_SLOTS           32
#define WINMM_MIXER_SLOTS        16

#define WINMM_PERIOD_MIN_MS       1U
#define WINMM_PERIOD_MAX_MS 1000000U

#define TIME_ONESHOT              0x0000U
#define TIME_PERIODIC             0x0001U
#define TIME_CALLBACK_TYPEMASK    0x0030U
#define TIME_CALLBACK_FUNCTION    0x0000U
#define TIME_CALLBACK_EVENT_SET   0x0010U
#define TIME_CALLBACK_EVENT_PULSE 0x0020U
#define TIME_KILL_SYNCHRONOUS     0x0100U
#define TIME_VALID_FLAGS          (TIME_PERIODIC | TIME_CALLBACK_TYPEMASK | \
                                   TIME_KILL_SYNCHRONOUS)

#define WINMM_DISPATCHER_FREE     0
#define WINMM_DISPATCHER_STARTING 1
#define WINMM_DISPATCHER_RUNNING  2
#define WINMM_DISPATCHER_STOPPED  3

#define MMSYSERR_NOERROR          0U
#define MMSYSERR_ERROR            1U
#define MMSYSERR_BADDEVICEID      2U
#define MMSYSERR_ALLOCATED        4U
#define MMSYSERR_INVALHANDLE      5U
#define MMSYSERR_NODRIVER         6U
#define MMSYSERR_NOMEM            7U
#define MMSYSERR_NOTSUPPORTED     8U
#define MMSYSERR_BADERRNUM         9U
#define MMSYSERR_INVALFLAG       10U
#define MMSYSERR_INVALPARAM      11U
#define WAVERR_BADFORMAT         32U
#define WAVERR_STILLPLAYING      33U
#define WAVERR_UNPREPARED        34U

#define WAVE_MAPPER          0xFFFFFFFFU
#define WAVE_FORMAT_QUERY    0x00000001U
#define WAVE_FORMAT_DIRECT   0x00000008U
#define WAVE_FORMAT_PCM      0x0001U

#define SND_ASYNC            0x00000001U
#define SND_NODEFAULT        0x00000002U
#define SND_MEMORY           0x00000004U
#define SND_LOOP             0x00000008U
#define SND_NOSTOP           0x00000010U
#define SND_PURGE            0x00000040U
#define SND_APPLICATION      0x00000080U
#define SND_NOWAIT           0x00002000U
#define SND_ALIAS            0x00010000U
#define SND_ALIAS_ID         0x00110000U
#define SND_FILENAME         0x00020000U
#define SND_RESOURCE         0x00040004U
#define SND_SENTRY           0x00080000U
#define SND_RING             0x00100000U
#define SND_SYSTEM           0x00200000U

#define WINMM_OPEN_EXISTING  3U

#define CALLBACK_TYPEMASK    0x00070000U
#define CALLBACK_NULL        0x00000000U
#define CALLBACK_WINDOW      0x00010000U
#define CALLBACK_TASK        0x00020000U
#define CALLBACK_FUNCTION    0x00030000U
#define CALLBACK_THREAD      CALLBACK_TASK
#define CALLBACK_EVENT       0x00050000U

#define WOM_OPEN             0x03BBU
#define WOM_CLOSE            0x03BCU
#define WOM_DONE             0x03BDU

#define WHDR_DONE            0x00000001U
#define WHDR_PREPARED        0x00000002U
#define WHDR_BEGINLOOP       0x00000004U
#define WHDR_ENDLOOP         0x00000008U
#define WHDR_INQUEUE         0x00000010U

#define TIME_MS              0x0001U
#define TIME_SAMPLES         0x0002U
#define TIME_BYTES           0x0004U

#define WAVECAPS_VOLUME      0x0001U
#define WAVECAPS_LRVOLUME    0x0002U

#define AUX_MAPPER               0xFFFFFFFFU
#define AUXCAPS_AUXIN             2U
#define AUXCAPS_VOLUME       0x0001U
#define AUXCAPS_LRVOLUME     0x0002U

#define MIXERR_INVALLINE       1024U
#define MIXERR_INVALCONTROL    1025U
#define MIXERR_INVALVALUE      1026U

#define MIXER_OBJECTF_HANDLE      0x80000000U
#define MIXER_OBJECTF_TYPEMASK    0x70000000U
#define MIXER_OBJECTF_MASK        0xF0000000U
#define MIXER_OBJECTF_MIXER       0x00000000U
#define MIXER_OBJECTF_WAVEOUT     0x10000000U
#define MIXER_OBJECTF_WAVEIN      0x20000000U
#define MIXER_OBJECTF_MIDIOUT     0x30000000U
#define MIXER_OBJECTF_MIDIIN      0x40000000U
#define MIXER_OBJECTF_AUX         0x50000000U

#define MIXER_GETLINEINFOF_DESTINATION   0U
#define MIXER_GETLINEINFOF_SOURCE        1U
#define MIXER_GETLINEINFOF_LINEID        2U
#define MIXER_GETLINEINFOF_COMPONENTTYPE 3U
#define MIXER_GETLINEINFOF_TARGETTYPE    4U
#define MIXER_GETLINEINFOF_QUERYMASK    0xFU

#define MIXER_GETLINECONTROLSF_ALL        0U
#define MIXER_GETLINECONTROLSF_ONEBYID    1U
#define MIXER_GETLINECONTROLSF_ONEBYTYPE  2U
#define MIXER_GETLINECONTROLSF_QUERYMASK 0xFU

#define MIXER_GETCONTROLDETAILSF_VALUE     0U
#define MIXER_GETCONTROLDETAILSF_QUERYMASK 0xFU

#define MIXERLINE_LINEF_ACTIVE                    0x00000001U
#define MIXERLINE_COMPONENTTYPE_DST_SPEAKERS      0x00000004U
#define MIXERLINE_TARGETTYPE_WAVEOUT               1U
#define MIXERCONTROL_CONTROLTYPE_VOLUME       0x50030001U

#define WINMM_MIXER_LINE_ID       1U
#define WINMM_MIXER_CONTROL_ID    1U

#define MCIERR_BASE                    256U
#define MCIERR_INVALID_DEVICE_ID      (MCIERR_BASE + 1U)
#define MCIERR_UNRECOGNIZED_KEYWORD   (MCIERR_BASE + 3U)
#define MCIERR_UNRECOGNIZED_COMMAND   (MCIERR_BASE + 5U)
#define MCIERR_INVALID_DEVICE_NAME    (MCIERR_BASE + 7U)
#define MCIERR_OUT_OF_MEMORY          (MCIERR_BASE + 8U)
#define MCIERR_MISSING_COMMAND_STRING (MCIERR_BASE + 11U)
#define MCIERR_PARAM_OVERFLOW         (MCIERR_BASE + 12U)
#define MCIERR_MISSING_STRING_ARGUMENT (MCIERR_BASE + 13U)
#define MCIERR_BAD_INTEGER            (MCIERR_BASE + 14U)
#define MCIERR_MISSING_PARAMETER      (MCIERR_BASE + 17U)
#define MCIERR_UNSUPPORTED_FUNCTION   (MCIERR_BASE + 18U)
#define MCIERR_FILE_NOT_FOUND         (MCIERR_BASE + 19U)
#define MCIERR_DEVICE_NOT_READY       (MCIERR_BASE + 20U)
#define MCIERR_CANNOT_USE_ALL         (MCIERR_BASE + 23U)
#define MCIERR_EXTENSION_NOT_FOUND    (MCIERR_BASE + 25U)
#define MCIERR_OUTOFRANGE             (MCIERR_BASE + 26U)
#define MCIERR_FLAGS_NOT_COMPATIBLE   (MCIERR_BASE + 28U)
#define MCIERR_DUPLICATE_ALIAS        (MCIERR_BASE + 33U)
#define MCIERR_BAD_TIME_FORMAT        (MCIERR_BASE + 37U)
#define MCIERR_INVALID_FILE           (MCIERR_BASE + 40U)
#define MCIERR_NULL_PARAMETER_BLOCK   (MCIERR_BASE + 41U)
#define MCIERR_FILENAME_REQUIRED      (MCIERR_BASE + 48U)
#define MCIERR_EXTRA_CHARACTERS       (MCIERR_BASE + 49U)
#define MCIERR_DEVICE_NOT_INSTALLED   (MCIERR_BASE + 50U)

#define MCI_OPEN                 0x0803U
#define MCI_CLOSE                0x0804U
#define MCI_PLAY                 0x0806U
#define MCI_SEEK                 0x0807U
#define MCI_STOP                 0x0808U
#define MCI_PAUSE                0x0809U
#define MCI_GETDEVCAPS           0x080BU
#define MCI_SET                  0x080DU
#define MCI_STATUS               0x0814U
#define MCI_RESUME               0x0855U

#define MCI_ALL_DEVICE_ID        0xFFFFFFFFU
#define MCI_DEVTYPE_CD_AUDIO     516U
#define MCI_DEVTYPE_WAVEFORM_AUDIO 522U
#define MCI_MODE_NOT_READY       524U
#define MCI_MODE_STOP            525U
#define MCI_MODE_PLAY            526U
#define MCI_MODE_SEEK            528U
#define MCI_MODE_PAUSE           529U
#define MCI_MODE_OPEN            530U

#define MCI_FORMAT_MILLISECONDS  0U
#define MCI_FORMAT_BYTES         8U
#define MCI_FORMAT_SAMPLES       9U

#define MCI_NOTIFY               0x00000001U
#define MCI_WAIT                 0x00000002U
#define MCI_FROM                 0x00000004U
#define MCI_TO                   0x00000008U
#define MCI_TRACK                0x00000010U
#define MCI_OPEN_SHAREABLE       0x00000100U
#define MCI_OPEN_ELEMENT         0x00000200U
#define MCI_OPEN_ALIAS           0x00000400U
#define MCI_OPEN_ELEMENT_ID      0x00000800U
#define MCI_OPEN_TYPE_ID         0x00001000U
#define MCI_OPEN_TYPE            0x00002000U
#define MCI_SEEK_TO_START        0x00000100U
#define MCI_SEEK_TO_END          0x00000200U
#define MCI_STATUS_ITEM          0x00000100U
#define MCI_STATUS_START         0x00000200U
#define MCI_STATUS_LENGTH        1U
#define MCI_STATUS_POSITION      2U
#define MCI_STATUS_NUMBER_OF_TRACKS 3U
#define MCI_STATUS_MODE          4U
#define MCI_STATUS_MEDIA_PRESENT 5U
#define MCI_STATUS_TIME_FORMAT   6U
#define MCI_STATUS_READY         7U
#define MCI_STATUS_CURRENT_TRACK 8U
#define MCI_GETDEVCAPS_ITEM      0x00000100U
#define MCI_GETDEVCAPS_CAN_RECORD 1U
#define MCI_GETDEVCAPS_HAS_AUDIO 2U
#define MCI_GETDEVCAPS_HAS_VIDEO 3U
#define MCI_GETDEVCAPS_DEVICE_TYPE 4U
#define MCI_GETDEVCAPS_USES_FILES 5U
#define MCI_GETDEVCAPS_COMPOUND_DEVICE 6U
#define MCI_GETDEVCAPS_CAN_EJECT 7U
#define MCI_GETDEVCAPS_CAN_PLAY  8U
#define MCI_GETDEVCAPS_CAN_SAVE  9U
#define MCI_SET_TIME_FORMAT      0x00000400U

#define MM_MCINOTIFY             0x03B9U
#define MCI_NOTIFY_SUCCESSFUL    0x0001U
#define MCI_NOTIFY_ABORTED       0x0004U
#define MCI_NOTIFY_FAILURE       0x0008U

typedef struct __attribute__((packed)) {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_second;
    uint32_t average_bytes_per_second;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t extra_size;
} WINMM_WAVEFORMATEX;

typedef struct __attribute__((packed)) {
    uint32_t data;
    uint32_t buffer_length;
    uint32_t bytes_recorded;
    uint32_t user;
    uint32_t flags;
    uint32_t loops;
    uint32_t next;
    uint32_t reserved;
} WINMM_WAVEHDR32;

typedef struct {
    PVOID data;
    DWORD buffer_length;
    DWORD bytes_recorded;
    ULONG_PTR user;
    DWORD flags;
    DWORD loops;
    PVOID next;
    ULONG_PTR reserved;
} WINMM_WAVEHDR64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    DWORD device_id;
    uint32_t device_type;
    uint32_t element_name;
    uint32_t alias;
} WINMM_MCI_OPEN32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    DWORD device_id;
    DWORD padding;
    uint64_t device_type;
    uint64_t element_name;
    uint64_t alias;
} WINMM_MCI_OPEN64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    DWORD from;
    DWORD to;
} WINMM_MCI_PLAY32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    DWORD from;
    DWORD to;
} WINMM_MCI_PLAY64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    DWORD to;
} WINMM_MCI_SEEK32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    DWORD to;
    DWORD padding;
} WINMM_MCI_SEEK64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    uint32_t result;
    DWORD item;
    DWORD track;
} WINMM_MCI_STATUS32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    uint64_t result;
    DWORD item;
    DWORD track;
} WINMM_MCI_STATUS64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    uint32_t result;
    DWORD item;
} WINMM_MCI_GETDEVCAPS32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    uint64_t result;
    DWORD item;
    DWORD padding;
} WINMM_MCI_GETDEVCAPS64;

typedef struct __attribute__((packed)) {
    uint32_t callback;
    DWORD time_format;
    DWORD audio;
} WINMM_MCI_SET32;

typedef struct __attribute__((packed)) {
    uint64_t callback;
    DWORD time_format;
    DWORD audio;
} WINMM_MCI_SET64;

typedef struct __attribute__((packed)) {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    DWORD formats;
    WORD channels;
    WORD reserved;
    DWORD support;
} WINMM_WAVEOUTCAPSA;

typedef struct __attribute__((packed)) {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    WORD technology;
    WORD reserved;
    DWORD support;
} WINMM_AUXCAPSA;

typedef struct __attribute__((packed)) {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    WCHAR product_name[32];
    WORD technology;
    WORD reserved;
    DWORD support;
} WINMM_AUXCAPSW;

typedef struct __attribute__((packed)) {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    char product_name[32];
    DWORD support;
    DWORD destinations;
} WINMM_MIXERCAPSA;

typedef struct __attribute__((packed)) {
    WORD manufacturer_id;
    WORD product_id;
    DWORD driver_version;
    WCHAR product_name[32];
    DWORD support;
    DWORD destinations;
} WINMM_MIXERCAPSW;

#define WINMM_MIXER_LINE_FIELDS(user_type, char_type) \
    DWORD cb_struct;                               \
    DWORD destination;                             \
    DWORD source;                                  \
    DWORD line_id;                                 \
    DWORD flags;                                   \
    user_type user;                                \
    DWORD component_type;                          \
    DWORD channels;                                \
    DWORD connections;                             \
    DWORD controls;                                \
    char_type short_name[16];                      \
    char_type name[64];                            \
    DWORD target_type;                             \
    DWORD target_device_id;                        \
    WORD target_manufacturer_id;                   \
    WORD target_product_id;                        \
    DWORD target_driver_version;                   \
    char_type target_product_name[32]

typedef struct __attribute__((packed)) {
    WINMM_MIXER_LINE_FIELDS(uint32_t, char);
} WINMM_MIXERLINEA32;

typedef struct __attribute__((packed)) {
    WINMM_MIXER_LINE_FIELDS(uint64_t, char);
} WINMM_MIXERLINEA64;

typedef struct __attribute__((packed)) {
    WINMM_MIXER_LINE_FIELDS(uint32_t, WCHAR);
} WINMM_MIXERLINEW32;

typedef struct __attribute__((packed)) {
    WINMM_MIXER_LINE_FIELDS(uint64_t, WCHAR);
} WINMM_MIXERLINEW64;

#undef WINMM_MIXER_LINE_FIELDS

#define WINMM_MIXER_CONTROL_FIELDS(char_type) \
    DWORD cb_struct;                            \
    DWORD control_id;                           \
    DWORD control_type;                         \
    DWORD flags;                                \
    DWORD multiple_items;                       \
    char_type short_name[16];                   \
    char_type name[64];                         \
    DWORD bounds[6];                            \
    DWORD metrics[6]

typedef struct __attribute__((packed)) {
    WINMM_MIXER_CONTROL_FIELDS(char);
} WINMM_MIXERCONTROLA;

typedef struct __attribute__((packed)) {
    WINMM_MIXER_CONTROL_FIELDS(WCHAR);
} WINMM_MIXERCONTROLW;

#undef WINMM_MIXER_CONTROL_FIELDS

typedef struct __attribute__((packed)) {
    DWORD cb_struct;
    DWORD line_id;
    DWORD control_id_or_type;
    DWORD controls;
    DWORD control_size;
    uint32_t control_array;
} WINMM_MIXERLINECONTROLS32;

typedef struct __attribute__((packed)) {
    DWORD cb_struct;
    DWORD line_id;
    DWORD control_id_or_type;
    DWORD controls;
    DWORD control_size;
    uint64_t control_array;
} WINMM_MIXERLINECONTROLS64;

typedef struct __attribute__((packed)) {
    DWORD cb_struct;
    DWORD control_id;
    DWORD channels;
    DWORD multiple_items;
    DWORD detail_size;
    uint32_t details;
} WINMM_MIXERCONTROLDETAILS32;

typedef struct __attribute__((packed)) {
    DWORD cb_struct;
    DWORD control_id;
    DWORD channels;
    uint64_t owner_or_multiple_items;
    DWORD detail_size;
    uint64_t details;
} WINMM_MIXERCONTROLDETAILS64;

_Static_assert(sizeof(WINMM_AUXCAPSA) == 48, "AUXCAPSA ABI");
_Static_assert(sizeof(WINMM_AUXCAPSW) == 80, "AUXCAPSW ABI");
_Static_assert(sizeof(WINMM_MIXERCAPSA) == 48, "MIXERCAPSA ABI");
_Static_assert(sizeof(WINMM_MIXERCAPSW) == 80, "MIXERCAPSW ABI");
_Static_assert(sizeof(WINMM_MIXERLINEA32) == 168, "MIXERLINEA PE32 ABI");
_Static_assert(sizeof(WINMM_MIXERLINEA64) == 172, "MIXERLINEA PE64 ABI");
_Static_assert(sizeof(WINMM_MIXERLINEW32) == 280, "MIXERLINEW PE32 ABI");
_Static_assert(sizeof(WINMM_MIXERLINEW64) == 284, "MIXERLINEW PE64 ABI");
_Static_assert(sizeof(WINMM_MIXERCONTROLA) == 148, "MIXERCONTROLA ABI");
_Static_assert(sizeof(WINMM_MIXERCONTROLW) == 228, "MIXERCONTROLW ABI");
_Static_assert(sizeof(WINMM_MIXERLINECONTROLS32) == 24,
               "MIXERLINECONTROLS PE32 ABI");
_Static_assert(sizeof(WINMM_MIXERLINECONTROLS64) == 28,
               "MIXERLINECONTROLS PE64 ABI");
_Static_assert(sizeof(WINMM_MIXERCONTROLDETAILS32) == 24,
               "MIXERCONTROLDETAILS PE32 ABI");
_Static_assert(sizeof(WINMM_MIXERCONTROLDETAILS64) == 32,
               "MIXERCONTROLDETAILS PE64 ABI");
_Static_assert(sizeof(WINMM_MCI_OPEN32) == 20, "MCI_OPEN PE32 ABI");
_Static_assert(sizeof(WINMM_MCI_OPEN64) == 40, "MCI_OPEN PE64 ABI");
_Static_assert(sizeof(WINMM_MCI_PLAY32) == 12, "MCI_PLAY PE32 ABI");
_Static_assert(sizeof(WINMM_MCI_PLAY64) == 16, "MCI_PLAY PE64 ABI");
_Static_assert(sizeof(WINMM_MCI_STATUS32) == 16, "MCI_STATUS PE32 ABI");
_Static_assert(sizeof(WINMM_MCI_STATUS64) == 24, "MCI_STATUS PE64 ABI");

typedef struct {
    PVOID data;
    DWORD buffer_length;
    DWORD flags;
    DWORD loops;
} WINMM_WAVEHDR_VIEW;

typedef struct winmm_wave_buffer {
    struct winmm_wave_buffer *next;
    PVOID guest_header;
    BYTE *data;
    DWORD bytes;
    DWORD repeats_left;
    pcm_cursor_t cursor;
    BOOL completed;
} WINMM_WAVE_BUFFER;

typedef struct {
    BOOL in_use;
    BOOL compat32;
    BOOL paused;
    DWORD owner_pid;
    PVOID handle;
    pcm_format_t format;
    WORD block_align;
    PVOID callback;
    ULONG_PTR instance;
    DWORD callback_flags;
    DWORD volume;
    uint64_t completed_bytes;
    WINMM_WAVE_BUFFER *queue_head;
    WINMM_WAVE_BUFFER *queue_tail;
    WINMM_WAVE_BUFFER *current;
} WINMM_WAVE_OUT;

typedef struct {
    BOOL in_use;
    BOOL asynchronous;
    BOOL looping;
    BOOL completed;
    DWORD owner_pid;
    uint32_t token;
    BYTE *data;
    DWORD bytes;
    pcm_format_t format;
    pcm_cursor_t cursor;
} WINMM_PLAY_SOUND;

typedef struct {
    BOOL in_use;
    BOOL compat32;
    BOOL playing;
    BOOL paused;
    BOOL completed;
    BOOL notify_pending;
    DWORD owner_pid;
    UINT device_id;
    DWORD mode;
    DWORD time_format;
    ULONG_PTR notify_window;
    BYTE *data;
    DWORD bytes;
    DWORD play_end_frame;
    pcm_format_t format;
    pcm_cursor_t cursor;
    char alias[128];
} WINMM_MCI_DEVICE;

typedef struct {
    volatile LONG allocated;
    volatile LONG canceled;
    volatile LONG callback_running;
    UINT id;
    DWORD owner_pid;
    DWORD callback_tid;
    UINT delay_ms;
    UINT resolution_ms;
    PVOID callback;
    ULONG_PTR user;
    UINT flags;
    DWORD due_time;
    BOOL compat32;
} WINMM_TIMER;

typedef struct {
    volatile LONG state;
    volatile LONG stop;
    DWORD owner_pid;
    BOOL compat32;
    HANDLE worker;
    DWORD worker_tid;
} WINMM_TIMER_DISPATCHER;

typedef struct {
    DWORD owner_pid;
    UINT period_ms;
    UINT references;
} WINMM_PERIOD_REQUEST;

typedef struct {
    BOOL in_use;
    BOOL compat32;
    DWORD owner_pid;
    PVOID handle;
    PVOID callback;
    ULONG_PTR instance;
    DWORD callback_flags;
} WINMM_MIXER;

typedef struct {
    UINT wPeriodMin;
    UINT wPeriodMax;
} WINMM_TIMECAPS;

static WINMM_TIMER winmm_timers[WINMM_TIMER_SLOTS];
static WINMM_TIMER_DISPATCHER winmm_dispatchers[WINMM_DISPATCHER_SLOTS];
static WINMM_PERIOD_REQUEST winmm_periods[WINMM_PERIOD_SLOTS];
static WINMM_WAVE_OUT winmm_wave_outs[WINMM_WAVE_OUT_SLOTS];
static WINMM_PLAY_SOUND winmm_play_sounds[WINMM_PLAY_SOUND_SLOTS];
static WINMM_MCI_DEVICE winmm_mci_devices[WINMM_MCI_SLOTS];
static WINMM_MIXER winmm_mixers[WINMM_MIXER_SLOTS];
static int32_t winmm_wave_accumulator[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
static int16_t winmm_wave_output[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
static volatile uint32_t winmm_timer_lock;
static volatile uint32_t winmm_wave_transition_lock;
static volatile UINT winmm_next_timer_id = 1;
static volatile uint32_t winmm_next_wave_handle = 1;
static volatile uint32_t winmm_next_mixer_handle = 1;
static volatile uint32_t winmm_next_play_token = 1;
static volatile uint32_t winmm_next_mci_device_id = 1;
static volatile uint32_t winmm_callback_trace_count;
static volatile uint32_t winmm_unsupported_trace_count;
static volatile uint32_t winmm_play_sound_failure_trace_count;
static uint32_t winmm_dispatcher_thunk32;
static BOOL winmm_wave_mixer_running;
static audio_source_id_t winmm_wave_audio_source;

static uint64_t winmm_timer_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&winmm_timer_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
    return flags;
}

static void winmm_timer_unlock_irqrestore(uint64_t flags)
{
    __sync_lock_release(&winmm_timer_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void winmm_wave_transition_acquire(void)
{
    while (__sync_lock_test_and_set(&winmm_wave_transition_lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static void winmm_wave_transition_release(void)
{
    __sync_lock_release(&winmm_wave_transition_lock);
}

static BOOL winmm_wave_read_header(PVOID header, BOOL compat32,
                                   WINMM_WAVEHDR_VIEW *view)
{
    if (!header || !view)
        return FALSE;
    SIZE_T size = compat32 ? sizeof(WINMM_WAVEHDR32)
                           : sizeof(WINMM_WAVEHDR64);
    if (!win32_user_range_readable(header, size, compat32)) {
        serial_puts("[WINMM] rejected unreadable WAVEHDR\n");
        return FALSE;
    }
    memset(view, 0, sizeof(*view));
    if (compat32) {
        const WINMM_WAVEHDR32 *source = (const WINMM_WAVEHDR32 *)header;
        view->data = (PVOID)(ULONG_PTR)source->data;
        view->buffer_length = source->buffer_length;
        view->flags = source->flags;
        view->loops = source->loops;
    } else {
        const WINMM_WAVEHDR64 *source = (const WINMM_WAVEHDR64 *)header;
        view->data = source->data;
        view->buffer_length = source->buffer_length;
        view->flags = source->flags;
        view->loops = source->loops;
    }
    return TRUE;
}

static BOOL winmm_wave_set_header_flags(PVOID header, BOOL compat32,
                                        DWORD set_flags, DWORD clear_flags)
{
    if (!header)
        return FALSE;
    SIZE_T size = compat32 ? sizeof(WINMM_WAVEHDR32)
                           : sizeof(WINMM_WAVEHDR64);
    if (!win32_user_range_writable(header, size, compat32)) {
        serial_puts("[WINMM] rejected unwritable WAVEHDR\n");
        return FALSE;
    }
    DWORD *flags = compat32
        ? &((WINMM_WAVEHDR32 *)header)->flags
        : &((WINMM_WAVEHDR64 *)header)->flags;
    *flags = (*flags | set_flags) & ~clear_flags;
    return TRUE;
}

static BOOL winmm_wave_store_handle(PVOID output, BOOL compat32, PVOID handle)
{
    SIZE_T size = compat32 ? sizeof(uint32_t) : sizeof(PVOID);
    if (!win32_user_range_writable(output, size, compat32)) {
        serial_puts("[WINMM] rejected invalid handle output\n");
        return FALSE;
    }
    if (compat32)
        *(uint32_t *)output = (uint32_t)(ULONG_PTR)handle;
    else
        *(PVOID *)output = handle;
    return TRUE;
}

static BOOL winmm_wave_parse_format(const WINMM_WAVEFORMATEX *wave,
                                    pcm_format_t *format,
                                    WORD *block_align)
{
    if (!wave || !format || !block_align ||
        wave->format_tag != WAVE_FORMAT_PCM ||
        (wave->channels != 1 && wave->channels != 2) ||
        (wave->bits_per_sample != 8 && wave->bits_per_sample != 16) ||
        wave->samples_per_second < 1000U ||
        wave->samples_per_second > 192000U)
        return FALSE;

    WORD expected_align = (WORD)(wave->channels *
        (wave->bits_per_sample / 8U));
    DWORD expected_average = wave->samples_per_second * expected_align;
    if (wave->block_align != expected_align ||
        wave->average_bytes_per_second != expected_average)
        return FALSE;

    format->sample_rate = wave->samples_per_second;
    format->channels = wave->channels;
    format->sample_format = wave->bits_per_sample == 16
                          ? PCM_SAMPLE_S16_LE : PCM_SAMPLE_U8;
    *block_align = expected_align;
    return TRUE;
}

static WINMM_WAVE_OUT *winmm_wave_find_locked(PVOID handle,
                                               DWORD owner_pid)
{
    if (!handle || !owner_pid)
        return NULL;
    for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
        WINMM_WAVE_OUT *wave = &winmm_wave_outs[i];
        if (wave->in_use && wave->owner_pid == owner_pid &&
            wave->handle == handle)
            return wave;
    }
    return NULL;
}

static PVOID winmm_wave_allocate_handle_locked(void)
{
    for (;;) {
        uint32_t sequence = __atomic_fetch_add(&winmm_next_wave_handle, 1,
                                               __ATOMIC_RELAXED);
        ULONG_PTR value = 0x57000000U | (sequence & 0x00FFFFFFU);
        if (value == 0x57000000U)
            continue;
        PVOID candidate = (PVOID)value;
        BOOL used = FALSE;
        for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
            if (winmm_wave_outs[i].in_use &&
                winmm_wave_outs[i].handle == candidate) {
                used = TRUE;
                break;
            }
        }
        if (!used)
            return candidate;
    }
}

static uint32_t winmm_wave_gain_q16(WORD volume)
{
    return (uint32_t)(((uint64_t)volume * PCM_GAIN_UNITY + 32767U) /
                      65535U);
}

static void winmm_invoke_wave_callback(PVOID handle, UINT message,
                                       PVOID callback, ULONG_PTR instance,
                                       DWORD callback_flags, PVOID parameter,
                                       BOOL compat32)
{
    if (!callback)
        return;

    switch (callback_flags & CALLBACK_TYPEMASK) {
    case CALLBACK_FUNCTION:
        if (compat32) {
            uint32_t args[5] = {
                (uint32_t)(ULONG_PTR)handle,
                message,
                (uint32_t)instance,
                (uint32_t)(ULONG_PTR)parameter,
                0,
            };
            compat32_callback_args((uint32_t)(ULONG_PTR)callback, 5, args);
        } else {
            typedef void (WINAPI *WINMM_WAVE_PROC)(PVOID, UINT, ULONG_PTR,
                                                    ULONG_PTR, ULONG_PTR);
            ((WINMM_WAVE_PROC)callback)(handle, message, instance,
                (ULONG_PTR)parameter, 0);
        }
        break;
    case CALLBACK_WINDOW:
        PostMessageA((HWND)callback, message, (WPARAM)handle,
                     (LPARAM)parameter);
        break;
    case CALLBACK_THREAD:
        PostThreadMessageA((DWORD)(ULONG_PTR)callback, message,
                           (WPARAM)handle, (LPARAM)parameter);
        break;
    case CALLBACK_EVENT:
        SetEvent((HANDLE)callback);
        break;
    default:
        break;
    }
}

static BOOL winmm_wave_has_audio_locked(void)
{
    for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
        WINMM_WAVE_OUT *wave = &winmm_wave_outs[i];
        if (wave->in_use && !wave->paused && wave->current)
            return TRUE;
    }
    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (sound->in_use && !sound->completed)
            return TRUE;
    }
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        if (winmm_mci_devices[i].in_use && winmm_mci_devices[i].playing)
            return TRUE;
    }
    return FALSE;
}

static bool winmm_wave_audio_fill(int16_t *output, uint32_t frames)
{
    if (!output || !frames || frames > AUDIO_OUTPUT_BLOCK_FRAMES)
        return false;

    memset(winmm_wave_accumulator, 0,
           frames * 2U * sizeof(winmm_wave_accumulator[0]));
    BOOL active = FALSE;
    uint64_t irq_flags = winmm_timer_lock_irqsave();

    for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
        WINMM_WAVE_OUT *wave = &winmm_wave_outs[i];
        if (!wave->in_use || wave->paused || !wave->current)
            continue;

        active = TRUE;
        uint32_t output_offset = 0;
        while (wave->current && output_offset < frames) {
            WINMM_WAVE_BUFFER *buffer = wave->current;
            bool ended = false;
            uint32_t mixed = pcm_mix_circular_s16_stereo(
                winmm_wave_accumulator + output_offset * 2U,
                frames - output_offset, buffer->data, buffer->bytes,
                &wave->format, AUDIO_OUTPUT_RATE_HZ,
                winmm_wave_gain_q16((WORD)(wave->volume & 0xFFFFU)),
                winmm_wave_gain_q16((WORD)(wave->volume >> 16)),
                false, &buffer->cursor, &ended);
            output_offset += mixed;

            if (!ended)
                break;
            if (buffer->repeats_left > 1U) {
                buffer->repeats_left--;
                buffer->cursor.frame_q32 = 0;
                continue;
            }

            buffer->completed = TRUE;
            wave->completed_bytes += buffer->bytes;
            wave->current = buffer->next;
            if (!mixed && !wave->current)
                break;
        }
    }

    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (!sound->in_use || sound->completed)
            continue;

        active = TRUE;
        bool ended = false;
        (void)pcm_mix_circular_s16_stereo(
            winmm_wave_accumulator, frames, sound->data, sound->bytes,
            &sound->format, AUDIO_OUTPUT_RATE_HZ,
            PCM_GAIN_UNITY, PCM_GAIN_UNITY, sound->looping,
            &sound->cursor, &ended);
        if (ended)
            sound->completed = TRUE;
    }

    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (!device->in_use || !device->playing)
            continue;

        active = TRUE;
        uint32_t frame_bytes = pcm_frame_bytes(&device->format);
        uint32_t end_bytes = device->play_end_frame * frame_bytes;
        bool ended = false;
        (void)pcm_mix_circular_s16_stereo(
            winmm_wave_accumulator, frames, device->data, end_bytes,
            &device->format, AUDIO_OUTPUT_RATE_HZ,
            PCM_GAIN_UNITY, PCM_GAIN_UNITY, false,
            &device->cursor, &ended);
        if (ended) {
            device->playing = FALSE;
            device->paused = FALSE;
            device->completed = TRUE;
            device->mode = MCI_MODE_STOP;
        }
    }

    if (!active)
        winmm_wave_mixer_running = FALSE;
    winmm_timer_unlock_irqrestore(irq_flags);

    if (!active) {
        memset(output, 0, frames * 2U * sizeof(output[0]));
        return false;
    }

    for (uint32_t i = 0; i < frames * 2U; i++) {
        int32_t sample = winmm_wave_accumulator[i];
        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        output[i] = (int16_t)sample;
    }
    return true;
}

static audio_register_result_t winmm_wave_refresh_mixer(void)
{
    audio_register_result_t result = AUDIO_REGISTER_OK;
    winmm_wave_transition_acquire();

    uint64_t irq_flags = winmm_timer_lock_irqsave();
    BOOL active = winmm_wave_has_audio_locked();
    BOOL running = winmm_wave_mixer_running;
    audio_source_id_t source = winmm_wave_audio_source;
    winmm_timer_unlock_irqrestore(irq_flags);

    if (active && !running) {
        source = AUDIO_SOURCE_INVALID;
        result = audio_source_register_ex(
            winmm_wave_audio_fill, winmm_wave_output,
            AUDIO_OUTPUT_BLOCK_FRAMES, AUDIO_OUTPUT_RATE_HZ, &source);
        BOOL started = result == AUDIO_REGISTER_OK;
        irq_flags = winmm_timer_lock_irqsave();
        winmm_wave_mixer_running = started;
        winmm_wave_audio_source = started ? source : AUDIO_SOURCE_INVALID;
        active = winmm_wave_has_audio_locked();
        winmm_timer_unlock_irqrestore(irq_flags);
        if (started && !active) {
            audio_source_unregister(source);
            irq_flags = winmm_timer_lock_irqsave();
            winmm_wave_mixer_running = FALSE;
            winmm_wave_audio_source = AUDIO_SOURCE_INVALID;
            winmm_timer_unlock_irqrestore(irq_flags);
        }
        if (!active)
            result = AUDIO_REGISTER_OK;
    } else if (!active && running) {
        audio_source_unregister(source);
        irq_flags = winmm_timer_lock_irqsave();
        winmm_wave_mixer_running = FALSE;
        winmm_wave_audio_source = AUDIO_SOURCE_INVALID;
        winmm_timer_unlock_irqrestore(irq_flags);
    }

    winmm_wave_transition_release();
    return result;
}

static BOOL winmm_wave_unlink_buffer_locked(WINMM_WAVE_OUT *wave,
                                             WINMM_WAVE_BUFFER *target)
{
    WINMM_WAVE_BUFFER *previous = NULL;
    WINMM_WAVE_BUFFER *buffer = wave ? wave->queue_head : NULL;
    while (buffer && buffer != target) {
        previous = buffer;
        buffer = buffer->next;
    }
    if (!buffer)
        return FALSE;

    if (previous)
        previous->next = buffer->next;
    else
        wave->queue_head = buffer->next;
    if (wave->queue_tail == buffer)
        wave->queue_tail = previous;
    if (wave->current == buffer)
        wave->current = buffer->next;
    buffer->next = NULL;
    return TRUE;
}

static UINT winmm_wave_registration_mmresult(audio_register_result_t result)
{
    serial_puts("[WINMM] wave output rejected: ");
    serial_puts(audio_register_result_name(result));
    serial_puts("\n");

    switch (result) {
    case AUDIO_REGISTER_NO_BACKEND:
        return MMSYSERR_NODRIVER;
    case AUDIO_REGISTER_NO_SLOTS:
        return MMSYSERR_ALLOCATED;
    case AUDIO_REGISTER_INVALID:
        return MMSYSERR_INVALPARAM;
    case AUDIO_REGISTER_NO_CLOCK:
    case AUDIO_REGISTER_SOURCE_INACTIVE:
    default:
        return MMSYSERR_ERROR;
    }
}

static BOOL winmm_wave_dispatch_completions(DWORD owner_pid)
{
    BOOL dispatched = FALSE;
    for (int completed = 0; completed < 64; completed++) {
        WINMM_WAVE_BUFFER *buffer = NULL;
        PVOID handle = NULL;
        PVOID callback = NULL;
        ULONG_PTR instance = 0;
        DWORD callback_flags = 0;
        BOOL compat32 = FALSE;

        uint64_t irq_flags = winmm_timer_lock_irqsave();
        for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
            WINMM_WAVE_OUT *wave = &winmm_wave_outs[i];
            if (!wave->in_use || wave->owner_pid != owner_pid ||
                !wave->queue_head || !wave->queue_head->completed)
                continue;
            buffer = wave->queue_head;
            wave->queue_head = buffer->next;
            if (!wave->queue_head)
                wave->queue_tail = NULL;
            handle = wave->handle;
            callback = wave->callback;
            instance = wave->instance;
            callback_flags = wave->callback_flags;
            compat32 = wave->compat32;
            break;
        }
        winmm_timer_unlock_irqrestore(irq_flags);

        if (!buffer)
            break;
        if (winmm_wave_set_header_flags(buffer->guest_header, compat32,
                                        WHDR_DONE, WHDR_INQUEUE)) {
            winmm_invoke_wave_callback(handle, WOM_DONE, callback, instance,
                                       callback_flags, buffer->guest_header,
                                       compat32);
        } else {
            serial_puts("[WINMM] dropped completion for stale WAVEHDR\n");
        }
        kfree(buffer->data);
        kfree(buffer);
        dispatched = TRUE;
    }
    return dispatched;
}

static BOOL winmm_play_sound_dispatch_completions(DWORD owner_pid)
{
    BYTE *released[WINMM_PLAY_SOUND_SLOTS];
    int released_count = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (!sound->in_use || sound->owner_pid != owner_pid ||
            !sound->asynchronous || !sound->completed)
            continue;
        released[released_count++] = sound->data;
        memset(sound, 0, sizeof(*sound));
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    if (!released_count)
        return FALSE;
    winmm_wave_refresh_mixer();
    for (int i = 0; i < released_count; i++)
        kfree(released[i]);
    return TRUE;
}

static BOOL winmm_mci_dispatch_completions(DWORD owner_pid)
{
    ULONG_PTR windows[WINMM_MCI_SLOTS];
    UINT device_ids[WINMM_MCI_SLOTS];
    int count = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (!device->in_use || device->owner_pid != owner_pid ||
            !device->completed || !device->notify_pending)
            continue;
        windows[count] = device->notify_window;
        device_ids[count] = device->device_id;
        count++;
        device->completed = FALSE;
        device->notify_pending = FALSE;
        device->notify_window = 0;
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    for (int i = 0; i < count; i++) {
        if (windows[i])
            PostMessageA((HWND)windows[i], MM_MCINOTIFY,
                         MCI_NOTIFY_SUCCESSFUL, device_ids[i]);
    }
    return count != 0;
}

static BOOL winmm_time_reached(DWORD now, DWORD due_time)
{
    return (LONG)(now - due_time) >= 0;
}

static void winmm_timer_clear_locked(WINMM_TIMER *timer)
{
    timer->canceled = 0;
    timer->callback_running = 0;
    timer->id = 0;
    timer->owner_pid = 0;
    timer->callback_tid = 0;
    timer->delay_ms = 0;
    timer->resolution_ms = 0;
    timer->callback = NULL;
    timer->user = 0;
    timer->flags = 0;
    timer->due_time = 0;
    timer->compat32 = FALSE;
    __atomic_store_n(&timer->allocated, 0, __ATOMIC_RELEASE);
}

static WINMM_TIMER *winmm_timer_find_locked(DWORD owner_pid, UINT timer_id)
{
    for (int i = 0; i < WINMM_TIMER_SLOTS; i++) {
        WINMM_TIMER *timer = &winmm_timers[i];
        if (__atomic_load_n(&timer->allocated, __ATOMIC_ACQUIRE) &&
            timer->owner_pid == owner_pid && timer->id == timer_id)
            return timer;
    }
    return NULL;
}

static void winmm_invoke_timer_callback(UINT timer_id, PVOID callback,
                                        ULONG_PTR user, UINT flags,
                                        BOOL compat32)
{
    UINT callback_type = flags & TIME_CALLBACK_TYPEMASK;
    if (callback_type == TIME_CALLBACK_EVENT_SET) {
        SetEvent((HANDLE)callback);
        return;
    }
    if (callback_type == TIME_CALLBACK_EVENT_PULSE) {
        PulseEvent((HANDLE)callback);
        return;
    }

    uint32_t trace = __atomic_fetch_add(&winmm_callback_trace_count, 1,
                                        __ATOMIC_RELAXED);
    if (trace < 24U) {
        serial_puts("[WINMM-TIMER] callback id=");
        serial_putdec(timer_id);
        serial_puts(" user=0x");
        serial_puthex((uint64_t)user, compat32 ? 8 : 16);
        serial_puts("\n");
    } else if (trace == 24U) {
        serial_puts("[WINMM-TIMER] callback trace suppressed\n");
    }

    if (compat32) {
        uint32_t args[5] = {
            timer_id,
            0,
            (uint32_t)user,
            0,
            0,
        };
        compat32_callback_args((uint32_t)(ULONG_PTR)callback, 5, args);
    } else {
        typedef void (WINAPI *WINMM_TIMER_PROC)(UINT, UINT, ULONG_PTR,
                                                ULONG_PTR, ULONG_PTR);
        ((WINMM_TIMER_PROC)callback)(timer_id, 0, user, 0, 0);
    }
}

static DWORD WINAPI winmm_timer_dispatcher(PVOID parameter)
{
    ULONG_PTR encoded = (ULONG_PTR)parameter;
    if (!encoded || encoded > WINMM_DISPATCHER_SLOTS)
        return TIMERR_NOCANDO;

    WINMM_TIMER_DISPATCHER *dispatcher =
        &winmm_dispatchers[encoded - 1U];
    while (__atomic_load_n(&dispatcher->state, __ATOMIC_ACQUIRE) ==
           WINMM_DISPATCHER_STARTING)
        Sleep(1);
    if (__atomic_load_n(&dispatcher->state, __ATOMIC_ACQUIRE) !=
        WINMM_DISPATCHER_RUNNING)
        return TIMERR_NOCANDO;

    dispatcher->worker_tid = GetCurrentThreadId();
    serial_puts("[WINMM-TIMER] dispatcher start pid=");
    serial_putdec(dispatcher->owner_pid);
    serial_puts(dispatcher->compat32 ? " PE32\n" : " PE64\n");

    while (!__atomic_load_n(&dispatcher->stop, __ATOMIC_ACQUIRE)) {
        BOOL dispatched = FALSE;
        DWORD now = shim_timeGetTime();

        for (int i = 0; i < WINMM_TIMER_SLOTS; i++) {
            UINT timer_id = 0;
            PVOID callback = NULL;
            ULONG_PTR user = 0;
            UINT timer_flags = 0;
            BOOL compat32 = FALSE;

            uint64_t irq_flags = winmm_timer_lock_irqsave();
            WINMM_TIMER *timer = &winmm_timers[i];
            if (!__atomic_load_n(&timer->allocated, __ATOMIC_ACQUIRE) ||
                timer->owner_pid != dispatcher->owner_pid) {
                winmm_timer_unlock_irqrestore(irq_flags);
                continue;
            }
            if (__atomic_load_n(&timer->canceled, __ATOMIC_ACQUIRE)) {
                if (!__atomic_load_n(&timer->callback_running,
                                     __ATOMIC_ACQUIRE))
                    winmm_timer_clear_locked(timer);
                winmm_timer_unlock_irqrestore(irq_flags);
                continue;
            }
            if (!winmm_time_reached(now, timer->due_time)) {
                winmm_timer_unlock_irqrestore(irq_flags);
                continue;
            }

            timer_id = timer->id;
            callback = timer->callback;
            user = timer->user;
            timer_flags = timer->flags;
            compat32 = timer->compat32;
            timer->callback_tid = dispatcher->worker_tid;
            __atomic_store_n(&timer->callback_running, 1,
                             __ATOMIC_RELEASE);

            if (timer_flags & TIME_PERIODIC) {
                DWORD elapsed = now - timer->due_time;
                uint64_t periods = (uint64_t)elapsed / timer->delay_ms + 1U;
                timer->due_time += (DWORD)(periods * timer->delay_ms);
            } else {
                __atomic_store_n(&timer->canceled, 1, __ATOMIC_RELEASE);
            }
            winmm_timer_unlock_irqrestore(irq_flags);

            if (!__atomic_load_n(&timer->canceled, __ATOMIC_ACQUIRE) ||
                !(timer_flags & TIME_PERIODIC)) {
                winmm_invoke_timer_callback(timer_id, callback, user,
                                             timer_flags, compat32);
            }

            irq_flags = winmm_timer_lock_irqsave();
            timer = &winmm_timers[i];
            if (__atomic_load_n(&timer->allocated, __ATOMIC_ACQUIRE) &&
                timer->owner_pid == dispatcher->owner_pid &&
                timer->id == timer_id) {
                __atomic_store_n(&timer->callback_running, 0,
                                 __ATOMIC_RELEASE);
                timer->callback_tid = 0;
                if (__atomic_load_n(&timer->canceled, __ATOMIC_ACQUIRE))
                    winmm_timer_clear_locked(timer);
            }
            winmm_timer_unlock_irqrestore(irq_flags);
            dispatched = TRUE;
        }

        if (winmm_wave_dispatch_completions(dispatcher->owner_pid))
            dispatched = TRUE;
        if (winmm_play_sound_dispatch_completions(dispatcher->owner_pid))
            dispatched = TRUE;
        if (winmm_mci_dispatch_completions(dispatcher->owner_pid))
            dispatched = TRUE;

        if (!dispatched)
            Sleep(1);
    }

    serial_puts("[WINMM-TIMER] dispatcher stop pid=");
    serial_putdec(dispatcher->owner_pid);
    serial_puts("\n");
    __atomic_store_n(&dispatcher->state, WINMM_DISPATCHER_STOPPED,
                     __ATOMIC_RELEASE);
    return TIMERR_NOERROR;
}

static WINMM_TIMER_DISPATCHER *winmm_get_dispatcher(DWORD owner_pid,
                                                     BOOL compat32)
{
    WINMM_TIMER_DISPATCHER *dispatcher = NULL;
    int slot = -1;
    BOOL initialize = FALSE;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_DISPATCHER_SLOTS; i++) {
        if (winmm_dispatchers[i].state != WINMM_DISPATCHER_FREE &&
            winmm_dispatchers[i].owner_pid == owner_pid) {
            dispatcher = &winmm_dispatchers[i];
            slot = i;
            break;
        }
        if (slot < 0 &&
            winmm_dispatchers[i].state == WINMM_DISPATCHER_FREE)
            slot = i;
    }

    if (!dispatcher && slot >= 0) {
        dispatcher = &winmm_dispatchers[slot];
        memset(dispatcher, 0, sizeof(*dispatcher));
        dispatcher->owner_pid = owner_pid;
        dispatcher->compat32 = compat32;
        dispatcher->state = WINMM_DISPATCHER_STARTING;
        initialize = TRUE;
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    if (!dispatcher)
        return NULL;
    if (!initialize) {
        while (__atomic_load_n(&dispatcher->state, __ATOMIC_ACQUIRE) ==
               WINMM_DISPATCHER_STARTING)
            Sleep(1);
        return dispatcher->state == WINMM_DISPATCHER_RUNNING
             ? dispatcher : NULL;
    }

    LPTHREAD_START_ROUTINE start = winmm_timer_dispatcher;
    if (compat32) {
        if (!winmm_dispatcher_thunk32 && compat32_is_initialized()) {
            winmm_dispatcher_thunk32 = compat32_make_thunk_ex(
                (uint64_t)(ULONG_PTR)winmm_timer_dispatcher,
                "winmm!timer_dispatcher", 1, CC_STDCALL);
        }
        if (!winmm_dispatcher_thunk32) {
            dispatcher->state = WINMM_DISPATCHER_FREE;
            dispatcher->owner_pid = 0;
            return NULL;
        }
        start = (LPTHREAD_START_ROUTINE)(ULONG_PTR)winmm_dispatcher_thunk32;
    }

    dispatcher->worker = CreateThread(
        NULL, 0, start, (PVOID)(ULONG_PTR)(slot + 1), 0,
        &dispatcher->worker_tid);
    if (!dispatcher->worker) {
        dispatcher->owner_pid = 0;
        dispatcher->worker_tid = 0;
        dispatcher->state = WINMM_DISPATCHER_FREE;
        return NULL;
    }
    __atomic_store_n(&dispatcher->state, WINMM_DISPATCHER_RUNNING,
                     __ATOMIC_RELEASE);
    return dispatcher;
}

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
    uint64_t freq = idt_get_tsc_freq();
    if (!freq) freq = 3000000000ULL;
    DWORD result = (DWORD)(tsc / (freq / 1000));

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
    if (period < WINMM_PERIOD_MIN_MS || period > WINMM_PERIOD_MAX_MS)
        return TIMERR_NOCANDO;

    DWORD owner_pid = GetCurrentProcessId();
    int free_slot = -1;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_PERIOD_SLOTS; i++) {
        WINMM_PERIOD_REQUEST *request = &winmm_periods[i];
        if (!request->references) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (request->owner_pid == owner_pid &&
            request->period_ms == period) {
            request->references++;
            winmm_timer_unlock_irqrestore(irq_flags);
            return TIMERR_NOERROR;
        }
    }
    if (free_slot >= 0) {
        winmm_periods[free_slot].owner_pid = owner_pid;
        winmm_periods[free_slot].period_ms = period;
        winmm_periods[free_slot].references = 1;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    return free_slot >= 0 ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

UINT WINAPI shim_timeEndPeriod(UINT period)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_PERIOD_SLOTS; i++) {
        WINMM_PERIOD_REQUEST *request = &winmm_periods[i];
        if (request->references && request->owner_pid == owner_pid &&
            request->period_ms == period) {
            if (!--request->references)
                memset(request, 0, sizeof(*request));
            winmm_timer_unlock_irqrestore(irq_flags);
            return TIMERR_NOERROR;
        }
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    return TIMERR_NOCANDO;
}

UINT WINAPI shim_timeGetDevCaps(PVOID capabilities, UINT size)
{
    if (!capabilities || size < sizeof(WINMM_TIMECAPS))
        return TIMERR_STRUCT;
    WINMM_TIMECAPS *caps = (WINMM_TIMECAPS *)capabilities;
    caps->wPeriodMin = WINMM_PERIOD_MIN_MS;
    caps->wPeriodMax = WINMM_PERIOD_MAX_MS;
    return TIMERR_NOERROR;
}

UINT WINAPI shim_timeSetEvent(UINT delay, UINT resolution, PVOID callback,
                              ULONG_PTR user, UINT flags)
{
    UINT callback_type = flags & TIME_CALLBACK_TYPEMASK;
    if (delay < WINMM_PERIOD_MIN_MS || delay > WINMM_PERIOD_MAX_MS ||
        !callback || (flags & ~TIME_VALID_FLAGS) ||
        callback_type == TIME_CALLBACK_TYPEMASK)
        return 0;

    DWORD owner_pid = GetCurrentProcessId();
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!winmm_get_dispatcher(owner_pid, compat32))
        return 0;

    WINMM_TIMER *timer = NULL;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_TIMER_SLOTS; i++) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&winmm_timers[i].allocated,
                                        &expected, 1, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            timer = &winmm_timers[i];
            break;
        }
    }
    if (!timer) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return 0;
    }

    UINT timer_id;
    do {
        timer_id = __atomic_fetch_add(&winmm_next_timer_id, 1,
                                      __ATOMIC_RELAXED);
        if (!timer_id)
            timer_id = __atomic_fetch_add(&winmm_next_timer_id, 1,
                                          __ATOMIC_RELAXED);
    } while (!timer_id || winmm_timer_find_locked(owner_pid, timer_id));

    timer->canceled = 0;
    timer->callback_running = 0;
    timer->id = timer_id;
    timer->owner_pid = owner_pid;
    timer->callback_tid = 0;
    timer->delay_ms = delay;
    timer->resolution_ms = resolution;
    timer->callback = callback;
    timer->user = user;
    timer->flags = flags;
    timer->due_time = shim_timeGetTime() + delay;
    timer->compat32 = compat32;
    winmm_timer_unlock_irqrestore(irq_flags);

    serial_puts("[WINMM-TIMER] set id=");
    serial_putdec(timer_id);
    serial_puts(" pid=");
    serial_putdec(owner_pid);
    serial_puts(" delay=");
    serial_putdec(delay);
    serial_puts(" resolution=");
    serial_putdec(resolution);
    serial_puts(" flags=0x");
    serial_puthex(flags, 4);
    serial_puts(" callback=0x");
    serial_puthex((uint64_t)(ULONG_PTR)callback, compat32 ? 8 : 16);
    serial_puts("\n");
    return timer_id;
}

UINT WINAPI shim_timeKillEvent(UINT timerID)
{
    DWORD owner_pid = GetCurrentProcessId();
    DWORD caller_tid = GetCurrentThreadId();
    BOOL synchronous = FALSE;
    BOOL running = FALSE;

    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_TIMER *timer = winmm_timer_find_locked(owner_pid, timerID);
    if (!timer) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return TIMERR_NOCANDO;
    }

    synchronous = (timer->flags & TIME_KILL_SYNCHRONOUS) != 0;
    running = __atomic_load_n(&timer->callback_running,
                              __ATOMIC_ACQUIRE) != 0;
    __atomic_store_n(&timer->canceled, 1, __ATOMIC_RELEASE);
    if (!running)
        winmm_timer_clear_locked(timer);
    else if (timer->callback_tid == caller_tid)
        synchronous = FALSE;
    winmm_timer_unlock_irqrestore(irq_flags);

    while (synchronous) {
        irq_flags = winmm_timer_lock_irqsave();
        timer = winmm_timer_find_locked(owner_pid, timerID);
        running = timer && __atomic_load_n(&timer->callback_running,
                                           __ATOMIC_ACQUIRE);
        winmm_timer_unlock_irqrestore(irq_flags);
        if (!running)
            break;
        Sleep(1);
    }

    serial_puts("[WINMM-TIMER] kill id=");
    serial_putdec(timerID);
    serial_puts(synchronous ? " synchronized\n" : "\n");
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

static void winmm_trace_unsupported(const char *api)
{
    uint32_t trace = __atomic_fetch_add(&winmm_unsupported_trace_count, 1U,
                                        __ATOMIC_RELAXED);
    if (trace < 16U) {
        serial_puts("[WINMM] unsupported: ");
        serial_puts(api);
        serial_puts("\n");
    } else if (trace == 16U) {
        serial_puts("[WINMM] unsupported trace suppressed\n");
    }
}

typedef struct {
    const BYTE *data;
    DWORD bytes;
    pcm_format_t format;
} WINMM_PARSED_WAVE;

typedef enum {
    WINMM_WAVE_LOAD_OK = 0,
    WINMM_WAVE_LOAD_NOT_FOUND,
    WINMM_WAVE_LOAD_INVALID,
    WINMM_WAVE_LOAD_NO_MEMORY,
} WINMM_WAVE_LOAD_RESULT;

static WORD winmm_read_le16(const BYTE *data)
{
    return (WORD)((WORD)data[0] | ((WORD)data[1] << 8));
}

static DWORD winmm_read_le32(const BYTE *data)
{
    return (DWORD)data[0] | ((DWORD)data[1] << 8) |
           ((DWORD)data[2] << 16) | ((DWORD)data[3] << 24);
}

static BOOL winmm_fourcc_is(const BYTE *data, char a, char b, char c, char d)
{
    return data[0] == (BYTE)a && data[1] == (BYTE)b &&
           data[2] == (BYTE)c && data[3] == (BYTE)d;
}

static void winmm_trace_play_sound_failure(const char *reason)
{
    uint32_t trace = __atomic_fetch_add(&winmm_play_sound_failure_trace_count,
                                        1U, __ATOMIC_RELAXED);
    if (trace < 16U) {
        serial_puts("[WINMM] PlaySound rejected: ");
        serial_puts(reason);
        serial_puts("\n");
    } else if (trace == 16U) {
        serial_puts("[WINMM] PlaySound rejection trace suppressed\n");
    }
}

static BOOL winmm_wave_memory_size(const BYTE *image, DWORD *image_size)
{
    if (!image || !image_size ||
        !winmm_fourcc_is(image, 'R', 'I', 'F', 'F') ||
        !winmm_fourcc_is(image + 8, 'W', 'A', 'V', 'E'))
        return FALSE;

    DWORD riff_size = winmm_read_le32(image + 4);
    if (riff_size < 4U || riff_size > 0xFFFFFFFFU - 8U)
        return FALSE;
    *image_size = riff_size + 8U;
    return *image_size >= 12U;
}

static BOOL winmm_parse_wave_image(const BYTE *image, DWORD image_size,
                                    WINMM_PARSED_WAVE *parsed)
{
    if (!image || !parsed || image_size < 12U ||
        !winmm_fourcc_is(image, 'R', 'I', 'F', 'F') ||
        !winmm_fourcc_is(image + 8, 'W', 'A', 'V', 'E'))
        return FALSE;

    uint64_t riff_end = (uint64_t)winmm_read_le32(image + 4) + 8U;
    if (riff_end < 12U || riff_end > image_size)
        return FALSE;

    BOOL have_format = FALSE;
    const BYTE *wave_data = NULL;
    DWORD wave_bytes = 0;
    pcm_format_t format = {0};
    WORD block_align = 0;
    uint64_t offset = 12U;

    while (offset + 8U <= riff_end) {
        const BYTE *chunk = image + offset;
        DWORD chunk_size = winmm_read_le32(chunk + 4);
        uint64_t payload = offset + 8U;
        if ((uint64_t)chunk_size > riff_end - payload)
            return FALSE;

        if (winmm_fourcc_is(chunk, 'f', 'm', 't', ' ')) {
            if (chunk_size < 16U)
                return FALSE;
            const BYTE *value = image + payload;
            WINMM_WAVEFORMATEX wave = {
                .format_tag = winmm_read_le16(value),
                .channels = winmm_read_le16(value + 2),
                .samples_per_second = winmm_read_le32(value + 4),
                .average_bytes_per_second = winmm_read_le32(value + 8),
                .block_align = winmm_read_le16(value + 12),
                .bits_per_sample = winmm_read_le16(value + 14),
                .extra_size = chunk_size >= 18U
                            ? winmm_read_le16(value + 16) : 0,
            };
            if (!winmm_wave_parse_format(&wave, &format, &block_align))
                return FALSE;
            have_format = TRUE;
        } else if (winmm_fourcc_is(chunk, 'd', 'a', 't', 'a') &&
                   !wave_data && chunk_size) {
            wave_data = image + payload;
            wave_bytes = chunk_size;
        }

        offset = payload + chunk_size;
        if ((chunk_size & 1U) && offset < riff_end)
            offset++;
    }

    if (!have_format || !wave_data || !wave_bytes || !block_align ||
        wave_bytes % block_align)
        return FALSE;
    parsed->data = wave_data;
    parsed->bytes = wave_bytes;
    parsed->format = format;
    return TRUE;
}

static WINMM_WAVE_LOAD_RESULT winmm_copy_wave_image_result(
    const BYTE *image, DWORD image_size, BYTE **data, DWORD *bytes,
    pcm_format_t *format)
{
    if (!data || !bytes || !format)
        return WINMM_WAVE_LOAD_INVALID;
    *data = NULL;
    *bytes = 0;

    WINMM_PARSED_WAVE parsed;
    if (!winmm_parse_wave_image(image, image_size, &parsed))
        return WINMM_WAVE_LOAD_INVALID;
    BYTE *copy = (BYTE *)kmalloc(parsed.bytes);
    if (!copy)
        return WINMM_WAVE_LOAD_NO_MEMORY;
    memcpy(copy, parsed.data, parsed.bytes);
    *data = copy;
    *bytes = parsed.bytes;
    *format = parsed.format;
    return WINMM_WAVE_LOAD_OK;
}

static BOOL winmm_copy_wave_image(const BYTE *image, DWORD image_size,
                                  BYTE **data, DWORD *bytes,
                                  pcm_format_t *format)
{
    return winmm_copy_wave_image_result(image, image_size, data, bytes,
                                        format) == WINMM_WAVE_LOAD_OK;
}

static BOOL winmm_sound_name_valid(PCVOID name, BOOL wide)
{
    if (!name || (ULONG_PTR)name <= 0xFFFFU)
        return FALSE;
    if (wide) {
        const WCHAR *value = (const WCHAR *)name;
        for (UINT i = 0; i < 256U; i++) {
            if (!value[i])
                return TRUE;
        }
    } else {
        const char *value = (const char *)name;
        for (UINT i = 0; i < 256U; i++) {
            if (!value[i])
                return TRUE;
        }
    }
    return FALSE;
}

static BOOL winmm_copy_string(char *destination, UINT capacity,
                              const char *source)
{
    if (!destination || !capacity || !source)
        return FALSE;
    UINT length = 0;
    while (source[length]) {
        if (length + 1U >= capacity)
            return FALSE;
        destination[length] = source[length];
        length++;
    }
    destination[length] = 0;
    return TRUE;
}

static WINMM_WAVE_LOAD_RESULT winmm_prepare_wave_file_direct(
    PCVOID file_name, BOOL wide, BYTE **data, DWORD *bytes,
    pcm_format_t *format)
{
    if (!winmm_sound_name_valid(file_name, wide))
        return WINMM_WAVE_LOAD_INVALID;
    HANDLE file = wide
        ? CreateFileW((PCWSTR)file_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                      WINMM_OPEN_EXISTING, 0, NULL)
        : CreateFileA((PCSTR)file_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                      WINMM_OPEN_EXISTING, 0, NULL);
    if (!file || file == INVALID_HANDLE_VALUE)
        return WINMM_WAVE_LOAD_NOT_FOUND;

    DWORD high_size = 0;
    DWORD file_size = GetFileSize(file, &high_size);
    if (high_size || file_size == 0xFFFFFFFFU || file_size < 12U) {
        CloseHandle(file);
        return WINMM_WAVE_LOAD_INVALID;
    }

    BYTE *image = (BYTE *)kmalloc(file_size);
    if (!image) {
        CloseHandle(file);
        return WINMM_WAVE_LOAD_NO_MEMORY;
    }

    DWORD total = 0;
    BOOL read_ok = TRUE;
    while (total < file_size) {
        DWORD transferred = 0;
        if (!ReadFile(file, image + total, file_size - total, &transferred,
                      NULL) || !transferred) {
            read_ok = FALSE;
            break;
        }
        total += transferred;
    }
    CloseHandle(file);

    WINMM_WAVE_LOAD_RESULT result = read_ok && total == file_size
        ? winmm_copy_wave_image_result(image, file_size, data, bytes, format)
        : WINMM_WAVE_LOAD_INVALID;
    kfree(image);
    return result;
}

static BOOL winmm_copy_sound_name_a(PCVOID file_name, BOOL wide,
                                     char *copy, UINT capacity)
{
    if (!winmm_sound_name_valid(file_name, wide) || !copy || !capacity)
        return FALSE;

    UINT length = 0;
    if (wide) {
        const WCHAR *source = (const WCHAR *)file_name;
        while (source[length]) {
            if (length + 1U >= capacity || source[length] > 0xFFU)
                return FALSE;
            copy[length] = (char)source[length];
            length++;
        }
    } else {
        const char *source = (const char *)file_name;
        while (source[length]) {
            if (length + 1U >= capacity)
                return FALSE;
            copy[length] = source[length];
            length++;
        }
    }
    copy[length] = 0;
    return length != 0;
}

static BOOL winmm_sound_name_has_path(const char *file_name)
{
    while (*file_name) {
        if (*file_name == '\\' || *file_name == '/' || *file_name == ':')
            return TRUE;
        file_name++;
    }
    return FALSE;
}

static WINMM_WAVE_LOAD_RESULT winmm_prepare_wave_in_directory(
    const char *directory, UINT directory_length, const char *file_name,
    BYTE **data, DWORD *bytes, pcm_format_t *format)
{
    UINT first = 0;
    UINT last = directory_length;
    while (first < last &&
           (directory[first] == ' ' || directory[first] == '\t'))
        first++;
    while (last > first &&
           (directory[last - 1U] == ' ' || directory[last - 1U] == '\t'))
        last--;
    if (last - first >= 2U && directory[first] == '"' &&
        directory[last - 1U] == '"') {
        first++;
        last--;
    }
    if (first == last)
        return WINMM_WAVE_LOAD_NOT_FOUND;

    char candidate[260];
    UINT length = last - first;
    UINT file_length = 0;
    while (file_name[file_length])
        file_length++;
    BOOL separator = directory[last - 1U] != '\\' &&
                     directory[last - 1U] != '/';
    if (length + (separator ? 1U : 0U) + file_length + 1U >
        sizeof(candidate))
        return WINMM_WAVE_LOAD_NOT_FOUND;

    for (UINT i = 0; i < length; i++)
        candidate[i] = directory[first + i];
    if (separator)
        candidate[length++] = '\\';
    for (UINT i = 0; i <= file_length; i++)
        candidate[length + i] = file_name[i];
    return winmm_prepare_wave_file_direct(candidate, FALSE, data, bytes,
                                           format);
}

static WINMM_WAVE_LOAD_RESULT winmm_prepare_wave_file_result(
    PCVOID file_name, BOOL wide, BYTE **data, DWORD *bytes,
    pcm_format_t *format)
{
    WINMM_WAVE_LOAD_RESULT result = winmm_prepare_wave_file_direct(
        file_name, wide, data, bytes, format);
    if (result != WINMM_WAVE_LOAD_NOT_FOUND)
        return result;

    char name[260];
    if (!winmm_copy_sound_name_a(file_name, wide, name, sizeof(name)) ||
        winmm_sound_name_has_path(name))
        return WINMM_WAVE_LOAD_NOT_FOUND;

    char directory[260];
    DWORD length = GetWindowsDirectoryA(directory, sizeof(directory));
    if (length && length < sizeof(directory)) {
        result = winmm_prepare_wave_in_directory(
            directory, length, name, data, bytes, format);
        if (result != WINMM_WAVE_LOAD_NOT_FOUND)
            return result;
    }

    length = GetSystemDirectoryA(directory, sizeof(directory));
    if (length && length < sizeof(directory)) {
        result = winmm_prepare_wave_in_directory(
            directory, length, name, data, bytes, format);
        if (result != WINMM_WAVE_LOAD_NOT_FOUND)
            return result;
    }

    DWORD required = GetEnvironmentVariableA("PATH", NULL, 0);
    if (required <= 1U || required > 32768U)
        return WINMM_WAVE_LOAD_NOT_FOUND;
    char *search_path = (char *)kmalloc(required);
    if (!search_path)
        return WINMM_WAVE_LOAD_NO_MEMORY;
    DWORD path_length = GetEnvironmentVariableA("PATH", search_path,
                                                 required);
    if (path_length && path_length < required) {
        DWORD component = 0;
        for (DWORD i = 0; i <= path_length; i++) {
            if (i != path_length && search_path[i] != ';')
                continue;
            result = winmm_prepare_wave_in_directory(
                search_path + component, i - component, name,
                data, bytes, format);
            if (result != WINMM_WAVE_LOAD_NOT_FOUND) {
                kfree(search_path);
                return result;
            }
            component = i + 1U;
        }
    }
    kfree(search_path);
    return WINMM_WAVE_LOAD_NOT_FOUND;
}

static BOOL winmm_prepare_wave_file(PCVOID file_name, BOOL wide,
                                    BYTE **data, DWORD *bytes,
                                    pcm_format_t *format)
{
    return winmm_prepare_wave_file_result(file_name, wide, data, bytes,
                                          format) == WINMM_WAVE_LOAD_OK;
}

static const char *winmm_alias_id_name(ULONG_PTR identifier)
{
    switch ((DWORD)identifier) {
    case 0x2A53U: return "SystemAsterisk";
    case 0x3F53U: return "SystemQuestion";
    case 0x4853U: return "SystemHand";
    case 0x4553U: return "SystemExit";
    case 0x5353U: return "SystemStart";
    case 0x5753U: return "SystemWelcome";
    case 0x2153U: return "SystemExclamation";
    case 0x4453U: return "SystemDefault";
    default: return NULL;
    }
}

static BOOL winmm_sound_alias_a(PCVOID sound, BOOL wide, BOOL alias_id,
                                char *alias, UINT capacity)
{
    if (alias_id) {
        const char *name = winmm_alias_id_name((ULONG_PTR)sound);
        return name && winmm_copy_string(alias, capacity, name);
    }
    if (!winmm_sound_name_valid(sound, wide))
        return FALSE;

    UINT length = 0;
    if (wide) {
        const WCHAR *value = (const WCHAR *)sound;
        while (value[length]) {
            if (length + 1U >= capacity || value[length] > 0xFFU)
                return FALSE;
            alias[length] = (char)value[length];
            length++;
        }
    } else {
        const char *value = (const char *)sound;
        while (value[length]) {
            if (length + 1U >= capacity)
                return FALSE;
            alias[length] = value[length];
            length++;
        }
    }
    alias[length] = 0;
    return length != 0;
}

static BOOL winmm_append_path(char *path, UINT capacity, UINT *length,
                              const char *part)
{
    while (*part) {
        if (*length + 1U >= capacity)
            return FALSE;
        path[(*length)++] = *part++;
    }
    path[*length] = 0;
    return TRUE;
}

static BOOL winmm_query_sound_scheme(const char *application,
                                     const char *alias, const char *scheme,
                                     char *file_name, UINT capacity)
{
    char key_path[768];
    UINT length = 0;
    key_path[0] = 0;
    if (!winmm_append_path(key_path, sizeof(key_path), &length,
                           "AppEvents\\Schemes\\Apps\\") ||
        !winmm_append_path(key_path, sizeof(key_path), &length, application) ||
        !winmm_append_path(key_path, sizeof(key_path), &length, "\\") ||
        !winmm_append_path(key_path, sizeof(key_path), &length, alias) ||
        !winmm_append_path(key_path, sizeof(key_path), &length, "\\") ||
        !winmm_append_path(key_path, sizeof(key_path), &length, scheme))
        return FALSE;

    HKEY key = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, key_path, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return FALSE;

    DWORD type = REG_NONE;
    DWORD size = capacity;
    LONG status = RegQueryValueExA(key, NULL, NULL, &type,
                                   (BYTE *)file_name, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !size ||
        size > capacity)
        return FALSE;
    file_name[size < capacity ? size : capacity - 1U] = 0;
    if (!file_name[0])
        return FALSE;

    if (type == REG_EXPAND_SZ) {
        typedef DWORD (WINAPI *WINMM_EXPAND_ENV_A)(PCSTR, PSTR, DWORD);
        WINMM_EXPAND_ENV_A expand = (WINMM_EXPAND_ENV_A)
            kernel32_resolve("ExpandEnvironmentStringsA", 0, FALSE);
        char expanded[1024];
        if (!expand || capacity > sizeof(expanded))
            return FALSE;
        DWORD required = expand(file_name, expanded, capacity);
        if (!required || required > capacity ||
            !winmm_copy_string(file_name, capacity, expanded))
            return FALSE;
    }

    UINT value_length = 0;
    while (file_name[value_length])
        value_length++;
    if (value_length >= 2U && file_name[0] == '"' &&
        file_name[value_length - 1U] == '"') {
        for (UINT i = 1; i < value_length - 1U; i++)
            file_name[i - 1U] = file_name[i];
        file_name[value_length - 2U] = 0;
    }
    return file_name[0] != 0;
}

static BOOL winmm_application_name(char *name, UINT capacity)
{
    char image_name[512];
    DWORD length = GetModuleFileNameA(NULL, image_name, sizeof(image_name));
    if (!length || length >= sizeof(image_name))
        return FALSE;

    DWORD base = 0;
    for (DWORD i = 0; i < length; i++) {
        if (image_name[i] == '\\' || image_name[i] == '/')
            base = i + 1U;
    }
    DWORD extension = length;
    for (DWORD i = base; i < length; i++) {
        if (image_name[i] == '.')
            extension = i;
    }
    if (extension > base)
        image_name[extension] = 0;
    return image_name[base] && winmm_copy_string(name, capacity,
                                                 image_name + base);
}

static BOOL winmm_ascii_equal_insensitive(const char *left,
                                           const char *right)
{
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return FALSE;
    }
    return *left == *right;
}

static BOOL winmm_resolve_sound_alias(const char *alias, BOOL application,
                                      char *file_name, UINT capacity)
{
    const char *registry_alias =
        winmm_ascii_equal_insensitive(alias, "SystemDefault")
            ? ".Default" : alias;
    if (application) {
        char app_name[260];
        return winmm_application_name(app_name, sizeof(app_name)) &&
            (winmm_query_sound_scheme(app_name, registry_alias, ".Current",
                                      file_name, capacity) ||
             winmm_query_sound_scheme(app_name, registry_alias, ".Default",
                                      file_name, capacity));
    }
    return winmm_query_sound_scheme(".Default", registry_alias, ".Current",
                                    file_name, capacity) ||
           winmm_query_sound_scheme(".Default", registry_alias, ".Default",
                                    file_name, capacity);
}

static BOOL winmm_prepare_alias_wave(const char *alias, BOOL application,
                                     BYTE **data, DWORD *bytes,
                                     pcm_format_t *format)
{
    char file_name[1024];
    return winmm_resolve_sound_alias(alias, application, file_name,
                                     sizeof(file_name)) &&
           winmm_prepare_wave_file(file_name, FALSE, data, bytes, format);
}

static BOOL winmm_prepare_default_wave(BYTE **data, DWORD *bytes,
                                       pcm_format_t *format)
{
    return winmm_prepare_alias_wave("SystemDefault", FALSE, data, bytes,
                                    format);
}

static BOOL winmm_resource_name_w(PCVOID sound, BOOL wide, WCHAR *buffer,
                                  UINT capacity, PCWSTR *resource_name)
{
    if (!sound || !resource_name)
        return FALSE;
    if ((ULONG_PTR)sound <= 0xFFFFU) {
        *resource_name = (PCWSTR)(ULONG_PTR)sound;
        return TRUE;
    }
    if (wide) {
        if (!winmm_sound_name_valid(sound, TRUE))
            return FALSE;
        *resource_name = (PCWSTR)sound;
        return TRUE;
    }

    const char *source = (const char *)sound;
    UINT length = 0;
    while (source[length]) {
        if (length + 1U >= capacity)
            return FALSE;
        buffer[length] = (WCHAR)(BYTE)source[length];
        length++;
    }
    buffer[length] = 0;
    *resource_name = buffer;
    return TRUE;
}

static BOOL winmm_prepare_play_sound_source(PCVOID sound, PVOID module,
                                             DWORD flags, BOOL wide,
                                             BYTE **data, DWORD *bytes,
                                             pcm_format_t *format)
{
    BOOL resource = (flags & SND_RESOURCE) == SND_RESOURCE;
    BOOL memory = !resource && (flags & SND_MEMORY);
    BOOL file_name = (flags & SND_FILENAME) != 0;
    BOOL alias = (flags & SND_ALIAS) != 0;
    BOOL alias_id = (flags & SND_ALIAS_ID) == SND_ALIAS_ID;
    BOOL prepared = FALSE;

    if (resource) {
        static const WCHAR wave_type[] = { 'W', 'A', 'V', 'E', 0 };
        WCHAR name_buffer[256];
        PCWSTR resource_name = NULL;
        PCVOID image = NULL;
        DWORD image_size = 0;
        prepared = winmm_resource_name_w(sound, wide, name_buffer,
                                          sizeof(name_buffer) /
                                              sizeof(name_buffer[0]),
                                          &resource_name) &&
            kernel32_resource_data_w((HANDLE)module, resource_name, wave_type,
                                     &image, &image_size) &&
            winmm_copy_wave_image((const BYTE *)image, image_size,
                                  data, bytes, format);
    } else if (memory) {
        DWORD image_size = 0;
        prepared = winmm_wave_memory_size((const BYTE *)sound, &image_size) &&
            winmm_copy_wave_image((const BYTE *)sound, image_size,
                                  data, bytes, format);
    } else if (file_name) {
        prepared = winmm_prepare_wave_file(sound, wide, data, bytes, format);
    } else {
        char alias_name[256];
        if (winmm_sound_alias_a(sound, wide, alias_id, alias_name,
                                sizeof(alias_name)))
            prepared = winmm_prepare_alias_wave(
                alias_name, (flags & SND_APPLICATION) != 0,
                data, bytes, format);
        if (!prepared && !alias)
            prepared = winmm_prepare_wave_file(sound, wide, data, bytes,
                                                format);
    }

    if (!prepared && !(flags & SND_NODEFAULT))
        prepared = winmm_prepare_default_wave(data, bytes, format);
    return prepared;
}

static void winmm_stop_play_sounds(DWORD owner_pid)
{
    BYTE *released[WINMM_PLAY_SOUND_SLOTS];
    int released_count = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (!sound->in_use || sound->owner_pid != owner_pid)
            continue;
        released[released_count++] = sound->data;
        memset(sound, 0, sizeof(*sound));
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    if (released_count)
        winmm_wave_refresh_mixer();
    for (int i = 0; i < released_count; i++)
        kfree(released[i]);
}

static uint32_t winmm_allocate_play_token(void)
{
    uint32_t token;
    do {
        token = __atomic_fetch_add(&winmm_next_play_token, 1U,
                                   __ATOMIC_RELAXED);
    } while (!token);
    return token;
}

static BOOL winmm_start_play_sound(BYTE *data, DWORD bytes,
                                   const pcm_format_t *format, DWORD flags,
                                   DWORD owner_pid, BOOL compat32)
{
    BOOL asynchronous = (flags & SND_ASYNC) != 0;
    if (asynchronous && !winmm_get_dispatcher(owner_pid, compat32)) {
        kfree(data);
        winmm_trace_play_sound_failure("dispatcher unavailable");
        return FALSE;
    }

    BYTE *released[WINMM_PLAY_SOUND_SLOTS];
    int released_count = 0;
    int target = -1;
    BOOL current_active = FALSE;
    uint32_t token = winmm_allocate_play_token();
    uint64_t irq_flags = winmm_timer_lock_irqsave();

    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (sound->in_use && sound->owner_pid == owner_pid) {
            if (!sound->completed)
                current_active = TRUE;
            if (target < 0)
                target = i;
        }
    }
    if ((flags & SND_NOSTOP) && current_active) {
        winmm_timer_unlock_irqrestore(irq_flags);
        kfree(data);
        return FALSE;
    }
    if (target < 0) {
        for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
            if (!winmm_play_sounds[i].in_use) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) {
        winmm_timer_unlock_irqrestore(irq_flags);
        kfree(data);
        winmm_trace_play_sound_failure("voice capacity exhausted");
        return FALSE;
    }

    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (!sound->in_use || sound->owner_pid != owner_pid)
            continue;
        released[released_count++] = sound->data;
        memset(sound, 0, sizeof(*sound));
    }

    WINMM_PLAY_SOUND *slot = &winmm_play_sounds[target];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = TRUE;
    slot->asynchronous = asynchronous;
    slot->looping = (flags & SND_LOOP) != 0;
    slot->owner_pid = owner_pid;
    slot->token = token;
    slot->data = data;
    slot->bytes = bytes;
    slot->format = *format;
    winmm_timer_unlock_irqrestore(irq_flags);

    for (int i = 0; i < released_count; i++)
        kfree(released[i]);

    audio_register_result_t registration = winmm_wave_refresh_mixer();
    if (registration != AUDIO_REGISTER_OK) {
        BYTE *failed_data = NULL;
        irq_flags = winmm_timer_lock_irqsave();
        for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
            slot = &winmm_play_sounds[i];
            if (slot->in_use && slot->owner_pid == owner_pid &&
                slot->token == token) {
                failed_data = slot->data;
                memset(slot, 0, sizeof(*slot));
                break;
            }
        }
        winmm_timer_unlock_irqrestore(irq_flags);
        winmm_wave_refresh_mixer();
        if (failed_data)
            kfree(failed_data);
        winmm_trace_play_sound_failure(audio_register_result_name(registration));
        return FALSE;
    }

    if (asynchronous)
        return TRUE;

    for (;;) {
        BYTE *completed_data = NULL;
        BOOL found = FALSE;
        BOOL completed = FALSE;
        irq_flags = winmm_timer_lock_irqsave();
        for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
            slot = &winmm_play_sounds[i];
            if (!slot->in_use || slot->owner_pid != owner_pid ||
                slot->token != token)
                continue;
            found = TRUE;
            completed = slot->completed;
            if (completed) {
                completed_data = slot->data;
                memset(slot, 0, sizeof(*slot));
            }
            break;
        }
        winmm_timer_unlock_irqrestore(irq_flags);

        if (!found)
            return TRUE;
        if (completed) {
            winmm_wave_refresh_mixer();
            kfree(completed_data);
            return TRUE;
        }
        Sleep(1);
    }
}

static BOOL winmm_play_sound_common(PCVOID sound, PVOID module, DWORD flags,
                                    BOOL wide)
{
    DWORD owner_pid = GetCurrentProcessId();
    if (!sound) {
        winmm_stop_play_sounds(owner_pid);
        return TRUE;
    }

    const DWORD valid_flags = SND_ASYNC | SND_NODEFAULT | SND_MEMORY |
        SND_LOOP | SND_NOSTOP | SND_PURGE | SND_APPLICATION | SND_NOWAIT |
        SND_ALIAS | SND_ALIAS_ID | SND_FILENAME | SND_RESOURCE |
        SND_SENTRY | SND_RING | SND_SYSTEM;
    BOOL resource = (flags & SND_RESOURCE) == SND_RESOURCE;
    BOOL memory = !resource && (flags & SND_MEMORY);
    BOOL file_name = (flags & SND_FILENAME) != 0;
    BOOL alias = (flags & SND_ALIAS) != 0;
    UINT selectors = (resource ? 1U : 0U) + (memory ? 1U : 0U) +
                     (file_name ? 1U : 0U) + (alias ? 1U : 0U);

    if ((flags & ~valid_flags) || selectors > 1U ||
        ((flags & SND_LOOP) && !(flags & SND_ASYNC)) ||
        (module && !resource) || (resource && !module) ||
        ((flags & SND_APPLICATION) && selectors && !alias)) {
        winmm_trace_play_sound_failure("invalid flags or source selector");
        return FALSE;
    }
    if (!audio_output_is_ready()) {
        winmm_trace_play_sound_failure("no output backend");
        return FALSE;
    }

    BYTE *data = NULL;
    DWORD bytes = 0;
    pcm_format_t format;
    if (!winmm_prepare_play_sound_source(sound, module, flags, wide,
                                          &data, &bytes, &format)) {
        winmm_trace_play_sound_failure("source unavailable or malformed");
        return FALSE;
    }
    return winmm_start_play_sound(data, bytes, &format, flags, owner_pid,
                                  g_compat32_mode ? TRUE : FALSE);
}

BOOL WINAPI shim_PlaySoundA(const char *sound, PVOID hmod, DWORD flags)
{
    return winmm_play_sound_common(sound, hmod, flags, FALSE);
}

BOOL WINAPI shim_PlaySoundW(PCWSTR sound, PVOID hmod, DWORD flags)
{
    return winmm_play_sound_common(sound, hmod, flags, TRUE);
}

static BOOL WINAPI shim_sndPlaySoundA(const char *sound, UINT flags)
{
    return shim_PlaySoundA(sound, NULL, flags);
}

static BOOL WINAPI shim_sndPlaySoundW(PCWSTR sound, UINT flags)
{
    return shim_PlaySoundW(sound, NULL, flags);
}

UINT WINAPI shim_waveOutGetNumDevs(void)
{
    return audio_output_is_ready() ? 1U : 0U;
}

/* ── MCI (Media Control Interface) ─────────────────────────── */

typedef struct {
    ULONG_PTR callback;
    PCVOID device_type;
    PCVOID element_name;
    PCVOID alias;
} WINMM_MCI_OPEN_VIEW;

static WINMM_MCI_DEVICE *winmm_mci_find_locked(UINT device_id,
                                                DWORD owner_pid)
{
    if (!device_id || device_id == MCI_ALL_DEVICE_ID || !owner_pid)
        return NULL;
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (device->in_use && device->device_id == device_id &&
            device->owner_pid == owner_pid)
            return device;
    }
    return NULL;
}

static WINMM_MCI_DEVICE *winmm_mci_find_alias_locked(const char *alias,
                                                      DWORD owner_pid)
{
    if (!alias || !alias[0] || !owner_pid)
        return NULL;
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (device->in_use && device->owner_pid == owner_pid &&
            device->alias[0] &&
            winmm_ascii_equal_insensitive(device->alias, alias))
            return device;
    }
    return NULL;
}

static UINT winmm_mci_allocate_device_id(void)
{
    UINT device_id;
    do {
        device_id = __atomic_fetch_add(&winmm_next_mci_device_id, 1U,
                                       __ATOMIC_RELAXED);
    } while (!device_id || device_id == MCI_ALL_DEVICE_ID);
    return device_id;
}

static ULONG_PTR winmm_mci_callback(ULONG_PTR parameter, BOOL compat32)
{
    if (!parameter)
        return 0;
    return compat32 ? *(const uint32_t *)(ULONG_PTR)parameter
                    : *(const uint64_t *)(ULONG_PTR)parameter;
}

static BOOL winmm_mci_read_open(ULONG_PTR parameter, BOOL compat32,
                                WINMM_MCI_OPEN_VIEW *view)
{
    if (!parameter || !view)
        return FALSE;
    memset(view, 0, sizeof(*view));
    if (compat32) {
        WINMM_MCI_OPEN32 *source =
            (WINMM_MCI_OPEN32 *)(ULONG_PTR)parameter;
        view->callback = source->callback;
        view->device_type = (PCVOID)(ULONG_PTR)source->device_type;
        view->element_name = (PCVOID)(ULONG_PTR)source->element_name;
        view->alias = (PCVOID)(ULONG_PTR)source->alias;
    } else {
        WINMM_MCI_OPEN64 *source =
            (WINMM_MCI_OPEN64 *)(ULONG_PTR)parameter;
        view->callback = source->callback;
        view->device_type = (PCVOID)(ULONG_PTR)source->device_type;
        view->element_name = (PCVOID)(ULONG_PTR)source->element_name;
        view->alias = (PCVOID)(ULONG_PTR)source->alias;
    }
    return TRUE;
}

static void winmm_mci_write_open_id(ULONG_PTR parameter, BOOL compat32,
                                    UINT device_id)
{
    if (compat32)
        ((WINMM_MCI_OPEN32 *)(ULONG_PTR)parameter)->device_id = device_id;
    else
        ((WINMM_MCI_OPEN64 *)(ULONG_PTR)parameter)->device_id = device_id;
}

static BOOL winmm_mci_name_has_wave_extension(const char *name)
{
    UINT length = 0;
    while (name[length])
        length++;
    return length >= 4U &&
        winmm_ascii_equal_insensitive(name + length - 4U, ".wav");
}

static DWORD winmm_mci_open_view(ULONG_PTR flags,
                                 const WINMM_MCI_OPEN_VIEW *view,
                                 BOOL wide, BOOL process_compat32,
                                 UINT *opened_device)
{
    const DWORD valid_flags = MCI_NOTIFY | MCI_WAIT | MCI_OPEN_SHAREABLE |
        MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS | MCI_OPEN_ELEMENT_ID |
        MCI_OPEN_TYPE_ID | MCI_OPEN_TYPE;
    if (opened_device)
        *opened_device = 0;
    if (!view)
        return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & ~valid_flags) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)) ||
        ((flags & MCI_OPEN_TYPE_ID) && !(flags & MCI_OPEN_TYPE)) ||
        ((flags & MCI_OPEN_ELEMENT_ID) && !(flags & MCI_OPEN_ELEMENT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    if (flags & MCI_OPEN_ELEMENT_ID)
        return MCIERR_UNSUPPORTED_FUNCTION;

    UINT device_type = 0;
    char type_name[64];
    if (flags & MCI_OPEN_TYPE) {
        if (!view->device_type)
            return MCIERR_INVALID_DEVICE_NAME;
        if (flags & MCI_OPEN_TYPE_ID) {
            device_type = (UINT)((ULONG_PTR)view->device_type & 0xFFFFU);
        } else if (!winmm_copy_sound_name_a(view->device_type, wide, type_name,
                                             sizeof(type_name))) {
            return MCIERR_INVALID_DEVICE_NAME;
        } else if (winmm_ascii_equal_insensitive(type_name, "waveaudio")) {
            device_type = MCI_DEVTYPE_WAVEFORM_AUDIO;
        } else if (winmm_ascii_equal_insensitive(type_name, "cdaudio")) {
            device_type = MCI_DEVTYPE_CD_AUDIO;
        } else {
            return MCIERR_DEVICE_NOT_INSTALLED;
        }
    }

    char element_name[260];
    if (flags & MCI_OPEN_ELEMENT) {
        if (!view->element_name ||
            !winmm_copy_sound_name_a(view->element_name, wide, element_name,
                                     sizeof(element_name)))
            return MCIERR_FILENAME_REQUIRED;
        if (!device_type && winmm_mci_name_has_wave_extension(element_name))
            device_type = MCI_DEVTYPE_WAVEFORM_AUDIO;
    }
    if (!device_type)
        return (flags & MCI_OPEN_ELEMENT) ? MCIERR_EXTENSION_NOT_FOUND
                                          : MCIERR_INVALID_DEVICE_NAME;
    if (device_type != MCI_DEVTYPE_WAVEFORM_AUDIO)
        return MCIERR_DEVICE_NOT_INSTALLED;
    if (!(flags & MCI_OPEN_ELEMENT))
        return MCIERR_FILENAME_REQUIRED;

    char alias[128] = {0};
    if ((flags & MCI_OPEN_ALIAS) &&
        (!view->alias || !winmm_copy_sound_name_a(view->alias, wide, alias,
                                                  sizeof(alias))))
        return MCIERR_MISSING_STRING_ARGUMENT;

    BYTE *data = NULL;
    DWORD bytes = 0;
    pcm_format_t format;
    WINMM_WAVE_LOAD_RESULT load_result = winmm_prepare_wave_file_result(
        view->element_name, wide, &data, &bytes, &format);
    if (load_result == WINMM_WAVE_LOAD_NOT_FOUND)
        return MCIERR_FILE_NOT_FOUND;
    if (load_result == WINMM_WAVE_LOAD_NO_MEMORY)
        return MCIERR_OUT_OF_MEMORY;
    if (load_result != WINMM_WAVE_LOAD_OK)
        return MCIERR_INVALID_FILE;

    DWORD owner_pid = GetCurrentProcessId();
    WINMM_MCI_DEVICE *slot = NULL;
    UINT device_id = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    if (alias[0] && winmm_mci_find_alias_locked(alias, owner_pid)) {
        winmm_timer_unlock_irqrestore(irq_flags);
        kfree(data);
        return MCIERR_DUPLICATE_ALIAS;
    }
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        if (!winmm_mci_devices[i].in_use) {
            slot = &winmm_mci_devices[i];
            break;
        }
    }
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->in_use = TRUE;
        slot->compat32 = process_compat32;
        slot->owner_pid = owner_pid;
        slot->device_id = winmm_mci_allocate_device_id();
        device_id = slot->device_id;
        slot->mode = MCI_MODE_STOP;
        slot->time_format = MCI_FORMAT_MILLISECONDS;
        slot->data = data;
        slot->bytes = bytes;
        slot->format = format;
        slot->play_end_frame = bytes / pcm_frame_bytes(&format);
        if (alias[0])
            winmm_copy_string(slot->alias, sizeof(slot->alias), alias);
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device_id) {
        kfree(data);
        return MCIERR_OUT_OF_MEMORY;
    }

    if (opened_device)
        *opened_device = device_id;
    if ((flags & MCI_NOTIFY) && view->callback)
        PostMessageA((HWND)view->callback, MM_MCINOTIFY,
                     MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_open_device(ULONG_PTR flags, ULONG_PTR parameter,
                                   BOOL wide, BOOL compat32,
                                   UINT *opened_device)
{
    WINMM_MCI_OPEN_VIEW view;
    if (!winmm_mci_read_open(parameter, compat32, &view))
        return MCIERR_NULL_PARAMETER_BLOCK;
    UINT device_id = 0;
    DWORD result = winmm_mci_open_view(flags, &view, wide, compat32,
                                       &device_id);
    if (!result) {
        winmm_mci_write_open_id(parameter, compat32, device_id);
        if (opened_device)
            *opened_device = device_id;
    }
    return result;
}

static DWORD winmm_mci_frames_to_time(const WINMM_MCI_DEVICE *device,
                                      uint64_t frames)
{
    uint64_t value;
    if (device->time_format == MCI_FORMAT_BYTES)
        value = frames * pcm_frame_bytes(&device->format);
    else if (device->time_format == MCI_FORMAT_SAMPLES)
        value = frames;
    else
        value = frames * 1000U / device->format.sample_rate;
    return value > 0xFFFFFFFFU ? 0xFFFFFFFFU : (DWORD)value;
}

static BOOL winmm_mci_time_to_frames(const WINMM_MCI_DEVICE *device,
                                     DWORD value, DWORD *frames)
{
    uint64_t converted;
    if (!frames)
        return FALSE;
    if (device->time_format == MCI_FORMAT_BYTES) {
        uint32_t frame_bytes = pcm_frame_bytes(&device->format);
        if (!frame_bytes || value % frame_bytes)
            return FALSE;
        converted = value / frame_bytes;
    } else if (device->time_format == MCI_FORMAT_SAMPLES) {
        converted = value;
    } else if (device->time_format == MCI_FORMAT_MILLISECONDS) {
        converted = (uint64_t)value * device->format.sample_rate / 1000U;
    } else {
        return FALSE;
    }
    if (converted > 0xFFFFFFFFU)
        return FALSE;
    *frames = (DWORD)converted;
    return TRUE;
}

static void winmm_mci_post_notify(ULONG_PTR window, UINT status,
                                  UINT device_id)
{
    if (window)
        PostMessageA((HWND)window, MM_MCINOTIFY, status, device_id);
}

static ULONG_PTR winmm_mci_abort_notify_locked(WINMM_MCI_DEVICE *device)
{
    ULONG_PTR window = 0;
    if (device->notify_pending) {
        window = device->notify_window;
        device->notify_pending = FALSE;
        device->notify_window = 0;
    }
    device->completed = FALSE;
    return window;
}

static DWORD winmm_mci_close_device(UINT device_id, ULONG_PTR flags,
                                    ULONG_PTR parameter, BOOL compat32)
{
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT)) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    if ((flags & MCI_NOTIFY) && !parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;

    DWORD owner_pid = GetCurrentProcessId();
    BYTE *released[WINMM_MCI_SLOTS];
    ULONG_PTR aborted[WINMM_MCI_SLOTS];
    UINT closed_ids[WINMM_MCI_SLOTS];
    int count = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (!device->in_use || device->owner_pid != owner_pid ||
            (device_id != MCI_ALL_DEVICE_ID &&
             device->device_id != device_id))
            continue;
        released[count] = device->data;
        aborted[count] = winmm_mci_abort_notify_locked(device);
        closed_ids[count] = device->device_id;
        count++;
        memset(device, 0, sizeof(*device));
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!count)
        return MCIERR_INVALID_DEVICE_ID;

    winmm_wave_refresh_mixer();
    for (int i = 0; i < count; i++) {
        winmm_mci_post_notify(aborted[i], MCI_NOTIFY_ABORTED, closed_ids[i]);
        kfree(released[i]);
    }
    ULONG_PTR callback = winmm_mci_callback(parameter, compat32);
    if (flags & MCI_NOTIFY)
        winmm_mci_post_notify(callback, MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_play_device(UINT device_id, ULONG_PTR flags,
                                   ULONG_PTR parameter, BOOL parameter_compat32,
                                   BOOL process_compat32)
{
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT | MCI_FROM | MCI_TO)) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    if ((flags & (MCI_NOTIFY | MCI_FROM | MCI_TO)) && !parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;

    DWORD from = 0;
    DWORD to = 0;
    ULONG_PTR callback = 0;
    if (parameter) {
        if (parameter_compat32) {
            WINMM_MCI_PLAY32 *play =
                (WINMM_MCI_PLAY32 *)(ULONG_PTR)parameter;
            callback = play->callback;
            from = play->from;
            to = play->to;
        } else {
            WINMM_MCI_PLAY64 *play =
                (WINMM_MCI_PLAY64 *)(ULONG_PTR)parameter;
            callback = play->callback;
            from = play->from;
            to = play->to;
        }
    }

    DWORD owner_pid = GetCurrentProcessId();
    if ((flags & MCI_NOTIFY) &&
        !winmm_get_dispatcher(owner_pid, process_compat32))
        return MCIERR_OUT_OF_MEMORY;

    ULONG_PTR aborted = 0;
    BOOL start_playback = FALSE;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (!device) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_INVALID_DEVICE_ID;
    }
    DWORD total_frames = device->bytes / pcm_frame_bytes(&device->format);
    DWORD start_frame = (DWORD)(device->cursor.frame_q32 >> 32);
    DWORD end_frame = total_frames;
    if ((flags & MCI_FROM) &&
        !winmm_mci_time_to_frames(device, from, &start_frame)) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_BAD_TIME_FORMAT;
    }
    if ((flags & MCI_TO) &&
        !winmm_mci_time_to_frames(device, to, &end_frame)) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_BAD_TIME_FORMAT;
    }
    if (start_frame > total_frames || end_frame > total_frames ||
        end_frame < start_frame) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_OUTOFRANGE;
    }

    aborted = winmm_mci_abort_notify_locked(device);
    device->cursor.frame_q32 = (uint64_t)start_frame << 32;
    device->play_end_frame = end_frame;
    device->paused = FALSE;
    device->notify_pending = (flags & MCI_NOTIFY) != 0;
    device->notify_window = callback;
    if (start_frame == end_frame) {
        device->playing = FALSE;
        device->completed = TRUE;
        device->mode = MCI_MODE_STOP;
    } else {
        device->playing = TRUE;
        device->completed = FALSE;
        device->mode = MCI_MODE_PLAY;
        start_playback = TRUE;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    winmm_mci_post_notify(aborted, MCI_NOTIFY_ABORTED, device_id);

    if (start_playback) {
        audio_register_result_t registration = winmm_wave_refresh_mixer();
        if (registration != AUDIO_REGISTER_OK) {
            irq_flags = winmm_timer_lock_irqsave();
            device = winmm_mci_find_locked(device_id, owner_pid);
            ULONG_PTR failed_notify = 0;
            if (device) {
                device->playing = FALSE;
                device->mode = MCI_MODE_STOP;
                failed_notify = winmm_mci_abort_notify_locked(device);
            }
            winmm_timer_unlock_irqrestore(irq_flags);
            winmm_mci_post_notify(failed_notify, MCI_NOTIFY_FAILURE,
                                  device_id);
            return MCIERR_DEVICE_NOT_READY;
        }
    } else if (flags & MCI_NOTIFY) {
        (void)winmm_mci_dispatch_completions(owner_pid);
    }

    if (!(flags & MCI_WAIT))
        return 0;
    for (;;) {
        irq_flags = winmm_timer_lock_irqsave();
        device = winmm_mci_find_locked(device_id, owner_pid);
        BOOL playing = device && device->playing;
        if (device && !playing)
            device->completed = FALSE;
        winmm_timer_unlock_irqrestore(irq_flags);
        if (!device)
            return MCIERR_INVALID_DEVICE_ID;
        if (!playing)
            return 0;
        Sleep(1);
    }
}

static DWORD winmm_mci_stop_or_pause(UINT device_id, ULONG_PTR flags,
                                     ULONG_PTR parameter, BOOL compat32,
                                     BOOL pause)
{
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT)) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    if ((flags & MCI_NOTIFY) && !parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;
    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR callback = winmm_mci_callback(parameter, compat32);
    ULONG_PTR aborted = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (device) {
        aborted = winmm_mci_abort_notify_locked(device);
        if (!pause || device->playing || device->paused) {
            device->playing = FALSE;
            device->paused = pause;
            device->mode = pause ? MCI_MODE_PAUSE : MCI_MODE_STOP;
        }
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device)
        return MCIERR_INVALID_DEVICE_ID;
    winmm_wave_refresh_mixer();
    winmm_mci_post_notify(aborted, MCI_NOTIFY_ABORTED, device_id);
    if (flags & MCI_NOTIFY)
        winmm_mci_post_notify(callback, MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_resume_device(UINT device_id, ULONG_PTR flags,
                                     ULONG_PTR parameter, BOOL compat32)
{
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT)) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    if ((flags & MCI_NOTIFY) && !parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;
    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR callback = winmm_mci_callback(parameter, compat32);
    BOOL resumed = FALSE;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (device && device->paused) {
        device->paused = FALSE;
        device->playing = TRUE;
        device->mode = MCI_MODE_PLAY;
        resumed = TRUE;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device)
        return MCIERR_INVALID_DEVICE_ID;
    if (resumed && winmm_wave_refresh_mixer() != AUDIO_REGISTER_OK) {
        irq_flags = winmm_timer_lock_irqsave();
        device = winmm_mci_find_locked(device_id, owner_pid);
        if (device) {
            device->playing = FALSE;
            device->paused = TRUE;
            device->mode = MCI_MODE_PAUSE;
        }
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_DEVICE_NOT_READY;
    }
    if (flags & MCI_NOTIFY)
        winmm_mci_post_notify(callback, MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_seek_device(UINT device_id, ULONG_PTR flags,
                                   ULONG_PTR parameter, BOOL compat32)
{
    const DWORD seek_flags = MCI_TO | MCI_SEEK_TO_START | MCI_SEEK_TO_END;
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT | seek_flags)) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    UINT destinations = ((flags & MCI_TO) ? 1U : 0U) +
        ((flags & MCI_SEEK_TO_START) ? 1U : 0U) +
        ((flags & MCI_SEEK_TO_END) ? 1U : 0U);
    if (destinations != 1U)
        return MCIERR_MISSING_PARAMETER;
    if ((flags & (MCI_TO | MCI_NOTIFY)) && !parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;

    DWORD requested = 0;
    ULONG_PTR callback = winmm_mci_callback(parameter, compat32);
    if (flags & MCI_TO) {
        requested = compat32
            ? ((WINMM_MCI_SEEK32 *)(ULONG_PTR)parameter)->to
            : ((WINMM_MCI_SEEK64 *)(ULONG_PTR)parameter)->to;
    }

    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR aborted = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (!device) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_INVALID_DEVICE_ID;
    }
    DWORD total_frames = device->bytes / pcm_frame_bytes(&device->format);
    DWORD target = 0;
    if (flags & MCI_SEEK_TO_END)
        target = total_frames;
    else if ((flags & MCI_TO) &&
             !winmm_mci_time_to_frames(device, requested, &target)) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_BAD_TIME_FORMAT;
    }
    if (target > total_frames) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MCIERR_OUTOFRANGE;
    }
    aborted = winmm_mci_abort_notify_locked(device);
    device->playing = FALSE;
    device->paused = FALSE;
    device->cursor.frame_q32 = (uint64_t)target << 32;
    device->mode = MCI_MODE_STOP;
    winmm_timer_unlock_irqrestore(irq_flags);
    winmm_wave_refresh_mixer();
    winmm_mci_post_notify(aborted, MCI_NOTIFY_ABORTED, device_id);
    if (flags & MCI_NOTIFY)
        winmm_mci_post_notify(callback, MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_status_device(UINT device_id, ULONG_PTR flags,
                                     ULONG_PTR parameter, BOOL compat32)
{
    if (!parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & ~(MCI_WAIT | MCI_STATUS_ITEM | MCI_STATUS_START |
                   MCI_TRACK)) || !(flags & MCI_STATUS_ITEM))
        return MCIERR_FLAGS_NOT_COMPATIBLE;

    DWORD item = compat32
        ? ((WINMM_MCI_STATUS32 *)(ULONG_PTR)parameter)->item
        : ((WINMM_MCI_STATUS64 *)(ULONG_PTR)parameter)->item;
    if (flags & MCI_TRACK)
        return MCIERR_UNSUPPORTED_FUNCTION;

    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR result = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (device) {
        DWORD total_frames = device->bytes /
            pcm_frame_bytes(&device->format);
        switch (item) {
        case MCI_STATUS_LENGTH:
            result = winmm_mci_frames_to_time(device, total_frames);
            break;
        case MCI_STATUS_POSITION:
            result = (flags & MCI_STATUS_START) ? 0 :
                winmm_mci_frames_to_time(device,
                                         device->cursor.frame_q32 >> 32);
            break;
        case MCI_STATUS_NUMBER_OF_TRACKS: result = 1; break;
        case MCI_STATUS_MODE: result = device->mode; break;
        case MCI_STATUS_MEDIA_PRESENT: result = TRUE; break;
        case MCI_STATUS_TIME_FORMAT: result = device->time_format; break;
        case MCI_STATUS_READY: result = TRUE; break;
        case MCI_STATUS_CURRENT_TRACK: result = 1; break;
        default:
            winmm_timer_unlock_irqrestore(irq_flags);
            return MCIERR_UNSUPPORTED_FUNCTION;
        }
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device)
        return MCIERR_INVALID_DEVICE_ID;
    if (compat32)
        ((WINMM_MCI_STATUS32 *)(ULONG_PTR)parameter)->result =
            (uint32_t)result;
    else
        ((WINMM_MCI_STATUS64 *)(ULONG_PTR)parameter)->result = result;
    return 0;
}

static DWORD winmm_mci_get_caps(UINT device_id, ULONG_PTR flags,
                                ULONG_PTR parameter, BOOL compat32)
{
    if (!parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & ~(MCI_WAIT | MCI_GETDEVCAPS_ITEM)) ||
        !(flags & MCI_GETDEVCAPS_ITEM))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    DWORD item = compat32
        ? ((WINMM_MCI_GETDEVCAPS32 *)(ULONG_PTR)parameter)->item
        : ((WINMM_MCI_GETDEVCAPS64 *)(ULONG_PTR)parameter)->item;
    ULONG_PTR result;
    switch (item) {
    case MCI_GETDEVCAPS_CAN_RECORD: result = FALSE; break;
    case MCI_GETDEVCAPS_HAS_AUDIO: result = TRUE; break;
    case MCI_GETDEVCAPS_HAS_VIDEO: result = FALSE; break;
    case MCI_GETDEVCAPS_DEVICE_TYPE:
        result = MCI_DEVTYPE_WAVEFORM_AUDIO;
        break;
    case MCI_GETDEVCAPS_USES_FILES: result = TRUE; break;
    case MCI_GETDEVCAPS_COMPOUND_DEVICE: result = TRUE; break;
    case MCI_GETDEVCAPS_CAN_EJECT: result = FALSE; break;
    case MCI_GETDEVCAPS_CAN_PLAY: result = TRUE; break;
    case MCI_GETDEVCAPS_CAN_SAVE: result = FALSE; break;
    default: return MCIERR_UNSUPPORTED_FUNCTION;
    }

    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device)
        return MCIERR_INVALID_DEVICE_ID;
    if (compat32)
        ((WINMM_MCI_GETDEVCAPS32 *)(ULONG_PTR)parameter)->result =
            (uint32_t)result;
    else
        ((WINMM_MCI_GETDEVCAPS64 *)(ULONG_PTR)parameter)->result = result;
    return 0;
}

static DWORD winmm_mci_set_device(UINT device_id, ULONG_PTR flags,
                                  ULONG_PTR parameter, BOOL compat32)
{
    if (!parameter)
        return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & ~(MCI_NOTIFY | MCI_WAIT | MCI_SET_TIME_FORMAT)) ||
        !(flags & MCI_SET_TIME_FORMAT) ||
        ((flags & MCI_NOTIFY) && (flags & MCI_WAIT)))
        return MCIERR_FLAGS_NOT_COMPATIBLE;
    DWORD time_format = compat32
        ? ((WINMM_MCI_SET32 *)(ULONG_PTR)parameter)->time_format
        : ((WINMM_MCI_SET64 *)(ULONG_PTR)parameter)->time_format;
    if (time_format != MCI_FORMAT_MILLISECONDS &&
        time_format != MCI_FORMAT_BYTES &&
        time_format != MCI_FORMAT_SAMPLES)
        return MCIERR_BAD_TIME_FORMAT;

    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_locked(device_id, owner_pid);
    if (device)
        device->time_format = time_format;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!device)
        return MCIERR_INVALID_DEVICE_ID;
    if (flags & MCI_NOTIFY)
        winmm_mci_post_notify(winmm_mci_callback(parameter, compat32),
                              MCI_NOTIFY_SUCCESSFUL, device_id);
    return 0;
}

static DWORD winmm_mci_send_command(UINT device_id, UINT message,
                                    ULONG_PTR flags, ULONG_PTR parameter,
                                    BOOL wide)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    switch (message) {
    case MCI_OPEN:
        return winmm_mci_open_device(flags, parameter, wide, compat32, NULL);
    case MCI_CLOSE:
        return winmm_mci_close_device(device_id, flags, parameter, compat32);
    case MCI_PLAY:
        return winmm_mci_play_device(device_id, flags, parameter, compat32,
                                     compat32);
    case MCI_STOP:
        return winmm_mci_stop_or_pause(device_id, flags, parameter, compat32,
                                       FALSE);
    case MCI_PAUSE:
        return winmm_mci_stop_or_pause(device_id, flags, parameter, compat32,
                                       TRUE);
    case MCI_RESUME:
        return winmm_mci_resume_device(device_id, flags, parameter, compat32);
    case MCI_SEEK:
        return winmm_mci_seek_device(device_id, flags, parameter, compat32);
    case MCI_STATUS:
        return winmm_mci_status_device(device_id, flags, parameter, compat32);
    case MCI_GETDEVCAPS:
        return winmm_mci_get_caps(device_id, flags, parameter, compat32);
    case MCI_SET:
        return winmm_mci_set_device(device_id, flags, parameter, compat32);
    default:
        winmm_trace_unsupported(wide ? "mciSendCommandW"
                                     : "mciSendCommandA");
        return MCIERR_UNSUPPORTED_FUNCTION;
    }
}

DWORD WINAPI shim_mciSendCommandA(UINT device, UINT msg, ULONG_PTR flags,
                                   ULONG_PTR param)
{
    return winmm_mci_send_command(device, msg, flags, param, FALSE);
}

static DWORD WINAPI shim_mciSendCommandW(UINT device, UINT msg,
                                          ULONG_PTR flags, ULONG_PTR param)
{
    return winmm_mci_send_command(device, msg, flags, param, TRUE);
}

#define WINMM_MCI_STRING_TOKENS 20U
#define WINMM_MCI_TOKEN_CHARS  260U

static DWORD winmm_mci_tokenize(const char *command,
                                char tokens[WINMM_MCI_STRING_TOKENS]
                                           [WINMM_MCI_TOKEN_CHARS],
                                UINT *token_count)
{
    const char *cursor = command;
    UINT count = 0;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (!*cursor)
            break;
        if (count == WINMM_MCI_STRING_TOKENS)
            return MCIERR_EXTRA_CHARACTERS;

        BOOL quoted = *cursor == '"';
        if (quoted)
            cursor++;
        UINT length = 0;
        while (*cursor &&
               (quoted ? *cursor != '"'
                       : (*cursor != ' ' && *cursor != '\t'))) {
            if (length + 1U >= WINMM_MCI_TOKEN_CHARS)
                return MCIERR_PARAM_OVERFLOW;
            tokens[count][length++] = *cursor++;
        }
        if (quoted) {
            if (*cursor != '"')
                return MCIERR_MISSING_STRING_ARGUMENT;
            cursor++;
            if (*cursor && *cursor != ' ' && *cursor != '\t')
                return MCIERR_EXTRA_CHARACTERS;
        }
        tokens[count][length] = 0;
        if (!length)
            return MCIERR_MISSING_STRING_ARGUMENT;
        count++;
    }
    *token_count = count;
    return count ? 0 : MCIERR_MISSING_COMMAND_STRING;
}

static BOOL winmm_mci_parse_number(const char *text, DWORD *value)
{
    if (!text || !text[0] || !value)
        return FALSE;
    uint64_t number = 0;
    for (UINT i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9')
            return FALSE;
        number = number * 10U + (UINT)(text[i] - '0');
        if (number > 0xFFFFFFFFU)
            return FALSE;
    }
    *value = (DWORD)number;
    return TRUE;
}

static UINT winmm_mci_get_device_id_name(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (winmm_ascii_equal_insensitive(name, "all"))
        return MCI_ALL_DEVICE_ID;
    DWORD numeric = 0;
    if (winmm_mci_parse_number(name, &numeric) && numeric)
        return numeric;

    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MCI_DEVICE *device =
        winmm_mci_find_alias_locked(name, owner_pid);
    UINT device_id = device ? device->device_id : 0;
    winmm_timer_unlock_irqrestore(irq_flags);
    return device_id;
}

static UINT winmm_mci_get_device_id(PCVOID name, BOOL wide)
{
    char narrow[128];
    if (!winmm_copy_sound_name_a(name, wide, narrow, sizeof(narrow)))
        return 0;
    return winmm_mci_get_device_id_name(narrow);
}

static UINT WINAPI shim_mciGetDeviceIDA(const char *name)
{
    return winmm_mci_get_device_id(name, FALSE);
}

static UINT WINAPI shim_mciGetDeviceIDW(PCWSTR name)
{
    return winmm_mci_get_device_id(name, TRUE);
}

static DWORD winmm_mci_return_string(char *output, UINT capacity,
                                     const char *text)
{
    UINT length = 0;
    while (text[length])
        length++;
    if (!output || capacity <= length)
        return MCIERR_PARAM_OVERFLOW;
    for (UINT i = 0; i <= length; i++)
        output[i] = text[i];
    return 0;
}

static DWORD winmm_mci_return_number(char *output, UINT capacity,
                                     DWORD value)
{
    char reverse[16];
    char number[16];
    UINT digits = 0;
    do {
        reverse[digits++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && digits < sizeof(reverse));
    for (UINT i = 0; i < digits; i++)
        number[i] = reverse[digits - i - 1U];
    number[digits] = 0;
    return winmm_mci_return_string(output, capacity, number);
}

static DWORD winmm_mci_parse_common_flags(
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT first, UINT count, DWORD *flags)
{
    for (UINT i = first; i < count; i++) {
        if (winmm_ascii_equal_insensitive(tokens[i], "wait")) {
            if (*flags & MCI_WAIT)
                return MCIERR_FLAGS_NOT_COMPATIBLE;
            *flags |= MCI_WAIT;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "notify")) {
            if (*flags & MCI_NOTIFY)
                return MCIERR_FLAGS_NOT_COMPATIBLE;
            *flags |= MCI_NOTIFY;
        } else {
            return MCIERR_UNRECOGNIZED_KEYWORD;
        }
    }
    return ((*flags & MCI_WAIT) && (*flags & MCI_NOTIFY))
        ? MCIERR_FLAGS_NOT_COMPATIBLE : 0;
}

static DWORD winmm_mci_send_string_open(
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT count, PVOID notify_window)
{
    if (count < 2U)
        return MCIERR_MISSING_STRING_ARGUMENT;

    WINMM_MCI_OPEN_VIEW view = {0};
    view.callback = (ULONG_PTR)notify_window;
    DWORD flags = 0;
    if (winmm_ascii_equal_insensitive(tokens[1], "cdaudio") ||
        winmm_ascii_equal_insensitive(tokens[1], "waveaudio")) {
        view.device_type = tokens[1];
        flags |= MCI_OPEN_TYPE;
    } else {
        view.element_name = tokens[1];
        flags |= MCI_OPEN_ELEMENT;
    }

    for (UINT i = 2; i < count; i++) {
        if (winmm_ascii_equal_insensitive(tokens[i], "type")) {
            if (++i >= count || (flags & MCI_OPEN_TYPE))
                return MCIERR_MISSING_STRING_ARGUMENT;
            view.device_type = tokens[i];
            flags |= MCI_OPEN_TYPE;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "alias")) {
            if (++i >= count || (flags & MCI_OPEN_ALIAS))
                return MCIERR_MISSING_STRING_ARGUMENT;
            view.alias = tokens[i];
            flags |= MCI_OPEN_ALIAS;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "shareable")) {
            flags |= MCI_OPEN_SHAREABLE;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "wait")) {
            flags |= MCI_WAIT;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "notify")) {
            flags |= MCI_NOTIFY;
        } else {
            return MCIERR_UNRECOGNIZED_KEYWORD;
        }
    }
    return winmm_mci_open_view(flags, &view, FALSE,
                               g_compat32_mode ? TRUE : FALSE, NULL);
}

static DWORD winmm_mci_send_string_play(
    UINT device_id,
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT count, PVOID notify_window)
{
    WINMM_MCI_PLAY64 play = { .callback = (ULONG_PTR)notify_window };
    DWORD flags = 0;
    DWORD value = 0;
    for (UINT i = 2; i < count; i++) {
        if (winmm_ascii_equal_insensitive(tokens[i], "from")) {
            if (++i >= count || (flags & MCI_FROM) ||
                !winmm_mci_parse_number(tokens[i], &value))
                return MCIERR_BAD_INTEGER;
            play.from = value;
            flags |= MCI_FROM;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "to")) {
            if (++i >= count || (flags & MCI_TO) ||
                !winmm_mci_parse_number(tokens[i], &value))
                return MCIERR_BAD_INTEGER;
            play.to = value;
            flags |= MCI_TO;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "wait")) {
            flags |= MCI_WAIT;
        } else if (winmm_ascii_equal_insensitive(tokens[i], "notify")) {
            flags |= MCI_NOTIFY;
        } else {
            return MCIERR_UNRECOGNIZED_KEYWORD;
        }
    }
    return winmm_mci_play_device(device_id, flags, (ULONG_PTR)&play, FALSE,
                                 g_compat32_mode ? TRUE : FALSE);
}

static DWORD winmm_mci_send_string_seek(
    UINT device_id,
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT count, PVOID notify_window)
{
    if (count < 3U)
        return MCIERR_MISSING_PARAMETER;
    WINMM_MCI_SEEK64 seek = { .callback = (ULONG_PTR)notify_window };
    DWORD flags = 0;
    DWORD position = 0;
    UINT next = 2;
    if (winmm_ascii_equal_insensitive(tokens[next], "to")) {
        if (++next >= count)
            return MCIERR_MISSING_PARAMETER;
    }
    if (winmm_ascii_equal_insensitive(tokens[next], "start"))
        flags |= MCI_SEEK_TO_START;
    else if (winmm_ascii_equal_insensitive(tokens[next], "end"))
        flags |= MCI_SEEK_TO_END;
    else if (winmm_mci_parse_number(tokens[next], &position)) {
        seek.to = position;
        flags |= MCI_TO;
    } else
        return MCIERR_BAD_INTEGER;
    DWORD result = winmm_mci_parse_common_flags(tokens, next + 1U, count,
                                                 &flags);
    return result ? result :
        winmm_mci_seek_device(device_id, flags, (ULONG_PTR)&seek, FALSE);
}

static DWORD winmm_mci_send_string_status(
    UINT device_id,
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT count, char *output, UINT capacity)
{
    if (count < 3U)
        return MCIERR_MISSING_PARAMETER;
    WINMM_MCI_STATUS64 status = {0};
    UINT next = 3;
    if (winmm_ascii_equal_insensitive(tokens[2], "length"))
        status.item = MCI_STATUS_LENGTH;
    else if (winmm_ascii_equal_insensitive(tokens[2], "position"))
        status.item = MCI_STATUS_POSITION;
    else if (winmm_ascii_equal_insensitive(tokens[2], "mode"))
        status.item = MCI_STATUS_MODE;
    else if (winmm_ascii_equal_insensitive(tokens[2], "ready"))
        status.item = MCI_STATUS_READY;
    else if (winmm_ascii_equal_insensitive(tokens[2], "time") &&
             count > 3U &&
             winmm_ascii_equal_insensitive(tokens[3], "format")) {
        status.item = MCI_STATUS_TIME_FORMAT;
        next = 4;
    } else if (winmm_ascii_equal_insensitive(tokens[2], "media") &&
               count > 3U &&
               winmm_ascii_equal_insensitive(tokens[3], "present")) {
        status.item = MCI_STATUS_MEDIA_PRESENT;
        next = 4;
    } else if (winmm_ascii_equal_insensitive(tokens[2], "current") &&
               count > 3U &&
               winmm_ascii_equal_insensitive(tokens[3], "track")) {
        status.item = MCI_STATUS_CURRENT_TRACK;
        next = 4;
    } else if (winmm_ascii_equal_insensitive(tokens[2], "number") &&
               count > 4U &&
               winmm_ascii_equal_insensitive(tokens[3], "of") &&
               winmm_ascii_equal_insensitive(tokens[4], "tracks")) {
        status.item = MCI_STATUS_NUMBER_OF_TRACKS;
        next = 5;
    } else {
        return MCIERR_UNRECOGNIZED_KEYWORD;
    }

    DWORD flags = MCI_STATUS_ITEM;
    DWORD result = winmm_mci_parse_common_flags(tokens, next, count, &flags);
    if (result)
        return result;
    result = winmm_mci_status_device(device_id, flags,
                                     (ULONG_PTR)&status, FALSE);
    if (result)
        return result;
    if (status.item == MCI_STATUS_MODE) {
        const char *mode = status.result == MCI_MODE_PLAY ? "playing" :
            status.result == MCI_MODE_PAUSE ? "paused" :
            status.result == MCI_MODE_OPEN ? "open" :
            status.result == MCI_MODE_NOT_READY ? "not ready" : "stopped";
        return winmm_mci_return_string(output, capacity, mode);
    }
    if (status.item == MCI_STATUS_READY ||
        status.item == MCI_STATUS_MEDIA_PRESENT)
        return winmm_mci_return_string(output, capacity,
                                       status.result ? "true" : "false");
    return winmm_mci_return_number(output, capacity, (DWORD)status.result);
}

static DWORD winmm_mci_send_string_set(
    UINT device_id,
    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS],
    UINT count, PVOID notify_window)
{
    if (count < 5U ||
        !winmm_ascii_equal_insensitive(tokens[2], "time") ||
        !winmm_ascii_equal_insensitive(tokens[3], "format"))
        return MCIERR_UNRECOGNIZED_KEYWORD;
    WINMM_MCI_SET64 set = { .callback = (ULONG_PTR)notify_window };
    if (winmm_ascii_equal_insensitive(tokens[4], "milliseconds"))
        set.time_format = MCI_FORMAT_MILLISECONDS;
    else if (winmm_ascii_equal_insensitive(tokens[4], "bytes"))
        set.time_format = MCI_FORMAT_BYTES;
    else if (winmm_ascii_equal_insensitive(tokens[4], "samples"))
        set.time_format = MCI_FORMAT_SAMPLES;
    else
        return MCIERR_BAD_TIME_FORMAT;
    DWORD flags = MCI_SET_TIME_FORMAT;
    DWORD result = winmm_mci_parse_common_flags(tokens, 5U, count, &flags);
    return result ? result :
        winmm_mci_set_device(device_id, flags, (ULONG_PTR)&set, FALSE);
}

static DWORD winmm_mci_send_string_common(const char *command, char *output,
                                          UINT capacity, PVOID notify_window)
{
    if (output && capacity)
        output[0] = 0;
    if (!command || !command[0])
        return MCIERR_MISSING_COMMAND_STRING;

    char tokens[WINMM_MCI_STRING_TOKENS][WINMM_MCI_TOKEN_CHARS];
    UINT count = 0;
    DWORD result = winmm_mci_tokenize(command, tokens, &count);
    if (result)
        return result;
    if (winmm_ascii_equal_insensitive(tokens[0], "open"))
        return winmm_mci_send_string_open(tokens, count, notify_window);
    if (count < 2U)
        return MCIERR_MISSING_STRING_ARGUMENT;

    UINT device_id = winmm_mci_get_device_id_name(tokens[1]);
    if (!device_id)
        return MCIERR_INVALID_DEVICE_NAME;
    if (winmm_ascii_equal_insensitive(tokens[0], "play"))
        return winmm_mci_send_string_play(device_id, tokens, count,
                                          notify_window);
    if (winmm_ascii_equal_insensitive(tokens[0], "seek"))
        return winmm_mci_send_string_seek(device_id, tokens, count,
                                          notify_window);
    if (winmm_ascii_equal_insensitive(tokens[0], "status"))
        return winmm_mci_send_string_status(device_id, tokens, count,
                                            output, capacity);
    if (winmm_ascii_equal_insensitive(tokens[0], "set"))
        return winmm_mci_send_string_set(device_id, tokens, count,
                                         notify_window);

    DWORD flags = 0;
    result = winmm_mci_parse_common_flags(tokens, 2U, count, &flags);
    if (result)
        return result;
    uint64_t callback = (ULONG_PTR)notify_window;
    ULONG_PTR parameter = (flags & MCI_NOTIFY) ? (ULONG_PTR)&callback : 0;
    if (winmm_ascii_equal_insensitive(tokens[0], "close"))
        return winmm_mci_close_device(device_id, flags, parameter, FALSE);
    if (device_id == MCI_ALL_DEVICE_ID)
        return MCIERR_CANNOT_USE_ALL;
    if (winmm_ascii_equal_insensitive(tokens[0], "stop"))
        return winmm_mci_stop_or_pause(device_id, flags, parameter, FALSE,
                                       FALSE);
    if (winmm_ascii_equal_insensitive(tokens[0], "pause"))
        return winmm_mci_stop_or_pause(device_id, flags, parameter, FALSE,
                                       TRUE);
    if (winmm_ascii_equal_insensitive(tokens[0], "resume"))
        return winmm_mci_resume_device(device_id, flags, parameter, FALSE);
    return MCIERR_UNRECOGNIZED_COMMAND;
}

DWORD WINAPI shim_mciSendStringA(const char *cmd, char *ret, UINT retLen,
                                  PVOID hwnd)
{
    return winmm_mci_send_string_common(cmd, ret, retLen, hwnd);
}

DWORD WINAPI shim_mciSendStringW(PCWSTR cmd, PWSTR ret, UINT retLen,
                                  PVOID hwnd)
{
    if (ret && retLen)
        ret[0] = 0;
    if (!cmd || !cmd[0])
        return MCIERR_MISSING_COMMAND_STRING;
    char narrow[1024];
    UINT length = 0;
    while (cmd[length]) {
        if (length + 1U >= sizeof(narrow) || cmd[length] > 0xFFU)
            return MCIERR_PARAM_OVERFLOW;
        narrow[length] = (char)cmd[length];
        length++;
    }
    narrow[length] = 0;

    char result_text[128];
    char *narrow_result = (ret && retLen) ? result_text : NULL;
    UINT result_capacity = retLen < sizeof(result_text)
        ? retLen : sizeof(result_text);
    DWORD result = winmm_mci_send_string_common(
        narrow, narrow_result, result_capacity, hwnd);
    if (!result && narrow_result) {
        UINT i = 0;
        while (result_text[i] && i + 1U < retLen) {
            ret[i] = (WCHAR)(BYTE)result_text[i];
            i++;
        }
        ret[i] = 0;
    }
    return result;
}

/* waveOut playback */

static const char *winmm_mmresult_text(UINT error)
{
    switch (error) {
    case MMSYSERR_NOERROR: return "No error.";
    case MMSYSERR_ERROR: return "Unspecified multimedia error.";
    case MMSYSERR_BADDEVICEID: return "The device identifier is invalid.";
    case MMSYSERR_ALLOCATED: return "The multimedia resource is allocated.";
    case MMSYSERR_INVALHANDLE: return "The device handle is invalid.";
    case MMSYSERR_NODRIVER: return "No multimedia device driver is available.";
    case MMSYSERR_NOMEM: return "Not enough memory is available.";
    case MMSYSERR_NOTSUPPORTED: return "The function is not supported.";
    case MMSYSERR_BADERRNUM: return "The multimedia error number is invalid.";
    case MMSYSERR_INVALFLAG: return "A flag is invalid.";
    case MMSYSERR_INVALPARAM: return "A parameter is invalid.";
    case WAVERR_BADFORMAT: return "The waveform format is not supported.";
    case WAVERR_STILLPLAYING: return "The waveform is still playing.";
    case WAVERR_UNPREPARED: return "The waveform header is not prepared.";
    default: return NULL;
    }
}

static const char *winmm_mci_error_text(DWORD error)
{
    switch (error) {
    case MMSYSERR_NOERROR: return "No error.";
    case MCIERR_INVALID_DEVICE_ID:
        return "The MCI device identifier is invalid.";
    case MCIERR_UNRECOGNIZED_KEYWORD:
        return "The MCI command keyword is not recognized.";
    case MCIERR_UNRECOGNIZED_COMMAND:
        return "The MCI command is not recognized.";
    case MCIERR_INVALID_DEVICE_NAME:
        return "The MCI device name is invalid.";
    case MCIERR_OUT_OF_MEMORY:
        return "There is not enough memory for the MCI operation.";
    case MCIERR_MISSING_COMMAND_STRING:
        return "The MCI command string is missing.";
    case MCIERR_PARAM_OVERFLOW:
        return "An MCI command parameter is too long.";
    case MCIERR_MISSING_STRING_ARGUMENT:
        return "A required MCI string argument is missing.";
    case MCIERR_BAD_INTEGER:
        return "An MCI numeric argument is invalid.";
    case MCIERR_MISSING_PARAMETER:
        return "A required MCI parameter is missing.";
    case MCIERR_UNSUPPORTED_FUNCTION:
        return "The requested MCI function is not supported.";
    case MCIERR_FILE_NOT_FOUND:
        return "The specified media file was not found.";
    case MCIERR_DEVICE_NOT_READY:
        return "The MCI device is not ready.";
    case MCIERR_CANNOT_USE_ALL:
        return "The all-device identifier is not valid for this command.";
    case MCIERR_EXTENSION_NOT_FOUND:
        return "No MCI device matches the filename extension.";
    case MCIERR_OUTOFRANGE:
        return "An MCI parameter is out of range.";
    case MCIERR_FLAGS_NOT_COMPATIBLE:
        return "The specified MCI flags are incompatible.";
    case MCIERR_DUPLICATE_ALIAS:
        return "The specified MCI alias is already in use.";
    case MCIERR_BAD_TIME_FORMAT:
        return "The specified MCI time format is invalid.";
    case MCIERR_INVALID_FILE:
        return "The specified media file is invalid.";
    case MCIERR_NULL_PARAMETER_BLOCK:
        return "The MCI parameter block is null.";
    case MCIERR_FILENAME_REQUIRED:
        return "A media filename is required.";
    case MCIERR_EXTRA_CHARACTERS:
        return "The MCI command contains extra characters.";
    case MCIERR_DEVICE_NOT_INSTALLED:
        return "The requested MCI device is not installed.";
    default: return NULL;
    }
}

static void winmm_copy_text_a(char *destination, UINT capacity,
                              const char *source)
{
    UINT i = 0;
    if (!destination || !capacity)
        return;
    while (i + 1U < capacity && source[i]) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = 0;
}

static void winmm_copy_text_w(PWSTR destination, UINT capacity,
                              const char *source)
{
    UINT i = 0;
    if (!destination || !capacity)
        return;
    while (i + 1U < capacity && source[i]) {
        destination[i] = (WCHAR)(BYTE)source[i];
        i++;
    }
    destination[i] = 0;
}

static UINT WINAPI shim_waveOutGetErrorTextA(UINT error, char *text,
                                              UINT chars)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!text || !chars ||
        !win32_user_range_writable(text, chars, compat32))
        return MMSYSERR_INVALPARAM;
    text[0] = 0;
    const char *message = winmm_mmresult_text(error);
    if (!message)
        return MMSYSERR_BADERRNUM;
    winmm_copy_text_a(text, chars, message);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutGetErrorTextW(UINT error, PWSTR text,
                                              UINT chars)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!text || !chars ||
        !win32_user_range_writable(
            text, (SIZE_T)chars * sizeof(*text), compat32))
        return MMSYSERR_INVALPARAM;
    text[0] = 0;
    const char *message = winmm_mmresult_text(error);
    if (!message)
        return MMSYSERR_BADERRNUM;
    winmm_copy_text_w(text, chars, message);
    return MMSYSERR_NOERROR;
}

static BOOL WINAPI shim_mciGetErrorStringA(DWORD error, char *text,
                                            UINT chars)
{
    const char *message = winmm_mci_error_text(error);
    if (!message || !text || !chars)
        return FALSE;
    winmm_copy_text_a(text, chars, message);
    return TRUE;
}

static BOOL WINAPI shim_mciGetErrorStringW(DWORD error, PWSTR text,
                                            UINT chars)
{
    const char *message = winmm_mci_error_text(error);
    if (!message || !text || !chars)
        return FALSE;
    winmm_copy_text_w(text, chars, message);
    return TRUE;
}

static UINT winmm_wave_header_size(BOOL compat32)
{
    return compat32 ? sizeof(WINMM_WAVEHDR32) : sizeof(WINMM_WAVEHDR64);
}

static void winmm_wave_complete_list(WINMM_WAVE_BUFFER *buffer,
                                     PVOID handle, PVOID callback,
                                     ULONG_PTR instance,
                                     DWORD callback_flags, BOOL compat32,
                                     BOOL notify)
{
    while (buffer) {
        WINMM_WAVE_BUFFER *next = buffer->next;
        if (notify) {
            if (winmm_wave_set_header_flags(buffer->guest_header, compat32,
                                            WHDR_DONE, WHDR_INQUEUE)) {
                winmm_invoke_wave_callback(handle, WOM_DONE, callback,
                                           instance, callback_flags,
                                           buffer->guest_header, compat32);
            } else {
                serial_puts("[WINMM] dropped reset for stale WAVEHDR\n");
            }
        }
        kfree(buffer->data);
        kfree(buffer);
        buffer = next;
    }
}

static UINT WINAPI shim_waveOutReset(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    WINMM_WAVE_BUFFER *buffers = NULL;
    PVOID callback = NULL;
    ULONG_PTR instance = 0;
    DWORD callback_flags = 0;
    BOOL compat32 = FALSE;

    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave) {
        buffers = wave->queue_head;
        callback = wave->callback;
        instance = wave->instance;
        callback_flags = wave->callback_flags;
        compat32 = wave->compat32;
        wave->queue_head = NULL;
        wave->queue_tail = NULL;
        wave->current = NULL;
        wave->paused = FALSE;
        wave->completed_bytes = 0;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;

    winmm_wave_refresh_mixer();
    winmm_wave_complete_list(buffers, handle, callback, instance,
                             callback_flags, compat32, TRUE);
    return MMSYSERR_NOERROR;
}
static UINT WINAPI shim_waveOutUnprepareHeader(PVOID handle, PVOID header,
                                                UINT size)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    BOOL compat32 = wave ? wave->compat32 : FALSE;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    if (!header || size < winmm_wave_header_size(compat32))
        return MMSYSERR_INVALPARAM;

    WINMM_WAVEHDR_VIEW view;
    if (!winmm_wave_read_header(header, compat32, &view))
        return MMSYSERR_INVALPARAM;
    if (view.flags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;
    if (!winmm_wave_set_header_flags(header, compat32, 0, WHDR_PREPARED))
        return MMSYSERR_INVALPARAM;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutGetPosition(PVOID handle, PVOID time,
                                            UINT size)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!time || size < 8U ||
        !win32_user_range_readable(time, sizeof(DWORD), compat32) ||
        !win32_user_range_writable(time, 2U * sizeof(DWORD), compat32))
        return MMSYSERR_INVALPARAM;

    DWORD owner_pid = GetCurrentProcessId();
    uint64_t bytes = 0;
    pcm_format_t format = {0};
    WORD block_align = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave) {
        bytes = wave->completed_bytes;
        format = wave->format;
        block_align = wave->block_align;
        if (wave->current && block_align) {
            uint64_t current = (wave->current->cursor.frame_q32 >> 32) *
                               block_align;
            if (current > wave->current->bytes)
                current = wave->current->bytes;
            bytes += current;
        }
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;

    DWORD *values = (DWORD *)time;
    switch (values[0]) {
    case TIME_MS:
        values[1] = format.sample_rate && block_align
                  ? (DWORD)(((bytes / block_align) * 1000ULL) /
                            format.sample_rate) : 0;
        break;
    case TIME_SAMPLES:
        values[1] = block_align ? (DWORD)(bytes / block_align) : 0;
        break;
    case TIME_BYTES:
        values[1] = (DWORD)bytes;
        break;
    default:
        values[0] = TIME_BYTES;
        values[1] = (DWORD)bytes;
        break;
    }
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutGetDevCapsA(UINT device, PVOID capabilities,
                                           UINT size)
{
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;
    if (device != 0 && device != WAVE_MAPPER)
        return MMSYSERR_BADDEVICEID;
    if (!capabilities || !size)
        return MMSYSERR_INVALPARAM;

    WINMM_WAVEOUTCAPSA caps = {
        .manufacturer_id = 1,
        .product_id = 1,
        .driver_version = 0x00010000U,
        .product_name = "OsitoK PCM Output",
        .formats = 0x0000FFFFU,
        .channels = 2,
        .reserved = 0,
        .support = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME,
    };
    UINT copied = size < sizeof(caps) ? size : sizeof(caps);
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!win32_user_range_writable(capabilities, copied, compat32))
        return MMSYSERR_INVALPARAM;
    memcpy(capabilities, &caps, copied);
    return MMSYSERR_NOERROR;
}
static UINT WINAPI shim_waveOutOpen(PVOID output, UINT device, PVOID format,
                                    ULONG_PTR callback, ULONG_PTR instance,
                                    DWORD flags)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (output && !winmm_wave_store_handle(output, compat32, NULL))
        return MMSYSERR_INVALPARAM;
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;
    if (device != 0 && device != WAVE_MAPPER)
        return MMSYSERR_BADDEVICEID;

    DWORD callback_type = flags & CALLBACK_TYPEMASK;
    if ((flags & ~(CALLBACK_TYPEMASK | 0x0000000FU)) ||
        callback_type == 0x00040000U || callback_type == 0x00060000U ||
        callback_type == 0x00070000U)
        return MMSYSERR_INVALFLAG;

    pcm_format_t parsed_format;
    WORD block_align = 0;
    WINMM_WAVEFORMATEX format_snapshot;
    if (!win32_user_range_readable(format, sizeof(format_snapshot),
                                   compat32)) {
        serial_puts("[WINMM] rejected unreadable WAVEFORMATEX\n");
        return MMSYSERR_INVALPARAM;
    }
    memcpy(&format_snapshot, format, sizeof(format_snapshot));
    if (!winmm_wave_parse_format(&format_snapshot, &parsed_format,
                                 &block_align))
        return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY)
        return MMSYSERR_NOERROR;
    if (!output)
        return MMSYSERR_INVALPARAM;

    DWORD owner_pid = GetCurrentProcessId();
    if (!winmm_get_dispatcher(owner_pid, compat32))
        return MMSYSERR_NOMEM;

    WINMM_WAVE_OUT *wave = NULL;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
        if (!winmm_wave_outs[i].in_use) {
            wave = &winmm_wave_outs[i];
            break;
        }
    }
    if (wave) {
        memset(wave, 0, sizeof(*wave));
        wave->in_use = TRUE;
        wave->compat32 = compat32;
        wave->owner_pid = owner_pid;
        wave->handle = winmm_wave_allocate_handle_locked();
        wave->format = parsed_format;
        wave->block_align = block_align;
        wave->callback = (PVOID)callback;
        wave->instance = instance;
        wave->callback_flags = callback_type;
        wave->volume = 0xFFFFFFFFU;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_ALLOCATED;

    winmm_wave_store_handle(output, compat32, wave->handle);
    winmm_invoke_wave_callback(wave->handle, WOM_OPEN, wave->callback,
                               wave->instance, wave->callback_flags, NULL,
                               wave->compat32);
    return MMSYSERR_NOERROR;
}
static UINT WINAPI shim_waveOutMessage(PVOID handle, UINT message,
                                        ULONG_PTR parameter1,
                                        ULONG_PTR parameter2)
{
    (void)message;
    (void)parameter1;
    (void)parameter2;
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    winmm_timer_unlock_irqrestore(irq_flags);
    return wave ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveOutClose(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    PVOID callback = NULL;
    ULONG_PTR instance = 0;
    DWORD callback_flags = 0;
    BOOL compat32 = FALSE;

    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (!wave) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return MMSYSERR_INVALHANDLE;
    }
    if (wave->queue_head) {
        winmm_timer_unlock_irqrestore(irq_flags);
        return WAVERR_STILLPLAYING;
    }
    callback = wave->callback;
    instance = wave->instance;
    callback_flags = wave->callback_flags;
    compat32 = wave->compat32;
    memset(wave, 0, sizeof(*wave));
    winmm_timer_unlock_irqrestore(irq_flags);

    winmm_wave_refresh_mixer();
    winmm_invoke_wave_callback(handle, WOM_CLOSE, callback, instance,
                               callback_flags, NULL, compat32);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutWrite(PVOID handle, PVOID header, UINT size)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    BOOL compat32 = wave ? wave->compat32 : FALSE;
    WORD block_align = wave ? wave->block_align : 0;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    if (!header || size < winmm_wave_header_size(compat32))
        return MMSYSERR_INVALPARAM;

    WINMM_WAVEHDR_VIEW view;
    if (!winmm_wave_read_header(header, compat32, &view) ||
        !win32_user_range_writable(
            header, winmm_wave_header_size(compat32), compat32))
        return MMSYSERR_INVALPARAM;
    if (!(view.flags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;
    if ((view.flags & WHDR_INQUEUE) || !view.data || !view.buffer_length ||
        !block_align || view.buffer_length % block_align)
        return MMSYSERR_INVALPARAM;
    if (!win32_user_range_readable(view.data, view.buffer_length,
                                   compat32)) {
        serial_puts("[WINMM] rejected unreadable waveOut payload\n");
        return MMSYSERR_INVALPARAM;
    }

    WINMM_WAVE_BUFFER *buffer =
        (WINMM_WAVE_BUFFER *)kmalloc(sizeof(*buffer));
    if (!buffer)
        return MMSYSERR_NOMEM;
    memset(buffer, 0, sizeof(*buffer));
    buffer->data = (BYTE *)kmalloc(view.buffer_length);
    if (!buffer->data) {
        kfree(buffer);
        return MMSYSERR_NOMEM;
    }
    memcpy(buffer->data, view.data, view.buffer_length);
    buffer->guest_header = header;
    buffer->bytes = view.buffer_length;
    buffer->repeats_left =
        (view.flags & (WHDR_BEGINLOOP | WHDR_ENDLOOP)) ==
            (WHDR_BEGINLOOP | WHDR_ENDLOOP) && view.loops > 1U
        ? view.loops : 1U;

    irq_flags = winmm_timer_lock_irqsave();
    wave = winmm_wave_find_locked(handle, owner_pid);
    if (!wave) {
        winmm_timer_unlock_irqrestore(irq_flags);
        kfree(buffer->data);
        kfree(buffer);
        return MMSYSERR_INVALHANDLE;
    }
    for (WINMM_WAVE_BUFFER *queued = wave->queue_head; queued;
         queued = queued->next) {
        if (queued->guest_header == header) {
            winmm_timer_unlock_irqrestore(irq_flags);
            kfree(buffer->data);
            kfree(buffer);
            return WAVERR_STILLPLAYING;
        }
    }
    if (wave->queue_tail)
        wave->queue_tail->next = buffer;
    else
        wave->queue_head = buffer;
    wave->queue_tail = buffer;
    if (!wave->current)
        wave->current = buffer;
    if (!winmm_wave_set_header_flags(header, compat32, WHDR_INQUEUE,
                                     WHDR_DONE)) {
        winmm_wave_unlink_buffer_locked(wave, buffer);
        winmm_timer_unlock_irqrestore(irq_flags);
        kfree(buffer->data);
        kfree(buffer);
        return MMSYSERR_INVALPARAM;
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    audio_register_result_t registration = winmm_wave_refresh_mixer();
    if (registration != AUDIO_REGISTER_OK) {
        BOOL removed = FALSE;
        irq_flags = winmm_timer_lock_irqsave();
        wave = winmm_wave_find_locked(handle, owner_pid);
        if (wave)
            removed = winmm_wave_unlink_buffer_locked(wave, buffer);
        winmm_timer_unlock_irqrestore(irq_flags);

        if (removed) {
            DWORD set_flags = view.flags & WHDR_DONE;
            DWORD clear_flags = WHDR_INQUEUE;
            if (!(view.flags & WHDR_DONE))
                clear_flags |= WHDR_DONE;
            winmm_wave_set_header_flags(header, compat32, set_flags,
                                        clear_flags);
            kfree(buffer->data);
            kfree(buffer);
        } else {
            serial_puts("[WINMM] failed write already left the queue\n");
        }
        return winmm_wave_registration_mmresult(registration);
    }
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutPrepareHeader(PVOID handle, PVOID header,
                                              UINT size)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    BOOL compat32 = wave ? wave->compat32 : FALSE;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    if (!header || size < winmm_wave_header_size(compat32))
        return MMSYSERR_INVALPARAM;

    WINMM_WAVEHDR_VIEW view;
    if (!winmm_wave_read_header(header, compat32, &view))
        return MMSYSERR_INVALPARAM;
    if (view.flags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;
    if (!winmm_wave_set_header_flags(header, compat32, WHDR_PREPARED, 0))
        return MMSYSERR_INVALPARAM;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutPause(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave)
        wave->paused = TRUE;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    winmm_wave_refresh_mixer();
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_waveOutRestart(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave)
        wave->paused = FALSE;
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    audio_register_result_t registration = winmm_wave_refresh_mixer();
    if (registration == AUDIO_REGISTER_OK)
        return MMSYSERR_NOERROR;

    irq_flags = winmm_timer_lock_irqsave();
    wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave)
        wave->paused = TRUE;
    winmm_timer_unlock_irqrestore(irq_flags);
    return winmm_wave_registration_mmresult(registration);
}

static UINT WINAPI shim_waveOutBreakLoop(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave && wave->current)
        wave->current->repeats_left = 1;
    winmm_timer_unlock_irqrestore(irq_flags);
    return wave ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveOutSetVolume(PVOID handle, DWORD volume)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave)
        wave->volume = volume;
    winmm_timer_unlock_irqrestore(irq_flags);
    return wave ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveOutGetVolume(PVOID handle, DWORD *volume)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!win32_user_range_writable(volume, sizeof(*volume), compat32))
        return MMSYSERR_INVALPARAM;
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    if (wave)
        *volume = wave->volume;
    winmm_timer_unlock_irqrestore(irq_flags);
    return wave ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveOutGetID(PVOID handle, UINT *device_id)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (!win32_user_range_writable(device_id, sizeof(*device_id), compat32))
        return MMSYSERR_INVALPARAM;
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_WAVE_OUT *wave = winmm_wave_find_locked(handle, owner_pid);
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!wave)
        return MMSYSERR_INVALHANDLE;
    *device_id = 0;
    return MMSYSERR_NOERROR;
}

/* Capture is not backed by a hardware source yet. Keep the complete WinMM
 * surface graceful so probing applications can select another input path. */
static UINT WINAPI shim_waveInGetNumDevs(void)
{
    return 0;
}

static UINT WINAPI shim_waveInGetDevCapsA(UINT device, PVOID capabilities,
                                          UINT size)
{
    (void)device;
    (void)capabilities;
    (void)size;
    return MMSYSERR_NODRIVER;
}

static UINT WINAPI shim_waveInGetDevCapsW(UINT device, PVOID capabilities,
                                          UINT size)
{
    return shim_waveInGetDevCapsA(device, capabilities, size);
}

static UINT WINAPI shim_waveInGetErrorTextA(UINT error, char *text,
                                             UINT chars)
{
    return shim_waveOutGetErrorTextA(error, text, chars);
}

static UINT WINAPI shim_waveInGetErrorTextW(UINT error, PWSTR text,
                                             UINT chars)
{
    return shim_waveOutGetErrorTextW(error, text, chars);
}

static UINT WINAPI shim_waveInOpen(PVOID output, UINT device, PVOID format,
                                    ULONG_PTR callback, ULONG_PTR instance,
                                    DWORD flags)
{
    (void)device;
    (void)format;
    (void)callback;
    (void)instance;
    (void)flags;
    if (output && !winmm_wave_store_handle(
            output, g_compat32_mode ? TRUE : FALSE, NULL))
        return MMSYSERR_INVALPARAM;
    return MMSYSERR_NODRIVER;
}

static UINT WINAPI shim_waveInInvalidHandle1(PVOID input)
{
    (void)input;
    return MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveInInvalidHandle2(PVOID input, PVOID value)
{
    (void)input;
    (void)value;
    return MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveInInvalidHandle3(PVOID input, PVOID value,
                                              UINT size)
{
    (void)input;
    (void)value;
    (void)size;
    return MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_waveInMessage(PVOID input, UINT message,
                                       ULONG_PTR parameter1,
                                       ULONG_PTR parameter2)
{
    (void)input;
    (void)message;
    (void)parameter1;
    (void)parameter2;
    return MMSYSERR_INVALHANDLE;
}

/* AUX and mixer output topology */

static UINT winmm_aux_validate_device(ULONG_PTR device)
{
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;
    if (device != 0 && device != AUX_MAPPER)
        return MMSYSERR_BADDEVICEID;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_auxGetNumDevs(void)
{
    return audio_output_is_ready() ? 1U : 0U;
}

static UINT WINAPI shim_auxGetDevCapsA(ULONG_PTR device, PVOID capabilities,
                                        UINT size)
{
    UINT result = winmm_aux_validate_device(device);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!capabilities || !size)
        return MMSYSERR_INVALPARAM;

    WINMM_AUXCAPSA caps = {
        .manufacturer_id = 1,
        .product_id = 1,
        .driver_version = 0x00010000U,
        .product_name = "OsitoK Master Output",
        .technology = AUXCAPS_AUXIN,
        .reserved = 0,
        .support = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME,
    };
    UINT copied = size < sizeof(caps) ? size : sizeof(caps);
    memcpy(capabilities, &caps, copied);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_auxGetDevCapsW(ULONG_PTR device, PVOID capabilities,
                                        UINT size)
{
    UINT result = winmm_aux_validate_device(device);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!capabilities || !size)
        return MMSYSERR_INVALPARAM;

    WINMM_AUXCAPSW caps;
    memset(&caps, 0, sizeof(caps));
    caps.manufacturer_id = 1;
    caps.product_id = 1;
    caps.driver_version = 0x00010000U;
    caps.technology = AUXCAPS_AUXIN;
    caps.support = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME;
    winmm_copy_text_w(caps.product_name, 32, "OsitoK Master Output");
    UINT copied = size < sizeof(caps) ? size : sizeof(caps);
    memcpy(capabilities, &caps, copied);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_auxSetVolume(UINT device, DWORD volume)
{
    UINT result = winmm_aux_validate_device(device);
    if (result != MMSYSERR_NOERROR)
        return result;
    audio_output_set_volume((uint16_t)volume, (uint16_t)(volume >> 16));
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_auxGetVolume(UINT device, DWORD *volume)
{
    UINT result = winmm_aux_validate_device(device);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!volume)
        return MMSYSERR_INVALPARAM;
    uint16_t left, right;
    audio_output_get_volume(&left, &right);
    *volume = (DWORD)left | ((DWORD)right << 16);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_auxOutMessage(UINT device, UINT message,
                                       ULONG_PTR parameter1,
                                       ULONG_PTR parameter2)
{
    (void)message;
    (void)parameter1;
    (void)parameter2;
    UINT result = winmm_aux_validate_device(device);
    return result == MMSYSERR_NOERROR ? MMSYSERR_NOTSUPPORTED : result;
}

static WINMM_MIXER *winmm_mixer_find_locked(PVOID handle, DWORD owner_pid)
{
    if (!handle || !owner_pid)
        return NULL;
    for (int i = 0; i < WINMM_MIXER_SLOTS; i++) {
        WINMM_MIXER *mixer = &winmm_mixers[i];
        if (mixer->in_use && mixer->owner_pid == owner_pid &&
            mixer->handle == handle)
            return mixer;
    }
    return NULL;
}

static PVOID winmm_mixer_allocate_handle_locked(void)
{
    for (;;) {
        uint32_t sequence = __atomic_fetch_add(&winmm_next_mixer_handle, 1,
                                               __ATOMIC_RELAXED);
        ULONG_PTR value = 0x4D000000U | (sequence & 0x00FFFFFFU);
        if (value == 0x4D000000U)
            continue;
        PVOID candidate = (PVOID)value;
        BOOL used = FALSE;
        for (int i = 0; i < WINMM_MIXER_SLOTS; i++) {
            if (winmm_mixers[i].in_use &&
                winmm_mixers[i].handle == candidate) {
                used = TRUE;
                break;
            }
        }
        if (!used)
            return candidate;
    }
}

static UINT winmm_mixer_validate_id(ULONG_PTR mixer_id)
{
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;
    if (mixer_id == 0)
        return MMSYSERR_NOERROR;

    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MIXER *mixer = winmm_mixer_find_locked((PVOID)mixer_id, owner_pid);
    winmm_timer_unlock_irqrestore(irq_flags);
    return mixer ? MMSYSERR_NOERROR : MMSYSERR_BADDEVICEID;
}

static UINT winmm_mixer_validate_object(PVOID object, DWORD flags)
{
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;

    DWORD type = flags & MIXER_OBJECTF_TYPEMASK;
    BOOL is_handle = (flags & MIXER_OBJECTF_HANDLE) != 0;
    ULONG_PTR value = (ULONG_PTR)object;
    DWORD owner_pid = GetCurrentProcessId();

    if (type == MIXER_OBJECTF_MIXER) {
        if (!is_handle)
            return value == 0 ? MMSYSERR_NOERROR : MMSYSERR_BADDEVICEID;
        uint64_t irq_flags = winmm_timer_lock_irqsave();
        WINMM_MIXER *mixer = winmm_mixer_find_locked(object, owner_pid);
        winmm_timer_unlock_irqrestore(irq_flags);
        return mixer ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
    }

    if (type == MIXER_OBJECTF_WAVEOUT) {
        if (!is_handle)
            return (value == 0 || value == WAVE_MAPPER)
                 ? MMSYSERR_NOERROR : MMSYSERR_BADDEVICEID;
        uint64_t irq_flags = winmm_timer_lock_irqsave();
        WINMM_WAVE_OUT *wave = winmm_wave_find_locked(object, owner_pid);
        winmm_timer_unlock_irqrestore(irq_flags);
        return wave ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
    }

    if (type == MIXER_OBJECTF_AUX && !is_handle)
        return winmm_aux_validate_device(value);
    if (type == MIXER_OBJECTF_WAVEIN)
        return MMSYSERR_NODRIVER;
    return MMSYSERR_BADDEVICEID;
}

static BOOL winmm_mixer_valid_query_flags(DWORD flags, DWORD query_mask)
{
    return (flags & ~(MIXER_OBJECTF_MASK | query_mask)) == 0;
}

static UINT WINAPI shim_mixerGetNumDevs(void)
{
    return audio_output_is_ready() ? 1U : 0U;
}

static UINT WINAPI shim_mixerGetDevCapsA(ULONG_PTR mixer_id,
                                          PVOID capabilities, UINT size)
{
    UINT result = winmm_mixer_validate_id(mixer_id);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!capabilities || !size)
        return MMSYSERR_INVALPARAM;

    WINMM_MIXERCAPSA caps = {
        .manufacturer_id = 1,
        .product_id = 1,
        .driver_version = 0x00010000U,
        .product_name = "OsitoK Audio Mixer",
        .support = 0,
        .destinations = 1,
    };
    UINT copied = size < sizeof(caps) ? size : sizeof(caps);
    memcpy(capabilities, &caps, copied);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerGetDevCapsW(ULONG_PTR mixer_id,
                                          PVOID capabilities, UINT size)
{
    UINT result = winmm_mixer_validate_id(mixer_id);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!capabilities || !size)
        return MMSYSERR_INVALPARAM;

    WINMM_MIXERCAPSW caps;
    memset(&caps, 0, sizeof(caps));
    caps.manufacturer_id = 1;
    caps.product_id = 1;
    caps.driver_version = 0x00010000U;
    caps.destinations = 1;
    winmm_copy_text_w(caps.product_name, 32, "OsitoK Audio Mixer");
    UINT copied = size < sizeof(caps) ? size : sizeof(caps);
    memcpy(capabilities, &caps, copied);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerOpen(PVOID output, UINT device,
                                   ULONG_PTR callback, ULONG_PTR instance,
                                   DWORD flags)
{
    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    if (output && !winmm_wave_store_handle(output, compat32, NULL))
        return MMSYSERR_INVALPARAM;
    if (!audio_output_is_ready())
        return MMSYSERR_NODRIVER;
    if (device != 0)
        return MMSYSERR_BADDEVICEID;
    if (!output)
        return MMSYSERR_INVALPARAM;

    DWORD callback_type = flags & CALLBACK_TYPEMASK;
    if ((flags & ~CALLBACK_TYPEMASK) || callback_type == 0x00040000U ||
        callback_type == 0x00060000U || callback_type == 0x00070000U)
        return MMSYSERR_INVALFLAG;

    WINMM_MIXER *mixer = NULL;
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_MIXER_SLOTS; i++) {
        if (!winmm_mixers[i].in_use) {
            mixer = &winmm_mixers[i];
            break;
        }
    }
    if (mixer) {
        memset(mixer, 0, sizeof(*mixer));
        mixer->in_use = TRUE;
        mixer->compat32 = compat32;
        mixer->owner_pid = GetCurrentProcessId();
        mixer->handle = winmm_mixer_allocate_handle_locked();
        mixer->callback = (PVOID)callback;
        mixer->instance = instance;
        mixer->callback_flags = callback_type;
    }
    winmm_timer_unlock_irqrestore(irq_flags);
    if (!mixer)
        return MMSYSERR_ALLOCATED;

    winmm_wave_store_handle(output, compat32, mixer->handle);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerClose(PVOID handle)
{
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MIXER *mixer = winmm_mixer_find_locked(handle, owner_pid);
    if (mixer)
        memset(mixer, 0, sizeof(*mixer));
    winmm_timer_unlock_irqrestore(irq_flags);
    return mixer ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

static DWORD WINAPI shim_mixerMessage(PVOID handle, UINT message,
                                       ULONG_PTR parameter1,
                                       ULONG_PTR parameter2)
{
    (void)message;
    (void)parameter1;
    (void)parameter2;
    DWORD owner_pid = GetCurrentProcessId();
    uint64_t irq_flags = winmm_timer_lock_irqsave();
    WINMM_MIXER *mixer = winmm_mixer_find_locked(handle, owner_pid);
    winmm_timer_unlock_irqrestore(irq_flags);
    return mixer ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE;
}

static UINT WINAPI shim_mixerGetID(PVOID object, UINT *mixer_id, DWORD flags)
{
    if (!winmm_mixer_valid_query_flags(flags, 0))
        return MMSYSERR_INVALFLAG;
    UINT result = winmm_mixer_validate_object(object, flags);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!mixer_id)
        return MMSYSERR_INVALPARAM;
    *mixer_id = 0;
    return MMSYSERR_NOERROR;
}

/* ── joyGetPosEx ──────────────────────────────────────────── */

static UINT winmm_mixer_get_line_info(PVOID object, PVOID information,
                                       DWORD flags, BOOL wide)
{
    if (!winmm_mixer_valid_query_flags(flags,
                                        MIXER_GETLINEINFOF_QUERYMASK))
        return MMSYSERR_INVALFLAG;
    UINT result = winmm_mixer_validate_object(object, flags);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (!information)
        return MMSYSERR_INVALPARAM;

    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    DWORD required = wide
        ? (compat32 ? sizeof(WINMM_MIXERLINEW32)
                    : sizeof(WINMM_MIXERLINEW64))
        : (compat32 ? sizeof(WINMM_MIXERLINEA32)
                    : sizeof(WINMM_MIXERLINEA64));
    DWORD cb_struct = *(DWORD *)information;
    if (cb_struct < required)
        return MMSYSERR_INVALPARAM;

    DWORD destination, source, line_id, component_type, target_type;
    if (wide && compat32) {
        WINMM_MIXERLINEW32 *line = (WINMM_MIXERLINEW32 *)information;
        destination = line->destination;
        source = line->source;
        line_id = line->line_id;
        component_type = line->component_type;
        target_type = line->target_type;
    } else if (wide) {
        WINMM_MIXERLINEW64 *line = (WINMM_MIXERLINEW64 *)information;
        destination = line->destination;
        source = line->source;
        line_id = line->line_id;
        component_type = line->component_type;
        target_type = line->target_type;
    } else if (compat32) {
        WINMM_MIXERLINEA32 *line = (WINMM_MIXERLINEA32 *)information;
        destination = line->destination;
        source = line->source;
        line_id = line->line_id;
        component_type = line->component_type;
        target_type = line->target_type;
    } else {
        WINMM_MIXERLINEA64 *line = (WINMM_MIXERLINEA64 *)information;
        destination = line->destination;
        source = line->source;
        line_id = line->line_id;
        component_type = line->component_type;
        target_type = line->target_type;
    }

    switch (flags & MIXER_GETLINEINFOF_QUERYMASK) {
    case MIXER_GETLINEINFOF_DESTINATION:
        if (destination != 0)
            return MIXERR_INVALLINE;
        break;
    case MIXER_GETLINEINFOF_SOURCE:
        (void)source;
        return MIXERR_INVALLINE;
    case MIXER_GETLINEINFOF_LINEID:
        if (line_id != WINMM_MIXER_LINE_ID)
            return MIXERR_INVALLINE;
        break;
    case MIXER_GETLINEINFOF_COMPONENTTYPE:
        if (component_type != MIXERLINE_COMPONENTTYPE_DST_SPEAKERS)
            return MIXERR_INVALLINE;
        break;
    case MIXER_GETLINEINFOF_TARGETTYPE:
        if (target_type != MIXERLINE_TARGETTYPE_WAVEOUT)
            return MIXERR_INVALLINE;
        break;
    default:
        return MMSYSERR_INVALFLAG;
    }

#define WINMM_FILL_MIXER_LINE(line, copy_text)                    \
    do {                                                          \
        (line).cb_struct = cb_struct;                              \
        (line).destination = 0;                                   \
        (line).source = 0;                                        \
        (line).line_id = WINMM_MIXER_LINE_ID;                     \
        (line).flags = MIXERLINE_LINEF_ACTIVE;                    \
        (line).component_type =                                   \
            MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;                 \
        (line).channels = 2;                                      \
        (line).connections = 0;                                   \
        (line).controls = 1;                                      \
        (line).target_type = MIXERLINE_TARGETTYPE_WAVEOUT;        \
        (line).target_device_id = 0;                              \
        (line).target_manufacturer_id = 1;                        \
        (line).target_product_id = 1;                             \
        (line).target_driver_version = 0x00010000U;               \
        copy_text((line).short_name, 16, "Master");              \
        copy_text((line).name, 64, "OsitoK Master Output");      \
        copy_text((line).target_product_name, 32,                 \
                  "OsitoK PCM Output");                          \
    } while (0)

    if (wide && compat32) {
        WINMM_MIXERLINEW32 line;
        memset(&line, 0, sizeof(line));
        WINMM_FILL_MIXER_LINE(line, winmm_copy_text_w);
        memcpy(information, &line, sizeof(line));
    } else if (wide) {
        WINMM_MIXERLINEW64 line;
        memset(&line, 0, sizeof(line));
        WINMM_FILL_MIXER_LINE(line, winmm_copy_text_w);
        memcpy(information, &line, sizeof(line));
    } else if (compat32) {
        WINMM_MIXERLINEA32 line;
        memset(&line, 0, sizeof(line));
        WINMM_FILL_MIXER_LINE(line, winmm_copy_text_a);
        memcpy(information, &line, sizeof(line));
    } else {
        WINMM_MIXERLINEA64 line;
        memset(&line, 0, sizeof(line));
        WINMM_FILL_MIXER_LINE(line, winmm_copy_text_a);
        memcpy(information, &line, sizeof(line));
    }

#undef WINMM_FILL_MIXER_LINE
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerGetLineInfoA(PVOID object, PVOID information,
                                           DWORD flags)
{
    return winmm_mixer_get_line_info(object, information, flags, FALSE);
}

static UINT WINAPI shim_mixerGetLineInfoW(PVOID object, PVOID information,
                                           DWORD flags)
{
    return winmm_mixer_get_line_info(object, information, flags, TRUE);
}

typedef struct {
    DWORD cb_struct;
    DWORD line_id;
    DWORD control_id_or_type;
    DWORD controls;
    DWORD control_size;
    PVOID control_array;
} WINMM_MIXERLINECONTROLS_VIEW;

static BOOL winmm_mixer_read_line_controls(
    PVOID controls, BOOL compat32, WINMM_MIXERLINECONTROLS_VIEW *view)
{
    if (!controls || !view)
        return FALSE;
    if (compat32) {
        WINMM_MIXERLINECONTROLS32 *source =
            (WINMM_MIXERLINECONTROLS32 *)controls;
        view->cb_struct = source->cb_struct;
        view->line_id = source->line_id;
        view->control_id_or_type = source->control_id_or_type;
        view->controls = source->controls;
        view->control_size = source->control_size;
        view->control_array = (PVOID)(ULONG_PTR)source->control_array;
    } else {
        WINMM_MIXERLINECONTROLS64 *source =
            (WINMM_MIXERLINECONTROLS64 *)controls;
        view->cb_struct = source->cb_struct;
        view->line_id = source->line_id;
        view->control_id_or_type = source->control_id_or_type;
        view->controls = source->controls;
        view->control_size = source->control_size;
        view->control_array = (PVOID)(ULONG_PTR)source->control_array;
    }
    return TRUE;
}

static UINT winmm_mixer_get_line_controls(PVOID object, PVOID controls,
                                           DWORD flags, BOOL wide)
{
    if (!winmm_mixer_valid_query_flags(
            flags, MIXER_GETLINECONTROLSF_QUERYMASK))
        return MMSYSERR_INVALFLAG;
    UINT result = winmm_mixer_validate_object(object, flags);
    if (result != MMSYSERR_NOERROR)
        return result;

    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    WINMM_MIXERLINECONTROLS_VIEW view;
    if (!winmm_mixer_read_line_controls(controls, compat32, &view) ||
        view.cb_struct < (compat32 ? sizeof(WINMM_MIXERLINECONTROLS32)
                                   : sizeof(WINMM_MIXERLINECONTROLS64)) ||
        !view.control_array || !view.controls)
        return MMSYSERR_INVALPARAM;
    if (view.line_id != WINMM_MIXER_LINE_ID)
        return MIXERR_INVALLINE;

    switch (flags & MIXER_GETLINECONTROLSF_QUERYMASK) {
    case MIXER_GETLINECONTROLSF_ALL:
        break;
    case MIXER_GETLINECONTROLSF_ONEBYID:
        if (view.control_id_or_type != WINMM_MIXER_CONTROL_ID)
            return MIXERR_INVALCONTROL;
        break;
    case MIXER_GETLINECONTROLSF_ONEBYTYPE:
        if (view.control_id_or_type != MIXERCONTROL_CONTROLTYPE_VOLUME)
            return MIXERR_INVALCONTROL;
        break;
    default:
        return MMSYSERR_INVALFLAG;
    }

    DWORD required = wide ? sizeof(WINMM_MIXERCONTROLW)
                          : sizeof(WINMM_MIXERCONTROLA);
    if (view.control_size < required)
        return MMSYSERR_INVALPARAM;

#define WINMM_FILL_MIXER_CONTROL(control, copy_text)                \
    do {                                                            \
        (control).cb_struct = sizeof(control);                       \
        (control).control_id = WINMM_MIXER_CONTROL_ID;              \
        (control).control_type = MIXERCONTROL_CONTROLTYPE_VOLUME;   \
        (control).flags = 0;                                        \
        (control).multiple_items = 0;                               \
        (control).bounds[0] = 0;                                    \
        (control).bounds[1] = 65535U;                               \
        (control).metrics[0] = 65536U;                              \
        copy_text((control).short_name, 16, "Volume");             \
        copy_text((control).name, 64, "Master Volume");            \
    } while (0)

    if (wide) {
        WINMM_MIXERCONTROLW control;
        memset(&control, 0, sizeof(control));
        WINMM_FILL_MIXER_CONTROL(control, winmm_copy_text_w);
        memcpy(view.control_array, &control, sizeof(control));
    } else {
        WINMM_MIXERCONTROLA control;
        memset(&control, 0, sizeof(control));
        WINMM_FILL_MIXER_CONTROL(control, winmm_copy_text_a);
        memcpy(view.control_array, &control, sizeof(control));
    }

#undef WINMM_FILL_MIXER_CONTROL
    if (compat32)
        ((WINMM_MIXERLINECONTROLS32 *)controls)->controls = 1;
    else
        ((WINMM_MIXERLINECONTROLS64 *)controls)->controls = 1;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerGetLineControlsA(PVOID object, PVOID controls,
                                               DWORD flags)
{
    return winmm_mixer_get_line_controls(object, controls, flags, FALSE);
}

static UINT WINAPI shim_mixerGetLineControlsW(PVOID object, PVOID controls,
                                               DWORD flags)
{
    return winmm_mixer_get_line_controls(object, controls, flags, TRUE);
}

typedef struct {
    DWORD cb_struct;
    DWORD control_id;
    DWORD channels;
    DWORD multiple_items;
    DWORD detail_size;
    PVOID details;
} WINMM_MIXERCONTROLDETAILS_VIEW;

static BOOL winmm_mixer_read_control_details(
    PVOID details, BOOL compat32, WINMM_MIXERCONTROLDETAILS_VIEW *view)
{
    if (!details || !view)
        return FALSE;
    if (compat32) {
        WINMM_MIXERCONTROLDETAILS32 *source =
            (WINMM_MIXERCONTROLDETAILS32 *)details;
        view->cb_struct = source->cb_struct;
        view->control_id = source->control_id;
        view->channels = source->channels;
        view->multiple_items = source->multiple_items;
        view->detail_size = source->detail_size;
        view->details = (PVOID)(ULONG_PTR)source->details;
    } else {
        WINMM_MIXERCONTROLDETAILS64 *source =
            (WINMM_MIXERCONTROLDETAILS64 *)details;
        view->cb_struct = source->cb_struct;
        view->control_id = source->control_id;
        view->channels = source->channels;
        view->multiple_items = (DWORD)source->owner_or_multiple_items;
        view->detail_size = source->detail_size;
        view->details = (PVOID)(ULONG_PTR)source->details;
    }
    return TRUE;
}

static UINT winmm_mixer_control_details(PVOID object, PVOID details,
                                         DWORD flags, BOOL set)
{
    if (!winmm_mixer_valid_query_flags(
            flags, MIXER_GETCONTROLDETAILSF_QUERYMASK) ||
        (flags & MIXER_GETCONTROLDETAILSF_QUERYMASK) !=
            MIXER_GETCONTROLDETAILSF_VALUE)
        return MMSYSERR_INVALFLAG;
    UINT result = winmm_mixer_validate_object(object, flags);
    if (result != MMSYSERR_NOERROR)
        return result;

    BOOL compat32 = g_compat32_mode ? TRUE : FALSE;
    WINMM_MIXERCONTROLDETAILS_VIEW view;
    if (!winmm_mixer_read_control_details(details, compat32, &view) ||
        view.cb_struct < (compat32 ? sizeof(WINMM_MIXERCONTROLDETAILS32)
                                   : sizeof(WINMM_MIXERCONTROLDETAILS64)) ||
        !view.details || view.detail_size < sizeof(DWORD) ||
        (view.channels != 1 && view.channels != 2))
        return MMSYSERR_INVALPARAM;
    if (view.control_id != WINMM_MIXER_CONTROL_ID)
        return MIXERR_INVALCONTROL;

    DWORD *values = (DWORD *)view.details;
    if (set) {
        if (values[0] > 65535U ||
            (view.channels == 2 && values[1] > 65535U))
            return MIXERR_INVALVALUE;
        uint16_t left = (uint16_t)values[0];
        uint16_t right = view.channels == 2 ? (uint16_t)values[1] : left;
        audio_output_set_volume(left, right);
    } else {
        uint16_t left, right;
        audio_output_get_volume(&left, &right);
        if (view.channels == 1) {
            values[0] = ((DWORD)left + (DWORD)right + 1U) / 2U;
        } else {
            values[0] = left;
            values[1] = right;
        }
    }
    return MMSYSERR_NOERROR;
}

static UINT WINAPI shim_mixerGetControlDetailsA(PVOID object, PVOID details,
                                                 DWORD flags)
{
    return winmm_mixer_control_details(object, details, flags, FALSE);
}

static UINT WINAPI shim_mixerGetControlDetailsW(PVOID object, PVOID details,
                                                 DWORD flags)
{
    return winmm_mixer_control_details(object, details, flags, FALSE);
}

static UINT WINAPI shim_mixerSetControlDetails(PVOID object, PVOID details,
                                                DWORD flags)
{
    return winmm_mixer_control_details(object, details, flags, TRUE);
}

static UINT WINAPI shim_joyGetPosEx(UINT id, PVOID info)
    { (void)id; (void)info; return 167; /* JOYERR_UNPLUGGED */ }

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT winmm_exports[] = {
    { "timeGetTime",              (PVOID)shim_timeGetTime,             0, CC_STDCALL },
    { "timeBeginPeriod",          (PVOID)shim_timeBeginPeriod,         1, CC_STDCALL },
    { "timeEndPeriod",            (PVOID)shim_timeEndPeriod,           1, CC_STDCALL },
    { "timeGetDevCaps",           (PVOID)shim_timeGetDevCaps,          2, CC_STDCALL },
    { "timeSetEvent",             (PVOID)shim_timeSetEvent,            5, CC_STDCALL },
    { "timeKillEvent",            (PVOID)shim_timeKillEvent,           1, CC_STDCALL },
    { "joyGetNumDevs",            (PVOID)shim_joyGetNumDevs,           0, CC_STDCALL },
    { "joyGetDevCapsA",           (PVOID)shim_joyGetDevCapsA,          3, CC_STDCALL },
    { "joyGetPosEx",              (PVOID)shim_joyGetPosEx,             2, CC_STDCALL },
    { "PlaySoundA",               (PVOID)shim_PlaySoundA,              3, CC_STDCALL },
    { "PlaySoundW",               (PVOID)shim_PlaySoundW,              3, CC_STDCALL },
    { "sndPlaySoundA",            (PVOID)shim_sndPlaySoundA,           2, CC_STDCALL },
    { "sndPlaySoundW",            (PVOID)shim_sndPlaySoundW,           2, CC_STDCALL },
    { "waveOutGetNumDevs",        (PVOID)shim_waveOutGetNumDevs,       0, CC_STDCALL },
    { "waveOutReset",             (PVOID)shim_waveOutReset,            1, CC_STDCALL },
    { "waveOutUnprepareHeader",   (PVOID)shim_waveOutUnprepareHeader,  3, CC_STDCALL },
    { "waveOutGetPosition",       (PVOID)shim_waveOutGetPosition,      3, CC_STDCALL },
    { "waveOutGetDevCapsA",       (PVOID)shim_waveOutGetDevCapsA,      3, CC_STDCALL },
    { "waveOutGetErrorTextA",     (PVOID)shim_waveOutGetErrorTextA,    3, CC_STDCALL },
    { "waveOutGetErrorTextW",     (PVOID)shim_waveOutGetErrorTextW,    3, CC_STDCALL },
    { "waveOutOpen",              (PVOID)shim_waveOutOpen,             6, CC_STDCALL },
    { "waveOutMessage",           (PVOID)shim_waveOutMessage,          4, CC_STDCALL },
    { "waveOutClose",             (PVOID)shim_waveOutClose,            1, CC_STDCALL },
    { "waveOutWrite",             (PVOID)shim_waveOutWrite,            3, CC_STDCALL },
    { "waveOutPrepareHeader",     (PVOID)shim_waveOutPrepareHeader,    3, CC_STDCALL },
    { "waveOutPause",             (PVOID)shim_waveOutPause,            1, CC_STDCALL },
    { "waveOutRestart",           (PVOID)shim_waveOutRestart,          1, CC_STDCALL },
    { "waveOutBreakLoop",         (PVOID)shim_waveOutBreakLoop,        1, CC_STDCALL },
    { "waveOutSetVolume",         (PVOID)shim_waveOutSetVolume,        2, CC_STDCALL },
    { "waveOutGetVolume",         (PVOID)shim_waveOutGetVolume,        2, CC_STDCALL },
    { "waveOutGetID",             (PVOID)shim_waveOutGetID,            2, CC_STDCALL },
    { "waveInGetNumDevs",         (PVOID)shim_waveInGetNumDevs,        0, CC_STDCALL },
    { "waveInGetDevCapsA",        (PVOID)shim_waveInGetDevCapsA,       3, CC_STDCALL },
    { "waveInGetDevCapsW",        (PVOID)shim_waveInGetDevCapsW,       3, CC_STDCALL },
    { "waveInGetErrorTextA",      (PVOID)shim_waveInGetErrorTextA,     3, CC_STDCALL },
    { "waveInGetErrorTextW",      (PVOID)shim_waveInGetErrorTextW,     3, CC_STDCALL },
    { "waveInMessage",            (PVOID)shim_waveInMessage,           4, CC_STDCALL },
    { "waveInOpen",               (PVOID)shim_waveInOpen,              6, CC_STDCALL },
    { "waveInClose",              (PVOID)shim_waveInInvalidHandle1,    1, CC_STDCALL },
    { "waveInPrepareHeader",      (PVOID)shim_waveInInvalidHandle3,    3, CC_STDCALL },
    { "waveInUnprepareHeader",    (PVOID)shim_waveInInvalidHandle3,    3, CC_STDCALL },
    { "waveInAddBuffer",          (PVOID)shim_waveInInvalidHandle3,    3, CC_STDCALL },
    { "waveInStart",              (PVOID)shim_waveInInvalidHandle1,    1, CC_STDCALL },
    { "waveInStop",               (PVOID)shim_waveInInvalidHandle1,    1, CC_STDCALL },
    { "waveInReset",              (PVOID)shim_waveInInvalidHandle1,    1, CC_STDCALL },
    { "waveInGetPosition",        (PVOID)shim_waveInInvalidHandle3,    3, CC_STDCALL },
    { "waveInGetID",              (PVOID)shim_waveInInvalidHandle2,    2, CC_STDCALL },
    { "auxGetNumDevs",            (PVOID)shim_auxGetNumDevs,           0, CC_STDCALL },
    { "auxGetDevCapsA",           (PVOID)shim_auxGetDevCapsA,          3, CC_STDCALL },
    { "auxGetDevCapsW",           (PVOID)shim_auxGetDevCapsW,          3, CC_STDCALL },
    { "auxSetVolume",             (PVOID)shim_auxSetVolume,            2, CC_STDCALL },
    { "auxGetVolume",             (PVOID)shim_auxGetVolume,            2, CC_STDCALL },
    { "auxOutMessage",            (PVOID)shim_auxOutMessage,           4, CC_STDCALL },
    { "mixerGetNumDevs",          (PVOID)shim_mixerGetNumDevs,         0, CC_STDCALL },
    { "mixerOpen",                (PVOID)shim_mixerOpen,               5, CC_STDCALL },
    { "mixerClose",               (PVOID)shim_mixerClose,              1, CC_STDCALL },
    { "mixerMessage",             (PVOID)shim_mixerMessage,            4, CC_STDCALL },
    { "mixerGetID",               (PVOID)shim_mixerGetID,              3, CC_STDCALL },
    { "mixerGetControlDetailsA",  (PVOID)shim_mixerGetControlDetailsA, 3, CC_STDCALL },
    { "mixerGetControlDetailsW",  (PVOID)shim_mixerGetControlDetailsW, 3, CC_STDCALL },
    { "mixerGetDevCapsA",         (PVOID)shim_mixerGetDevCapsA,        3, CC_STDCALL },
    { "mixerGetDevCapsW",         (PVOID)shim_mixerGetDevCapsW,        3, CC_STDCALL },
    { "mixerGetLineInfoA",        (PVOID)shim_mixerGetLineInfoA,       3, CC_STDCALL },
    { "mixerGetLineInfoW",        (PVOID)shim_mixerGetLineInfoW,       3, CC_STDCALL },
    { "mixerGetLineControlsA",    (PVOID)shim_mixerGetLineControlsA,   3, CC_STDCALL },
    { "mixerGetLineControlsW",    (PVOID)shim_mixerGetLineControlsW,   3, CC_STDCALL },
    { "mixerSetControlDetails",   (PVOID)shim_mixerSetControlDetails,  3, CC_STDCALL },
    { "mciSendCommandA",          (PVOID)shim_mciSendCommandA,         4, CC_STDCALL },
    { "mciSendCommandW",          (PVOID)shim_mciSendCommandW,         4, CC_STDCALL },
    { "mciSendStringA",           (PVOID)shim_mciSendStringA,          4, CC_STDCALL },
    { "mciSendStringW",           (PVOID)shim_mciSendStringW,          4, CC_STDCALL },
    { "mciGetDeviceIDA",          (PVOID)shim_mciGetDeviceIDA,         1, CC_STDCALL },
    { "mciGetDeviceIDW",          (PVOID)shim_mciGetDeviceIDW,         1, CC_STDCALL },
    { "mciGetErrorStringA",       (PVOID)shim_mciGetErrorStringA,      3, CC_STDCALL },
    { "mciGetErrorStringW",       (PVOID)shim_mciGetErrorStringW,      3, CC_STDCALL },
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

void winmm_release_process(DWORD process_id)
{
    if (!process_id)
        return;

    HANDLE workers[WINMM_DISPATCHER_SLOTS];
    DWORD worker_tids[WINMM_DISPATCHER_SLOTS];
    WINMM_WAVE_BUFFER *wave_buffers[WINMM_WAVE_OUT_SLOTS];
    BYTE *play_sound_data[WINMM_PLAY_SOUND_SLOTS];
    BYTE *mci_data[WINMM_MCI_SLOTS];
    int worker_count = 0;
    int wave_buffer_count = 0;
    int play_sound_count = 0;
    int mci_count = 0;
    uint64_t irq_flags = winmm_timer_lock_irqsave();

    for (int i = 0; i < WINMM_TIMER_SLOTS; i++) {
        WINMM_TIMER *timer = &winmm_timers[i];
        if (__atomic_load_n(&timer->allocated, __ATOMIC_ACQUIRE) &&
            timer->owner_pid == process_id) {
            __atomic_store_n(&timer->canceled, 1, __ATOMIC_RELEASE);
        }
    }
    for (int i = 0; i < WINMM_PERIOD_SLOTS; i++) {
        if (winmm_periods[i].references &&
            winmm_periods[i].owner_pid == process_id)
            memset(&winmm_periods[i], 0, sizeof(winmm_periods[i]));
    }
    for (int i = 0; i < WINMM_WAVE_OUT_SLOTS; i++) {
        WINMM_WAVE_OUT *wave = &winmm_wave_outs[i];
        if (wave->in_use && wave->owner_pid == process_id) {
            wave_buffers[wave_buffer_count++] = wave->queue_head;
            memset(wave, 0, sizeof(*wave));
        }
    }
    for (int i = 0; i < WINMM_PLAY_SOUND_SLOTS; i++) {
        WINMM_PLAY_SOUND *sound = &winmm_play_sounds[i];
        if (sound->in_use && sound->owner_pid == process_id) {
            play_sound_data[play_sound_count++] = sound->data;
            memset(sound, 0, sizeof(*sound));
        }
    }
    for (int i = 0; i < WINMM_MCI_SLOTS; i++) {
        WINMM_MCI_DEVICE *device = &winmm_mci_devices[i];
        if (device->in_use && device->owner_pid == process_id) {
            mci_data[mci_count++] = device->data;
            memset(device, 0, sizeof(*device));
        }
    }
    for (int i = 0; i < WINMM_MIXER_SLOTS; i++) {
        if (winmm_mixers[i].in_use &&
            winmm_mixers[i].owner_pid == process_id)
            memset(&winmm_mixers[i], 0, sizeof(winmm_mixers[i]));
    }
    for (int i = 0; i < WINMM_DISPATCHER_SLOTS; i++) {
        WINMM_TIMER_DISPATCHER *dispatcher = &winmm_dispatchers[i];
        if (dispatcher->state != WINMM_DISPATCHER_FREE &&
            dispatcher->owner_pid == process_id) {
            __atomic_store_n(&dispatcher->stop, 1, __ATOMIC_RELEASE);
            workers[worker_count] = dispatcher->worker;
            worker_tids[worker_count] = dispatcher->worker_tid;
            worker_count++;
        }
    }
    winmm_timer_unlock_irqrestore(irq_flags);

    winmm_wave_refresh_mixer();

    DWORD caller_tid = GetCurrentThreadId();
    for (int i = 0; i < worker_count; i++) {
        if (!workers[i] || worker_tids[i] == caller_tid)
            continue;
        DWORD result = WaitForSingleObject(workers[i], 250);
        if (result == 0x00000102U) {
            TerminateThread(workers[i], 0);
            (void)WaitForSingleObject(workers[i], 100);
        }
        CloseHandle(workers[i]);
    }

    for (int i = 0; i < wave_buffer_count; i++)
        winmm_wave_complete_list(wave_buffers[i], NULL, NULL, 0, 0,
                                 FALSE, FALSE);
    for (int i = 0; i < play_sound_count; i++)
        kfree(play_sound_data[i]);
    for (int i = 0; i < mci_count; i++)
        kfree(mci_data[i]);

    irq_flags = winmm_timer_lock_irqsave();
    for (int i = 0; i < WINMM_TIMER_SLOTS; i++) {
        if (winmm_timers[i].owner_pid == process_id)
            winmm_timer_clear_locked(&winmm_timers[i]);
    }
    for (int i = 0; i < WINMM_DISPATCHER_SLOTS; i++) {
        if (winmm_dispatchers[i].owner_pid == process_id)
            memset(&winmm_dispatchers[i], 0,
                   sizeof(winmm_dispatchers[i]));
    }
    winmm_timer_unlock_irqrestore(irq_flags);
}

void winmm_shim_init(void)
{
    winmm_dispatcher_thunk32 = 0;
    __atomic_store_n(&winmm_callback_trace_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&winmm_unsupported_trace_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&winmm_play_sound_failure_trace_count, 0,
                     __ATOMIC_RELEASE);
    serial_puts("[WINMM] winmm.dll shim initialized\n");
    serial_puts("[WINMM] waveOut backend: ");
    serial_puts(audio_output_backend_name());
    serial_puts("\n");
}
