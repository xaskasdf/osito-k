/*
 * OsitoK Windows Compatibility Layer — dsound.dll Shim
 *
 * Real DirectSound audio output via Intel HDA driver.
 * Implements IDirectSoundBuffer COM interface with circular audio buffers,
 * sample rate conversion (22050/44100 → 48000), and multi-buffer mixing.
 *
 * UT99 audio pipeline:
 *   DirectSoundCreate → SetCooperativeLevel →
 *   CreateSoundBuffer(primary) → SetFormat →
 *   CreateSoundBuffer(secondary, N bytes) → Lock → write PCM →
 *   Unlock → Play(LOOPING) → GetCurrentPosition → repeat
 */

#include "dsound_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* memset/memcpy provided by types.h (included via nttypes.h) */

/* HDA driver interface — extern to avoid including hda.h */
extern void hda_play_buffer(const int16_t *samples, uint32_t num_samples);
extern void hda_stop(void);
extern int  hda_is_ready(void);

/* ── HRESULT / DirectSound constants ──────────────────────── */

#define DS_OK             ((HRESULT)0)
#define E_NOINTERFACE     ((HRESULT)0x80004002)
#define DSERR_GENERIC     ((HRESULT)0x80004005)
#define DSERR_INVALIDPARAM ((HRESULT)0x80070057)

/* DSBUFFERDESC.dwFlags */
#define DSBCAPS_PRIMARYBUFFER   0x00000001
#define DSBCAPS_CTRLVOLUME      0x00000080
#define DSBCAPS_CTRLPAN         0x00000040
#define DSBCAPS_CTRLFREQUENCY   0x00000020
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBCAPS_LOCSOFTWARE     0x00000008

/* DSBPLAY flags */
#define DSBPLAY_LOOPING         0x00000001

/* DSBLOCK flags */
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER    0x00000002

/* DSBSTATUS flags */
#define DSBSTATUS_PLAYING       0x00000001
#define DSBSTATUS_BUFFERLOST    0x00000002
#define DSBSTATUS_LOOPING       0x00000004

/* WAVE format tags */
#define WAVE_FORMAT_PCM         0x0001

/* HDA output rate */
#define HDA_RATE                48000

/* ── WAVEFORMATEX (packed, matches Windows layout) ────────── */

typedef struct __attribute__((packed)) {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
} WAVEFORMATEX;

/* ── DSBUFFERDESC (32-bit layout, matches PE32 caller) ────── */

typedef struct __attribute__((packed)) {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwBufferBytes;
    uint32_t dwReserved;
    uint32_t lpwfxFormat;       /* 32-bit pointer to WAVEFORMATEX */
} DSBUFFERDESC32;

/* ── Sound buffer state ───────────────────────────────────── */

#define MAX_SOUND_BUFFERS   8
#define MIX_BUF_SAMPLES     (HDA_RATE / 10)  /* 100ms worth of 48kHz stereo */

typedef struct {
    /* Circular audio data */
    uint8_t *data;
    uint32_t data_size;         /* buffer size in bytes */

    /* Cursors */
    uint32_t play_cursor;       /* read position (bytes) */
    uint32_t write_cursor;      /* write position (bytes) */

    /* Format */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;

    /* State */
    int      playing;
    int      looping;
    int      is_primary;
    int      in_use;

    /* Volume/pan (hundredths of dB) */
    int32_t  volume;            /* -10000 to 0 */
    int32_t  pan;               /* -10000 to +10000 */
} DSBuffer;

static DSBuffer g_buffers[MAX_SOUND_BUFFERS];

/* Mixed output buffer for HDA (48kHz stereo int16_t) */
static int16_t g_mix_buf[MIX_BUF_SAMPLES * 2];  /* *2 for stereo */

/* ── IDirectSoundBuffer COM vtable ────────────────────────── */

/*
 * IDirectSoundBuffer vtable layout (DirectSound 7):
 *  0: QueryInterface
 *  1: AddRef
 *  2: Release
 *  3: GetCaps
 *  4: GetCurrentPosition
 *  5: GetFormat
 *  6: GetVolume
 *  7: GetPan
 *  8: GetFrequency
 *  9: GetStatus
 * 10: Initialize
 * 11: Lock
 * 12: Play
 * 13: SetCurrentPosition
 * 14: SetFormat
 * 15: SetVolume
 * 16: SetPan
 * 17: SetFrequency
 * 18: Stop
 * 19: Unlock
 * 20: Restore
 */

