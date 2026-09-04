/*
 * OsitoK x86-64 audio deadline scheduler.
 *
 * Registered producers fill independent PCM16 stereo blocks. The scheduler
 * mixes them into one HDA block. The APIC timer calls audio_check_deadline()
 * before process scheduling and uses audio_get_next_deadline_us() when
 * selecting its next TSC deadline.
 */

#include "../include/audio_sched.h"
#include "../drivers/hda.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_tsc_freq(void);

#define AUDIO_SOURCE_SLOTS 16U

typedef struct {
    audio_fill_fn fill;
    audio_fill_context_fn fill_context;
    void *context;
    int16_t *buffer;
    audio_source_id_t id;
    bool active;
} audio_source_t;

static audio_source_t audio_sources[AUDIO_SOURCE_SLOTS];
static int32_t audio_accumulator[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
static int16_t audio_output[AUDIO_OUTPUT_BLOCK_FRAMES * 2U];
static uint64_t audio_deadline_tsc;
static uint64_t audio_fills;
static uint64_t audio_overruns;
static volatile uint32_t audio_transition;
static volatile uint32_t audio_master_volume = 0xFFFFFFFFU;
static bool audio_running;
static audio_source_id_t audio_next_source_id = 1U;
static audio_source_id_t audio_legacy_source;

bool audio_output_is_ready(void)
{
    return hda_is_ready();
}

const char *audio_output_backend_name(void)
{
    return hda_is_ready() ? "hda" : "none";
}

uint32_t audio_get_source_capacity(void)
{
    return AUDIO_SOURCE_SLOTS;
}

void audio_output_set_volume(uint16_t left, uint16_t right)
{
    uint32_t packed = (uint32_t)left | ((uint32_t)right << 16);
    __atomic_store_n(&audio_master_volume, packed, __ATOMIC_RELEASE);
}

void audio_output_get_volume(uint16_t *left, uint16_t *right)
{
    uint32_t packed = __atomic_load_n(&audio_master_volume,
                                      __ATOMIC_ACQUIRE);
    if (left)
        *left = (uint16_t)packed;
    if (right)
        *right = (uint16_t)(packed >> 16);
}

static inline uint64_t audio_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t audio_period_tsc(uint64_t tsc_freq, uint32_t frames,
                                 uint32_t sample_rate)
{
    return (tsc_freq / sample_rate) * frames +
           ((tsc_freq % sample_rate) * frames) / sample_rate;
}

static void audio_transition_acquire(void)
{
    while (__sync_lock_test_and_set(&audio_transition, 1U))
        __asm__ volatile ("pause" ::: "memory");
}

static bool audio_transition_try_acquire(void)
{
    return __sync_lock_test_and_set(&audio_transition, 1U) == 0;
}

static void audio_transition_release(void)
{
    __sync_lock_release(&audio_transition);
}

static bool audio_has_sources_locked(void)
{
    for (uint32_t i = 0; i < AUDIO_SOURCE_SLOTS; i++) {
        if (audio_sources[i].active)
            return true;
    }
    return false;
}

static bool audio_render_locked(void)
{
    memset(audio_accumulator, 0, sizeof(audio_accumulator));
    bool active = false;

    for (uint32_t slot = 0; slot < AUDIO_SOURCE_SLOTS; slot++) {
        audio_source_t *source = &audio_sources[slot];
        if (!source->active)
            continue;
        bool filled = source->fill_context
            ? source->fill_context(source->context, source->buffer,
                                   AUDIO_OUTPUT_BLOCK_FRAMES)
            : source->fill(source->buffer, AUDIO_OUTPUT_BLOCK_FRAMES);
        if (!filled) {
            memset(source, 0, sizeof(*source));
            continue;
        }
        active = true;
        for (uint32_t i = 0; i < AUDIO_OUTPUT_BLOCK_FRAMES * 2U; i++)
            audio_accumulator[i] += source->buffer[i];
    }

    if (!active)
        return false;

    uint32_t volume = __atomic_load_n(&audio_master_volume,
                                      __ATOMIC_ACQUIRE);
    uint32_t gains[2] = { volume & 0xFFFFU, volume >> 16 };
    for (uint32_t i = 0; i < AUDIO_OUTPUT_BLOCK_FRAMES * 2U; i++) {
        int64_t scaled = (int64_t)audio_accumulator[i] * gains[i & 1U];
        int32_t sample = (int32_t)(scaled / 65535);
        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        audio_output[i] = (int16_t)sample;
    }
    return true;
}

static audio_source_id_t audio_allocate_source_id_locked(void)
{
    for (;;) {
        audio_source_id_t candidate = audio_next_source_id++;
        if (!candidate)
            continue;
        bool used = false;
        for (uint32_t i = 0; i < AUDIO_SOURCE_SLOTS; i++) {
            if (audio_sources[i].active && audio_sources[i].id == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
}

static audio_register_result_t audio_source_register_common(
    audio_fill_fn fill, audio_fill_context_fn fill_context, void *context,
    int16_t *buffer, uint32_t frames, uint32_t sample_rate,
    audio_source_id_t *source_id)
{
    if (source_id)
        *source_id = AUDIO_SOURCE_INVALID;
    if ((!fill && !fill_context) || (fill && fill_context) ||
        !buffer || !source_id ||
        frames != AUDIO_OUTPUT_BLOCK_FRAMES ||
        sample_rate != AUDIO_OUTPUT_RATE_HZ)
        return AUDIO_REGISTER_INVALID;
    if (!audio_output_is_ready())
        return AUDIO_REGISTER_NO_BACKEND;

    uint64_t tsc_freq = idt_get_tsc_freq();
    if (!tsc_freq)
        return AUDIO_REGISTER_NO_CLOCK;

    audio_transition_acquire();
    int free_slot = -1;
    for (uint32_t i = 0; i < AUDIO_SOURCE_SLOTS; i++) {
        if (!audio_sources[i].active) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) {
        audio_transition_release();
        return AUDIO_REGISTER_NO_SLOTS;
    }

    audio_source_t *source = &audio_sources[free_slot];
    source->fill = fill;
    source->fill_context = fill_context;
    source->context = context;
    source->buffer = buffer;
    source->id = audio_allocate_source_id_locked();
    source->active = true;

    if (!audio_running) {
        audio_fills = 0;
        audio_overruns = 0;
        if (!audio_render_locked()) {
            memset(source, 0, sizeof(*source));
            audio_transition_release();
            return AUDIO_REGISTER_SOURCE_INACTIVE;
        }
        hda_play_buffer(audio_output, AUDIO_OUTPUT_BLOCK_FRAMES);
        audio_fills = 1;
        audio_running = true;
        __atomic_store_n(
            &audio_deadline_tsc,
            audio_rdtsc() + audio_period_tsc(
                tsc_freq, AUDIO_OUTPUT_BLOCK_FRAMES, AUDIO_OUTPUT_RATE_HZ),
            __ATOMIC_RELEASE);
    }

    *source_id = source->id;
    audio_transition_release();

    serial_puts("[AUDIO] Source registered: ");
    serial_putdec(*source_id);
    serial_puts(" at ");
    serial_putdec(sample_rate);
    serial_puts(" Hz, ");
    serial_putdec(frames);
    serial_puts(" frames/buffer\n");
    return AUDIO_REGISTER_OK;
}

audio_register_result_t audio_source_register_ex(
    audio_fill_fn fill, int16_t *buffer, uint32_t frames,
    uint32_t sample_rate, audio_source_id_t *source_id)
{
    return audio_source_register_common(fill, NULL, NULL, buffer, frames,
                                        sample_rate, source_id);
}

audio_register_result_t audio_source_register_context_ex(
    audio_fill_context_fn fill, void *context, int16_t *buffer,
    uint32_t frames, uint32_t sample_rate, audio_source_id_t *source_id)
{
    return audio_source_register_common(NULL, fill, context, buffer, frames,
                                        sample_rate, source_id);
}

const char *audio_register_result_name(audio_register_result_t result)
{
    switch (result) {
    case AUDIO_REGISTER_OK:
        return "ok";
    case AUDIO_REGISTER_INVALID:
        return "invalid canonical source";
    case AUDIO_REGISTER_NO_BACKEND:
        return "no output backend";
    case AUDIO_REGISTER_NO_CLOCK:
        return "audio clock unavailable";
    case AUDIO_REGISTER_NO_SLOTS:
        return "source capacity exhausted";
    case AUDIO_REGISTER_SOURCE_INACTIVE:
        return "source inactive during start";
    default:
        return "unknown registration error";
    }
}

bool audio_source_register(audio_fill_fn fill, int16_t *buffer,
                           uint32_t frames, uint32_t sample_rate,
                           audio_source_id_t *source_id)
{
    return audio_source_register_ex(fill, buffer, frames, sample_rate,
                                    source_id) == AUDIO_REGISTER_OK;
}

bool audio_source_register_context(audio_fill_context_fn fill, void *context,
                                   int16_t *buffer, uint32_t frames,
                                   uint32_t sample_rate,
                                   audio_source_id_t *source_id)
{
    return audio_source_register_context_ex(
        fill, context, buffer, frames, sample_rate,
        source_id) == AUDIO_REGISTER_OK;
}

void audio_source_unregister(audio_source_id_t source_id)
{
    if (!source_id)
        return;

    audio_transition_acquire();
    for (uint32_t i = 0; i < AUDIO_SOURCE_SLOTS; i++) {
        if (audio_sources[i].active && audio_sources[i].id == source_id) {
            memset(&audio_sources[i], 0, sizeof(audio_sources[i]));
            break;
        }
    }
    if (audio_running && !audio_has_sources_locked()) {
        audio_running = false;
        __atomic_store_n(&audio_deadline_tsc, 0, __ATOMIC_RELEASE);
        hda_stop();
    }
    audio_transition_release();
}

void audio_unregister(void)
{
    audio_source_id_t source = audio_legacy_source;
    audio_legacy_source = AUDIO_SOURCE_INVALID;
    audio_source_unregister(source);
}

bool audio_register_callback(audio_fill_fn fill, int16_t *buffer,
                             uint32_t frames, uint32_t sample_rate)
{
    audio_unregister();
    return audio_source_register(fill, buffer, frames, sample_rate,
                                 &audio_legacy_source);
}

void __attribute__((section(".text.hot"))) audio_check_deadline(void)
{
    if (!audio_transition_try_acquire())
        return;
    if (!audio_running) {
        audio_transition_release();
        return;
    }

    uint64_t tsc_freq = idt_get_tsc_freq();
    uint64_t deadline =
        __atomic_load_n(&audio_deadline_tsc, __ATOMIC_ACQUIRE);
    if (!tsc_freq || !deadline) {
        audio_transition_release();
        return;
    }

    uint64_t now = audio_rdtsc();
    uint64_t margin = tsc_freq / 10000; /* 100 microseconds */

    if (now + margin >= deadline) {
        if (!audio_render_locked()) {
            audio_running = false;
            __atomic_store_n(&audio_deadline_tsc, 0, __ATOMIC_RELEASE);
            hda_stop();
            audio_transition_release();
            return;
        }
        audio_fills++;

        if (now > deadline + margin * 10)
            audio_overruns++;

        uint64_t period = audio_period_tsc(
            tsc_freq, AUDIO_OUTPUT_BLOCK_FRAMES, AUDIO_OUTPUT_RATE_HZ);
        deadline += period;
        if (deadline < now)
            deadline = now + period;
        __atomic_store_n(&audio_deadline_tsc, deadline, __ATOMIC_RELEASE);

        if (audio_output_is_ready())
            hda_play_buffer(audio_output, AUDIO_OUTPUT_BLOCK_FRAMES);
    }

    audio_transition_release();
}

uint64_t audio_get_next_deadline_us(void)
{
    if (!__atomic_load_n(&audio_running, __ATOMIC_ACQUIRE))
        return 0;

    uint64_t tsc_freq = idt_get_tsc_freq();
    uint64_t deadline =
        __atomic_load_n(&audio_deadline_tsc, __ATOMIC_ACQUIRE);
    if (!tsc_freq || !deadline)
        return 0;

    uint64_t now = audio_rdtsc();
    if (deadline <= now)
        return 1;

    uint64_t delta = deadline - now;
    uint64_t seconds = delta / tsc_freq;
    uint64_t remainder = delta % tsc_freq;
    uint64_t usec = seconds * 1000000ULL +
                    (remainder * 1000000ULL) / tsc_freq;
    return usec ? usec : 1;
}

uint64_t audio_get_fills(void)
{
    return __atomic_load_n(&audio_fills, __ATOMIC_RELAXED);
}

uint64_t audio_get_overruns(void)
{
    return __atomic_load_n(&audio_overruns, __ATOMIC_RELAXED);
}

uint32_t audio_get_output_latency_blocks(void)
{
    return HDA_BDL_ENTRIES;
}
