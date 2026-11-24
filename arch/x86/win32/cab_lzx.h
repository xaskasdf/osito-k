/* Cabinet LZX, 32 KiB CFDATA frames, window sizes 15 through 21 bits.
 * This is the original CAB format, not LZX Delta or WIM compression. */
#ifndef OSITOK_CAB_LZX_H
#define OSITOK_CAB_LZX_H
#include "../include/types.h"
typedef struct cab_lzx cab_lzx_t;
cab_lzx_t *cab_lzx_create(unsigned window_bits);
void cab_lzx_destroy(cab_lzx_t *state);
/* Output includes the entire unmodified folder prefix as match history.
 * Decode calls must be consecutive. The caller bounds the output allocation. */
int cab_lzx_frame(cab_lzx_t *state, const uint8_t *input, uint32_t input_size,
                  uint8_t *output, uint32_t output_offset, uint32_t frame_size);
/* Reverse E8 preprocessing only after all requested frames are decoded, so
 * translated bytes never enter the match dictionary. */
void cab_lzx_translate(cab_lzx_t *state, uint8_t *output, uint32_t size);
#endif