typedef struct IDirectSoundBufferVtbl IDirectSoundBufferVtbl;
typedef struct IDirectSoundBufferObj  IDirectSoundBufferObj;

struct IDirectSoundBufferObj {
    IDirectSoundBufferVtbl *lpVtbl;
    int buf_index;
};

/* Forward declarations */
static HRESULT WINAPI dsb_QueryInterface(PVOID self, LPCGUID iid, PVOID *ppv);
static ULONG   WINAPI dsb_AddRef(PVOID self);
static ULONG   WINAPI dsb_Release(PVOID self);
static HRESULT WINAPI dsb_GetCaps(PVOID self, PVOID pCaps);
static HRESULT WINAPI dsb_GetCurrentPosition(PVOID self, DWORD *pdwPlay, DWORD *pdwWrite);
static HRESULT WINAPI dsb_GetFormat(PVOID self, PVOID lpwfx, DWORD dwSize, DWORD *lpdwWritten);
static HRESULT WINAPI dsb_GetVolume(PVOID self, LONG *plVolume);
static HRESULT WINAPI dsb_GetPan(PVOID self, LONG *plPan);
static HRESULT WINAPI dsb_GetFrequency(PVOID self, DWORD *pdwFreq);
static HRESULT WINAPI dsb_GetStatus(PVOID self, DWORD *pdwStatus);
static HRESULT WINAPI dsb_Initialize(PVOID self, PVOID pDS, PVOID pDesc);
static HRESULT WINAPI dsb_Lock(PVOID self, DWORD dwOfs, DWORD dwBytes,
                                PVOID *ppv1, DWORD *pdwLen1,
                                PVOID *ppv2, DWORD *pdwLen2, DWORD dwFlags);
static HRESULT WINAPI dsb_Play(PVOID self, DWORD dwRes1, DWORD dwRes2, DWORD dwFlags);
static HRESULT WINAPI dsb_SetCurrentPosition(PVOID self, DWORD dwNewPos);
static HRESULT WINAPI dsb_SetFormat(PVOID self, PVOID pcfxFormat);
static HRESULT WINAPI dsb_SetVolume(PVOID self, LONG lVolume);
static HRESULT WINAPI dsb_SetPan(PVOID self, LONG lPan);
static HRESULT WINAPI dsb_SetFrequency(PVOID self, DWORD dwFreq);
static HRESULT WINAPI dsb_Stop(PVOID self);
static HRESULT WINAPI dsb_Unlock(PVOID self, PVOID pv1, DWORD dwLen1,
                                  PVOID pv2, DWORD dwLen2);
static HRESULT WINAPI dsb_Restore(PVOID self);

struct IDirectSoundBufferVtbl {
    HRESULT (WINAPI *QueryInterface)(PVOID, LPCGUID, PVOID *);
    ULONG   (WINAPI *AddRef)(PVOID);
    ULONG   (WINAPI *Release)(PVOID);
    HRESULT (WINAPI *GetCaps)(PVOID, PVOID);
    HRESULT (WINAPI *GetCurrentPosition)(PVOID, DWORD *, DWORD *);
    HRESULT (WINAPI *GetFormat)(PVOID, PVOID, DWORD, DWORD *);
    HRESULT (WINAPI *GetVolume)(PVOID, LONG *);
    HRESULT (WINAPI *GetPan)(PVOID, LONG *);
    HRESULT (WINAPI *GetFrequency)(PVOID, DWORD *);
    HRESULT (WINAPI *GetStatus)(PVOID, DWORD *);
    HRESULT (WINAPI *Initialize)(PVOID, PVOID, PVOID);
    HRESULT (WINAPI *Lock)(PVOID, DWORD, DWORD, PVOID *, DWORD *,
                           PVOID *, DWORD *, DWORD);
    HRESULT (WINAPI *Play)(PVOID, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *SetCurrentPosition)(PVOID, DWORD);
    HRESULT (WINAPI *SetFormat)(PVOID, PVOID);
    HRESULT (WINAPI *SetVolume)(PVOID, LONG);
    HRESULT (WINAPI *SetPan)(PVOID, LONG);
    HRESULT (WINAPI *SetFrequency)(PVOID, DWORD);
    HRESULT (WINAPI *Stop)(PVOID);
    HRESULT (WINAPI *Unlock)(PVOID, PVOID, DWORD, PVOID, DWORD);
    HRESULT (WINAPI *Restore)(PVOID);
};

