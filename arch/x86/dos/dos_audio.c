/*
 * Sound Blaster 16 frontend for DOS guests.
 *
 * The guest sees the conventional A220/I5/D1/H5 interface. ISA DMA samples
 * are converted by the shared PCM layer and submitted as an independent
 * source to the kernel audio scheduler; this module never touches HDA.
 */

#include "dos_audio.h"
#include "dos_hostmem.h"
#include "dos_types.h"
#include "../include/audio_sched.h"
#include "../include/pcm.h"

extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern uint64_t idt_get_monotonic_ns(void);

#define DOS_SB_BASE              0x220U
#define DOS_SB_IRQ_LINE          5U
#define DOS_SB_DMA8_CHANNEL      1U
#define DOS_SB_DMA16_CHANNEL     5U
#define DOS_SB_CARD_TYPE         6U
#define DOS_SB_RESPONSE_CAPACITY 64U
#define DOS_SB_DIRECT_DAC_CAPACITY 2048U
#define DOS_SB_IRQ_8BIT          0x01U
#define DOS_SB_IRQ_16BIT         0x02U
#define DOS_SB_NS_PER_SECOND     1000000000ULL

#define DOS_DMA_LOCAL_CHANNEL(channel) ((channel) & 3U)
#define DOS_DMA8_ADDRESS_PORT(channel) ((channel) * 2U)
#define DOS_DMA8_COUNT_PORT(channel)   (DOS_DMA8_ADDRESS_PORT(channel) + 1U)
#define DOS_DMA16_ADDRESS_PORT(channel) \
    (0xC0U + (((channel) - 4U) * 4U))
#define DOS_DMA16_COUNT_PORT(channel)  (DOS_DMA16_ADDRESS_PORT(channel) + 2U)
#define DOS_DMA_PAGE_PORT(channel) \
    ((channel) == 0U ? 0x87U : (channel) == 1U ? 0x83U : \
     (channel) == 2U ? 0x81U : (channel) == 3U ? 0x82U : \
     (channel) == 4U ? 0x8FU : (channel) == 5U ? 0x8BU : \
     (channel) == 6U ? 0x89U : 0x8AU)
#define DOS_DMA_TC_MASK(channel) \
    (1U << DOS_DMA_LOCAL_CHANNEL(channel))
#define DOS_DMA_REQUEST_MASK(channel) \
    (1U << (DOS_DMA_LOCAL_CHANNEL(channel) + 4U))

_Static_assert(DOS_SB_DMA8_CHANNEL < 4U,
               "Sound Blaster 8-bit DMA channel must use controller 1");
_Static_assert(DOS_SB_DMA16_CHANNEL >= 5U && DOS_SB_DMA16_CHANNEL <= 7U,
               "Sound Blaster 16-bit DMA channel must use controller 2");

typedef struct {
    uint16_t address;
    uint16_t count;
    uint16_t base_address;
    uint16_t base_count;
    uint8_t page;
    uint8_t mode;
    bool masked;
    bool terminal_count;
    bool software_request;
} DOS_DMA_CHANNEL;

typedef enum {
    DOS_SB_TRANSFER_NONE = 0,
    DOS_SB_TRANSFER_PLAYBACK,
    DOS_SB_TRANSFER_CAPTURE,
    DOS_SB_TRANSFER_SILENCE,
    DOS_SB_TRANSFER_DIRECT_DAC,
} DOS_SB_TRANSFER_MODE;

typedef enum {
    DOS_SB_ENCODING_PCM = 0,
    DOS_SB_ENCODING_ADPCM_2,
    DOS_SB_ENCODING_ADPCM_3,
    DOS_SB_ENCODING_ADPCM_4,
} DOS_SB_ENCODING;

typedef enum {
    DOS_SB_MIDI_NONE = 0,
    DOS_SB_MIDI_POLL_INPUT,
    DOS_SB_MIDI_IRQ_INPUT,
    DOS_SB_MIDI_UART_POLL,
    DOS_SB_MIDI_UART_IRQ,
    DOS_SB_MIDI_UART_POLL_TIMESTAMP,
    DOS_SB_MIDI_UART_IRQ_TIMESTAMP,
} DOS_SB_MIDI_MODE;

typedef struct {
    uint8_t *guest_memory;
    uint32_t guest_memory_size;
    volatile uint32_t lock;
    volatile uint32_t transition;
    bool present;
    bool test_mode;
    bool reset_asserted;
    bool speaker_enabled;
    bool playing;
    bool paused;
    bool auto_init;
    bool exit_auto_init;
    bool high_speed;
    bool sixteen_bit;
    bool output_rate_is_transfer_rate;
    bool input_rate_is_transfer_rate;
    bool legacy_input_stereo;
    bool adpcm_reference_pending;
    bool adpcm_have_byte;
    bool adpcm_have_current;
    bool adpcm_have_next;
    bool direct_dac_primed;
    bool e2_dma_pending;
    bool midi_backend_warned;
    bool pending_dma_command;
    bool waiting_for_dma;
    DOS_SB_TRANSFER_MODE transfer_mode;
    DOS_SB_ENCODING encoding;
    DOS_SB_MIDI_MODE midi_mode;

    uint8_t command;
    uint8_t parameter_count;
    uint8_t parameter_expected;
    uint8_t parameters[4];
    uint8_t responses[DOS_SB_RESPONSE_CAPACITY];
    uint8_t response_head;
    uint8_t response_tail;
    uint8_t response_count;
    uint8_t test_register;
    uint8_t mixer_index;
    uint8_t mixer[256];
    uint8_t dma_flip8;
    uint8_t dma_flip16;
    uint8_t dma_command8;
    uint8_t dma_command16;
    uint8_t adpcm_reference;
    uint8_t adpcm_step;
    uint8_t adpcm_byte;
    uint8_t adpcm_portion;
    uint8_t adpcm_current;
    uint8_t adpcm_next;
    uint8_t direct_dac_level;
    uint8_t e2_value;
    uint8_t pending_command;
    uint8_t pending_parameters[3];
    uint8_t waiting_command;
    uint8_t waiting_parameters[3];
    uint16_t direct_dac_head;
    uint16_t direct_dac_tail;
    uint16_t direct_dac_count;
    uint32_t block_units;
    uint32_t sample_rate;
    uint32_t output_rate;
    uint32_t input_rate;
    uint32_t unknown_commands;
    uint32_t playback_errors;
    uint32_t capture_errors;
    uint32_t source_errors;
    uint32_t capture_commands;
    uint32_t direct_dac_overruns;
    uint32_t direct_dac_ns_remainder;
    uint32_t e2_count;
    uint64_t midi_output_bytes;

    DOS_DMA_CHANNEL dma8;
    DOS_DMA_CHANNEL dma16;
    pcm_format_t format;
    pcm_cursor_t cursor;
    uint32_t dma_base;
    uint32_t dma_bytes;
    uint32_t block_bytes;
    uint32_t dma_offset;
    uint32_t adpcm_input_offset;
    uint64_t adpcm_phase_q32;
    uint64_t direct_dac_render_ns;
    uint16_t dma_start_address;
    uint16_t dma_start_count;
    volatile uint32_t irq_pending;
    volatile uint32_t irq_latched;

    audio_source_id_t source;
    uint64_t direct_dac_event_ns[DOS_SB_DIRECT_DAC_CAPACITY];
    uint8_t direct_dac_event_sample[DOS_SB_DIRECT_DAC_CAPACITY];
    int32_t accumulator[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
    int16_t output[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
} DOS_AUDIO_STATE;

static uint64_t dos_audio_lock_irqsave(DOS_AUDIO_STATE *state)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&state->lock, 1U))
        __asm__ volatile ("pause" ::: "memory");
    return flags;
}

