#ifndef OSITOK_DOS_AUDIO_H
#define OSITOK_DOS_AUDIO_H

#include "../include/types.h"

struct dos_vm;

typedef struct {
    uint16_t base_port;
    uint8_t irq;
    uint8_t dma8;
    uint8_t dma16;
    uint8_t card_type;
} dos_audio_resources_t;

bool dos_audio_init(struct dos_vm *vm);
void dos_audio_shutdown(struct dos_vm *vm);
bool dos_audio_available(const struct dos_vm *vm);
bool dos_audio_get_resources(const struct dos_vm *vm,
                             dos_audio_resources_t *resources);

bool dos_audio_port_read8(struct dos_vm *vm, uint16_t port,
                          uint8_t *value);
bool dos_audio_port_write8(struct dos_vm *vm, uint16_t port,
                           uint8_t value);
bool dos_audio_take_irq(struct dos_vm *vm, uint8_t *irq,
                        uint32_t *pending_mask);
void dos_audio_restore_irq(struct dos_vm *vm, uint32_t pending_mask);

int dos_audio_selftest(void);

#endif