static IDirectSoundBufferVtbl g_dsb_vtbl = {
    dsb_QueryInterface,
    dsb_AddRef,
    dsb_Release,
    dsb_GetCaps,
    dsb_GetCurrentPosition,
    dsb_GetFormat,
    dsb_GetVolume,
    dsb_GetPan,
    dsb_GetFrequency,
    dsb_GetStatus,
    dsb_Initialize,
    dsb_Lock,
    dsb_Play,
    dsb_SetCurrentPosition,
    dsb_SetFormat,
    dsb_SetVolume,
    dsb_SetPan,
    dsb_SetFrequency,
    dsb_Stop,
    dsb_Unlock,
    dsb_Restore,
};

static IDirectSoundBufferObj g_dsb_objs[MAX_SOUND_BUFFERS];

/* Audio data backing store — allocated per-buffer on CreateSoundBuffer.
 * Was static 512KB BSS; now lazy-alloc to avoid boot-time memory pressure. */
#define DSB_MAX_DATA_SIZE  65536
static uint8_t *g_dsb_data_ptrs[MAX_SOUND_BUFFERS];  /* Per-buffer pointers */
#define g_dsb_data_get(i) (g_dsb_data_ptrs[i] ? g_dsb_data_ptrs[i] : dsb_alloc_data(i))
static uint8_t *dsb_alloc_data(int idx) {
    extern void *kmalloc(uint64_t);
    g_dsb_data_ptrs[idx] = (uint8_t *)kmalloc(DSB_MAX_DATA_SIZE);
    if (g_dsb_data_ptrs[idx]) memset(g_dsb_data_ptrs[idx], 0, DSB_MAX_DATA_SIZE);
    return g_dsb_data_ptrs[idx];
}

/* ── Helper: get DSBuffer from COM self pointer ───────────── */

static DSBuffer *dsb_from_self(PVOID self)
{
    IDirectSoundBufferObj *obj = (IDirectSoundBufferObj *)self;
    if (!obj || obj->buf_index < 0 || obj->buf_index >= MAX_SOUND_BUFFERS)
        return NULL;
    return &g_buffers[obj->buf_index];
}

/* ── Volume conversion ────────────────────────────────────── */

/*
 * DirectSound volume is in hundredths of dB: 0 = full, -10000 = silence.
 * Convert to a linear 0-256 scale for simple integer multiply.
 * Approximation: every -1000 hundredths-dB halves the volume.
 */
static uint32_t ds_vol_to_linear(int32_t vol_hdb)
{
    if (vol_hdb >= 0) return 256;
    if (vol_hdb <= -10000) return 0;

    /* Piecewise linear approximation: 10 steps of -1000 each */
    uint32_t scale = 256;
    int32_t v = vol_hdb;
    while (v <= -1000) {
        scale >>= 1;  /* halve */
        v += 1000;
    }
    /* Interpolate remaining fraction: linear within the last 1000 range */
    if (v < 0) {
        /* v is in range (-999, 0). scale * (1000+v)/1000 */
        scale = scale * (uint32_t)(1000 + v) / 1000;
    }
    return scale;
}

/* ── Audio mixing and resampling ──────────────────────────── */

/*
 * Mix all active buffers into g_mix_buf (48kHz stereo int16_t).
 * Handles resampling from source rate to 48kHz.
 * Returns number of stereo samples written.
 */
