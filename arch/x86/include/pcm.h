#ifndef OSITOK_PCM_H
#define OSITOK_PCM_H

#include "types.h"

#define PCM_GAIN_UNITY 65536U

typedef enum {
    PCM_SAMPLE_U8 = 1,
    PCM_SAMPLE_S16_LE = 2,
    PCM_SAMPLE_S8 = 3,
    PCM_SAMPLE_U16_LE = 4,
    PCM_SAMPLE_MU_LAW = 5,
    PCM_SAMPLE_A_LAW = 6,
} pcm_sample_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t sample_format;
} pcm_format_t;

typedef struct {
    uint64_t frame_q32;
} pcm_cursor_t;

bool pcm_format_valid(const pcm_format_t *format);
uint32_t pcm_frame_bytes(const pcm_format_t *format);
uint8_t pcm_silence_byte(const pcm_format_t *format);
void pcm_fill_silence(void *destination, uint32_t bytes,
                      const pcm_format_t *format);

/* Add a circular legacy PCM source to a signed 32-bit stereo accumulator.
 * The source cursor is retained in Q32 frames so arbitrary sample rates do
 * not lose fractional progress between output blocks. */
uint32_t pcm_mix_circular_s16_stereo(
    int32_t *destination, uint32_t destination_frames,
    const void *source, uint32_t source_bytes,
    const pcm_format_t *format, uint32_t output_rate,
    uint32_t left_gain_q16, uint32_t right_gain_q16,
    bool looping, pcm_cursor_t *cursor, bool *ended);

int pcm_selftest(void);

#endif
