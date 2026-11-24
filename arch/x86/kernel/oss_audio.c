/*
 * OSS /dev/dsp and /dev/audio playback frontend.
 *
 * The device keeps legacy-format bytes in a bounded queue. The shared audio
 * scheduler pulls and converts those bytes to canonical PCM16 stereo; HDA is
 * owned exclusively by the scheduler. Capture is not advertised and fails
 * with ENODEV instead of silently returning fabricated samples.
 */

#include "../include/oss_audio.h"
#include "../include/audio_sched.h"
#include "../include/pcm.h"
#include "smp.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

#define OSS_ENODEV 19
#define OSS_EBUSY 16
#define OSS_EIO 5
#define OSS_EFAULT 14
#define OSS_EINVAL 22
#define OSS_ENOTTY 25
#define OSS_EAGAIN 11

#define OSS_O_RDONLY 0U
#define OSS_O_WRONLY 1U
#define OSS_O_RDWR   2U
#define OSS_O_ACCMODE 3U
#define OSS_O_NONBLOCK 04000U

#define OSS_BUFFER_BYTES (64U * 1024U)
#define OSS_DEFAULT_FRAGMENT_BYTES 4096U
#define OSS_PENDING_COMPLETIONS 8U

#define OSS_AFMT_QUERY   0x00000000U
#define OSS_AFMT_MU_LAW  0x00000001U
#define OSS_AFMT_A_LAW   0x00000002U
#define OSS_AFMT_U8      0x00000008U
#define OSS_AFMT_S16_LE  0x00000010U
#define OSS_AFMT_S8      0x00000040U
#define OSS_AFMT_U16_LE  0x00000080U
#define OSS_AFMT_MASK (OSS_AFMT_MU_LAW | OSS_AFMT_A_LAW | OSS_AFMT_U8 | \
                       OSS_AFMT_S16_LE | OSS_AFMT_S8 | OSS_AFMT_U16_LE)

#define OSS_DSP_RESET       0x00005000U
#define OSS_DSP_SYNC        0x00005001U
#define OSS_DSP_SPEED       0xC0045002U
#define OSS_DSP_READ_RATE   0x80045002U
#define OSS_DSP_STEREO      0xC0045003U
#define OSS_DSP_GETBLKSIZE  0xC0045004U
#define OSS_DSP_SETFMT      0xC0045005U
#define OSS_DSP_READ_BITS   0x80045005U
#define OSS_DSP_CHANNELS    0xC0045006U
#define OSS_DSP_READ_CHANNELS 0x80045006U
#define OSS_DSP_POST        0x00005008U
#define OSS_DSP_SUBDIVIDE   0xC0045009U
#define OSS_DSP_SETFRAGMENT 0xC004500AU
#define OSS_DSP_GETFMTS     0x8004500BU
#define OSS_DSP_GETOSPACE   0x8010500CU
#define OSS_DSP_GETISPACE   0x8010500DU
#define OSS_DSP_NONBLOCK    0x0000500EU
#define OSS_DSP_GETCAPS     0x8004500FU
#define OSS_DSP_GETTRIGGER  0x80045010U
#define OSS_DSP_SETTRIGGER  0x40045010U
#define OSS_DSP_GETIPTR     0x800C5011U
#define OSS_DSP_GETOPTR     0x800C5012U
#define OSS_DSP_SETSYNCRO   0x00005015U
#define OSS_DSP_SETDUPLEX   0x00005016U
#define OSS_DSP_GETODELAY   0x80045017U

#define OSS_DSP_CAP_REALTIME 0x00000200U
#define OSS_DSP_CAP_TRIGGER  0x00001000U
#define OSS_DSP_CAP_MULTI    0x00004000U
#define OSS_PCM_ENABLE_INPUT  0x00000001U
#define OSS_PCM_ENABLE_OUTPUT 0x00000002U

typedef struct {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
} oss_audio_buf_info_t;

typedef struct {
    int bytes;
    int blocks;
    int pointer;
} oss_count_info_t;

typedef struct {
    uint64_t epoch;
    uint32_t bytes;
    uint32_t blocks;
} oss_completion_t;