static uint32_t ds_mix_active_buffers(void)
{
    uint32_t out_samples = MIX_BUF_SAMPLES;

    /* Clear mix buffer (int32_t accumulator stage uses g_mix_buf directly,
     * so we zero it first and accumulate in-place with clamping) */
    memset(g_mix_buf, 0, out_samples * 2 * sizeof(int16_t));

    /* We need a 32-bit accumulator to avoid clipping during mixing */
    static int32_t accum[MIX_BUF_SAMPLES * 2];
    memset(accum, 0, out_samples * 2 * sizeof(int32_t));

    int any_active = 0;

    for (int b = 0; b < MAX_SOUND_BUFFERS; b++) {
        DSBuffer *buf = &g_buffers[b];
        if (!buf->in_use || !buf->playing || buf->is_primary || !buf->data)
            continue;
        if (buf->data_size == 0 || buf->block_align == 0)
            continue;

        any_active = 1;
        uint32_t vol_scale = ds_vol_to_linear(buf->volume);

        /*
         * Resample: step through source buffer at (src_rate / 48000) ratio.
         * Use 16.16 fixed-point for the step size.
         */
        uint32_t src_rate = buf->sample_rate;
        if (src_rate == 0) src_rate = 22050;

        /* Fixed-point step: how many source frames per output frame */
        uint32_t step_fp = (src_rate << 16) / HDA_RATE;
        uint32_t src_pos_fp = (buf->play_cursor / buf->block_align) << 16;
        uint32_t src_total_frames = buf->data_size / buf->block_align;

        for (uint32_t i = 0; i < out_samples; i++) {
            uint32_t src_frame = (src_pos_fp >> 16) % src_total_frames;
            uint32_t byte_off = src_frame * buf->block_align;

            int32_t left = 0, right = 0;

            if (buf->bits_per_sample == 16) {
                int16_t *samp = (int16_t *)(buf->data + byte_off);
                left = samp[0];
                right = (buf->channels >= 2) ? samp[1] : left;
            } else if (buf->bits_per_sample == 8) {
                /* 8-bit PCM is unsigned, center at 128 */
                left = ((int32_t)buf->data[byte_off] - 128) << 8;
                right = (buf->channels >= 2) ?
                    ((int32_t)buf->data[byte_off + 1] - 128) << 8 : left;
            }

            /* Apply volume */
            left = (left * (int32_t)vol_scale) >> 8;
            right = (right * (int32_t)vol_scale) >> 8;

            accum[i * 2]     += left;
            accum[i * 2 + 1] += right;

            src_pos_fp += step_fp;

            /* Handle wraparound */
            if ((src_pos_fp >> 16) >= src_total_frames) {
                if (buf->looping) {
                    src_pos_fp -= (src_total_frames << 16);
                } else {
                    buf->playing = 0;
                    break;
                }
            }
        }

        /* Update play cursor */
        buf->play_cursor = ((src_pos_fp >> 16) % src_total_frames) * buf->block_align;
    }

    if (!any_active) return 0;

    /* Clamp accumulated values to int16_t range */
    for (uint32_t i = 0; i < out_samples * 2; i++) {
        int32_t v = accum[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        g_mix_buf[i] = (int16_t)v;
    }

    return out_samples;
}

/*
 * Submit mixed audio to HDA.
 * Called from Play() and could be called periodically for streaming.
 */
static void ds_submit_to_hda(void)
{
    uint32_t samples = ds_mix_active_buffers();
    if (samples > 0) {
        hda_play_buffer(g_mix_buf, samples);
    }
}

/* ── Audio mixer tick (call periodically for streaming playback) ── */

void dsound_mixer_tick(void)
{
    int any_playing = 0;
    for (int i = 0; i < MAX_SOUND_BUFFERS; i++) {
        if (g_buffers[i].in_use && g_buffers[i].playing) {
            any_playing = 1;
            if (g_buffers[i].looping) {
                uint32_t advance = g_buffers[i].sample_rate / 10;
                uint32_t bps = (g_buffers[i].bits_per_sample / 8) * g_buffers[i].channels;
                g_buffers[i].play_cursor += advance * bps;
                if (g_buffers[i].play_cursor >= g_buffers[i].data_size)
                    g_buffers[i].play_cursor %= g_buffers[i].data_size;
            }
        }
    }
    if (any_playing)
        ds_submit_to_hda();
}

/* ── IDirectSoundBuffer method implementations ────────────── */

static HRESULT WINAPI dsb_QueryInterface(PVOID self, LPCGUID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_NOINTERFACE;
    *ppv = self;
    return DS_OK;
}

static ULONG WINAPI dsb_AddRef(PVOID self)  { (void)self; return 2; }
static ULONG WINAPI dsb_Release(PVOID self) { (void)self; return 1; }

static HRESULT WINAPI dsb_GetCaps(PVOID self, PVOID pCaps)
{
    (void)self; (void)pCaps;
    /* UT99 checks this but doesn't require detailed caps */
    return DS_OK;
}

static HRESULT WINAPI dsb_GetCurrentPosition(PVOID self, DWORD *pdwPlay, DWORD *pdwWrite)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    if (pdwPlay)  *pdwPlay  = buf->play_cursor;
    if (pdwWrite) *pdwWrite = buf->write_cursor;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFormat(PVOID self, PVOID lpwfx, DWORD dwSize, DWORD *lpdwWritten)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    uint32_t copy_size = sizeof(WAVEFORMATEX);
    if (dwSize < copy_size) copy_size = dwSize;

    if (lpwfx) {
        WAVEFORMATEX *wfx = (WAVEFORMATEX *)lpwfx;
        memset(wfx, 0, copy_size);
        wfx->wFormatTag      = WAVE_FORMAT_PCM;
        wfx->nChannels       = buf->channels;
        wfx->nSamplesPerSec  = buf->sample_rate;
        wfx->wBitsPerSample  = buf->bits_per_sample;
        wfx->nBlockAlign     = buf->block_align;
        wfx->nAvgBytesPerSec = buf->sample_rate * buf->block_align;
        wfx->cbSize          = 0;
    }
    if (lpdwWritten) *lpdwWritten = copy_size;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetVolume(PVOID self, LONG *plVolume)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf || !plVolume) return DSERR_INVALIDPARAM;
    *plVolume = buf->volume;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetPan(PVOID self, LONG *plPan)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf || !plPan) return DSERR_INVALIDPARAM;
    *plPan = buf->pan;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFrequency(PVOID self, DWORD *pdwFreq)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf || !pdwFreq) return DSERR_INVALIDPARAM;
    *pdwFreq = buf->sample_rate;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetStatus(PVOID self, DWORD *pdwStatus)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf || !pdwStatus) return DSERR_INVALIDPARAM;

    *pdwStatus = 0;
    if (buf->playing) {
        *pdwStatus |= DSBSTATUS_PLAYING;
        if (buf->looping)
            *pdwStatus |= DSBSTATUS_LOOPING;
    }
    return DS_OK;
}

