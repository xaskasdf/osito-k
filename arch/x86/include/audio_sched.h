#ifndef OSITOK_AUDIO_SCHED_H
#define OSITOK_AUDIO_SCHED_H

#include "types.h"

/* Frontends submit this canonical format. The scheduler owns conversion from
 * API-specific legacy formats before blocks reach the hardware backend. */
#define AUDIO_OUTPUT_RATE_HZ  48000U
#define AUDIO_OUTPUT_CHANNELS 2U
#define AUDIO_OUTPUT_BITS     16U
#define AUDIO_OUTPUT_BLOCK_FRAMES (AUDIO_OUTPUT_RATE_HZ / 100U)

typedef bool (*audio_fill_fn)(int16_t *buffer, uint32_t frames);
typedef bool (*audio_fill_context_fn)(void *context, int16_t *buffer,
                                      uint32_t frames);
typedef uint32_t audio_source_id_t;

#define AUDIO_SOURCE_INVALID 0U

typedef enum {
    AUDIO_REGISTER_OK = 0,
    AUDIO_REGISTER_INVALID,
    AUDIO_REGISTER_NO_BACKEND,
    AUDIO_REGISTER_NO_CLOCK,
    AUDIO_REGISTER_NO_SLOTS,
    AUDIO_REGISTER_SOURCE_INACTIVE,
} audio_register_result_t;

bool audio_output_is_ready(void);
const char *audio_output_backend_name(void);
uint32_t audio_get_source_capacity(void);

/* Global output gain, expressed in the WinMM-compatible unsigned 16-bit
 * range. It is applied after all frontend sources have been mixed. */
void audio_output_set_volume(uint16_t left, uint16_t right);
void audio_output_get_volume(uint16_t *left, uint16_t *right);

/* Register one canonical PCM producer. Sources are mixed by the scheduler,
 * so independent compatibility frontends never take the hardware backend
 * away from each other. */
audio_register_result_t audio_source_register_ex(
    audio_fill_fn fill, int16_t *buffer, uint32_t frames,
    uint32_t sample_rate, audio_source_id_t *source_id);
audio_register_result_t audio_source_register_context_ex(
    audio_fill_context_fn fill, void *context, int16_t *buffer,
    uint32_t frames, uint32_t sample_rate, audio_source_id_t *source_id);
const char *audio_register_result_name(audio_register_result_t result);

/* Boolean compatibility wrappers for callers that do not need the cause. */
bool audio_source_register(audio_fill_fn fill, int16_t *buffer,
                           uint32_t frames, uint32_t sample_rate,
                           audio_source_id_t *source_id);
bool audio_source_register_context(audio_fill_context_fn fill, void *context,
                                   int16_t *buffer, uint32_t frames,
                                   uint32_t sample_rate,
                                   audio_source_id_t *source_id);
void audio_source_unregister(audio_source_id_t source_id);

/* Compatibility wrapper for the original single-producer API. */
bool audio_register_callback(audio_fill_fn fill, int16_t *buffer,
                             uint32_t frames, uint32_t sample_rate);
void audio_unregister(void);
void audio_check_deadline(void);
uint64_t audio_get_next_deadline_us(void);
uint64_t audio_get_fills(void);
uint64_t audio_get_overruns(void);
uint32_t audio_get_output_latency_blocks(void);

#endif