static void dos_audio_unlock_irqrestore(DOS_AUDIO_STATE *state,
                                        uint64_t flags)
{
    __sync_lock_release(&state->lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void dos_audio_transition_acquire(DOS_AUDIO_STATE *state)
{
    while (__sync_lock_test_and_set(&state->transition, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static void dos_audio_transition_release(DOS_AUDIO_STATE *state)
{
    __sync_lock_release(&state->transition);
}

static void dos_sb_response_push_locked(DOS_AUDIO_STATE *state,
                                        uint8_t value)
{
    if (state->response_count == DOS_SB_RESPONSE_CAPACITY)
        return;
    state->responses[state->response_tail] = value;
    state->response_tail = (uint8_t)(
        (state->response_tail + 1U) % DOS_SB_RESPONSE_CAPACITY);
    state->response_count++;
}

static void dos_sb_response_clear_locked(DOS_AUDIO_STATE *state)
{
    state->response_head = 0;
    state->response_tail = 0;
    state->response_count = 0;
}

static uint8_t dos_sb_response_pop_locked(DOS_AUDIO_STATE *state)
{
    if (!state->response_count)
        return 0xFFU;
    uint8_t value = state->responses[state->response_head];
    state->response_head = (uint8_t)(
        (state->response_head + 1U) % DOS_SB_RESPONSE_CAPACITY);
    state->response_count--;
    return value;
}

static void dos_sb_parser_reset_locked(DOS_AUDIO_STATE *state)
{
    state->command = 0;
    state->parameter_count = 0;
    state->parameter_expected = 0;
    dos_sb_response_clear_locked(state);
    state->playing = false;
    state->paused = false;
    state->auto_init = false;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->transfer_mode = DOS_SB_TRANSFER_NONE;
    state->encoding = DOS_SB_ENCODING_PCM;
    state->midi_mode = DOS_SB_MIDI_NONE;
    state->speaker_enabled = false;
    state->legacy_input_stereo = false;
    state->adpcm_reference_pending = false;
    state->adpcm_have_byte = false;
    state->adpcm_have_current = false;
    state->adpcm_have_next = false;
    state->direct_dac_primed = false;
    state->e2_dma_pending = false;
    state->pending_dma_command = false;
    state->waiting_for_dma = false;
    state->adpcm_reference = 0x80U;
    state->adpcm_step = 0;
    state->adpcm_portion = 0;
    state->direct_dac_level = 0x80U;
    state->direct_dac_head = 0;
    state->direct_dac_tail = 0;
    state->direct_dac_count = 0;
    state->direct_dac_ns_remainder = 0;
    state->adpcm_input_offset = 0;
    state->adpcm_phase_q32 = 0;
    state->direct_dac_render_ns = 0;
    state->e2_value = 0xAA;
    state->e2_count = 0;
    state->pending_command = 0;
    memset(state->pending_parameters, 0,
           sizeof(state->pending_parameters));
    state->cursor.frame_q32 = 0;
    state->dma_offset = 0;
    __atomic_store_n(&state->irq_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&state->irq_latched, 0, __ATOMIC_RELEASE);
}

static uint32_t dos_sb_clamp_rate(uint32_t rate)
{
    if (rate < 4000U)
        return 4000U;
    if (rate > 48000U)
        return 48000U;
    return rate;
}

static uint32_t dos_sb_effective_rate_locked(const DOS_AUDIO_STATE *state,
                                             bool input, bool stereo)
{
    uint32_t rate = input ? state->input_rate : state->output_rate;
    bool transfer_rate = input ? state->input_rate_is_transfer_rate
                               : state->output_rate_is_transfer_rate;
    if (transfer_rate && stereo)
        rate /= 2U;
    return dos_sb_clamp_rate(rate);
}

static uint8_t dos_sb_clamp_u8(int32_t sample)
{
    if (sample < 0)
        return 0;
    if (sample > 255)
        return 255;
    return (uint8_t)sample;
}

static uint8_t dos_sb_decode_adpcm_portion_locked(DOS_AUDIO_STATE *state,
                                                   uint8_t code)
{
    uint32_t magnitude;
    uint32_t level;
    uint32_t delta;
    bool negative;

    switch (state->encoding) {
    case DOS_SB_ENCODING_ADPCM_2:
        code &= 0x03U;
        magnitude = code & 0x01U;
        negative = (code & 0x02U) != 0;
        level = state->adpcm_step / 4U;
        delta = level
            ? (2U * magnitude + 1U) << (level - 1U)
            : magnitude;
        if (!magnitude && state->adpcm_step)
            state->adpcm_step -= 4U;
        else if (magnitude && state->adpcm_step < 20U)
            state->adpcm_step += 4U;
        break;
    case DOS_SB_ENCODING_ADPCM_3: {
        static const uint8_t factors[5] = { 1U, 2U, 4U, 8U, 10U };
        static const uint8_t biases[5] = { 0U, 1U, 2U, 4U, 5U };
        code &= 0x07U;
        magnitude = code & 0x03U;
        negative = (code & 0x04U) != 0;
        level = state->adpcm_step / 8U;
        if (level > 4U)
            level = 4U;
        delta = magnitude * factors[level] + biases[level];
        if (!magnitude && state->adpcm_step)
            state->adpcm_step -= 8U;
        else if (magnitude == 3U && state->adpcm_step < 32U)
            state->adpcm_step += 8U;
        break;
    }
    case DOS_SB_ENCODING_ADPCM_4:
        code &= 0x0FU;
        magnitude = code & 0x07U;
        negative = (code & 0x08U) != 0;
        level = state->adpcm_step / 16U;
        delta = level
            ? (2U * magnitude + 1U) << (level - 1U)
            : magnitude;
        if (!magnitude && state->adpcm_step)
            state->adpcm_step -= 16U;
        else if (magnitude >= 5U && state->adpcm_step < 48U)
            state->adpcm_step += 16U;
        break;
    default:
        return state->adpcm_reference;
    }

    int32_t sample = state->adpcm_reference;
    sample += negative ? -(int32_t)delta : (int32_t)delta;
    state->adpcm_reference = dos_sb_clamp_u8(sample);
    return state->adpcm_reference;
}

static uint8_t dos_sb_expand_legacy_volume(uint8_t level)
{
    uint32_t value = level & 0x0FU;
    return (uint8_t)(((value * 31U + 7U) / 15U) << 3);
}

static uint8_t dos_sb_compress_sb16_volume(uint8_t value)
{
    uint32_t level = (value >> 3) & 0x1FU;
    return (uint8_t)((level * 15U + 15U) / 31U);
}

static void dos_sb_sync_legacy_volume_locked(DOS_AUDIO_STATE *state,
                                              uint8_t legacy_index,
                                              uint8_t left_index,
                                              uint8_t right_index)
{
    state->mixer[legacy_index] = (uint8_t)(
        (dos_sb_compress_sb16_volume(state->mixer[left_index]) << 4) |
         dos_sb_compress_sb16_volume(state->mixer[right_index]));
}

static void dos_sb_set_legacy_volume_locked(DOS_AUDIO_STATE *state,
                                             uint8_t legacy_index,
                                             uint8_t left_index,
                                             uint8_t right_index,
                                             uint8_t value)
{
    state->mixer[legacy_index] = value;
    state->mixer[left_index] =
        dos_sb_expand_legacy_volume((uint8_t)(value >> 4));
    state->mixer[right_index] =
        dos_sb_expand_legacy_volume((uint8_t)(value & 0x0FU));
}

static void dos_sb_mixer_reset_locked(DOS_AUDIO_STATE *state)
{
    memset(state->mixer, 0, sizeof(state->mixer));
    state->mixer[0x30] = 0xF8U;
    state->mixer[0x31] = 0xF8U;
    state->mixer[0x32] = 0xF8U;
    state->mixer[0x33] = 0xF8U;
    dos_sb_sync_legacy_volume_locked(state, 0x22U, 0x30U, 0x31U);
    dos_sb_sync_legacy_volume_locked(state, 0x04U, 0x32U, 0x33U);
}

static void dos_sb_mixer_write_locked(DOS_AUDIO_STATE *state,
                                       uint8_t index, uint8_t value)
{
    switch (index) {
    case 0x00:
        dos_sb_mixer_reset_locked(state);
        break;
    case 0x04:
        dos_sb_set_legacy_volume_locked(state, 0x04U, 0x32U, 0x33U,
                                        value);
        break;
    case 0x22:
        dos_sb_set_legacy_volume_locked(state, 0x22U, 0x30U, 0x31U,
                                        value);
        break;
    case 0x30:
    case 0x31:
        state->mixer[index] = value & 0xF8U;
        dos_sb_sync_legacy_volume_locked(state, 0x22U, 0x30U, 0x31U);
        break;
    case 0x32:
    case 0x33:
        state->mixer[index] = value & 0xF8U;
        dos_sb_sync_legacy_volume_locked(state, 0x04U, 0x32U, 0x33U);
        break;
    case 0x80:
    case 0x81:
    case 0x82:
        break;
    default:
        state->mixer[index] = value;
        break;
    }
}

static uint32_t dos_sb_volume_gain_locked(const DOS_AUDIO_STATE *state,
                                          bool right)
{
    uint32_t master_level =
        state->mixer[right ? 0x31U : 0x30U] >> 3;
    uint32_t voice_level =
        state->mixer[right ? 0x33U : 0x32U] >> 3;
    uint32_t combined = master_level * voice_level;
    return (combined * PCM_GAIN_UNITY + 480U) / 961U;
}

static void dos_sb_raise_irq_locked(DOS_AUDIO_STATE *state)
{
    uint32_t bit = state->sixteen_bit
                 ? DOS_SB_IRQ_16BIT : DOS_SB_IRQ_8BIT;
    __atomic_fetch_or(&state->irq_latched, bit, __ATOMIC_RELEASE);
    __atomic_fetch_or(&state->irq_pending, bit, __ATOMIC_RELEASE);
}

static uint32_t dos_dma_physical_address(const DOS_DMA_CHANNEL *channel,
                                         bool sixteen_bit)
{
    uint32_t page = channel->page;
    if (sixteen_bit)
        page &= 0xFEU;
    return (page << 16) |
           ((uint32_t)channel->address << (sixteen_bit ? 1U : 0U));
}

static bool dos_sb_dma_descriptor_locked(DOS_AUDIO_STATE *state,
                                         bool sixteen_bit,
                                         uint32_t *address,
                                         uint32_t *bytes,
                                         bool *masked)
{
    DOS_DMA_CHANNEL *channel = sixteen_bit
        ? &state->dma16 : &state->dma8;
    uint32_t unit = sixteen_bit ? 2U : 1U;
    uint32_t start = dos_dma_physical_address(channel, sixteen_bit);
    uint32_t length = ((uint32_t)channel->count + 1U) * unit;
    uint32_t window = sixteen_bit ? 0x20000U : 0x10000U;
    uint32_t window_offset = start & (window - 1U);
    if (!length || !state->guest_memory ||
        length > window - window_offset ||
        start >= state->guest_memory_size ||
        length > state->guest_memory_size - start)
        return false;
    *address = start;
    *bytes = length;
    *masked = channel->masked;
    return true;
}

static bool dos_sb_try_e2_dma_locked(DOS_AUDIO_STATE *state)
{
    DOS_DMA_CHANNEL *channel = &state->dma8;
    if (!state->e2_dma_pending || channel->masked ||
        (state->dma_command8 & 0x04U) ||
        (channel->mode & 0x0CU) != 0x04U)
        return false;

    uint32_t address = dos_dma_physical_address(channel, false);
    if (!state->guest_memory || address >= state->guest_memory_size)
        return false;

    state->guest_memory[address] = (uint8_t)state->e2_value;
    bool decrement = (channel->mode & 0x20U) != 0;
    channel->address = (uint16_t)(decrement
        ? (uint32_t)channel->address - 1U
        : (uint32_t)channel->address + 1U);
    if (channel->count) {
        channel->count--;
    } else {
        channel->terminal_count = true;
        if (channel->mode & 0x10U) {
            channel->address = channel->base_address;
            channel->count = channel->base_count;
        } else {
            channel->count = 0xFFFFU;
        }
    }
    state->e2_dma_pending = false;
    return true;
}

static void dos_sb_start_e2_dma_locked(DOS_AUDIO_STATE *state,
                                        uint8_t value)
{
    static const int16_t increments[4][9] = {
        {   1,  -2,  -4,   8, -16,  32,  64, -128, -106 },
        {  -1,   2,  -4,   8,  16, -32,  64, -128,  165 },
        {  -1,   2,   4,  -8,  16, -32, -64,  128, -151 },
        {   1,  -2,   4,  -8, -16,  32, -64,  128,   90 },
    };
    uint32_t sequence = state->e2_count & 3U;
    int32_t result = state->e2_value;
    for (uint32_t bit = 0; bit < 8U; bit++) {
        if (value & (1U << bit))
            result += increments[sequence][bit];
    }
    result += increments[sequence][8];
    state->e2_value = (uint8_t)result;
    state->e2_count++;
    state->e2_dma_pending = true;
    dos_sb_try_e2_dma_locked(state);
}

static void dos_sb_sync_dma_progress_locked(DOS_AUDIO_STATE *state,
                                            uint32_t completed_bytes)
{
    DOS_DMA_CHANNEL *channel = state->sixteen_bit
        ? &state->dma16 : &state->dma8;
    uint32_t unit = state->sixteen_bit ? 2U : 1U;
    uint32_t units = completed_bytes / unit;
    uint32_t initial_units = (uint32_t)state->dma_start_count + 1U;
    uint16_t start_address = state->dma_start_address;
    uint16_t start_count = state->dma_start_count;

    if (units >= initial_units) {
        channel->terminal_count = true;
        if (!(channel->mode & 0x10U)) {
            uint32_t terminal_address = (channel->mode & 0x20U)
                ? (uint32_t)start_address - initial_units
                : (uint32_t)start_address + initial_units;
            channel->address = (uint16_t)terminal_address;
            channel->count = 0xFFFFU;
            return;
        }

        units -= initial_units;
        uint32_t base_units = (uint32_t)channel->base_count + 1U;
        units %= base_units;
        start_address = channel->base_address;
        start_count = channel->base_count;
    }

    channel->address = (uint16_t)((channel->mode & 0x20U)
        ? (uint32_t)start_address - units
        : (uint32_t)start_address + units);
    channel->count = (uint16_t)((uint32_t)start_count - units);
}

static void dos_sb_prepare_adpcm_block_locked(DOS_AUDIO_STATE *state)
{
    state->adpcm_have_byte = false;
    state->adpcm_have_current = false;
    state->adpcm_have_next = false;
    state->adpcm_portion = 0;
    state->adpcm_input_offset = 0;
    state->adpcm_phase_q32 = 0;
}

static bool dos_sb_next_adpcm_sample_locked(DOS_AUDIO_STATE *state,
                                             uint8_t *sample)
{
    if (!sample || !state->block_bytes ||
        state->dma_offset + state->block_bytes > state->dma_bytes)
        return false;

    const uint8_t *source = state->guest_memory + state->dma_base +
                            state->dma_offset;
    if (state->adpcm_reference_pending) {
        if (state->adpcm_input_offset >= state->block_bytes)
            return false;
        state->adpcm_reference = source[state->adpcm_input_offset++];
        state->adpcm_step = 0;
        state->adpcm_reference_pending = false;
        dos_sb_sync_dma_progress_locked(
            state, state->dma_offset + state->adpcm_input_offset);
    }

    if (!state->adpcm_have_byte) {
        if (state->adpcm_input_offset >= state->block_bytes)
            return false;
        state->adpcm_byte = source[state->adpcm_input_offset++];
        state->adpcm_portion = 0;
        state->adpcm_have_byte = true;
        dos_sb_sync_dma_progress_locked(
            state, state->dma_offset + state->adpcm_input_offset);
    }

    uint8_t code;
    uint8_t portions;
    switch (state->encoding) {
    case DOS_SB_ENCODING_ADPCM_2:
        portions = 4U;
        code = (uint8_t)(state->adpcm_byte >>
                         (6U - state->adpcm_portion * 2U));
        code &= 0x03U;
        break;
    case DOS_SB_ENCODING_ADPCM_3:
        portions = 3U;
        if (state->adpcm_portion == 0U)
            code = (uint8_t)(state->adpcm_byte >> 5);
        else if (state->adpcm_portion == 1U)
            code = (uint8_t)((state->adpcm_byte >> 2) & 0x07U);
        else
            code = (uint8_t)((state->adpcm_byte & 0x03U) << 1);
        break;
    case DOS_SB_ENCODING_ADPCM_4:
        portions = 2U;
        code = state->adpcm_portion == 0U
            ? (uint8_t)(state->adpcm_byte >> 4)
            : (uint8_t)(state->adpcm_byte & 0x0FU);
        break;
    default:
        return false;
    }

    state->adpcm_portion++;
    if (state->adpcm_portion >= portions)
        state->adpcm_have_byte = false;
    *sample = dos_sb_decode_adpcm_portion_locked(state, code);
    return true;
}

static bool dos_sb_write_direct_dac_locked(DOS_AUDIO_STATE *state,
                                            uint8_t sample,
                                            uint64_t now_ns)
{
    bool starting = !state->playing ||
        state->transfer_mode != DOS_SB_TRANSFER_DIRECT_DAC;
    if (starting) {
        state->sample_rate = AUDIO_OUTPUT_RATE_HZ;
        state->format.sample_rate = AUDIO_OUTPUT_RATE_HZ;
        state->format.channels = 1U;
        state->format.sample_format = PCM_SAMPLE_U8;
        state->sixteen_bit = false;
        state->auto_init = false;
        state->exit_auto_init = false;
        state->high_speed = false;
        state->paused = false;
        state->playing = true;
        state->transfer_mode = DOS_SB_TRANSFER_DIRECT_DAC;
        state->encoding = DOS_SB_ENCODING_PCM;
        state->direct_dac_primed = false;
        state->direct_dac_level = sample;
        state->direct_dac_head = 0;
        state->direct_dac_tail = 0;
        state->direct_dac_count = 0;
        state->direct_dac_ns_remainder = 0;
        state->direct_dac_render_ns = now_ns;
        return true;
    }

    uint8_t previous = state->direct_dac_level;
    uint64_t event_ns = state->direct_dac_render_ns;
    if (state->direct_dac_count) {
        uint16_t last = (uint16_t)(
            (state->direct_dac_tail + DOS_SB_DIRECT_DAC_CAPACITY - 1U) %
            DOS_SB_DIRECT_DAC_CAPACITY);
        previous = state->direct_dac_event_sample[last];
        event_ns = state->direct_dac_event_ns[last];
    }
    if (sample == previous)
        return false;
    if (now_ns > event_ns)
        event_ns = now_ns;

    if (state->direct_dac_count == DOS_SB_DIRECT_DAC_CAPACITY) {
        uint16_t head = state->direct_dac_head;
        uint64_t discarded_ns = state->direct_dac_event_ns[head];
        state->direct_dac_level =
            state->direct_dac_event_sample[head];
        state->direct_dac_head = (uint16_t)(
            (head + 1U) % DOS_SB_DIRECT_DAC_CAPACITY);
        state->direct_dac_count--;
        if (discarded_ns > state->direct_dac_render_ns) {
            state->direct_dac_render_ns = discarded_ns;
            state->direct_dac_ns_remainder = 0;
        }
        state->direct_dac_overruns++;
    }

    uint16_t tail = state->direct_dac_tail;
    state->direct_dac_event_ns[tail] = event_ns;
    state->direct_dac_event_sample[tail] = sample;
    state->direct_dac_tail = (uint16_t)(
        (tail + 1U) % DOS_SB_DIRECT_DAC_CAPACITY);
    state->direct_dac_count++;
    return false;
}

static bool dos_sb_begin_playback_locked(DOS_AUDIO_STATE *state,
                                         bool sixteen_bit, bool stereo,
                                         bool signed_samples, bool automatic,
                                         uint32_t units)
{
    uint32_t address = 0;
    uint32_t dma_bytes = 0;
    bool masked = true;
    if (!state->present)
        return false;
    uint32_t sample_rate =
        dos_sb_effective_rate_locked(state, false, stereo);
    if (!sample_rate ||
        !dos_sb_dma_descriptor_locked(state, sixteen_bit, &address,
                                      &dma_bytes, &masked)) {
        state->sixteen_bit = sixteen_bit;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->playback_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    uint32_t sample_bytes = sixteen_bit ? 2U : 1U;
    uint32_t requested = units ? units * sample_bytes : dma_bytes;
    if (automatic && state->block_units)
        requested = (uint32_t)state->block_units * sample_bytes;
    if (requested > dma_bytes)
        requested = dma_bytes;

    uint32_t block_align = sample_bytes * (stereo ? 2U : 1U);
    requested -= requested % block_align;
    if (!requested) {
        state->sixteen_bit = sixteen_bit;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->playback_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    state->sample_rate = sample_rate;
    state->format.sample_rate = sample_rate;
    state->format.channels = stereo ? 2U : 1U;
    if (sixteen_bit) {
        state->format.sample_format = signed_samples
            ? PCM_SAMPLE_S16_LE : PCM_SAMPLE_U16_LE;
    } else {
        state->format.sample_format = signed_samples
            ? PCM_SAMPLE_S8 : PCM_SAMPLE_U8;
    }
    state->dma_base = address;
    state->dma_bytes = dma_bytes;
    state->block_bytes = requested;
    state->dma_offset = 0;
    DOS_DMA_CHANNEL *channel = sixteen_bit
        ? &state->dma16 : &state->dma8;
    state->dma_start_address = channel->address;
    state->dma_start_count = channel->count;
    state->cursor.frame_q32 = 0;
    state->sixteen_bit = sixteen_bit;
    state->auto_init = automatic;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->paused = false;
    state->playing = true;
    state->transfer_mode = DOS_SB_TRANSFER_PLAYBACK;
    state->encoding = DOS_SB_ENCODING_PCM;

    /* DMA may be armed before it is unmasked. The mixer remains registered
     * and emits silence until the guest enables the selected channel. */
    (void)masked;
    return true;
}

static bool dos_sb_begin_adpcm_playback_locked(DOS_AUDIO_STATE *state,
                                                DOS_SB_ENCODING encoding,
                                                bool with_reference,
                                                bool automatic,
                                                uint32_t units)
{
    uint32_t address = 0;
    uint32_t dma_bytes = 0;
    bool masked = true;
    if (!state->present ||
        !dos_sb_dma_descriptor_locked(state, false, &address,
                                      &dma_bytes, &masked)) {
        state->sixteen_bit = false;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->playback_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    uint32_t requested = units ? units : dma_bytes;
    if (automatic && state->block_units)
        requested = state->block_units;
    if (requested > dma_bytes)
        requested = dma_bytes;
    if (requested <= (with_reference ? 1U : 0U)) {
        state->sixteen_bit = false;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->playback_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    state->sample_rate =
        dos_sb_effective_rate_locked(state, false, false);
    state->format.sample_rate = state->sample_rate;
    state->format.channels = 1U;
    state->format.sample_format = PCM_SAMPLE_U8;
    state->dma_base = address;
    state->dma_bytes = dma_bytes;
    state->block_bytes = requested;
    state->dma_offset = 0;
    state->dma_start_address = state->dma8.address;
    state->dma_start_count = state->dma8.count;
    state->cursor.frame_q32 = 0;
    state->sixteen_bit = false;
    state->auto_init = automatic;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->paused = false;
    state->playing = true;
    state->transfer_mode = DOS_SB_TRANSFER_PLAYBACK;
    state->encoding = encoding;
    state->adpcm_reference_pending = with_reference;
    dos_sb_prepare_adpcm_block_locked(state);

    (void)masked;
    return true;
}

static void dos_sb_fill_capture_block_locked(DOS_AUDIO_STATE *state)
{
    uint32_t offset = state->dma_offset;
    if (offset + state->block_bytes > state->dma_bytes)
        offset = 0;
    pcm_fill_silence(state->guest_memory + state->dma_base + offset,
                     state->block_bytes, &state->format);
}

static bool dos_sb_begin_capture_locked(DOS_AUDIO_STATE *state,
                                        bool sixteen_bit, bool stereo,
                                        bool signed_samples, bool automatic,
                                        uint32_t units)
{
    uint32_t address = 0;
    uint32_t dma_bytes = 0;
    bool masked = true;
    state->capture_commands++;
    if (!state->present)
        return false;
    uint32_t sample_rate =
        dos_sb_effective_rate_locked(state, true, stereo);
    if (!sample_rate ||
        !dos_sb_dma_descriptor_locked(state, sixteen_bit, &address,
                                      &dma_bytes, &masked)) {
        state->sixteen_bit = sixteen_bit;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->capture_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    uint32_t sample_bytes = sixteen_bit ? 2U : 1U;
    uint32_t requested = units ? units * sample_bytes : dma_bytes;
    if (automatic && state->block_units)
        requested = (uint32_t)state->block_units * sample_bytes;
    if (requested > dma_bytes)
        requested = dma_bytes;

    uint32_t block_align = sample_bytes * (stereo ? 2U : 1U);
    requested -= requested % block_align;
    if (!requested) {
        state->sixteen_bit = sixteen_bit;
        state->playing = false;
        state->paused = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        state->capture_errors++;
        dos_sb_raise_irq_locked(state);
        return false;
    }

    state->sample_rate = sample_rate;
    state->format.sample_rate = sample_rate;
    state->format.channels = stereo ? 2U : 1U;
    if (sixteen_bit) {
        state->format.sample_format = signed_samples
            ? PCM_SAMPLE_S16_LE : PCM_SAMPLE_U16_LE;
    } else {
        state->format.sample_format = signed_samples
            ? PCM_SAMPLE_S8 : PCM_SAMPLE_U8;
    }
    state->dma_base = address;
    state->dma_bytes = dma_bytes;
    state->block_bytes = requested;
    state->dma_offset = 0;
    DOS_DMA_CHANNEL *channel = sixteen_bit
        ? &state->dma16 : &state->dma8;
    state->dma_start_address = channel->address;
    state->dma_start_count = channel->count;
    state->cursor.frame_q32 = 0;
    state->sixteen_bit = sixteen_bit;
    state->auto_init = automatic;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->paused = false;
    state->playing = true;
    state->transfer_mode = DOS_SB_TRANSFER_CAPTURE;
    state->encoding = DOS_SB_ENCODING_PCM;
    dos_sb_fill_capture_block_locked(state);

    (void)masked;
    return true;
}

static bool dos_sb_begin_silence_locked(DOS_AUDIO_STATE *state,
                                         uint32_t frames)
{
    uint32_t sample_rate =
        dos_sb_effective_rate_locked(state, false, false);
    if (!state->present || !sample_rate || !frames)
        return false;
    state->sample_rate = sample_rate;
    state->format.sample_rate = sample_rate;
    state->format.channels = 1U;
    state->format.sample_format = PCM_SAMPLE_U8;
    state->block_bytes = frames;
    state->cursor.frame_q32 = 0;
    state->sixteen_bit = false;
    state->auto_init = false;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->paused = false;
    state->playing = true;
    state->transfer_mode = DOS_SB_TRANSFER_SILENCE;
    state->encoding = DOS_SB_ENCODING_PCM;
    return true;
}

static bool dos_sb_start_pending_dma_locked(DOS_AUDIO_STATE *state);

static uint32_t dos_sb_advance_silent_transfer_locked(
    DOS_AUDIO_STATE *state, uint32_t output_frames)
{
    uint32_t frame_bytes = pcm_frame_bytes(&state->format);
    uint32_t source_frames = frame_bytes
        ? state->block_bytes / frame_bytes : 0;
    if (!source_frames || !state->format.sample_rate ||
        !AUDIO_OUTPUT_RATE_HZ) {
        state->playing = false;
        state->paused = false;
        state->auto_init = false;
        state->exit_auto_init = false;
        state->high_speed = false;
        state->transfer_mode = DOS_SB_TRANSFER_NONE;
        dos_sb_start_pending_dma_locked(state);
        return 0;
    }

    uint64_t end_q32 = (uint64_t)source_frames << 32;
    uint64_t step_q32 =
        ((uint64_t)state->format.sample_rate << 32) /
        AUDIO_OUTPUT_RATE_HZ;
    if (!step_q32)
        step_q32 = 1;

    uint32_t advanced = 0;
    while (state->playing && advanced < output_frames) {
        uint64_t remaining_q32 = end_q32 - state->cursor.frame_q32;
        uint64_t until_end = (remaining_q32 + step_q32 - 1U) /
                             step_q32;
        uint32_t chunk = output_frames - advanced;
        if (until_end < chunk)
            chunk = (uint32_t)until_end;
        state->cursor.frame_q32 += step_q32 * chunk;
        advanced += chunk;
        if (state->transfer_mode == DOS_SB_TRANSFER_CAPTURE) {
            uint32_t cursor_bytes =
                (uint32_t)(state->cursor.frame_q32 >> 32) * frame_bytes;
            if (cursor_bytes > state->block_bytes)
                cursor_bytes = state->block_bytes;
            dos_sb_sync_dma_progress_locked(
                state, state->dma_offset + cursor_bytes);
        }
        if (state->cursor.frame_q32 < end_q32)
            break;

        dos_sb_raise_irq_locked(state);
        state->cursor.frame_q32 = 0;
        if (state->auto_init && !state->exit_auto_init) {
            state->dma_offset += state->block_bytes;
            if (state->dma_offset + state->block_bytes > state->dma_bytes)
                state->dma_offset = 0;
            if (state->transfer_mode == DOS_SB_TRANSFER_CAPTURE)
                dos_sb_fill_capture_block_locked(state);
        } else {
            state->playing = false;
            state->paused = false;
            state->auto_init = false;
            state->exit_auto_init = false;
            state->high_speed = false;
            state->transfer_mode = DOS_SB_TRANSFER_NONE;
            dos_sb_start_pending_dma_locked(state);
            break;
        }
    }
    return advanced;
}

static bool dos_sb_finish_adpcm_block_locked(DOS_AUDIO_STATE *state)
{
    dos_sb_raise_irq_locked(state);
    if (state->auto_init && !state->exit_auto_init) {
        state->dma_offset += state->block_bytes;
        if (state->dma_offset + state->block_bytes > state->dma_bytes)
            state->dma_offset = 0;
        dos_sb_prepare_adpcm_block_locked(state);
        return true;
    }

    state->playing = false;
    state->paused = false;
    state->auto_init = false;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->transfer_mode = DOS_SB_TRANSFER_NONE;
    state->adpcm_have_current = false;
    state->adpcm_have_next = false;
    dos_sb_start_pending_dma_locked(state);
    return false;
}

static uint32_t dos_sb_mix_adpcm_locked(DOS_AUDIO_STATE *state,
                                         int32_t *destination,
                                         uint32_t frames)
{
    if (!destination || !frames || !state->format.sample_rate)
        return 0;

    const uint64_t one_q32 = 1ULL << 32;
    uint64_t step_q32 =
        ((uint64_t)state->format.sample_rate << 32) /
        AUDIO_OUTPUT_RATE_HZ;
    if (!step_q32)
        step_q32 = 1;
    uint32_t left_gain = dos_sb_volume_gain_locked(state, false);
    uint32_t right_gain = dos_sb_volume_gain_locked(state, true);

    uint32_t mixed = 0;
    while (state->playing && mixed < frames) {
        if (!state->adpcm_have_current) {
            if (!dos_sb_next_adpcm_sample_locked(
                    state, &state->adpcm_current)) {
                if (!dos_sb_finish_adpcm_block_locked(state))
                    break;
                continue;
            }
            state->adpcm_have_current = true;
            state->adpcm_have_next = dos_sb_next_adpcm_sample_locked(
                state, &state->adpcm_next);
        }

        int32_t sample0 =
            ((int32_t)state->adpcm_current - 128) * 256;
        int32_t sample1 = state->adpcm_have_next
            ? ((int32_t)state->adpcm_next - 128) * 256
            : sample0;
        uint32_t fraction = (uint32_t)state->adpcm_phase_q32;
        int32_t sample = sample0 +
            (int32_t)(((int64_t)(sample1 - sample0) * fraction) >> 32);
        destination[mixed * 2U] += (int32_t)(
            ((int64_t)sample * left_gain) >> 16);
        destination[mixed * 2U + 1U] += (int32_t)(
            ((int64_t)sample * right_gain) >> 16);
        mixed++;
        state->adpcm_phase_q32 += step_q32;

        while (state->playing && state->adpcm_phase_q32 >= one_q32) {
            state->adpcm_phase_q32 -= one_q32;
            if (state->adpcm_have_next) {
                state->adpcm_current = state->adpcm_next;
                state->adpcm_have_next = dos_sb_next_adpcm_sample_locked(
                    state, &state->adpcm_next);
            } else {
                dos_sb_finish_adpcm_block_locked(state);
                break;
            }
        }
    }
    return mixed;
}

static uint32_t dos_sb_mix_direct_dac_locked(DOS_AUDIO_STATE *state,
                                              int32_t *destination,
                                              uint32_t frames)
{
    if (!destination || !frames)
        return 0;

    /* Delay one scheduler block so all guest-timed writes for the rendered
     * interval are already timestamped when it is reconstructed. */
    if (!state->direct_dac_primed) {
        state->direct_dac_primed = true;
        return frames;
    }

    uint32_t left_gain = dos_sb_volume_gain_locked(state, false);
    uint32_t right_gain = dos_sb_volume_gain_locked(state, true);
    for (uint32_t frame = 0; frame < frames; frame++) {
        while (state->direct_dac_count) {
            uint16_t head = state->direct_dac_head;
            if (state->direct_dac_event_ns[head] >
                state->direct_dac_render_ns)
                break;
            state->direct_dac_level =
                state->direct_dac_event_sample[head];
            state->direct_dac_head = (uint16_t)(
                (head + 1U) % DOS_SB_DIRECT_DAC_CAPACITY);
            state->direct_dac_count--;
        }

        int32_t sample =
            ((int32_t)state->direct_dac_level - 128) * 256;
        destination[frame * 2U] += (int32_t)(
            ((int64_t)sample * left_gain) >> 16);
        destination[frame * 2U + 1U] += (int32_t)(
            ((int64_t)sample * right_gain) >> 16);

        uint64_t elapsed = DOS_SB_NS_PER_SECOND +
            state->direct_dac_ns_remainder;
        state->direct_dac_render_ns +=
            elapsed / AUDIO_OUTPUT_RATE_HZ;
        state->direct_dac_ns_remainder = (uint32_t)(
            elapsed % AUDIO_OUTPUT_RATE_HZ);
    }
    return frames;
}

static bool dos_dma_controller_disabled_locked(
    const DOS_AUDIO_STATE *state, bool sixteen_bit);

static bool dos_audio_fill(void *context, int16_t *output, uint32_t frames)
{
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)context;
    if (!state || !output || !frames ||
        frames > AUDIO_OUTPUT_BLOCK_FRAMES)
        return false;

    memset(output, 0, frames * 2U * sizeof(output[0]));
    memset(state->accumulator, 0,
           frames * 2U * sizeof(state->accumulator[0]));

    uint64_t flags = dos_audio_lock_irqsave(state);
    if (!state->present || !state->playing) {
        state->source = AUDIO_SOURCE_INVALID;
        dos_audio_unlock_irqrestore(state, flags);
        return false;
    }
    bool dma_transfer =
        state->transfer_mode == DOS_SB_TRANSFER_PLAYBACK ||
        state->transfer_mode == DOS_SB_TRANSFER_CAPTURE;
    if (state->paused ||
        (dma_transfer &&
         ((state->sixteen_bit ? state->dma16.masked : state->dma8.masked) ||
          dos_dma_controller_disabled_locked(state,
                                             state->sixteen_bit)))) {
        dos_audio_unlock_irqrestore(state, flags);
        return true;
    }

    if (state->transfer_mode == DOS_SB_TRANSFER_CAPTURE ||
        state->transfer_mode == DOS_SB_TRANSFER_SILENCE) {
        uint32_t advanced =
            dos_sb_advance_silent_transfer_locked(state, frames);
        bool keep_source = advanced != 0 || state->playing;
        dos_audio_unlock_irqrestore(state, flags);
        return keep_source;
    }

    uint32_t mixed_total = 0;
    if (state->transfer_mode == DOS_SB_TRANSFER_DIRECT_DAC) {
        mixed_total = dos_sb_mix_direct_dac_locked(
            state, state->accumulator, frames);
    } else if (state->encoding != DOS_SB_ENCODING_PCM) {
        mixed_total = dos_sb_mix_adpcm_locked(
            state, state->accumulator, frames);
    } else {
        while (state->playing && mixed_total < frames) {
            uint32_t source_offset = state->dma_offset;
            if (source_offset + state->block_bytes > state->dma_bytes)
                source_offset = 0;
            const uint8_t *source = state->guest_memory + state->dma_base +
                                    source_offset;
            bool ended = false;
            uint32_t mixed = pcm_mix_circular_s16_stereo(
                state->accumulator + mixed_total * 2U,
                frames - mixed_total, source, state->block_bytes,
                &state->format, AUDIO_OUTPUT_RATE_HZ,
                dos_sb_volume_gain_locked(state, false),
                dos_sb_volume_gain_locked(state, true), false,
                &state->cursor, &ended);
            uint32_t cursor_bytes =
                (uint32_t)(state->cursor.frame_q32 >> 32) *
                pcm_frame_bytes(&state->format);
            if (cursor_bytes > state->block_bytes)
                cursor_bytes = state->block_bytes;
            dos_sb_sync_dma_progress_locked(
                state, state->dma_offset + cursor_bytes);
            mixed_total += mixed;
            if (!ended)
                break;

            dos_sb_raise_irq_locked(state);
            state->cursor.frame_q32 = 0;
            if (state->auto_init && !state->exit_auto_init) {
                state->dma_offset += state->block_bytes;
                if (state->dma_offset + state->block_bytes >
                    state->dma_bytes)
                    state->dma_offset = 0;
                if (!mixed)
                    break;
            } else {
                state->playing = false;
                state->paused = false;
                state->auto_init = false;
                state->exit_auto_init = false;
                state->high_speed = false;
                state->transfer_mode = DOS_SB_TRANSFER_NONE;
                dos_sb_start_pending_dma_locked(state);
                break;
            }
        }
    }
    bool keep_source = mixed_total != 0 || state->playing;
    dos_audio_unlock_irqrestore(state, flags);

    for (uint32_t i = 0; i < frames * 2U; i++) {
        int32_t sample = state->accumulator[i];
        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        output[i] = (int16_t)sample;
    }
    return keep_source;
}

static void dos_audio_refresh(DOS_AUDIO_STATE *state)
{
    if (!state || state->test_mode)
        return;

    dos_audio_transition_acquire(state);
    uint64_t flags = dos_audio_lock_irqsave(state);
    bool active = state->present && state->playing;
    audio_source_id_t source = state->source;
    dos_audio_unlock_irqrestore(state, flags);

    bool registration_failed = false;
    audio_register_result_t registration = AUDIO_REGISTER_OK;
    if (active && source == AUDIO_SOURCE_INVALID) {
        audio_source_id_t registered = AUDIO_SOURCE_INVALID;
        registration = audio_source_register_context_ex(
            dos_audio_fill, state, state->output, AUDIO_OUTPUT_BLOCK_FRAMES,
            AUDIO_OUTPUT_RATE_HZ, &registered);
        bool started = registration == AUDIO_REGISTER_OK;
        flags = dos_audio_lock_irqsave(state);
        active = state->present && state->playing;
        state->source = started && active
            ? registered : AUDIO_SOURCE_INVALID;
        if (!started && active) {
            state->playing = false;
            state->paused = false;
            state->auto_init = false;
            state->exit_auto_init = false;
            state->high_speed = false;
            state->pending_dma_command = false;
            state->transfer_mode = DOS_SB_TRANSFER_NONE;
            state->source_errors++;
            dos_sb_raise_irq_locked(state);
            registration_failed = true;
        }
        dos_audio_unlock_irqrestore(state, flags);
        if (started && !active) {
            audio_source_unregister(registered);
            flags = dos_audio_lock_irqsave(state);
            state->source = AUDIO_SOURCE_INVALID;
            dos_audio_unlock_irqrestore(state, flags);
        }
    } else if (!active && source != AUDIO_SOURCE_INVALID) {
        flags = dos_audio_lock_irqsave(state);
        if (state->source == source)
            state->source = AUDIO_SOURCE_INVALID;
        dos_audio_unlock_irqrestore(state, flags);
        audio_source_unregister(source);
    }
    dos_audio_transition_release(state);
    if (registration_failed) {
        serial_puts("[DOS-AUDIO] playback stopped: ");
        serial_puts(audio_register_result_name(registration));
        serial_puts("\n");
    }
}

static bool dos_sb_midi_uart_active(const DOS_AUDIO_STATE *state)
{
    return state->midi_mode >= DOS_SB_MIDI_UART_POLL &&
           state->midi_mode <= DOS_SB_MIDI_UART_IRQ_TIMESTAMP;
}

static uint8_t dos_sb_parameter_count(uint8_t command)
{
    if (command == 0x04U || command == 0x08U || command == 0x0FU ||
        command == 0x10U || command == 0x32U || command == 0x38U ||
        command == 0x40U || command == 0xE0U || command == 0xE2U ||
        command == 0xE4U || command == 0xF9U)
        return 1;
    if (command == 0x05U || command == 0x0EU ||
        command == 0x14U || command == 0x15U || command == 0x16U ||
        command == 0x17U || command == 0x24U || command == 0x41U ||
        command == 0x42U || command == 0x48U ||
        (command >= 0x74U && command <= 0x77U) ||
        command == 0x80U)
        return 2;
    if ((command >= 0xB0U && command <= 0xBFU) ||
        (command >= 0xC0U && command <= 0xCFU))
        return 3;
    return 0;
}

static bool dos_sb_is_single_cycle_dma_command(uint8_t command)
{
    switch (command) {
    case 0x14:
    case 0x16:
    case 0x17:
    case 0x24:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x91:
    case 0x99:
        return true;
    default:
        return ((command >= 0xB0U && command <= 0xBFU) ||
                (command >= 0xC0U && command <= 0xCFU)) &&
               !(command & 0x04U);
    }
}

enum {
    DOS_SB_DMA_COMMAND = 1U,
    DOS_SB_DMA_16BIT = 2U,
    DOS_SB_DMA_CAPTURE = 4U,
    DOS_SB_DMA_AUTOMATIC = 8U,
};

static unsigned dos_sb_dma_command_kind(uint8_t command)
{
    if (command >= 0xB0U && command <= 0xCFU)
        return DOS_SB_DMA_COMMAND |
            (command < 0xC0U ? DOS_SB_DMA_16BIT : 0U) |
            ((command & 8U) ? DOS_SB_DMA_CAPTURE : 0U) |
            ((command & 4U) ? DOS_SB_DMA_AUTOMATIC : 0U);
    switch (command) {
    case 0x14: case 0x16: case 0x17:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x91:
        return DOS_SB_DMA_COMMAND;
    case 0x1C: case 0x1F: case 0x7D: case 0x7F: case 0x90:
        return DOS_SB_DMA_COMMAND | DOS_SB_DMA_AUTOMATIC;
    case 0x24: case 0x99:
        return DOS_SB_DMA_COMMAND | DOS_SB_DMA_CAPTURE;
    case 0x2C: case 0x98:
        return DOS_SB_DMA_COMMAND | DOS_SB_DMA_CAPTURE | DOS_SB_DMA_AUTOMATIC;
    default:
        return 0;
    }
}

static bool dos_sb_execute_locked(DOS_AUDIO_STATE *state, uint8_t command,
                                  bool *refresh, bool *started)
{
    uint8_t *p = state->parameters;
    *refresh = false;
    *started = false;

    if ((state->playing || state->waiting_for_dma) && state->auto_init &&
        dos_sb_is_single_cycle_dma_command(command)) {
        if (!state->pending_dma_command) {
            state->pending_dma_command = true;
            state->pending_command = command;
            memcpy(state->pending_parameters, p,
                   sizeof(state->pending_parameters));
        }
        state->exit_auto_init = true;
        return true;
    }

    unsigned dma_kind = dos_sb_dma_command_kind(command);
    if (dma_kind) {
        bool sixteen_bit = (dma_kind & DOS_SB_DMA_16BIT) != 0;
        DOS_DMA_CHANNEL *channel = sixteen_bit ? &state->dma16 : &state->dma8;
        state->waiting_for_dma = false;
        if (state->present && (channel->masked ||
            dos_dma_controller_disabled_locked(state, sixteen_bit))) {
            /* DSP asserts a request; only DMA enable grants memory access.
             * The guest may replace the exhausted descriptor while masked. */
            state->waiting_for_dma = true;
            state->waiting_command = command;
            memcpy(state->waiting_parameters, p, sizeof(state->waiting_parameters));
            state->playing = false;
            state->paused = false;
            state->sixteen_bit = sixteen_bit;
            state->auto_init = (dma_kind & DOS_SB_DMA_AUTOMATIC) != 0;
            state->exit_auto_init = false;
            state->high_speed = false;
            state->transfer_mode = (dma_kind & DOS_SB_DMA_CAPTURE)
                ? DOS_SB_TRANSFER_CAPTURE : DOS_SB_TRANSFER_PLAYBACK;
            *refresh = true;
            return true;
        }
    } else if (command == 0x10U || command == 0x80U) {
        state->waiting_for_dma = false;
    }

    switch (command) {
    case 0x10:
        *started = dos_sb_write_direct_dac_locked(
            state, p[0], idt_get_monotonic_ns());
        *refresh = *started;
        break;
    case 0x16:
    case 0x17: {
        uint32_t units = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_2, command == 0x17U,
            false, units + 1U);
        *refresh = *started;
        break;
    }
    case 0x74:
    case 0x75: {
        uint32_t units = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_4, command == 0x75U,
            false, units + 1U);
        *refresh = *started;
        break;
    }
    case 0x76:
    case 0x77: {
        uint32_t units = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_3, command == 0x77U,
            false, units + 1U);
        *refresh = *started;
        break;
    }
    case 0x1F:
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_2, true, true, 0);
        *refresh = *started;
        break;
    case 0x7D:
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_4, true, true, 0);
        *refresh = *started;
        break;
    case 0x7F:
        *started = dos_sb_begin_adpcm_playback_locked(
            state, DOS_SB_ENCODING_ADPCM_3, true, true, 0);
        *refresh = *started;
        break;
    case 0x14: {
        uint32_t units = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_playback_locked(
            state, false, false, false, false, units + 1U);
        *refresh = *started;
        break;
    }
    case 0x24: {
        uint32_t units = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_capture_locked(
            state, false, false, false, false, units + 1U);
        *refresh = *started;
        break;
    }
    case 0x1C:
        *started = dos_sb_begin_playback_locked(
            state, false, false, false, true, 0);
        *refresh = *started;
        break;
    case 0x90:
        *started = dos_sb_begin_playback_locked(
            state, false, (state->mixer[0x0EU] & 0x02U) != 0,
            false, true, 0);
        state->high_speed = *started;
        *refresh = *started;
        break;
    case 0x2C:
        *started = dos_sb_begin_capture_locked(
            state, false, false, false, true, 0);
        *refresh = *started;
        break;
    case 0x98:
        *started = dos_sb_begin_capture_locked(
            state, false, state->legacy_input_stereo,
            false, true, 0);
        state->high_speed = *started;
        *refresh = *started;
        break;
    case 0x91:
        *started = dos_sb_begin_playback_locked(
            state, false, (state->mixer[0x0EU] & 0x02U) != 0,
            false, false, state->block_units);
        state->high_speed = *started;
        *refresh = *started;
        break;
    case 0x99:
        *started = dos_sb_begin_capture_locked(
            state, false, state->legacy_input_stereo,
            false, false, state->block_units);
        state->high_speed = *started;
        *refresh = *started;
        break;
    case 0x20:
        state->capture_commands++;
        dos_sb_response_push_locked(state, 0x80U);
        break;
    case 0x30:
        state->midi_mode = DOS_SB_MIDI_POLL_INPUT;
        break;
    case 0x31:
        state->midi_mode = state->midi_mode == DOS_SB_MIDI_IRQ_INPUT
            ? DOS_SB_MIDI_NONE : DOS_SB_MIDI_IRQ_INPUT;
        break;
    case 0x34:
        state->midi_mode = DOS_SB_MIDI_UART_POLL;
        break;
    case 0x35:
        state->midi_mode = DOS_SB_MIDI_UART_IRQ;
        break;
    case 0x36:
        state->midi_mode = DOS_SB_MIDI_UART_POLL_TIMESTAMP;
        break;
    case 0x37:
        state->midi_mode = DOS_SB_MIDI_UART_IRQ_TIMESTAMP;
        break;
    case 0x38:
        state->midi_output_bytes++;
        break;
    case 0x40:
        state->output_rate = p[0] == 0xFFU
            ? 1000000U : 1000000U / (256U - p[0]);
        state->input_rate = state->output_rate;
        state->output_rate_is_transfer_rate = true;
        state->input_rate_is_transfer_rate = true;
        break;
    case 0x41:
        state->output_rate = dos_sb_clamp_rate(
            ((uint32_t)p[0] << 8) | p[1]);
        state->output_rate_is_transfer_rate = false;
        break;
    case 0x42:
        state->input_rate = dos_sb_clamp_rate(
            ((uint32_t)p[0] << 8) | p[1]);
        state->input_rate_is_transfer_rate = false;
        break;
    case 0x48:
        state->block_units =
            ((uint32_t)p[1] << 8) | p[0];
        state->block_units += 1U;
        break;
    case 0x80: {
        uint32_t frames = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        *started = dos_sb_begin_silence_locked(state, frames + 1U);
        *refresh = *started;
        break;
    }
    case 0xA0:
        state->legacy_input_stereo = false;
        break;
    case 0xA8:
        state->legacy_input_stereo = true;
        break;
    case 0xD0:
        if ((state->playing || state->waiting_for_dma) && !state->sixteen_bit &&
            (state->transfer_mode == DOS_SB_TRANSFER_PLAYBACK ||
             state->transfer_mode == DOS_SB_TRANSFER_CAPTURE))
            state->paused = true;
        break;
    case 0xD5:
        if ((state->playing || state->waiting_for_dma) && state->sixteen_bit &&
            (state->transfer_mode == DOS_SB_TRANSFER_PLAYBACK ||
             state->transfer_mode == DOS_SB_TRANSFER_CAPTURE))
            state->paused = true;
        break;
    case 0xD1:
        state->speaker_enabled = true;
        break;
    case 0xD3:
        /* DSP 4.x keeps DAC output connected but clears the status flag. */
        state->speaker_enabled = false;
        break;
    case 0xD4:
        if ((state->playing || state->waiting_for_dma) && !state->sixteen_bit &&
            (state->transfer_mode == DOS_SB_TRANSFER_PLAYBACK ||
             state->transfer_mode == DOS_SB_TRANSFER_CAPTURE)) {
            state->paused = false;
            *refresh = true;
        }
        break;
    case 0xD6:
        if ((state->playing || state->waiting_for_dma) && state->sixteen_bit &&
            (state->transfer_mode == DOS_SB_TRANSFER_PLAYBACK ||
             state->transfer_mode == DOS_SB_TRANSFER_CAPTURE)) {
            state->paused = false;
            *refresh = true;
        }
        break;
    case 0xD8:
        dos_sb_response_push_locked(state,
            state->speaker_enabled ? 0xFFU : 0U);
        break;
    case 0xD9:
        if (state->auto_init && state->sixteen_bit)
            state->exit_auto_init = true;
        break;
    case 0xDA:
        if (state->auto_init && !state->sixteen_bit)
            state->exit_auto_init = true;
        break;
    case 0xE0:
        dos_sb_response_clear_locked(state);
        dos_sb_response_push_locked(state, (uint8_t)~p[0]);
        break;
    case 0xE1:
        dos_sb_response_clear_locked(state);
        dos_sb_response_push_locked(state, 4U);
        dos_sb_response_push_locked(state, 5U);
        break;
    case 0xE2:
        dos_sb_start_e2_dma_locked(state, p[0]);
        break;
    case 0xE3: {
        static const char copyright[] =
            "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
        dos_sb_response_clear_locked(state);
        for (uint32_t i = 0; i < sizeof(copyright); i++)
            dos_sb_response_push_locked(state, (uint8_t)copyright[i]);
        break;
    }
    case 0xE4:
        state->test_register = p[0];
        break;
    case 0xE8:
        dos_sb_response_clear_locked(state);
        dos_sb_response_push_locked(state, state->test_register);
        break;
    case 0xF2:
        state->sixteen_bit = false;
        dos_sb_raise_irq_locked(state);
        break;
    case 0xF3:
        state->sixteen_bit = true;
        dos_sb_raise_irq_locked(state);
        break;
    default:
        if ((command >= 0xB0U && command <= 0xBFU) ||
            (command >= 0xC0U && command <= 0xCFU)) {
            bool sixteen_bit = command < 0xC0U;
            bool input = (command & 0x08U) != 0;
            bool automatic = (command & 0x04U) != 0;
            bool stereo = (p[0] & 0x20U) != 0;
            bool signed_samples = (p[0] & 0x10U) != 0;
            uint32_t units = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
            if (!input) {
                *started = dos_sb_begin_playback_locked(
                    state, sixteen_bit, stereo, signed_samples, automatic,
                    units + 1U);
                *refresh = *started;
            } else {
                *started = dos_sb_begin_capture_locked(
                    state, sixteen_bit, stereo, signed_samples, automatic,
                    units + 1U);
                *refresh = *started;
            }
        } else {
            state->unknown_commands++;
            return false;
        }
        break;
    }
    return true;
}

static bool dos_sb_start_pending_dma_locked(DOS_AUDIO_STATE *state)
{
    if (!state->pending_dma_command)
        return false;

    uint8_t command = state->pending_command;
    uint8_t parameters[sizeof(state->pending_parameters)];
    memcpy(parameters, state->pending_parameters, sizeof(parameters));
    state->pending_dma_command = false;
    state->pending_command = 0;
    memset(state->pending_parameters, 0,
           sizeof(state->pending_parameters));
    memcpy(state->parameters, parameters, sizeof(parameters));

    bool refresh = false;
    bool started = false;
    return dos_sb_execute_locked(state, command, &refresh, &started) &&
           started;
}

static bool dos_sb_write_dsp_locked(DOS_AUDIO_STATE *state, uint8_t value,
                                    bool *refresh, bool *started)
{
    *refresh = false;
    *started = false;
    if (!state->present)
        return true;

    if (state->high_speed || state->pending_dma_command)
        return true;

    if (dos_sb_midi_uart_active(state)) {
        state->midi_output_bytes++;
        return true;
    }

    if (!state->parameter_expected) {
        state->command = value;
        state->parameter_count = 0;
        state->parameter_expected = dos_sb_parameter_count(value);
        if (!state->parameter_expected)
            return dos_sb_execute_locked(state, value, refresh, started);
        return true;
    }

    if (state->parameter_count < sizeof(state->parameters))
        state->parameters[state->parameter_count++] = value;
    if (state->parameter_count < state->parameter_expected)
        return true;

    uint8_t command = state->command;
    state->parameter_count = 0;
    state->parameter_expected = 0;
    return dos_sb_execute_locked(state, command, refresh, started);
}

static void dos_dma_write_word(uint16_t *target, uint16_t *base,
                               uint8_t *flip, uint8_t value)
{
    if (!*flip)
        *target = (uint16_t)((*target & 0xFF00U) | value);
    else
        *target = (uint16_t)((*target & 0x00FFU) |
                             ((uint16_t)value << 8));
    *base = *target;
    *flip ^= 1U;
}

static uint8_t dos_dma_read_word(uint16_t value, uint8_t *flip)
{
    uint8_t result = !*flip ? (uint8_t)value : (uint8_t)(value >> 8);
    *flip ^= 1U;
    return result;
}

static bool dos_dma_controller_disabled_locked(
    const DOS_AUDIO_STATE *state, bool sixteen_bit)
{
    uint8_t command = sixteen_bit
        ? state->dma_command16 : state->dma_command8;
    return (command & 0x04U) != 0;
}

static uint8_t dos_dma_status_read_locked(DOS_AUDIO_STATE *state,
                                          bool sixteen_bit)
{
    DOS_DMA_CHANNEL *channel = sixteen_bit
        ? &state->dma16 : &state->dma8;
    uint8_t dma_channel = sixteen_bit
        ? DOS_SB_DMA16_CHANNEL : DOS_SB_DMA8_CHANNEL;
    bool hardware_request = (state->playing || state->waiting_for_dma) &&
        !state->paused &&
        state->transfer_mode != DOS_SB_TRANSFER_SILENCE &&
        state->sixteen_bit == sixteen_bit;
    uint8_t status = channel->terminal_count
        ? DOS_DMA_TC_MASK(dma_channel) : 0U;
    if (channel->software_request || hardware_request)
        status |= DOS_DMA_REQUEST_MASK(dma_channel);
    channel->terminal_count = false;
    return status;
}

static void dos_dma_master_clear_locked(DOS_AUDIO_STATE *state,
                                        bool sixteen_bit)
{
    DOS_DMA_CHANNEL *channel = sixteen_bit
        ? &state->dma16 : &state->dma8;
    uint8_t *command = sixteen_bit
        ? &state->dma_command16 : &state->dma_command8;
    uint8_t *flip = sixteen_bit
        ? &state->dma_flip16 : &state->dma_flip8;
    *command = 0;
    *flip = 0;
    channel->masked = true;
    channel->terminal_count = false;
    channel->software_request = false;
}

bool dos_audio_port_read8(struct dos_vm *vm, uint16_t port, uint8_t *value)
{
    if (!vm || !value || !vm->audio)
        return false;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    bool handled = true;
    uint64_t flags = dos_audio_lock_irqsave(state);

    switch (port) {
    case 0x08:
        *value = dos_dma_status_read_locked(state, false);
        break;
    case DOS_SB_BASE + 0x04:
        *value = state->mixer_index;
        break;
    case DOS_SB_BASE + 0x05:
        if (state->mixer_index == 0x80U)
            *value = 0x02U;
        else if (state->mixer_index == 0x81U)
            *value = 0x22U;
        else if (state->mixer_index == 0x82U)
            *value = (uint8_t)__atomic_load_n(&state->irq_latched,
                                               __ATOMIC_ACQUIRE);
        else
            *value = state->mixer[state->mixer_index];
        break;
    case DOS_SB_BASE + 0x0A:
        *value = dos_sb_response_pop_locked(state);
        break;
    case DOS_SB_BASE + 0x0C:
        *value = state->present ? 0U : 0x80U;
        break;
    case DOS_SB_BASE + 0x0E:
        *value = state->response_count ? 0x80U : 0U;
        __atomic_fetch_and(&state->irq_latched, ~DOS_SB_IRQ_8BIT,
                           __ATOMIC_RELEASE);
        break;
    case DOS_SB_BASE + 0x0F:
        *value = 0xFFU;
        __atomic_fetch_and(&state->irq_latched, ~DOS_SB_IRQ_16BIT,
                           __ATOMIC_RELEASE);
        break;
    case DOS_DMA8_ADDRESS_PORT(DOS_SB_DMA8_CHANNEL):
        *value = dos_dma_read_word(state->dma8.address,
                                   &state->dma_flip8);
        break;
    case DOS_DMA8_COUNT_PORT(DOS_SB_DMA8_CHANNEL):
        *value = dos_dma_read_word(state->dma8.count,
                                   &state->dma_flip8);
        break;
    case DOS_DMA_PAGE_PORT(DOS_SB_DMA8_CHANNEL):
        *value = state->dma8.page;
        break;
    case 0xD0:
        *value = dos_dma_status_read_locked(state, true);
        break;
    case DOS_DMA16_ADDRESS_PORT(DOS_SB_DMA16_CHANNEL):
        *value = dos_dma_read_word(state->dma16.address,
                                   &state->dma_flip16);
        break;
    case DOS_DMA16_COUNT_PORT(DOS_SB_DMA16_CHANNEL):
        *value = dos_dma_read_word(state->dma16.count,
                                   &state->dma_flip16);
        break;
    case DOS_DMA_PAGE_PORT(DOS_SB_DMA16_CHANNEL):
        *value = state->dma16.page;
        break;
    default:
        handled = port >= DOS_SB_BASE && port <= DOS_SB_BASE + 0x0FU;
        if (handled)
            *value = 0xFFU;
        break;
    }
    dos_audio_unlock_irqrestore(state, flags);
    return handled;
}

bool dos_audio_port_write8(struct dos_vm *vm, uint16_t port, uint8_t value)
{
    if (!vm || !vm->audio)
        return false;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    bool handled = true;
    bool refresh = false;
    bool started = false;
    bool known_command = true;
    uint8_t reported_command = value;
    uint64_t flags = dos_audio_lock_irqsave(state);
    uint32_t playback_errors = state->playback_errors;
    uint32_t capture_errors = state->capture_errors;
    uint32_t capture_commands = state->capture_commands;
    uint64_t midi_output_bytes = state->midi_output_bytes;
    bool report_midi_backend = false;

    switch (port) {
    case DOS_SB_BASE + 0x04:
        state->mixer_index = value;
        break;
    case DOS_SB_BASE + 0x05:
        dos_sb_mixer_write_locked(state, state->mixer_index, value);
        break;
    case DOS_SB_BASE + 0x06:
        if (value & 1U) {
            state->reset_asserted = true;
            dos_sb_parser_reset_locked(state);
            refresh = true;
        } else if (state->reset_asserted) {
            state->reset_asserted = false;
            dos_sb_parser_reset_locked(state);
            if (state->present)
                dos_sb_response_push_locked(state, 0xAAU);
            refresh = true;
        }
        break;
    case DOS_SB_BASE + 0x0C:
        if (state->parameter_expected)
            reported_command = state->command;
        known_command = dos_sb_write_dsp_locked(
            state, value, &refresh, &started);
        break;
    case 0x08:
        state->dma_command8 = value;
        dos_sb_try_e2_dma_locked(state);
        refresh = state->playing && !state->sixteen_bit;
        break;
    case 0x09:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL))
            state->dma8.software_request = (value & 4U) != 0;
        break;
    case DOS_DMA8_ADDRESS_PORT(DOS_SB_DMA8_CHANNEL):
        dos_dma_write_word(&state->dma8.address, &state->dma8.base_address,
                           &state->dma_flip8, value);
        break;
    case DOS_DMA8_COUNT_PORT(DOS_SB_DMA8_CHANNEL):
        dos_dma_write_word(&state->dma8.count, &state->dma8.base_count,
                           &state->dma_flip8, value);
        break;
    case 0x0A:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL)) {
            state->dma8.masked = (value & 4U) != 0;
            dos_sb_try_e2_dma_locked(state);
            refresh = state->playing;
        }
        break;
    case 0x0B:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL)) {
            state->dma8.mode = value;
            dos_sb_try_e2_dma_locked(state);
        }
        break;
    case 0x0C:
        state->dma_flip8 = 0;
        break;
    case 0x0D:
        dos_dma_master_clear_locked(state, false);
        refresh = state->playing && !state->sixteen_bit;
        break;
    case 0x0E:
        state->dma8.masked = false;
        dos_sb_try_e2_dma_locked(state);
        refresh = state->playing && !state->sixteen_bit;
        break;
    case 0x0F:
        state->dma8.masked =
            (value & DOS_DMA_TC_MASK(DOS_SB_DMA8_CHANNEL)) != 0;
        dos_sb_try_e2_dma_locked(state);
        refresh = state->playing && !state->sixteen_bit;
        break;
    case DOS_DMA_PAGE_PORT(DOS_SB_DMA8_CHANNEL):
        state->dma8.page = value;
        break;
    case 0xD0:
        state->dma_command16 = value;
        refresh = state->playing && state->sixteen_bit;
        break;
    case 0xD2:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA16_CHANNEL))
            state->dma16.software_request = (value & 4U) != 0;
        break;
    case DOS_DMA16_ADDRESS_PORT(DOS_SB_DMA16_CHANNEL):
        dos_dma_write_word(&state->dma16.address,
                           &state->dma16.base_address,
                           &state->dma_flip16, value);
        break;
    case DOS_DMA16_COUNT_PORT(DOS_SB_DMA16_CHANNEL):
        dos_dma_write_word(&state->dma16.count,
                           &state->dma16.base_count,
                           &state->dma_flip16, value);
        break;
    case 0xD4:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA16_CHANNEL)) {
            state->dma16.masked = (value & 4U) != 0;
            refresh = state->playing;
        }
        break;
    case 0xD6:
        if ((value & 3U) == DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA16_CHANNEL))
            state->dma16.mode = value;
        break;
    case 0xD8:
        state->dma_flip16 = 0;
        break;
    case 0xDA:
        dos_dma_master_clear_locked(state, true);
        refresh = state->playing && state->sixteen_bit;
        break;
    case 0xDC:
        state->dma16.masked = false;
        refresh = state->playing && state->sixteen_bit;
        break;
    case 0xDE:
        state->dma16.masked =
            (value & DOS_DMA_TC_MASK(DOS_SB_DMA16_CHANNEL)) != 0;
        refresh = state->playing && state->sixteen_bit;
        break;
    case DOS_DMA_PAGE_PORT(DOS_SB_DMA16_CHANNEL):
        state->dma16.page = value;
        break;
    default:
        handled = port >= DOS_SB_BASE && port <= DOS_SB_BASE + 0x0FU;
        break;
    }
    if (state->waiting_for_dma && !state->paused &&
        !(state->sixteen_bit ? state->dma16.masked : state->dma8.masked) &&
        !dos_dma_controller_disabled_locked(state, state->sixteen_bit)) {
        /* Keep a partially received DSP command intact when DMA is enabled
         * between its parameter bytes. The pending request owns its payload. */
        uint8_t saved_parameters[sizeof(state->parameters)];
        memcpy(saved_parameters, state->parameters, sizeof(saved_parameters));
        memcpy(state->parameters, state->waiting_parameters,
               sizeof(state->waiting_parameters));
        uint8_t command = state->waiting_command;
        bool exit_auto_init = state->exit_auto_init;
        state->waiting_for_dma = false;
        state->auto_init = false;
        bool dma_refresh = false, dma_started = false;
        (void)dos_sb_execute_locked(state, command, &dma_refresh, &dma_started);
        memcpy(state->parameters, saved_parameters, sizeof(saved_parameters));
        if (dma_started) state->exit_auto_init = exit_auto_init;
        refresh |= dma_refresh;
        started |= dma_started;
        reported_command = command;
    }
    if (!state->test_mode &&
        state->midi_output_bytes != midi_output_bytes &&
        !state->midi_backend_warned) {
        state->midi_backend_warned = true;
        report_midi_backend = true;
    }
    dos_audio_unlock_irqrestore(state, flags);

    if (!known_command && state->unknown_commands <= 16U) {
        serial_puts("[DOS-AUDIO] unsupported DSP command ");
        serial_puthex(reported_command, 2);
        serial_puts("\n");
    }
    if (state->playback_errors != playback_errors) {
        serial_puts("[DOS-AUDIO] playback rejected for DSP command ");
        serial_puthex(reported_command, 2);
        serial_puts(": invalid DMA buffer or format\n");
    }
    if (state->capture_errors != capture_errors) {
        serial_puts("[DOS-AUDIO] capture rejected for DSP command ");
        serial_puthex(reported_command, 2);
        serial_puts(": invalid DMA buffer or format\n");
    }
    if (state->capture_commands != capture_commands) {
        serial_puts("[DOS-AUDIO] capture uses silent input fallback for DSP command ");
        serial_puthex(reported_command, 2);
        serial_puts("\n");
    }
    if (report_midi_backend)
        serial_puts("[DOS-AUDIO] MIDI output accepted; no backend connected\n");
    if (started) {
        if (state->transfer_mode == DOS_SB_TRANSFER_DIRECT_DAC) {
            serial_puts("[DOS-AUDIO] direct DAC output (guest-timed)\n");
        } else {
            serial_puts(state->transfer_mode == DOS_SB_TRANSFER_CAPTURE
                            ? "[DOS-AUDIO] capture "
                            : state->transfer_mode == DOS_SB_TRANSFER_SILENCE
                                ? "[DOS-AUDIO] silence "
                                : "[DOS-AUDIO] playback ");
            serial_putdec(state->sample_rate);
            serial_puts(" Hz ");
            if (state->encoding != DOS_SB_ENCODING_PCM) {
                serial_puts("Creative ");
                serial_putdec(state->encoding == DOS_SB_ENCODING_ADPCM_2
                                  ? 2U
                                  : state->encoding == DOS_SB_ENCODING_ADPCM_3
                                      ? 3U : 4U);
                serial_puts("-bit ADPCM (8-bit mono)\n");
            } else {
                serial_putdec(state->sixteen_bit ? 16U : 8U);
                serial_puts(state->format.channels == 2U
                                ? "-bit stereo\n" : "-bit mono\n");
            }
        }
    }
    if (refresh)
        dos_audio_refresh(state);
    return handled;
}