static HRESULT WINAPI dsb_Initialize(PVOID self, PVOID pDS, PVOID pDesc)
{
    (void)self; (void)pDS; (void)pDesc;
    return DS_OK;
}

static HRESULT WINAPI dsb_Lock(PVOID self, DWORD dwOfs, DWORD dwBytes,
                                PVOID *ppv1, DWORD *pdwLen1,
                                PVOID *ppv2, DWORD *pdwLen2, DWORD dwFlags)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf || !buf->data) return DSERR_INVALIDPARAM;
    if (!ppv1 || !pdwLen1) return DSERR_INVALIDPARAM;

    uint32_t offset = dwOfs;
    uint32_t bytes  = dwBytes;

    if (dwFlags & DSBLOCK_FROMWRITECURSOR) {
        offset = buf->write_cursor;
    }
    if (dwFlags & DSBLOCK_ENTIREBUFFER) {
        offset = 0;
        bytes  = buf->data_size;
    }

    /* Clamp offset */
    if (offset >= buf->data_size)
        offset = offset % buf->data_size;

    /* Region 1: from offset to end (or offset+bytes if no wrap) */
    uint32_t avail1 = buf->data_size - offset;
    if (bytes <= avail1) {
        /* No wraparound */
        *ppv1    = buf->data + offset;
        *pdwLen1 = bytes;
        if (ppv2)    *ppv2    = NULL;
        if (pdwLen2) *pdwLen2 = 0;
    } else {
        /* Wraparound: region 1 = offset..end, region 2 = 0..remainder */
        *ppv1    = buf->data + offset;
        *pdwLen1 = avail1;
        if (ppv2 && pdwLen2) {
            uint32_t remainder = bytes - avail1;
            if (remainder > offset) remainder = offset;  /* can't overlap */
            if (remainder > buf->data_size) remainder = buf->data_size;
            *ppv2    = buf->data;
            *pdwLen2 = remainder;
        }
    }

    return DS_OK;
}

static HRESULT WINAPI dsb_Play(PVOID self, DWORD dwRes1, DWORD dwRes2, DWORD dwFlags)
{
    (void)dwRes1; (void)dwRes2;
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    buf->playing = 1;
    buf->looping = (dwFlags & DSBPLAY_LOOPING) ? 1 : 0;

    serial_puts("[DSOUND] Play (");
    serial_putdec(buf->sample_rate);
    serial_puts("Hz, ");
    serial_putdec(buf->channels);
    serial_puts("ch, ");
    serial_putdec(buf->bits_per_sample);
    serial_puts("bit, ");
    serial_putdec(buf->data_size);
    serial_puts(" bytes");
    if (buf->looping) serial_puts(", LOOP");
    serial_puts(")\n");

    /* Mix all active buffers and submit to HDA */
    ds_submit_to_hda();

    return DS_OK;
}