typedef struct {
    volatile uint32_t references;
    spinlock_t lock;
    audio_source_id_t source;
    pcm_format_t format;
    pcm_cursor_t cursor;
    bool output_triggered;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint32_t buffer_limit;
    uint32_t fragment_bytes;
    uint64_t bytes_played;
    uint64_t blocks_played;
    uint32_t completion_head;
    uint32_t completion_count;
    oss_completion_t completions[OSS_PENDING_COMPLETIONS];
    uint8_t data[OSS_BUFFER_BYTES];
    int32_t accumulator[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
    int16_t output[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
} oss_audio_state_t;

static uint64_t oss_lock_irqsave(oss_audio_state_t *state)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&state->lock);
    return flags;
}

static void oss_unlock_irqrestore(oss_audio_state_t *state, uint64_t flags)
{
    spin_unlock(&state->lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void oss_wait_for_interrupt(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; sti; hlt" : "=r"(flags) :: "memory");
    if (!(flags & (1ULL << 9)))
        __asm__ volatile ("cli" ::: "memory");
}

static uint32_t oss_format_id(const pcm_format_t *format)
{
    switch (format->sample_format) {
    case PCM_SAMPLE_MU_LAW: return OSS_AFMT_MU_LAW;
    case PCM_SAMPLE_A_LAW: return OSS_AFMT_A_LAW;
    case PCM_SAMPLE_U8: return OSS_AFMT_U8;
    case PCM_SAMPLE_S16_LE: return OSS_AFMT_S16_LE;
    case PCM_SAMPLE_S8: return OSS_AFMT_S8;
    case PCM_SAMPLE_U16_LE: return OSS_AFMT_U16_LE;
    default: return OSS_AFMT_QUERY;
    }
}

static bool oss_set_format_id(pcm_format_t *format, uint32_t id)
{
    switch (id) {
    case OSS_AFMT_MU_LAW: format->sample_format = PCM_SAMPLE_MU_LAW; break;
    case OSS_AFMT_A_LAW: format->sample_format = PCM_SAMPLE_A_LAW; break;
    case OSS_AFMT_U8: format->sample_format = PCM_SAMPLE_U8; break;
    case OSS_AFMT_S16_LE: format->sample_format = PCM_SAMPLE_S16_LE; break;
    case OSS_AFMT_S8: format->sample_format = PCM_SAMPLE_S8; break;
    case OSS_AFMT_U16_LE: format->sample_format = PCM_SAMPLE_U16_LE; break;
    default: return false;
    }
    return true;
}

static void oss_reset_queue_locked(oss_audio_state_t *state)
{
    state->cursor.frame_q32 = 0;
    state->data_offset = 0;
    state->data_bytes = 0;
    state->bytes_played = 0;
    state->blocks_played = 0;
    state->completion_head = 0;
    state->completion_count = 0;
}

static void oss_compact_locked(oss_audio_state_t *state)
{
    if (!state->data_offset)
        return;
    for (uint32_t i = 0; i < state->data_bytes; i++)
        state->data[i] = state->data[state->data_offset + i];
    state->data_offset = 0;
}

static void oss_retire_completions_locked(oss_audio_state_t *state,
                                          uint64_t epoch)
{
    while (state->completion_count) {
        oss_completion_t *completion =
            &state->completions[state->completion_head];
        if (completion->epoch > epoch)
            break;
        state->bytes_played += completion->bytes;
        state->blocks_played += completion->blocks;
        state->completion_head = (state->completion_head + 1U) %
                                 OSS_PENDING_COMPLETIONS;
        state->completion_count--;
    }
}

static void oss_submit_completion_locked(oss_audio_state_t *state,
                                         uint32_t bytes)
{
    uint64_t epoch = audio_get_fills() + 1U +
                     audio_get_output_latency_blocks();
    if (state->completion_count == OSS_PENDING_COMPLETIONS) {
        uint32_t last = (state->completion_head + state->completion_count - 1U) %
                        OSS_PENDING_COMPLETIONS;
        state->completions[last].bytes += bytes;
        state->completions[last].blocks++;
        state->completions[last].epoch = epoch;
        return;
    }
    uint32_t tail = (state->completion_head + state->completion_count) %
                    OSS_PENDING_COMPLETIONS;
    state->completions[tail].epoch = epoch;
    state->completions[tail].bytes = bytes;
    state->completions[tail].blocks = 1;
    state->completion_count++;
}

static uint32_t oss_delay_bytes_locked(const oss_audio_state_t *state)
{
    uint64_t bytes = state->data_bytes;
    for (uint32_t i = 0; i < state->completion_count; i++) {
        uint32_t slot = (state->completion_head + i) %
                        OSS_PENDING_COMPLETIONS;
        bytes += state->completions[slot].bytes;
    }
    return bytes > 0x7FFFFFFFULL ? 0x7FFFFFFFU : (uint32_t)bytes;
}

static bool oss_fill(void *context, int16_t *output, uint32_t frames)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    uint64_t flags = oss_lock_irqsave(state);
    oss_retire_completions_locked(state, audio_get_fills());
    memset(state->accumulator, 0,
           (size_t)frames * AUDIO_OUTPUT_CHANNELS * sizeof(int32_t));

    uint32_t mixed = 0;
    uint32_t frame_bytes = pcm_frame_bytes(&state->format);
    uint32_t source_frames = frame_bytes ? state->data_bytes / frame_bytes : 0;
    if (state->output_triggered && source_frames) {
        bool ended = false;
        mixed = pcm_mix_circular_s16_stereo(
            state->accumulator, frames, state->data + state->data_offset,
            source_frames * frame_bytes, &state->format,
            AUDIO_OUTPUT_RATE_HZ, PCM_GAIN_UNITY, PCM_GAIN_UNITY, false,
            &state->cursor, &ended);

        uint32_t consumed_frames = (uint32_t)(state->cursor.frame_q32 >> 32);
        if (consumed_frames > source_frames)
            consumed_frames = source_frames;
        uint32_t consumed_bytes = consumed_frames * frame_bytes;
        state->data_offset += consumed_bytes;
        state->data_bytes -= consumed_bytes;
        state->cursor.frame_q32 -= (uint64_t)consumed_frames << 32;
        if (mixed && consumed_bytes)
            oss_submit_completion_locked(state, consumed_bytes);
        if (!state->data_bytes) {
            state->data_offset = 0;
            state->cursor.frame_q32 = 0;
        }
    }

    for (uint32_t i = 0; i < frames * AUDIO_OUTPUT_CHANNELS; i++) {
        int32_t sample = state->accumulator[i];
        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        output[i] = (int16_t)sample;
    }
    oss_unlock_irqrestore(state, flags);
    return true;
}

static int oss_audio_registration_errno(audio_register_result_t result)
{
    switch (result) {
    case AUDIO_REGISTER_NO_BACKEND:
        return OSS_ENODEV;
    case AUDIO_REGISTER_NO_SLOTS:
        return OSS_EBUSY;
    case AUDIO_REGISTER_INVALID:
        return OSS_EINVAL;
    case AUDIO_REGISTER_NO_CLOCK:
    case AUDIO_REGISTER_SOURCE_INACTIVE:
    default:
        return OSS_EIO;
    }
}

int oss_audio_create(bool legacy_audio, uint32_t access_mode,
                     void **context_out)
{
    if (!context_out)
        return -OSS_EFAULT;
    *context_out = NULL;
    access_mode &= OSS_O_ACCMODE;
    if (access_mode == OSS_O_RDONLY)
        return -OSS_ENODEV;
    if (access_mode != OSS_O_WRONLY && access_mode != OSS_O_RDWR)
        return -OSS_EINVAL;
    if (!audio_output_is_ready())
        return -OSS_ENODEV;

    oss_audio_state_t *state = (oss_audio_state_t *)kmalloc(sizeof(*state));
    if (!state)
        return -12;
    memset(state, 0, sizeof(*state));
    state->references = 1;
    state->format.sample_rate = 8000;
    state->format.channels = 1;
    state->format.sample_format = legacy_audio
                                ? PCM_SAMPLE_MU_LAW : PCM_SAMPLE_U8;
    state->output_triggered = true;
    state->buffer_limit = OSS_BUFFER_BYTES;
    state->fragment_bytes = OSS_DEFAULT_FRAGMENT_BYTES;

    audio_register_result_t registration = audio_source_register_context_ex(
        oss_fill, state, state->output, AUDIO_OUTPUT_BLOCK_FRAMES,
        AUDIO_OUTPUT_RATE_HZ, &state->source);
    if (registration != AUDIO_REGISTER_OK) {
        kfree(state);
        return -oss_audio_registration_errno(registration);
    }
    *context_out = state;
    return 0;
}

static void oss_retain(void *context)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    if (state)
        __atomic_add_fetch(&state->references, 1U, __ATOMIC_RELAXED);
}