bool dos_audio_take_irq(struct dos_vm *vm, uint8_t *irq,
                        uint32_t *pending_mask)
{
    if (!vm || !irq || !pending_mask || !vm->audio)
        return false;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    uint32_t pending = __atomic_exchange_n(&state->irq_pending, 0,
                                            __ATOMIC_ACQ_REL);
    if (!pending)
        return false;
    *irq = DOS_SB_IRQ_LINE;
    *pending_mask = pending;
    return true;
}

void dos_audio_restore_irq(struct dos_vm *vm, uint32_t pending_mask)
{
    if (!vm || !vm->audio || !pending_mask)
        return;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    __atomic_fetch_or(&state->irq_pending, pending_mask, __ATOMIC_RELEASE);
}

bool dos_audio_available(const struct dos_vm *vm)
{
    if (!vm || !vm->audio)
        return false;
    const DOS_AUDIO_STATE *state = (const DOS_AUDIO_STATE *)vm->audio;
    return state->present;
}

bool dos_audio_get_resources(const struct dos_vm *vm,
                             dos_audio_resources_t *resources)
{
    if (!resources || !dos_audio_available(vm))
        return false;

    resources->base_port = DOS_SB_BASE;
    resources->irq = DOS_SB_IRQ_LINE;
    resources->dma8 = DOS_SB_DMA8_CHANNEL;
    resources->dma16 = DOS_SB_DMA16_CHANNEL;
    resources->card_type = DOS_SB_CARD_TYPE;
    return true;
}