static HRESULT WINAPI dsb_SetCurrentPosition(PVOID self, DWORD dwNewPos)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;
    buf->play_cursor = dwNewPos % buf->data_size;
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFormat(PVOID self, PVOID pcfxFormat)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    if (pcfxFormat) {
        WAVEFORMATEX *wfx = (WAVEFORMATEX *)pcfxFormat;
        buf->sample_rate     = wfx->nSamplesPerSec;
        buf->channels        = wfx->nChannels;
        buf->bits_per_sample = wfx->wBitsPerSample;
        buf->block_align     = wfx->nBlockAlign;
        if (buf->block_align == 0)
            buf->block_align = (buf->channels * buf->bits_per_sample) / 8;

        serial_puts("[DSOUND] SetFormat: ");
        serial_putdec(buf->sample_rate);
        serial_puts("Hz, ");
        serial_putdec(buf->channels);
        serial_puts("ch, ");
        serial_putdec(buf->bits_per_sample);
        serial_puts("bit\n");
    }

    /* Accept but don't change HDA hardware format — we resample */
    return DS_OK;
}

static HRESULT WINAPI dsb_SetVolume(PVOID self, LONG lVolume)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;
    buf->volume = lVolume;
    return DS_OK;
}

static HRESULT WINAPI dsb_SetPan(PVOID self, LONG lPan)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;
    buf->pan = lPan;
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFrequency(PVOID self, DWORD dwFreq)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;
    if (dwFreq != 0)
        buf->sample_rate = dwFreq;
    return DS_OK;
}

static HRESULT WINAPI dsb_Stop(PVOID self)
{
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    buf->playing = 0;
    buf->looping = 0;

    /* Check if any buffers are still playing */
    int any_playing = 0;
    for (int i = 0; i < MAX_SOUND_BUFFERS; i++) {
        if (g_buffers[i].in_use && g_buffers[i].playing)
            any_playing = 1;
    }
    if (!any_playing)
        hda_stop();

    serial_puts("[DSOUND] Stop\n");
    return DS_OK;
}

static HRESULT WINAPI dsb_Unlock(PVOID self, PVOID pv1, DWORD dwLen1,
                                  PVOID pv2, DWORD dwLen2)
{
    (void)pv1; (void)pv2;
    DSBuffer *buf = dsb_from_self(self);
    if (!buf) return DSERR_INVALIDPARAM;

    /* Advance write cursor past the unlocked region */
    uint32_t total_written = dwLen1 + dwLen2;
    buf->write_cursor = (buf->write_cursor + total_written) % buf->data_size;

    /* If this buffer is actively playing, re-mix and submit */
    if (buf->playing)
        ds_submit_to_hda();

    return DS_OK;
}

static HRESULT WINAPI dsb_Restore(PVOID self)
{
    (void)self;
    return DS_OK;
}