static int oss_sync(oss_audio_state_t *state);

static void oss_release(void *context, uint32_t oflags)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    if (!state || __atomic_sub_fetch(&state->references, 1U,
                                     __ATOMIC_ACQ_REL) != 0)
        return;
    if (!(oflags & OSS_O_NONBLOCK))
        (void)oss_sync(state);
    audio_source_unregister(state->source);
    kfree(state);
}

static ssize_t oss_read(void *context, void *buffer, size_t count,
                        uint32_t oflags)
{
    (void)context;
    (void)buffer;
    (void)count;
    (void)oflags;
    return -OSS_ENODEV;
}

static ssize_t oss_write(void *context, const void *buffer, size_t count,
                         uint32_t oflags)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    const uint8_t *source = (const uint8_t *)buffer;
    size_t written = 0;
    if (!state || (!buffer && count))
        return -OSS_EFAULT;

    while (written < count) {
        uint64_t flags = oss_lock_irqsave(state);
        uint32_t free_bytes = state->data_bytes < state->buffer_limit
                            ? state->buffer_limit - state->data_bytes : 0;
        if (free_bytes) {
            uint32_t end = state->data_offset + state->data_bytes;
            if (end == OSS_BUFFER_BYTES ||
                OSS_BUFFER_BYTES - end < free_bytes) {
                oss_compact_locked(state);
                end = state->data_bytes;
            }
            uint32_t contiguous = OSS_BUFFER_BYTES - end;
            uint64_t remaining = count - written;
            uint32_t chunk = free_bytes < contiguous ? free_bytes : contiguous;
            if ((uint64_t)chunk > remaining)
                chunk = (uint32_t)remaining;
            memcpy(state->data + end, source + written, chunk);
            state->data_bytes += chunk;
            written += chunk;
            oss_unlock_irqrestore(state, flags);
            continue;
        }
        oss_unlock_irqrestore(state, flags);
        if (written)
            return (ssize_t)written;
        if (oflags & OSS_O_NONBLOCK)
            return -OSS_EAGAIN;
        oss_wait_for_interrupt();
    }
    return (ssize_t)written;
}