bool dos_audio_init(struct dos_vm *vm)
{
    if (!vm || vm->audio)
        return false;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)kmalloc(sizeof(*state));
    if (!state)
        return false;
    memset(state, 0, sizeof(*state));
    state->guest_memory = vm->mem;
    state->guest_memory_size = vm->total_mem_size;
    state->present = audio_output_is_ready();
    state->sample_rate = 22050U;
    state->output_rate = 22050U;
    state->input_rate = 22050U;
    state->speaker_enabled = false;
    state->direct_dac_level = 0x80U;
    state->e2_value = 0xAA;
    state->dma8.masked = true;
    state->dma16.masked = true;
    dos_sb_mixer_reset_locked(state);
    vm->audio = state;

    if (state->present) {
        serial_puts("[DOS-AUDIO] Sound Blaster 16 base=");
        serial_puthex(DOS_SB_BASE, 3);
        serial_puts(" irq=");
        serial_putdec(DOS_SB_IRQ_LINE);
        serial_puts(" dma8=");
        serial_putdec(DOS_SB_DMA8_CHANNEL);
        serial_puts(" dma16=");
        serial_putdec(DOS_SB_DMA16_CHANNEL);
        serial_puts(" -> ");
        serial_puts(audio_output_backend_name());
        serial_puts("\n");
    } else {
        serial_puts("[DOS-AUDIO] disabled: no output backend\n");
    }
    return state->present;
}