/* ── IDirectSound COM interface ───────────────────────────── */

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
    (void)self; (void)pUnk;

    if (!ppDSB) return DSERR_INVALIDPARAM;
    *ppDSB = NULL;

    if (!desc) return DSERR_INVALIDPARAM;

    /* Check HDA readiness */
    if (!hda_is_ready()) {
        serial_puts("[DSOUND] CreateSoundBuffer: HDA not ready, failing\n");
        return DSERR_GENERIC;
    }

    DSBUFFERDESC32 *dsd = (DSBUFFERDESC32 *)desc;
    uint32_t flags = dsd->dwFlags;
    uint32_t buf_bytes = dsd->dwBufferBytes;
    WAVEFORMATEX *wfx = NULL;

    if (dsd->lpwfxFormat)
        wfx = (WAVEFORMATEX *)(uint64_t)dsd->lpwfxFormat;

    int is_primary = (flags & DSBCAPS_PRIMARYBUFFER) ? 1 : 0;

    serial_puts("[DSOUND] CreateSoundBuffer: ");
    if (is_primary) {
        serial_puts("PRIMARY\n");
    } else {
        serial_putdec(buf_bytes);
        serial_puts(" bytes");
        if (wfx) {
            serial_puts(", ");
            serial_putdec(wfx->nSamplesPerSec);
            serial_puts("Hz/");
            serial_putdec(wfx->nChannels);
            serial_puts("ch/");
            serial_putdec(wfx->wBitsPerSample);
            serial_puts("bit");
        }
        serial_puts("\n");
    }

    /* Find a free buffer slot */
    int slot = -1;
    for (int i = 0; i < MAX_SOUND_BUFFERS; i++) {
        if (!g_buffers[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        serial_puts("[DSOUND] CreateSoundBuffer: no free slots\n");
        return DSERR_GENERIC;
    }

    /* Initialize buffer state */
    DSBuffer *buf = &g_buffers[slot];
    memset(buf, 0, sizeof(DSBuffer));
    buf->in_use     = 1;
    buf->is_primary = is_primary;
    buf->volume     = 0;   /* full volume */
    buf->pan        = 0;   /* center */

    if (is_primary) {
        /* Primary buffer: format-only, no audio data.
         * Default to HDA native format. */
        buf->sample_rate     = HDA_RATE;
        buf->channels        = 2;
        buf->bits_per_sample = 16;
        buf->block_align     = 4;
        buf->data            = NULL;
        buf->data_size       = 0;
    } else {
        /* Secondary buffer: allocate audio data from static pool */
        uint32_t alloc_size = buf_bytes;
        if (alloc_size > DSB_MAX_DATA_SIZE)
            alloc_size = DSB_MAX_DATA_SIZE;
        if (alloc_size == 0)
            alloc_size = DSB_MAX_DATA_SIZE;

        buf->data      = g_dsb_data_get(slot);
        buf->data_size = alloc_size;
        memset(buf->data, 0, alloc_size);

        /* Parse WAVEFORMATEX if provided */
        if (wfx) {
            buf->sample_rate     = wfx->nSamplesPerSec;
            buf->channels        = wfx->nChannels;
            buf->bits_per_sample = wfx->wBitsPerSample;
            buf->block_align     = wfx->nBlockAlign;
            if (buf->block_align == 0)
                buf->block_align = (buf->channels * buf->bits_per_sample) / 8;
        } else {
            /* Default format */
            buf->sample_rate     = 22050;
            buf->channels        = 2;
            buf->bits_per_sample = 16;
            buf->block_align     = 4;
        }

        if (buf->sample_rate == 0) buf->sample_rate = 22050;
        if (buf->channels == 0)    buf->channels = 1;
        if (buf->bits_per_sample == 0) buf->bits_per_sample = 16;
        if (buf->block_align == 0) buf->block_align = (buf->channels * buf->bits_per_sample) / 8;
    }

    /* Set up COM object */
    g_dsb_objs[slot].lpVtbl    = &g_dsb_vtbl;
    g_dsb_objs[slot].buf_index = slot;

    *ppDSB = &g_dsb_objs[slot];
    return DS_OK;
}

static HRESULT WINAPI ds_GetCaps(PVOID self, PVOID pDSCaps)
{
    (void)self;
    /* Fill minimal DSCAPS: report we support 16-bit stereo */
    if (pDSCaps) {
        uint32_t *caps = (uint32_t *)pDSCaps;
        /* dwSize at offset 0 — caller usually pre-fills this */
        /* dwFlags at offset 4 */
        caps[1] = 0x00000060;  /* DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY16BIT */
    }
    return DS_OK;
}

static HRESULT WINAPI ds_SetCooperativeLevel(PVOID self, PVOID hwnd, DWORD dwLevel)
{
    (void)self; (void)hwnd; (void)dwLevel;
    serial_puts("[DSOUND] SetCooperativeLevel\n");
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
    serial_puts("[DSOUND] DirectSoundCreate");
    if (hda_is_ready())
        serial_puts(" (HDA ready)\n");
    else
        serial_puts(" (HDA not ready — audio disabled)\n");
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

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT dsound_exports[] = {
    { "DirectSoundCreate",     (PVOID)DirectSoundCreate,     3, CC_STDCALL },
    { "DirectSoundEnumerateA", (PVOID)DirectSoundEnumerateA, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *dsound_abi_table(int *count) {
    *count = (int)(sizeof(dsound_exports)/sizeof(dsound_exports[0]));
    return (const WIN32_EXPORT *)dsound_exports;
}

static int ds_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID dsound_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; dsound_exports[i].name; i++) {
        if (ds_strcmp(func_name, dsound_exports[i].name) == 0)
            return dsound_exports[i].func;
    }
    return NULL;
}

PVOID dsound_shim_init(void) { return (PVOID)dsound_exports; }
