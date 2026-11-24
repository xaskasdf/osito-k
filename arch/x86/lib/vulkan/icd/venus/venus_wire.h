/* Official Mesa Venus ring transport over OsitoK GPU syscalls. */
#ifndef OSITOK_VENUS_WIRE_H
#define OSITOK_VENUS_WIRE_H

#include <stddef.h>
#include <stdint.h>

struct venus_wire *venus_wire_open(int32_t ctx_id);
void venus_wire_close(struct venus_wire *w);

int venus_wire_call(struct venus_wire *w, const void *command,
                    uint32_t command_size, void *reply,
                    uint32_t reply_size);
int venus_wire_submit_async(struct venus_wire *w, const void *command,
                             uint32_t command_size);
int venus_wire_resource_create(struct venus_wire *w, uint64_t size,
                               uint64_t blob_id,
                               uint32_t *resource_id, void **mapping);
int venus_wire_resource_destroy(struct venus_wire *w,
                                uint32_t resource_id);

#endif
