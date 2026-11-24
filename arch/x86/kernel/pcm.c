#include "../include/pcm.h"

bool pcm_format_valid(const pcm_format_t *format)
{
    if (!format || !format->sample_rate)
        return false;
    if (format->channels != 1 && format->channels != 2)
        return false;
    return format->sample_format == PCM_SAMPLE_U8 ||
           format->sample_format == PCM_SAMPLE_S8 ||
           format->sample_format == PCM_SAMPLE_S16_LE ||
           format->sample_format == PCM_SAMPLE_U16_LE ||
           format->sample_format == PCM_SAMPLE_MU_LAW ||
           format->sample_format == PCM_SAMPLE_A_LAW;
}

uint32_t pcm_frame_bytes(const pcm_format_t *format)
{
    if (!pcm_format_valid(format))
        return 0;
    uint32_t sample_bytes =
        format->sample_format == PCM_SAMPLE_S16_LE ||
        format->sample_format == PCM_SAMPLE_U16_LE ? 2U : 1U;
    return sample_bytes * format->channels;
}

uint8_t pcm_silence_byte(const pcm_format_t *format)
{
    if (!pcm_format_valid(format))
        return 0;
    if (format->sample_format == PCM_SAMPLE_U8)
        return 0x80U;
    if (format->sample_format == PCM_SAMPLE_MU_LAW)
        return 0xFFU;
    if (format->sample_format == PCM_SAMPLE_A_LAW)
        return 0xD5U;
    return 0;
}

void pcm_fill_silence(void *destination, uint32_t bytes,
                      const pcm_format_t *format)
{
    if (!destination || !bytes || !pcm_format_valid(format))
        return;
    if (format->sample_format == PCM_SAMPLE_U16_LE) {
        uint8_t *output = (uint8_t *)destination;
        uint32_t offset = 0;
        while (offset + 1U < bytes) {
            output[offset++] = 0;
            output[offset++] = 0x80U;
        }
        if (offset < bytes)
            output[offset] = 0;
    } else {
        memset(destination, pcm_silence_byte(format), bytes);
    }
}

static int32_t pcm_decode_mu_law(uint8_t encoded)
{
    uint8_t value = (uint8_t)~encoded;
    int32_t magnitude = ((int32_t)(value & 0x0FU) << 3) + 0x84;
    magnitude <<= (value >> 4) & 0x07U;
    return value & 0x80U ? 0x84 - magnitude : magnitude - 0x84;
}

static int32_t pcm_decode_a_law(uint8_t encoded)
{
    uint8_t value = encoded ^ 0x55U;
    int32_t magnitude = (int32_t)(value & 0x0FU) << 4;
    uint8_t segment = (value >> 4) & 0x07U;
    if (!segment) {
        magnitude += 8;
    } else {
        magnitude += 0x108;
        if (segment > 1)
            magnitude <<= segment - 1U;
    }
    return value & 0x80U ? magnitude : -magnitude;
}

static int32_t pcm_read_sample(const uint8_t *source,
                               const pcm_format_t *format,
                               uint32_t frame, uint32_t channel)
{
    uint32_t frame_bytes = pcm_frame_bytes(format);
    uint32_t sample_bytes = frame_bytes / format->channels;
    if (format->channels == 1)
        channel = 0;
    const uint8_t *sample = source + frame * frame_bytes +
                            channel * sample_bytes;

    if (format->sample_format == PCM_SAMPLE_U8)
        return ((int32_t)sample[0] - 128) << 8;
    if (format->sample_format == PCM_SAMPLE_S8)
        return (int32_t)(int8_t)sample[0] << 8;
    if (format->sample_format == PCM_SAMPLE_MU_LAW)
        return pcm_decode_mu_law(sample[0]);
    if (format->sample_format == PCM_SAMPLE_A_LAW)
        return pcm_decode_a_law(sample[0]);

    uint16_t raw = (uint16_t)sample[0] | ((uint16_t)sample[1] << 8);
    if (format->sample_format == PCM_SAMPLE_U16_LE)
        return (int32_t)raw - 32768;
    return (int16_t)raw;
}

uint32_t pcm_mix_circular_s16_stereo(
    int32_t *destination, uint32_t destination_frames,
    const void *source, uint32_t source_bytes,
    const pcm_format_t *format, uint32_t output_rate,
    uint32_t left_gain_q16, uint32_t right_gain_q16,
    bool looping, pcm_cursor_t *cursor, bool *ended)
{
    if (ended)
        *ended = false;
    if (!destination || !destination_frames || !source || !cursor ||
        !pcm_format_valid(format) || !output_rate)
        return 0;

    uint32_t frame_bytes = pcm_frame_bytes(format);
    uint32_t source_frames = source_bytes / frame_bytes;
    if (!source_frames)
        return 0;

    uint64_t source_end_q32 = (uint64_t)source_frames << 32;
    uint64_t step_q32 = ((uint64_t)format->sample_rate << 32) /
                        output_rate;
    if (!step_q32)
        step_q32 = 1;

    uint64_t position = cursor->frame_q32;
    uint32_t mixed = 0;
    const uint8_t *bytes = (const uint8_t *)source;

    while (mixed < destination_frames) {
        if (position >= source_end_q32) {
            if (!looping) {
                if (ended)
                    *ended = true;
                break;
            }
            position %= source_end_q32;
        }

        uint32_t frame0 = (uint32_t)(position >> 32);
        uint32_t frame1 = frame0 + 1;
        if (frame1 >= source_frames)
            frame1 = looping ? 0 : frame0;
        uint32_t fraction = (uint32_t)position;

        int32_t left0 = pcm_read_sample(bytes, format, frame0, 0);
        int32_t right0 = pcm_read_sample(bytes, format, frame0, 1);
        int32_t left1 = pcm_read_sample(bytes, format, frame1, 0);
        int32_t right1 = pcm_read_sample(bytes, format, frame1, 1);
        int32_t left = left0 +
            (int32_t)(((int64_t)(left1 - left0) * fraction) >> 32);
        int32_t right = right0 +
            (int32_t)(((int64_t)(right1 - right0) * fraction) >> 32);

        destination[mixed * 2] +=
            (int32_t)(((int64_t)left * left_gain_q16) >> 16);
        destination[mixed * 2 + 1] +=
            (int32_t)(((int64_t)right * right_gain_q16) >> 16);
        mixed++;
        position += step_q32;
    }

    if (looping && position >= source_end_q32)
        position %= source_end_q32;
    if (!looping && position >= source_end_q32) {
        position = source_end_q32;
        if (ended)
            *ended = true;
    }
    cursor->frame_q32 = position;
    return mixed;
}

