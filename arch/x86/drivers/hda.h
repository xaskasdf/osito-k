/*
 * OsitoK x86-64 — Intel HDA (High Definition Audio) Driver
 *
 * Register definitions, structures, and API.
 * Supports: ICH6/ICH9 (QEMU), AMD Starship (1022:1487), NVIDIA GA102 (10de:1aef).
 */

#ifndef OSITOK_HDA_H
#define OSITOK_HDA_H

#include "../include/types.h"

/* ── Controller Registers (BAR0 offsets) ───────────────────────── */

#define HDA_GCAP        0x00    /* Global Capabilities (16-bit) */
#define HDA_VMIN        0x02    /* Minor Version (8-bit) */
#define HDA_VMAJ        0x03    /* Major Version (8-bit) */
#define HDA_OUTPAY      0x04    /* Output Payload Cap (16-bit) */
#define HDA_INPAY       0x06    /* Input Payload Cap (16-bit) */
#define HDA_GCTL        0x08    /* Global Control (32-bit) */
#define HDA_WAKEEN      0x0C    /* Wake Enable (16-bit) */
#define HDA_STATESTS    0x0E    /* State Change Status (16-bit) */
#define HDA_GSTS        0x10    /* Global Status (16-bit) */
#define HDA_INTCTL      0x20    /* Interrupt Control (32-bit) */
#define HDA_INTSTS      0x24    /* Interrupt Status (32-bit) */
#define HDA_WALCLK      0x30    /* Wall Clock Counter (32-bit) */
#define HDA_SSYNC       0x34    /* Stream Synchronization (32-bit) */

/* CORB registers */
#define HDA_CORBLBASE   0x40    /* CORB Lower Base Address (32-bit) */
#define HDA_CORBUBASE   0x44    /* CORB Upper Base Address (32-bit) */
#define HDA_CORBWP      0x48    /* CORB Write Pointer (16-bit) */
#define HDA_CORBRP      0x4A    /* CORB Read Pointer (16-bit) */
#define HDA_CORBCTL     0x4C    /* CORB Control (8-bit) */
#define HDA_CORBSTS     0x4D    /* CORB Status (8-bit) */
#define HDA_CORBSIZE    0x4E    /* CORB Size (8-bit) */

/* RIRB registers */
#define HDA_RIRBLBASE   0x50    /* RIRB Lower Base Address (32-bit) */
#define HDA_RIRBUBASE   0x54    /* RIRB Upper Base Address (32-bit) */
#define HDA_RIRBWP      0x58    /* RIRB Write Pointer (16-bit) */
#define HDA_RINTCNT     0x5A    /* Response Interrupt Count (16-bit) */
#define HDA_RIRBCTL     0x5C    /* RIRB Control (8-bit) */
#define HDA_RIRBSTS     0x5D    /* RIRB Status (8-bit) */
#define HDA_RIRBSIZE    0x5E    /* RIRB Size (8-bit) */

/* DMA Position */
#define HDA_DPLBASE     0x70    /* DMA Position Lower Base (32-bit) */
#define HDA_DPUBASE     0x74    /* DMA Position Upper Base (32-bit) */

/* Stream Descriptors: ISS at 0x80, OSS follows, each 0x20 bytes */
#define HDA_SD_BASE     0x80
#define HDA_SD_SIZE     0x20

/* Stream descriptor register offsets (relative to stream base) */
#define HDA_SD_CTL      0x00    /* Control (24-bit, access as 32-bit) */
#define HDA_SD_STS      0x03    /* Status (8-bit) */
#define HDA_SD_LPIB     0x04    /* Link Position In Buffer (32-bit) */
#define HDA_SD_CBL      0x08    /* Cyclic Buffer Length (32-bit) */
#define HDA_SD_LVI      0x0C    /* Last Valid Index (16-bit) */
#define HDA_SD_FIFOS    0x10    /* FIFO Size (16-bit) */
#define HDA_SD_FMT      0x12    /* Stream Format (16-bit) */
#define HDA_SD_BDLPL    0x18    /* BDL Pointer Low (32-bit) */
#define HDA_SD_BDLPU    0x1C    /* BDL Pointer High (32-bit) */

/* ── Control Bits ──────────────────────────────────────────────── */

/* GCTL */
#define HDA_GCTL_CRST   (1 << 0)   /* Controller Reset */
#define HDA_GCTL_UNSOL  (1 << 8)   /* Accept Unsolicited Responses */

/* SD_CTL (24-bit, read as 32-bit) */
#define HDA_SD_CTL_RUN  (1 << 1)   /* Stream Run */
#define HDA_SD_CTL_IOCE (1 << 2)   /* IOC Enable */

/* SD_STS */
#define HDA_SD_STS_BCIS (1 << 2)   /* Buffer Complete IRQ Status */
#define HDA_SD_STS_FIFOE (1 << 3)  /* FIFO Error */
#define HDA_SD_STS_DESE (1 << 4)   /* Descriptor Error */

/* CORBCTL / RIRBCTL */
#define HDA_CORBCTL_RUN (1 << 1)   /* CORB DMA Engine Run */
#define HDA_RIRBCTL_RUN (1 << 1)   /* RIRB DMA Engine Run */

/* INTCTL */
#define HDA_INTCTL_GIE  (1U << 31) /* Global Interrupt Enable */
#define HDA_INTCTL_CIE  (1 << 30)  /* Controller Interrupt Enable */

/* ── Stream Format (FMT register) ─────────────────────────────── */