static bool oss_read_ready(void *context)
{
    (void)context;
    return false;
}

static bool oss_write_ready(void *context)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    if (!state)
        return false;
    uint64_t flags = oss_lock_irqsave(state);
    uint32_t free_bytes = state->data_bytes < state->buffer_limit
                        ? state->buffer_limit - state->data_bytes : 0;
    bool ready = free_bytes >= state->fragment_bytes;
    oss_unlock_irqrestore(state, flags);
    return ready;
}

static int oss_sync(oss_audio_state_t *state)
{
    for (;;) {
        uint64_t flags = oss_lock_irqsave(state);
        state->output_triggered = true;
        oss_retire_completions_locked(state, audio_get_fills());
        uint32_t frame_bytes = pcm_frame_bytes(&state->format);
        uint32_t remainder = frame_bytes ? state->data_bytes % frame_bytes : 0;
        if (remainder) {
            uint32_t padding = frame_bytes - remainder;
            oss_compact_locked(state);
            for (uint32_t i = 0; i < padding; i++) {
                uint32_t position = state->data_bytes + i;
                uint8_t value = pcm_silence_byte(&state->format);
                if (state->format.sample_format == PCM_SAMPLE_U16_LE)
                    value = position & 1U ? 0x80U : 0U;
                state->data[position] = value;
            }
            state->data_bytes += padding;
        }
        bool empty = state->data_bytes == 0 &&
                     state->completion_count == 0;
        oss_unlock_irqrestore(state, flags);
        if (empty)
            return 0;
        if (!audio_output_is_ready())
            return -OSS_ENODEV;
        oss_wait_for_interrupt();
    }
}