void dos_audio_shutdown(struct dos_vm *vm)
{
    if (!vm || !vm->audio)
        return;
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    dos_audio_transition_acquire(state);
    uint64_t flags = dos_audio_lock_irqsave(state);
    state->present = false;
    state->playing = false;
    state->paused = false;
    state->auto_init = false;
    state->exit_auto_init = false;
    state->high_speed = false;
    state->pending_dma_command = false;
    state->transfer_mode = DOS_SB_TRANSFER_NONE;
    audio_source_id_t source = state->source;
    state->source = AUDIO_SOURCE_INVALID;
    dos_audio_unlock_irqrestore(state, flags);
    if (source != AUDIO_SOURCE_INVALID)
        audio_source_unregister(source);
    dos_audio_transition_release(state);
    vm->audio = NULL;
    kfree(state);
}

static int dos_audio_dma_wait_selftest(dos_vm_t *vm)
{
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)vm->audio;
    uint8_t *memory = state->guest_memory;
    int failures = 0;
    unsigned checks = 0;
#define DMA_WAIT_CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        serial_puts("[DOS-AUDIO-TEST] DMA wait failure line "); \
        serial_putdec(__LINE__); \
        serial_puts("\n"); \
    } \
} while (0)

    for (unsigned wide = 0; wide < 2; wide++) {
        for (unsigned capture = 0; capture < 2; capture++) {
            for (unsigned gate = 0; gate < 3; gate++) {
                dos_sb_parser_reset_locked(state);
                DOS_DMA_CHANNEL *channel = wide ? &state->dma16 : &state->dma8;
                *channel = (DOS_DMA_CHANNEL){ .address = 0xFFFFU,
                    .count = 0xFFFFU, .masked = gate != 1U };
                state->dma_command8 = state->dma_command16 = gate ? 4U : 0U;
                state->output_rate = state->input_rate = AUDIO_OUTPUT_RATE_HZ;
                state->output_rate_is_transfer_rate = false;
                state->input_rate_is_transfer_rate = false;
                uint8_t command = (wide ? 0xB0U : 0xC0U) | (capture ? 8U : 0U);
                uint8_t count = wide ? 1U : 3U;
                uint32_t errors = state->playback_errors + state->capture_errors;
                memset(memory + 0x1000U, 0x5A, 4U);
                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, command);
                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, count);
                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
                DMA_WAIT_CHECK(state->waiting_for_dma && !state->playing &&
                    !state->irq_pending && !state->irq_latched &&
                    state->playback_errors + state->capture_errors == errors);
                int16_t output[8];
                DMA_WAIT_CHECK(!dos_audio_fill(state, output, 4U) &&
                    memory[0x1000] == 0x5AU && memory[0x1003] == 0x5AU);
                uint8_t status = 0;
                dos_audio_port_read8(vm, wide ? 0xD0U : 0x08U, &status);
                DMA_WAIT_CHECK(status & 0x20U);

                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C,
                                      wide ? 0xD5U : 0xD0U);
                dos_audio_port_write8(vm, wide ? 0xD8U : 0x0CU, 0U);
                uint16_t address_port = wide ? 0xC4U : 0x02U;
                uint16_t count_port = wide ? 0xC6U : 0x03U;
                dos_audio_port_write8(vm, address_port, 0U);
                dos_audio_port_write8(vm, address_port, wide ? 8U : 16U);
                dos_audio_port_write8(vm, count_port, count);
                dos_audio_port_write8(vm, count_port, 0U);
                dos_audio_port_write8(vm, wide ? 0x8BU : 0x83U, 0U);
                dos_audio_port_write8(vm, wide ? 0xD6U : 0x0BU,
                                      capture ? 0x45U : 0x49U);
                dos_audio_port_write8(vm, wide ? 0xD4U : 0x0AU, 1U);
                dos_audio_port_write8(vm, wide ? 0xD0U : 0x08U, 0U);
                DMA_WAIT_CHECK(state->waiting_for_dma && state->paused &&
                    !state->playing && !state->irq_pending &&
                    memory[0x1000] == 0x5AU && memory[0x1003] == 0x5AU);
                dos_audio_port_read8(vm, wide ? 0xD0U : 0x08U, &status);
                DMA_WAIT_CHECK(!(status & 0x20U));
                dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C,
                                      wide ? 0xD6U : 0xD4U);
                DMA_WAIT_CHECK(!state->waiting_for_dma && state->playing &&
                    !state->paused && !state->irq_pending &&
                    state->dma_base == 0x1000U && state->block_bytes == 4U);
                DMA_WAIT_CHECK(memory[0x1000] == (capture ? (wide ? 0U : 0x80U) : 0x5AU) &&
                    memory[0x1003] == (capture ? 0x80U : 0x5AU));
                DMA_WAIT_CHECK(dos_audio_fill(state, output, wide ? 2U : 4U) &&
                    !state->playing && state->irq_pending == (wide ? 2U : 1U) &&
                    state->playback_errors + state->capture_errors == errors);
            }
        }
    }

    dos_sb_parser_reset_locked(state);
    state->dma8 = (DOS_DMA_CHANNEL){ .address = 0x1000U,
        .count = 3U, .base_address = 0x1000U, .base_count = 3U,
        .masked = true, .mode = 0x59U };
    state->dma_command8 = 0;
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x14U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 3U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
    /* DMA enable can arrive between the two bytes of a new rate command. */
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x41U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x12U);
    dos_audio_port_write8(vm, 0x0AU, 1U);
    DMA_WAIT_CHECK(state->playing && state->block_bytes == 4U &&
        state->command == 0x41U && state->parameter_expected == 2U &&
        state->parameter_count == 1U && state->parameters[0] == 0x12U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x34U);
    DMA_WAIT_CHECK(state->output_rate == 0x1234U && !state->parameter_expected);

    /* A queued single-cycle command still follows a waiting auto-init block. */
    dos_sb_parser_reset_locked(state);
    state->dma8.masked = true;
    state->block_units = 2U;
    state->output_rate = AUDIO_OUTPUT_RATE_HZ;
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x1CU);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x14U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
    DMA_WAIT_CHECK(state->waiting_for_dma && state->pending_dma_command &&
        state->exit_auto_init);
    dos_audio_port_write8(vm, 0x0AU, 1U);
    int16_t output[8];
    DMA_WAIT_CHECK(state->playing && state->auto_init && state->exit_auto_init &&
        dos_audio_fill(state, output, 2U) && state->playing &&
        !state->auto_init && !state->pending_dma_command &&
        state->block_bytes == 1U);

    /* ADPCM waits too; reset must cancel it before a later DMA enable. */
    dos_sb_parser_reset_locked(state);
    state->dma8.masked = true;
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0x75U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 1U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x0C, 0U);
    DMA_WAIT_CHECK(state->waiting_for_dma && !state->playing);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x06, 1U);
    dos_audio_port_write8(vm, DOS_SB_BASE + 0x06, 0U);
    dos_audio_port_write8(vm, 0x0AU, 1U);
    DMA_WAIT_CHECK(!state->waiting_for_dma && !state->playing &&
        !state->pending_dma_command && !state->irq_pending);