#define HDA_FMT_BASE_48   (0 << 14)  /* 48 kHz base rate */
#define HDA_FMT_BASE_441  (1 << 14)  /* 44.1 kHz base rate */
#define HDA_FMT_MUL_1     (0 << 11)  /* x1 multiplier */
#define HDA_FMT_DIV_1     (0 << 8)   /* /1 divisor */
#define HDA_FMT_16BIT     (1 << 4)   /* 16 bits per sample */
#define HDA_FMT_STEREO    1          /* 2 channels (N-1) */

/* 48kHz, 16-bit, stereo = 0x0011 */
#define HDA_FMT_DEFAULT   (HDA_FMT_BASE_48 | HDA_FMT_MUL_1 | HDA_FMT_DIV_1 | \
                           HDA_FMT_16BIT | HDA_FMT_STEREO)

/* ── Codec Verbs (unified encoding) ───────────────────────────── */
/*
 * Command word: (CAd << 28) | (NID << 20) | (verb << 8) | parm
 *
 * 12-bit verbs: verb = full ID (0xF00, 0x707, etc.), parm = 8-bit
 *  4-bit verbs: verb = ID << 8  (0x200, 0x300, etc.), parm = 16-bit
 *
 * The overlap is intentional — for 4-bit verbs, verb bits 7:0 are 0
 * and parm bits 15:8 fill those positions.
 */

/* 12-bit verbs (8-bit payload) */
#define HDA_VERB_GET_PARAM      0xF00   /* GET_PARAMETER */
#define HDA_VERB_GET_CONN_LIST  0xF02   /* GET_CONNECTION_LIST_ENTRY */
#define HDA_VERB_SET_CONV_CTRL  0x706   /* SET_CONVERTER_STREAM_CHANNEL */
#define HDA_VERB_SET_PIN_CTRL   0x707   /* SET_PIN_WIDGET_CONTROL */
#define HDA_VERB_SET_POWER      0x705   /* SET_POWER_STATE */
#define HDA_VERB_SET_EAPD       0x70C   /* SET_EAPD_BTLENABLE */
#define HDA_VERB_GET_AMP        0xB00   /* GET_AMP_GAIN_MUTE */

/* 4-bit verbs (16-bit payload) */
#define HDA_VERB_SET_STREAM_FMT 0x200   /* SET_CONVERTER_FORMAT */
#define HDA_VERB_SET_AMP        0x300   /* SET_AMP_GAIN_MUTE */
#define HDA_VERB_GET_STREAM_FMT 0xA00   /* GET_CONVERTER_FORMAT */

/* ── GET_PARAMETER IDs ─────────────────────────────────────────── */

#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_REVISION_ID   0x02
#define HDA_PARAM_NODE_COUNT    0x04
#define HDA_PARAM_FUNC_GRP_TYPE 0x05
#define HDA_PARAM_AUDIO_WIDGET  0x09
#define HDA_PARAM_PCM_RATES     0x0A
#define HDA_PARAM_PIN_CAP       0x0C
#define HDA_PARAM_CONN_LIST_LEN 0x0E
#define HDA_PARAM_AMP_OUT_CAP   0x12

/* ── Widget Types (bits 23:20 of Audio Widget Capabilities) ──── */

#define HDA_WIDGET_AUD_OUT  0x0   /* Audio Output (DAC) */
#define HDA_WIDGET_AUD_IN   0x1   /* Audio Input (ADC) */
#define HDA_WIDGET_AUD_MIX  0x2   /* Audio Mixer */
#define HDA_WIDGET_AUD_SEL  0x3   /* Audio Selector */
#define HDA_WIDGET_PIN      0x4   /* Pin Complex */
#define HDA_WIDGET_POWER    0x5   /* Power Widget */
#define HDA_WIDGET_VOL_KNOB 0x6   /* Volume Knob */
#define HDA_WIDGET_BEEP     0x7   /* Beep Generator */

/* ── Pin Widget Control ────────────────────────────────────────── */

#define HDA_PIN_OUT_EN      (1 << 6)
#define HDA_PIN_IN_EN       (1 << 5)
#define HDA_PIN_HP_EN       (1 << 7)

/* ── Amp Gain/Mute Payload (4-bit verb, 16-bit payload) ───────── */

#define HDA_AMP_SET_OUT     (1 << 15)
#define HDA_AMP_SET_IN      (1 << 14)
#define HDA_AMP_SET_LEFT    (1 << 13)
#define HDA_AMP_SET_RIGHT   (1 << 12)
#define HDA_AMP_MUTE        (1 << 7)
#define HDA_AMP_GAIN(x)     ((x) & 0x7F)

/* ── Buffer Descriptor List Entry ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t length;    /* Buffer length in bytes */
    uint32_t ioc;       /* Bit 0: Interrupt On Completion */
} hda_bdl_entry_t;

/* ── Driver Constants ──────────────────────────────────────────── */

#define HDA_CORB_ENTRIES    256
#define HDA_RIRB_ENTRIES    256
#define HDA_BDL_ENTRIES     2
#define HDA_SAMPLE_RATE     48000
#define HDA_BUF_SECONDS     1
#define HDA_BUF_SIZE        (HDA_SAMPLE_RATE * 2 * 2 * HDA_BUF_SECONDS)  /* 192KB */

/* ── Public API ────────────────────────────────────────────────── */

int  hda_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func);
void hda_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void hda_play_buffer(const int16_t *samples, uint32_t num_samples);
void hda_stop(void);
bool hda_is_ready(void);

#endif /* OSITOK_HDA_H */