static int64_t oss_ioctl(void *context, uint64_t request, uint64_t arg,
                         uint32_t *oflags)
{
    oss_audio_state_t *state = (oss_audio_state_t *)context;
    if (!state)
        return -OSS_ENODEV;

    if (request == OSS_DSP_SYNC)
        return oss_sync(state);
    if (request == OSS_DSP_NONBLOCK) {
        if (oflags)
            *oflags |= OSS_O_NONBLOCK;
        return 0;
    }
    if (request == OSS_DSP_SETDUPLEX || request == OSS_DSP_GETISPACE ||
        request == OSS_DSP_GETIPTR)
        return -OSS_ENODEV;
    if (request == OSS_DSP_SETSYNCRO)
        return -OSS_ENOTTY;

    uint64_t flags = oss_lock_irqsave(state);
    oss_retire_completions_locked(state, audio_get_fills());
    int64_t result = 0;
    switch ((uint32_t)request) {
    case OSS_DSP_RESET:
        oss_reset_queue_locked(state);
        state->output_triggered = true;
        break;
    case OSS_DSP_POST:
        state->output_triggered = true;
        break;
    case OSS_DSP_SPEED:
        if (!arg) { result = -OSS_EFAULT; break; }
        if (*(int *)arg > 0) {
            uint32_t rate = (uint32_t)*(int *)arg;
            if (rate < 4000U) rate = 4000U;
            if (rate > 192000U) rate = 192000U;
            if (rate != state->format.sample_rate)
                oss_reset_queue_locked(state);
            state->format.sample_rate = rate;
        }
        *(int *)arg = (int)state->format.sample_rate;
        break;
    case OSS_DSP_READ_RATE:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (int)state->format.sample_rate;
        break;
    case OSS_DSP_STEREO:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            uint16_t channels = *(int *)arg ? 2U : 1U;
            if (channels != state->format.channels)
                oss_reset_queue_locked(state);
            state->format.channels = channels;
            *(int *)arg = channels == 2U;
        }
        break;
    case OSS_DSP_CHANNELS:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            uint16_t channels = *(int *)arg >= 2 ? 2U : 1U;
            if (channels != state->format.channels)
                oss_reset_queue_locked(state);
            state->format.channels = channels;
            *(int *)arg = (int)channels;
        }
        break;
    case OSS_DSP_READ_CHANNELS:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (int)state->format.channels;
        break;
    case OSS_DSP_SETFMT:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            uint32_t requested = (uint32_t)*(int *)arg;
            if (requested != OSS_AFMT_QUERY) {
                pcm_format_t candidate = state->format;
                if (oss_set_format_id(&candidate, requested)) {
                    if (candidate.sample_format != state->format.sample_format)
                        oss_reset_queue_locked(state);
                    state->format = candidate;
                }
            }
            *(int *)arg = (int)oss_format_id(&state->format);
        }
        break;
    case OSS_DSP_READ_BITS:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (pcm_frame_bytes(&state->format) /
                       state->format.channels) == 2U ? 16 : 8;
        break;
    case OSS_DSP_GETFMTS:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (int)OSS_AFMT_MASK;
        break;
    case OSS_DSP_GETBLKSIZE:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (int)state->fragment_bytes;
        break;
    case OSS_DSP_SUBDIVIDE:
        if (!arg) { result = -OSS_EFAULT; break; }
        if (*(int *)arg != 1 && *(int *)arg != 2 && *(int *)arg != 4) {
            result = -OSS_EINVAL;
            break;
        }
        state->fragment_bytes = OSS_DEFAULT_FRAGMENT_BYTES /
                                (uint32_t)*(int *)arg;
        break;
    case OSS_DSP_SETFRAGMENT:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            uint32_t setting = (uint32_t)*(int *)arg;
            uint32_t exponent = setting & 0xFFFFU;
            uint32_t fragments = setting >> 16;
            if (exponent < 8U) exponent = 8U;
            if (exponent > 14U) exponent = 14U;
            state->fragment_bytes = 1U << exponent;
            if (fragments < 2U) fragments = 2U;
            uint64_t requested = (uint64_t)fragments * state->fragment_bytes;
            state->buffer_limit = requested < OSS_BUFFER_BYTES
                                ? (uint32_t)requested : OSS_BUFFER_BYTES;
            *(int *)arg = (int)(((state->buffer_limit /
                                  state->fragment_bytes) << 16) | exponent);
        }
        break;
    case OSS_DSP_GETOSPACE:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            oss_audio_buf_info_t *info = (oss_audio_buf_info_t *)arg;
            uint32_t free_bytes = state->data_bytes < state->buffer_limit
                                ? state->buffer_limit - state->data_bytes : 0;
            info->fragments = (int)(free_bytes / state->fragment_bytes);
            info->fragstotal = (int)(state->buffer_limit /
                                     state->fragment_bytes);
            info->fragsize = (int)state->fragment_bytes;
            info->bytes = (int)free_bytes;
        }
        break;
    case OSS_DSP_GETCAPS:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = OSS_DSP_CAP_REALTIME | OSS_DSP_CAP_TRIGGER |
                      OSS_DSP_CAP_MULTI;
        break;
    case OSS_DSP_GETTRIGGER:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = state->output_triggered ? OSS_PCM_ENABLE_OUTPUT : 0;
        break;
    case OSS_DSP_SETTRIGGER:
        if (!arg) { result = -OSS_EFAULT; break; }
        state->output_triggered = (*(int *)arg & OSS_PCM_ENABLE_OUTPUT) != 0;
        if ((*(int *)arg & OSS_PCM_ENABLE_INPUT) &&
            !(*(int *)arg & OSS_PCM_ENABLE_OUTPUT))
            result = -OSS_ENODEV;
        break;
    case OSS_DSP_GETOPTR:
        if (!arg) { result = -OSS_EFAULT; break; }
        {
            oss_count_info_t *info = (oss_count_info_t *)arg;
            info->bytes = (int)state->bytes_played;
            info->blocks = (int)state->blocks_played;
            info->pointer = (int)(state->bytes_played % state->buffer_limit);
            state->blocks_played = 0;
        }
        break;
    case OSS_DSP_GETODELAY:
        if (!arg) { result = -OSS_EFAULT; break; }
        *(int *)arg = (int)oss_delay_bytes_locked(state);
        break;
    default:
        result = -OSS_ENOTTY;
        break;
    }
    oss_unlock_irqrestore(state, flags);
    return result;
}

