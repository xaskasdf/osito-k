/*
 * OsitoK x86-64 — Audio-Driven Scheduling
 *
 * Uses TSC-deadline to schedule audio buffer fills with sub-ms
 * precision. The HDA DMA position tells us exactly when the next
 * buffer is needed. Audio callbacks preempt all other work.
 *
 * At 48KHz / 256 samples: buffer every 5.3ms, ~50us to fill.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_tsc_freq(void);
extern bool     idt_tsc_deadline_active(void);

/* ── Audio callback state ───────────────────────────────────── */

typedef void (*audio_fill_fn)(int16_t *buf, uint32_t frames);

static audio_fill_fn  audio_callback;
static int16_t       *audio_next_buffer;
static uint32_t       audio_buffer_frames;
static uint64_t       audio_deadline_tsc;
static uint32_t       audio_sample_rate;
static uint64_t       audio_fills;
static uint64_t       audio_overruns;

/* Register an audio callback that fires when the HDA DMA needs
 * more samples. Uses TSC-deadline for precise timing. */
void audio_register_callback(audio_fill_fn fill, int16_t *buf,
                              uint32_t frames, uint32_t sample_rate)
{
    audio_callback     = fill;
    audio_next_buffer  = buf;
    audio_buffer_frames = frames;
    audio_sample_rate  = sample_rate;
    audio_fills        = 0;
    audio_overruns     = 0;

    /* Schedule first callback */
    uint64_t tsc_freq = idt_get_tsc_freq();
    if (tsc_freq == 0) return;

    uint64_t tsc_per_frame = tsc_freq / sample_rate;
    uint64_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    audio_deadline_tsc = ((hi << 32) | lo) + frames * tsc_per_frame;

    serial_puts("[AUDIO] Callback registered: ");
    serial_putdec(sample_rate);
    serial_puts(" Hz, ");
    serial_putdec(frames);
    serial_puts(" frames/buffer\n");
}

void audio_unregister(void)
{
    audio_callback = NULL;
}

/* Called from sched_tick (100Hz or TSC-deadline rate).
 * Checks if audio deadline is approaching and preempts to fill. */
void __attribute__((section(".text.hot"))) audio_check_deadline(void)
{
    if (!audio_callback) return;

    uint64_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = (hi << 32) | lo;

    uint64_t tsc_freq = idt_get_tsc_freq();
    if (tsc_freq == 0) return;

    uint64_t margin = tsc_freq / 10000;  /* 100us margin */

    if (now + margin >= audio_deadline_tsc) {
        /* Audio buffer needed — fill it NOW */
        audio_callback(audio_next_buffer, audio_buffer_frames);
        audio_fills++;

        /* Check if we missed the deadline (overrun) */
        if (now > audio_deadline_tsc + margin * 10)
            audio_overruns++;

        /* Schedule next callback */
        uint64_t tsc_per_frame = tsc_freq / audio_sample_rate;
        audio_deadline_tsc += (uint64_t)audio_buffer_frames * tsc_per_frame;

        /* If deadline is already past (multiple overruns), snap forward */
        if (audio_deadline_tsc < now)
            audio_deadline_tsc = now + (uint64_t)audio_buffer_frames * tsc_per_frame;

        /* Copy filled buffer to HDA DMA buffer */
        extern void hda_play_buffer(const int16_t *samples, uint32_t num_samples);
        extern bool hda_is_ready(void);
        if (hda_is_ready())
            hda_play_buffer(audio_next_buffer, audio_buffer_frames);
    }
}

uint64_t audio_get_fills(void)    { return audio_fills; }
uint64_t audio_get_overruns(void) { return audio_overruns; }
