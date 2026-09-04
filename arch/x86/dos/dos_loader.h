#ifndef DOS_LOADER_H
#define DOS_LOADER_H

#include "dos_types.h"

typedef struct {
    uint16_t parent_psp;
    uint16_t termination_offset;
    uint16_t termination_segment;
    uint16_t environment_source;
    const uint8_t *jft;
    const uint8_t *command_tail;
    const uint8_t *fcb1;
    const uint8_t *fcb2;
    const char *command_line;
    bool copy_environment;
    bool initialize_cpu;
    bool has_termination_address;
} dos_load_spec_t;

void dos_psp_initialize_system_fields(dos_vm_t *vm, dos_psp_t *psp,
                                      uint16_t psp_segment,
                                      uint16_t mem_top);

int dos_detect_format(const uint8_t *data, uint64_t size);
int dos_load_com(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                 const char *progname, const char *cmdline);
int dos_load_mz(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                const char *progname, const char *cmdline);
int dos_load_com_process(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                         const char *progname,
                         const dos_load_spec_t *spec,
                         uint16_t *psp_out);
int dos_load_mz_process(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                        const char *progname,
                        const dos_load_spec_t *spec,
                        uint16_t *psp_out);
int dos_load_overlay(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                     uint16_t load_segment, uint16_t relocation_factor);

#endif