const fd_device_ops_t oss_audio_device_ops = {
    .read = oss_read,
    .write = oss_write,
    .ioctl = oss_ioctl,
    .read_ready = oss_read_ready,
    .write_ready = oss_write_ready,
    .retain = oss_retain,
    .release = oss_release,
    .rdev = 0x0E03U,
};

static int16_t oss_selftest_silence[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];

static bool oss_selftest_silence_fill(int16_t *output, uint32_t frames)
{
    memset(output, 0,
           (size_t)frames * AUDIO_OUTPUT_CHANNELS * sizeof(*output));
    return true;
}

static int oss_audio_capacity_selftest(void)
{
    uint32_t capacity = audio_get_source_capacity();
    audio_source_id_t *sources = NULL;
    uint32_t source_count = 0;
    void *probe = NULL;
    int failure = 0;

    if (!capacity)
        return 1;
    sources = (audio_source_id_t *)kmalloc(
        (uint64_t)capacity * sizeof(*sources));
    if (!sources)
        return 2;
    memset(sources, 0, (size_t)capacity * sizeof(*sources));

    while (source_count < capacity) {
        audio_source_id_t source = AUDIO_SOURCE_INVALID;
        audio_register_result_t result = audio_source_register_ex(
            oss_selftest_silence_fill, oss_selftest_silence,
            AUDIO_OUTPUT_BLOCK_FRAMES, AUDIO_OUTPUT_RATE_HZ, &source);
        if (result == AUDIO_REGISTER_NO_SLOTS)
            break;
        if (result != AUDIO_REGISTER_OK || source == AUDIO_SOURCE_INVALID) {
            failure = 3;
            goto cleanup;
        }
        sources[source_count++] = source;
    }

    audio_source_id_t extra_source = AUDIO_SOURCE_INVALID;
    if (audio_source_register_ex(
            oss_selftest_silence_fill, oss_selftest_silence,
            AUDIO_OUTPUT_BLOCK_FRAMES, AUDIO_OUTPUT_RATE_HZ,
            &extra_source) != AUDIO_REGISTER_NO_SLOTS ||
        extra_source != AUDIO_SOURCE_INVALID) {
        if (extra_source != AUDIO_SOURCE_INVALID)
            audio_source_unregister(extra_source);
        failure = 4;
        goto cleanup;
    }

    if (oss_audio_create(false, OSS_O_WRONLY, &probe) != -OSS_EBUSY || probe) {
        failure = 5;
        goto cleanup;
    }

    if (!source_count) {
        failure = 6;
        goto cleanup;
    }
    audio_source_unregister(sources[--source_count]);
    sources[source_count] = AUDIO_SOURCE_INVALID;

    if (oss_audio_create(false, OSS_O_WRONLY, &probe) < 0 || !probe) {
        failure = 7;
        goto cleanup;
    }

cleanup:
    if (probe)
        oss_release(probe, OSS_O_WRONLY | OSS_O_NONBLOCK);
    while (source_count)
        audio_source_unregister(sources[--source_count]);
    kfree(sources);
    return failure;
}