int pcm_selftest(void)
{
    static const uint8_t source[] = { 128, 255, 0, 128 };
    pcm_format_t format = { 48000, 1, PCM_SAMPLE_U8 };
    pcm_cursor_t cursor = { 0 };
    int32_t output[8] = { 0 };
    bool ended = false;

    uint32_t mixed = pcm_mix_circular_s16_stereo(
        output, 4, source, sizeof(source), &format, 48000,
        PCM_GAIN_UNITY, PCM_GAIN_UNITY, false, &cursor, &ended);
    if (mixed != 4 || !ended)
        return 1;
    if (output[0] != 0 || output[1] != 0 ||
        output[2] != 32512 || output[3] != 32512 ||
        output[4] != -32768 || output[5] != -32768 ||
        output[6] != 0 || output[7] != 0)
        return 2;
    uint8_t silence[4] = { 0, 0, 0, 0 };
    pcm_fill_silence(silence, sizeof(silence), &format);
    if (silence[0] != 0x80 || silence[1] != 0x80 ||
        silence[2] != 0x80 || silence[3] != 0x80)
        return 3;
    format.sample_format = PCM_SAMPLE_S16_LE;
    memset(silence, 0xFF, sizeof(silence));
    pcm_fill_silence(silence, sizeof(silence), &format);
    if (silence[0] || silence[1] || silence[2] || silence[3])
        return 4;
    format.sample_format = PCM_SAMPLE_U16_LE;
    memset(silence, 0xFF, sizeof(silence));
    pcm_fill_silence(silence, sizeof(silence), &format);
    if (silence[0] || silence[1] != 0x80 ||
        silence[2] || silence[3] != 0x80)
        return 5;

    static const uint8_t signed_source[] = { 0x80, 0x00, 0x7F };
    format.channels = 1;
    format.sample_format = PCM_SAMPLE_S8;
    cursor.frame_q32 = 0;
    memset(output, 0, sizeof(output));
    mixed = pcm_mix_circular_s16_stereo(
        output, 3, signed_source, sizeof(signed_source), &format, 48000,
        PCM_GAIN_UNITY, PCM_GAIN_UNITY, false, &cursor, &ended);
    if (mixed != 3 || !ended || output[0] != -32768 || output[1] != -32768 ||
        output[2] != 0 || output[3] != 0 ||
        output[4] != 32512 || output[5] != 32512)
        return 6;

    static const uint8_t mu_law_source[] = { 0xFF, 0x80, 0x00 };
    format.sample_format = PCM_SAMPLE_MU_LAW;
    cursor.frame_q32 = 0;
    memset(output, 0, sizeof(output));
    mixed = pcm_mix_circular_s16_stereo(
        output, 3, mu_law_source, sizeof(mu_law_source), &format, 48000,
        PCM_GAIN_UNITY, PCM_GAIN_UNITY, false, &cursor, &ended);
    if (mixed != 3 || !ended || output[0] != 0 || output[1] != 0 ||
        output[2] != 32124 || output[3] != 32124 ||
        output[4] != -32124 || output[5] != -32124)
        return 7;
    memset(silence, 0, sizeof(silence));
    pcm_fill_silence(silence, sizeof(silence), &format);
    if (silence[0] != 0xFF || silence[1] != 0xFF ||
        silence[2] != 0xFF || silence[3] != 0xFF)
        return 8;

    static const uint8_t a_law_source[] = { 0xD5, 0xAA, 0x2A };
    format.sample_format = PCM_SAMPLE_A_LAW;
    cursor.frame_q32 = 0;
    memset(output, 0, sizeof(output));
    mixed = pcm_mix_circular_s16_stereo(
        output, 3, a_law_source, sizeof(a_law_source), &format, 48000,
        PCM_GAIN_UNITY, PCM_GAIN_UNITY, false, &cursor, &ended);
    if (mixed != 3 || !ended || output[0] != 8 || output[1] != 8 ||
        output[2] != 32256 || output[3] != 32256 ||
        output[4] != -32256 || output[5] != -32256)
        return 9;
    memset(silence, 0, sizeof(silence));
    pcm_fill_silence(silence, sizeof(silence), &format);
    if (silence[0] != 0xD5 || silence[1] != 0xD5 ||
        silence[2] != 0xD5 || silence[3] != 0xD5)
        return 10;
    return 0;
}