#undef DMA_WAIT_CHECK
    serial_puts("[DOS-AUDIO-TEST] DMA wait checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec(failures);
    serial_puts("\n");
    return failures;
}

int dos_audio_selftest(void)
{
    const uint64_t pages = 2;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    DOS_AUDIO_STATE *state = (DOS_AUDIO_STATE *)kmalloc(sizeof(*state));
    if (!memory || !state) {
        if (memory)
            dos_host_free_pages(memory, pages);
        if (state)
            kfree(state);
        return 1;
    }

    memset(memory, 0, pages * 4096U);
    memset(state, 0, sizeof(*state));
    dos_vm_t vm = {0};
    vm.mem = memory;
    vm.total_mem_size = (uint32_t)(pages * 4096U);
    vm.audio = state;
    state->guest_memory = memory;
    state->guest_memory_size = vm.total_mem_size;
    state->present = true;
    state->test_mode = true;
    state->sample_rate = 22050U;
    state->output_rate = 22050U;
    state->input_rate = 22050U;
    state->speaker_enabled = false;
    state->direct_dac_level = 0x80U;
    state->e2_value = 0xAA;
    state->dma8.masked = true;
    state->dma16.masked = true;
    dos_sb_mixer_reset_locked(state);

    int failures = 0;
    dos_audio_resources_t resources;
    if (!dos_audio_get_resources(&vm, &resources) ||
        resources.base_port != DOS_SB_BASE ||
        resources.irq != DOS_SB_IRQ_LINE ||
        resources.dma8 != DOS_SB_DMA8_CHANNEL ||
        resources.dma16 != DOS_SB_DMA16_CHANNEL ||
        resources.card_type != DOS_SB_CARD_TYPE)
        failures++;

    DOS_DMA_CHANNEL dma_address_test = {
        .address = 0x9A2BU,
        .page = 0x13U,
    };
    if (dos_dma_physical_address(&dma_address_test, true) != 0x133456U)
        failures++;
    dma_address_test.address = 0x3456U;
    if (dos_dma_physical_address(&dma_address_test, false) != 0x133456U)
        failures++;

    uint16_t dma_current_test = 0;
    uint16_t dma_base_test = 0;
    uint8_t dma_flip_test = 0;
    dos_dma_write_word(&dma_current_test, &dma_base_test,
                       &dma_flip_test, 0x34U);
    dos_dma_write_word(&dma_current_test, &dma_base_test,
                       &dma_flip_test, 0x12U);
    if (dma_current_test != 0x1234U || dma_base_test != 0x1234U ||
        dma_flip_test)
        failures++;

    state->sixteen_bit = false;
    state->dma_start_address = 0x1000U;
    state->dma_start_count = 3U;
    state->dma8.address = state->dma_start_address;
    state->dma8.count = state->dma_start_count;
    state->dma8.mode = 0U;
    dos_sb_sync_dma_progress_locked(state, 2U);
    if (state->dma8.address != 0x1002U || state->dma8.count != 1U)
        failures++;
    dos_sb_sync_dma_progress_locked(state, 4U);
    if (state->dma8.address != 0x1004U ||
        state->dma8.count != 0xFFFFU ||
        !state->dma8.terminal_count)
        failures++;

    uint8_t dma_status = 0;
    if (!dos_audio_port_read8(&vm, 0x08U, &dma_status) ||
        !(dma_status & DOS_DMA_TC_MASK(DOS_SB_DMA8_CHANNEL)) ||
        state->dma8.terminal_count ||
        !dos_audio_port_read8(&vm, 0x08U, &dma_status) ||
        (dma_status & DOS_DMA_TC_MASK(DOS_SB_DMA8_CHANNEL)))
        failures++;

    dos_audio_port_write8(
        &vm, 0x09U,
        0x04U | DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL));
    if (!dos_audio_port_read8(&vm, 0x08U, &dma_status) ||
        !(dma_status & DOS_DMA_REQUEST_MASK(DOS_SB_DMA8_CHANNEL)) ||
        !dos_audio_port_read8(&vm, 0x08U, &dma_status) ||
        !(dma_status & DOS_DMA_REQUEST_MASK(DOS_SB_DMA8_CHANNEL)))
        failures++;
    dos_audio_port_write8(
        &vm, 0x09U, DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL));
    if (!dos_audio_port_read8(&vm, 0x08U, &dma_status) ||
        (dma_status & DOS_DMA_REQUEST_MASK(DOS_SB_DMA8_CHANNEL)))
        failures++;

    state->dma_start_address = 0x2000U;
    state->dma_start_count = 3U;
    state->dma8.address = state->dma_start_address;
    state->dma8.count = state->dma_start_count;
    state->dma8.base_address = 0x1800U;
    state->dma8.base_count = 7U;
    state->dma8.mode = 0x10U;
    dos_sb_sync_dma_progress_locked(state, 4U);
    if (state->dma8.address != 0x1800U || state->dma8.count != 7U ||
        !state->dma8.terminal_count)
        failures++;
    dos_sb_sync_dma_progress_locked(state, 6U);
    if (state->dma8.address != 0x1802U || state->dma8.count != 5U)
        failures++;

    state->dma_start_address = 0x2004U;
    state->dma_start_count = 3U;
    state->dma8.address = state->dma_start_address;
    state->dma8.count = state->dma_start_count;
    state->dma8.mode = 0x20U;
    dos_sb_sync_dma_progress_locked(state, 2U);
    if (state->dma8.address != 0x2002U || state->dma8.count != 1U)
        failures++;
    dos_sb_sync_dma_progress_locked(state, 4U);
    if (state->dma8.address != 0x2000U ||
        state->dma8.count != 0xFFFFU)
        failures++;

    state->sixteen_bit = true;
    state->dma_start_address = 0x0800U;
    state->dma_start_count = 1U;
    state->dma16.address = state->dma_start_address;
    state->dma16.count = state->dma_start_count;
    state->dma16.mode = 0U;
    dos_sb_sync_dma_progress_locked(state, 2U);
    if (state->dma16.address != 0x0801U || state->dma16.count != 0U)
        failures++;

    state->dma8.address = 0x3456U;
    state->dma8.count = 0x1234U;
    state->dma8.mode = 0x49U;
    state->dma8.terminal_count = true;
    state->dma8.software_request = true;
    state->dma_flip8 = 1U;
    dos_audio_port_write8(&vm, 0x08U, 0x04U);
    if (!dos_dma_controller_disabled_locked(state, false))
        failures++;
    dos_audio_port_write8(&vm, 0x0DU, 0U);
    if (state->dma_command8 || state->dma_flip8 || !state->dma8.masked ||
        state->dma8.terminal_count || state->dma8.software_request ||
        state->dma8.address != 0x3456U || state->dma8.count != 0x1234U ||
        state->dma8.mode != 0x49U)
        failures++;
    dos_audio_port_write8(&vm, 0x0EU, 0U);
    if (state->dma8.masked)
        failures++;
    dos_audio_port_write8(&vm, 0x0FU,
                          DOS_DMA_TC_MASK(DOS_SB_DMA8_CHANNEL));
    if (!state->dma8.masked)
        failures++;
    dos_audio_port_write8(&vm, 0x0FU, 0U);
    if (state->dma8.masked)
        failures++;

    state->dma16.terminal_count = true;
    state->dma16.software_request = true;
    state->dma_flip16 = 1U;
    dos_audio_port_write8(&vm, 0xD0U, 0x04U);
    if (!dos_dma_controller_disabled_locked(state, true))
        failures++;
    dos_audio_port_write8(&vm, 0xDAU, 0U);
    if (state->dma_command16 || state->dma_flip16 ||
        !state->dma16.masked || state->dma16.terminal_count ||
        state->dma16.software_request)
        failures++;
    dos_audio_port_write8(&vm, 0xDCU, 0U);
    if (state->dma16.masked)
        failures++;
    dos_audio_port_write8(&vm, 0xDEU,
                          DOS_DMA_TC_MASK(DOS_SB_DMA16_CHANNEL));
    if (!state->dma16.masked)
        failures++;

    memset(&state->dma8, 0, sizeof(state->dma8));
    memset(&state->dma16, 0, sizeof(state->dma16));
    state->dma8.masked = true;
    state->dma16.masked = true;
    state->sixteen_bit = false;
    state->dma_start_address = 0;
    state->dma_start_count = 0;

    uint8_t value = 0;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 1U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 0U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0E, &value) ||
        !(value & 0x80U) ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0xAAU)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE1U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 4U ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 5U)
        failures++;

    static const char expected_copyright[] =
        "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE3U);
    for (uint32_t i = 0; i < sizeof(expected_copyright); i++) {
        if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
            value != (uint8_t)expected_copyright[i]) {
            failures++;
            break;
        }
    }

    memory[0x1100] = 0;
    memory[0x1101] = 0;
    state->dma8.address = 0x1100U;
    state->dma8.count = 0;
    state->dma8.base_address = 0x1100U;
    state->dma8.base_count = 0;
    state->dma8.page = 0;
    state->dma8.mode = 0x45U;
    state->dma8.masked = true;
    state->dma8.terminal_count = false;
    state->dma_command8 = 0;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE2U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    if (!state->e2_dma_pending || memory[0x1100] != 0U ||
        state->e2_value != 0x40 || state->e2_count != 1U)
        failures++;
    dos_audio_port_write8(&vm, 0x0AU, 0x01U);
    if (state->e2_dma_pending || memory[0x1100] != 0x40U ||
        state->dma8.address != 0x1101U ||
        state->dma8.count != 0xFFFFU ||
        !state->dma8.terminal_count || state->irq_pending)
        failures++;

    state->dma8.count = 0;
    state->dma8.base_address = state->dma8.address;
    state->dma8.base_count = 0;
    state->dma8.terminal_count = false;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE2U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    if (state->e2_dma_pending || memory[0x1101] != 0xE5U ||
        state->e2_value != 0xE5 || state->e2_count != 2U ||
        state->dma8.address != 0x1102U ||
        state->dma8.count != 0xFFFFU ||
        !state->dma8.terminal_count || state->irq_pending)
        failures++;
    state->dma8.masked = true;
    state->dma8.terminal_count = false;

    uint32_t unknown_before_midi = state->unknown_commands;
    uint64_t midi_bytes_before = state->midi_output_bytes;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x38U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x90U);
    if (state->midi_output_bytes != midi_bytes_before + 1U ||
        state->unknown_commands != unknown_before_midi ||
        state->midi_mode != DOS_SB_MIDI_NONE)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x31U);
    if (state->midi_mode != DOS_SB_MIDI_IRQ_INPUT)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x31U);
    if (state->midi_mode != DOS_SB_MIDI_NONE)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x30U);
    if (state->midi_mode != DOS_SB_MIDI_POLL_INPUT)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x34U);
    if (state->midi_mode != DOS_SB_MIDI_UART_POLL)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x90U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x40U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x7FU);
    if (state->midi_output_bytes != midi_bytes_before + 4U ||
        state->parameter_expected || state->playing ||
        state->unknown_commands != unknown_before_midi ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0E, &value) ||
        value != 0U)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 1U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 0U);
    if (state->midi_mode != DOS_SB_MIDI_NONE ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0xAAU)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD8U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0U || state->speaker_enabled ||
        dos_sb_volume_gain_locked(state, false) != PCM_GAIN_UNITY)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD1U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD8U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0xFFU || !state->speaker_enabled)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD3U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD8U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0U || state->speaker_enabled ||
        dos_sb_volume_gain_locked(state, false) != PCM_GAIN_UNITY)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x04, 0x22U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x05, 0xF0U);
    if (state->mixer[0x30] != 0xF8U || state->mixer[0x31] != 0U ||
        dos_sb_volume_gain_locked(state, false) != PCM_GAIN_UNITY ||
        dos_sb_volume_gain_locked(state, true) != 0U)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x04, 0x31U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x05, 0xF8U);
    if (state->mixer[0x22] != 0xFFU)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x04, 0x00U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x05, 0U);
    if (state->mixer[0x04] != 0xFFU || state->mixer[0x22] != 0xFFU ||
        state->mixer[0x30] != 0xF8U || state->mixer[0x33] != 0xF8U)
        failures++;

    if (dos_sb_parameter_count(0x04U) != 1U ||
        dos_sb_parameter_count(0x05U) != 2U ||
        dos_sb_parameter_count(0x08U) != 1U ||
        dos_sb_parameter_count(0x0EU) != 2U ||
        dos_sb_parameter_count(0x0FU) != 1U ||
        dos_sb_parameter_count(0x10U) != 1U ||
        dos_sb_parameter_count(0x15U) != 2U ||
        dos_sb_parameter_count(0x16U) != 2U ||
        dos_sb_parameter_count(0x24U) != 2U ||
        dos_sb_parameter_count(0x32U) != 1U ||
        dos_sb_parameter_count(0x74U) != 2U ||
        dos_sb_parameter_count(0x80U) != 2U ||
        dos_sb_parameter_count(0xF9U) != 1U)
        failures++;

    dos_sb_response_clear_locked(state);
    uint32_t unknown_before_optional = state->unknown_commands;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x05U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE1U);
    if (state->command != 0x05U || state->parameter_expected != 2U ||
        state->parameter_count != 1U || state->response_count != 0U ||
        state->unknown_commands != unknown_before_optional)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x77U);
    if (state->parameter_expected != 0U || state->parameter_count != 0U ||
        state->response_count != 0U ||
        state->unknown_commands != unknown_before_optional + 1U)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE1U);
    if (state->response_count != 2U ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 4U ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 5U)
        failures++;

    uint32_t unknown_before_direct = state->unknown_commands;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x10U);
    if (state->parameter_expected != 1U || state->playing)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x80U);
    if (!state->playing ||
        state->transfer_mode != DOS_SB_TRANSFER_DIRECT_DAC ||
        state->unknown_commands != unknown_before_direct)
        failures++;
    dos_sb_parser_reset_locked(state);

    const uint64_t direct_start_ns = 1000000000ULL;
    if (!dos_sb_write_direct_dac_locked(
            state, 0x40U, direct_start_ns) ||
        dos_sb_write_direct_dac_locked(
            state, 0xC0U, direct_start_ns + 5000000ULL) ||
        state->direct_dac_count != 1U)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD0U);
    if (state->paused)
        failures++;

    memset(state->output, 0x7F, sizeof(state->output));
    if (!dos_audio_fill(state, state->output,
                        AUDIO_OUTPUT_BLOCK_FRAMES) ||
        state->output[0] != 0 ||
        state->output[AUDIO_OUTPUT_BLOCK_FRAMES * 2U - 1U] != 0)
        failures++;
    memset(state->output, 0, sizeof(state->output));
    if (!dos_audio_fill(state, state->output,
                        AUDIO_OUTPUT_BLOCK_FRAMES) ||
        state->output[0] != -16384 ||
        state->output[239U * 2U] != -16384 ||
        state->output[240U * 2U] != 16384 ||
        state->output[479U * 2U] != 16384 ||
        state->direct_dac_count != 0U ||
        state->direct_dac_overruns != 0U)
        failures++;
    dos_sb_parser_reset_locked(state);

    uint8_t adpcm_samples[4];
    state->encoding = DOS_SB_ENCODING_ADPCM_2;
    state->adpcm_reference = 128U;
    state->adpcm_step = 0;
    adpcm_samples[0] = dos_sb_decode_adpcm_portion_locked(state, 1U);
    adpcm_samples[1] = dos_sb_decode_adpcm_portion_locked(state, 1U);
    adpcm_samples[2] = dos_sb_decode_adpcm_portion_locked(state, 2U);
    adpcm_samples[3] = dos_sb_decode_adpcm_portion_locked(state, 3U);
    if (adpcm_samples[0] != 129U || adpcm_samples[1] != 132U ||
        adpcm_samples[2] != 130U || adpcm_samples[3] != 127U ||
        state->adpcm_step != 8U)
        failures++;

    state->encoding = DOS_SB_ENCODING_ADPCM_3;
    state->adpcm_reference = 128U;
    state->adpcm_step = 0;
    adpcm_samples[0] = dos_sb_decode_adpcm_portion_locked(state, 3U);
    adpcm_samples[1] = dos_sb_decode_adpcm_portion_locked(state, 3U);
    adpcm_samples[2] = dos_sb_decode_adpcm_portion_locked(state, 4U);
    adpcm_samples[3] = dos_sb_decode_adpcm_portion_locked(state, 7U);
    if (adpcm_samples[0] != 131U || adpcm_samples[1] != 138U ||
        adpcm_samples[2] != 136U || adpcm_samples[3] != 129U ||
        state->adpcm_step != 16U)
        failures++;

    state->encoding = DOS_SB_ENCODING_ADPCM_4;
    state->adpcm_reference = 128U;
    state->adpcm_step = 0;
    adpcm_samples[0] = dos_sb_decode_adpcm_portion_locked(state, 1U);
    adpcm_samples[1] = dos_sb_decode_adpcm_portion_locked(state, 7U);
    adpcm_samples[2] = dos_sb_decode_adpcm_portion_locked(state, 8U);
    adpcm_samples[3] = dos_sb_decode_adpcm_portion_locked(state, 15U);
    if (adpcm_samples[0] != 129U || adpcm_samples[1] != 136U ||
        adpcm_samples[2] != 135U || adpcm_samples[3] != 128U ||
        state->adpcm_step != 16U)
        failures++;
    state->encoding = DOS_SB_ENCODING_PCM;
    state->adpcm_reference = 0x80U;
    state->adpcm_step = 0;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x40U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD3U);
    if (state->output_rate != 22222U || state->input_rate != 22222U ||
        !state->output_rate_is_transfer_rate ||
        !state->input_rate_is_transfer_rate ||
        dos_sb_effective_rate_locked(state, false, false) != 22222U ||
        dos_sb_effective_rate_locked(state, false, true) != 11111U)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x41U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xACU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x44U);
    if (state->output_rate != 44100U ||
        state->output_rate_is_transfer_rate ||
        state->input_rate != 22222U ||
        !state->input_rate_is_transfer_rate)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x42U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x1FU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x40U);
    if (state->input_rate != 8000U ||
        state->input_rate_is_transfer_rate ||
        state->output_rate != 44100U ||
        state->output_rate_is_transfer_rate)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x48U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xFFU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xFFU);
    if (state->block_units != 65536U)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xA8U);
    if (!state->legacy_input_stereo)
        failures++;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xA0U);
    if (state->legacy_input_stereo)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x04, 0x0EU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x05, 0x02U);
    if (!(state->mixer[0x0E] & 0x02U))
        failures++;

    state->output_rate = 22050U;
    state->input_rate = 22050U;
    state->output_rate_is_transfer_rate = false;
    state->input_rate_is_transfer_rate = false;
    state->legacy_input_stereo = false;
    state->block_units = 0;
    state->mixer[0x0E] = 0;

    memory[0x1000] = 128U;
    memory[0x1001] = 255U;
    memory[0x1002] = 0U;
    memory[0x1003] = 129U;
    dos_audio_port_write8(&vm, 0x0C, 0U);
    dos_audio_port_write8(
        &vm, DOS_DMA8_ADDRESS_PORT(DOS_SB_DMA8_CHANNEL), 0x00U);
    dos_audio_port_write8(
        &vm, DOS_DMA8_ADDRESS_PORT(DOS_SB_DMA8_CHANNEL), 0x10U);
    dos_audio_port_write8(
        &vm, DOS_DMA8_COUNT_PORT(DOS_SB_DMA8_CHANNEL), 0x03U);
    dos_audio_port_write8(
        &vm, DOS_DMA8_COUNT_PORT(DOS_SB_DMA8_CHANNEL), 0x00U);
    dos_audio_port_write8(
        &vm, DOS_DMA_PAGE_PORT(DOS_SB_DMA8_CHANNEL), 0U);
    dos_audio_port_write8(
        &vm, 0x0B, 0x48U | DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL));
    dos_audio_port_write8(
        &vm, 0x0A, DOS_DMA_LOCAL_CHANNEL(DOS_SB_DMA8_CHANNEL));
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xD1U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x41U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xBBU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x80U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xC0U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x03U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);

    int16_t output[8] = {0};
    if (!state->playing || !dos_audio_fill(state, output, 4U) ||
        state->playing || output[0] != 0 || output[1] != 0 ||
        output[2] != 32512 || output[3] != 32512 ||
        output[4] != -32768 || output[5] != -32768 ||
        output[6] != 256 || output[7] != 256)
        failures++;

    uint8_t vector = 0;
    uint32_t pending = 0;
    if (!dos_audio_take_irq(&vm, &vector, &pending) ||
        vector != DOS_SB_IRQ_LINE || !(pending & DOS_SB_IRQ_8BIT)) {
        failures++;
    } else {
        dos_audio_restore_irq(&vm, pending);
        pending = 0;
        if (!dos_audio_take_irq(&vm, &vector, &pending) ||
            !(pending & DOS_SB_IRQ_8BIT))
            failures++;
    }

    memory[0x1000] = 128U;
    memory[0x1001] = 255U;
    memory[0x1002] = 0U;
    memory[0x1003] = 129U;
    state->dma8.address = 0x1000U;
    state->dma8.count = 3U;
    state->dma8.base_address = 0x1000U;
    state->dma8.base_count = 3U;
    state->dma8.page = 0U;
    state->dma8.mode = 0x49U;
    state->dma8.masked = false;
    state->dma8.terminal_count = false;
    state->dma_command8 = 0U;
    state->output_rate = AUDIO_OUTPUT_RATE_HZ;
    state->output_rate_is_transfer_rate = false;
    state->block_units = 2U;
    dos_sb_response_clear_locked(state);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x91U);
    uint32_t unknown_commands = state->unknown_commands;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE1U);
    if (!state->playing || !state->high_speed ||
        state->block_bytes != 2U || state->response_count != 0U ||
        state->unknown_commands != unknown_commands)
        failures++;

    memset(output, 0, sizeof(output));
    pending = 0;
    if (!dos_audio_fill(state, output, 2U) || state->playing ||
        state->high_speed || output[0] != 0 || output[1] != 0 ||
        output[2] != 32512 || output[3] != 32512 ||
        state->dma8.address != 0x1002U || state->dma8.count != 1U ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xE1U);
    if (state->response_count != 2U ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 4U ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 5U)
        failures++;
    state->block_units = 0U;

    memory[0x1000] = 128U;
    memory[0x1001] = 255U;
    memory[0x1002] = 0U;
    memory[0x1003] = 129U;
    state->dma8.address = 0x1000U;
    state->dma8.count = 3U;
    state->dma8.base_address = 0x1000U;
    state->dma8.base_count = 3U;
    state->dma8.page = 0U;
    state->dma8.mode = 0x59U;
    state->dma8.masked = false;
    state->dma8.terminal_count = false;
    state->dma_command8 = 0U;
    state->output_rate = AUDIO_OUTPUT_RATE_HZ;
    state->output_rate_is_transfer_rate = false;
    state->block_units = 2U;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x1CU);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x14U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    if (!state->playing || !state->auto_init ||
        !state->exit_auto_init || !state->pending_dma_command ||
        state->pending_command != 0x14U || state->block_bytes != 2U)
        failures++;

    memset(output, 0, sizeof(output));
    pending = 0;
    if (!dos_audio_fill(state, output, 2U) || !state->playing ||
        state->auto_init || state->exit_auto_init ||
        state->pending_dma_command || state->block_bytes != 1U ||
        output[0] != 0 || output[1] != 0 ||
        output[2] != 32512 || output[3] != 32512 ||
        state->dma8.address != 0x1002U || state->dma8.count != 1U ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    memset(output, 0, sizeof(output));
    pending = 0;
    if (!dos_audio_fill(state, output, 1U) || state->playing ||
        output[0] != -32768 || output[1] != -32768 ||
        state->dma8.address != 0x1003U || state->dma8.count != 0U ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;
    state->block_units = 0U;

    memory[0x1000] = 0x80U;
    memory[0x1001] = 0x17U;
    state->dma8.address = 0x1000U;
    state->dma8.count = 1U;
    state->dma8.base_address = 0x1000U;
    state->dma8.base_count = 1U;
    state->dma8.page = 0U;
    state->dma8.mode = 0x49U;
    state->dma8.masked = false;
    state->dma8.terminal_count = false;
    state->output_rate = AUDIO_OUTPUT_RATE_HZ;
    state->output_rate_is_transfer_rate = false;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x75U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x01U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    memset(output, 0, sizeof(output));
    pending = 0;
    if (!state->playing || state->encoding != DOS_SB_ENCODING_ADPCM_4 ||
        !dos_audio_fill(state, output, 2U) || state->playing ||
        output[0] != 256 || output[1] != 256 ||
        output[2] != 2048 || output[3] != 2048 ||
        state->dma8.address != 0x1002U ||
        state->dma8.count != 0xFFFFU ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    memory[0x1000] = 0x80U;
    memory[0x1001] = 0x17U;
    memory[0x1002] = 0x00U;
    memory[0x1003] = 0x00U;
    state->dma8.address = 0x1000U;
    state->dma8.count = 3U;
    state->dma8.base_address = 0x1000U;
    state->dma8.base_count = 3U;
    state->dma8.mode = 0x59U;
    state->dma8.terminal_count = false;
    state->block_units = 2U;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x7DU);
    memset(output, 0, sizeof(output));
    pending = 0;
    if (!state->playing || !state->auto_init ||
        !dos_audio_fill(state, output, 2U) || !state->playing ||
        output[0] != 256 || output[1] != 256 ||
        output[2] != 2048 || output[3] != 2048 ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xDAU);
    memset(output, 0, sizeof(output));
    pending = 0;
    if (!dos_audio_fill(state, output, 4U) || state->playing ||
        output[0] != 2304 || output[1] != 2304 ||
        output[2] != 2304 || output[3] != 2304 ||
        output[4] != 2304 || output[5] != 2304 ||
        output[6] != 2304 || output[7] != 2304 ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;
    state->block_units = 0;

    memory[0x1000] = 1U;
    memory[0x1001] = 2U;
    memory[0x1002] = 3U;
    memory[0x1003] = 4U;
    state->dma8.address = 0x1000U;
    state->dma8.count = 3U;
    state->dma8.page = 0U;
    state->dma8.masked = false;
    state->input_rate = AUDIO_OUTPUT_RATE_HZ;
    state->input_rate_is_transfer_rate = false;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x24U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x03U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    memset(output, 0x7F, sizeof(output));
    pending = 0;
    if (!state->playing ||
        state->transfer_mode != DOS_SB_TRANSFER_CAPTURE ||
        memory[0x1000] != 0x80U || memory[0x1001] != 0x80U ||
        memory[0x1002] != 0x80U || memory[0x1003] != 0x80U ||
        !dos_audio_fill(state, output, 4U) || state->playing ||
        output[0] || output[1] || output[2] || output[3] ||
        output[4] || output[5] || output[6] || output[7] ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x20U);
    if (!dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0x80U)
        failures++;

    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x80U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x03U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    memset(output, 0x7F, sizeof(output));
    pending = 0;
    if (!state->playing ||
        state->transfer_mode != DOS_SB_TRANSFER_SILENCE ||
        !dos_audio_fill(state, output, 4U) || state->playing ||
        output[0] || output[1] || output[2] || output[3] ||
        output[4] || output[5] || output[6] || output[7] ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    memory[0x1000] = 0xFFU;
    memory[0x1001] = 0xFFU;
    memory[0x1002] = 0xFFU;
    memory[0x1003] = 0xFFU;
    state->dma16.address = 0x0800U;
    state->dma16.count = 1U;
    state->dma16.page = 0U;
    state->dma16.masked = false;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xB8U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x01U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    memset(output, 0x7F, sizeof(output));
    pending = 0;
    if (!state->playing ||
        state->transfer_mode != DOS_SB_TRANSFER_CAPTURE ||
        memory[0x1000] != 0U || memory[0x1001] != 0x80U ||
        memory[0x1002] != 0U || memory[0x1003] != 0x80U ||
        !dos_audio_fill(state, output, 2U) || state->playing ||
        output[0] || output[1] || output[2] || output[3] ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_16BIT))
        failures++;

    state->dma8.address = 0x1FFFU;
    state->dma8.count = 1U;
    uint32_t errors = state->playback_errors;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0xC0U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x01U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x0C, 0x00U);
    pending = 0;
    if (state->playing || state->playback_errors != errors + 1U ||
        !dos_audio_take_irq(&vm, &vector, &pending) ||
        !(pending & DOS_SB_IRQ_8BIT))
        failures++;

    failures += dos_audio_dma_wait_selftest(&vm);

    state->present = false;
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 1U);
    dos_audio_port_write8(&vm, DOS_SB_BASE + 0x06, 0U);
    value = 0;
    pending = 0;
    if (dos_audio_available(&vm) ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0E, &value) || value ||
        !dos_audio_port_read8(&vm, DOS_SB_BASE + 0x0A, &value) ||
        value != 0xFFU || state->playing ||
        dos_audio_take_irq(&vm, &vector, &pending))
        failures++;

    vm.audio = NULL;
    kfree(state);
    dos_host_free_pages(memory, pages);
    return failures;
}