int oss_audio_selftest(void)
{
    int pcm_result = pcm_selftest();
    if (pcm_result)
        return 100 + pcm_result;

    if (!audio_output_is_ready()) {
        void *unavailable_context = (void *)(uint64_t)1U;
        int result = oss_audio_create(false, OSS_O_WRONLY,
                                      &unavailable_context);
        return result == -OSS_ENODEV && !unavailable_context ? 0 : 1;
    }

    void *context = NULL;
    if (oss_audio_create(false, OSS_O_WRONLY, &context) < 0)
        return 1;
    uint32_t oflags = OSS_O_WRONLY;
    int format = OSS_AFMT_MU_LAW;
    int rate = 8000;
    int channels = 1;
    int caps = 0;
    int formats = 0;
    uint8_t samples[256];
    for (uint32_t i = 0; i < sizeof(samples); i++)
        samples[i] = i < sizeof(samples) / 2U ? 0x80U : 0x00U;

    int failure = 0;
    if (oss_ioctl(context, OSS_DSP_SETFMT, (uint64_t)&format, &oflags) < 0 ||
        format != (int)OSS_AFMT_MU_LAW)
        failure = 2;
    else if (oss_ioctl(context, OSS_DSP_SPEED, (uint64_t)&rate, &oflags) < 0 ||
             rate != 8000)
        failure = 3;
    else if (oss_ioctl(context, OSS_DSP_CHANNELS,
                       (uint64_t)&channels, &oflags) < 0 || channels != 1)
        failure = 4;
    else if (oss_ioctl(context, OSS_DSP_GETCAPS,
                       (uint64_t)&caps, &oflags) < 0 ||
             !(caps & OSS_DSP_CAP_TRIGGER))
        failure = 5;
    else if (oss_ioctl(context, OSS_DSP_GETFMTS,
                       (uint64_t)&formats, &oflags) < 0 ||
             (formats & OSS_AFMT_MASK) != OSS_AFMT_MASK)
        failure = 6;
    else if (oss_write(context, samples, sizeof(samples), oflags) !=
             (ssize_t)sizeof(samples))
        failure = 7;
    else if (oss_ioctl(context, OSS_DSP_SYNC, 0, &oflags) < 0)
        failure = 8;

    oss_retain(context);
    oss_release(context, oflags);
    oss_release(context, oflags);
    if (!failure && audio_get_next_deadline_us() != 0)
        failure = 9;
    if (!failure) {
        int capacity_failure = oss_audio_capacity_selftest();
        if (capacity_failure)
            failure = 10 + capacity_failure;
    }
    if (!failure && audio_get_next_deadline_us() != 0)
        failure = 20;
    return failure;
}
