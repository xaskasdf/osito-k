/*
 * tensor_sdot.h -- quantized matvecs with the dot product instructions
 * (kernel/tensor_sdot.c)
 */
#ifndef OSITO_ARM_TENSOR_SDOT_H
#define OSITO_ARM_TENSOR_SDOT_H

#include "../include/types.h"

/* A matvec's input in 8 bits: per 32 values a scale d, the values as
 * round(x / d) in q, and their exact sum s (Q4_1's minimum multiplies it) */
typedef struct {
    float  *d, *s;
    int8_t *q;
} q8_input_t;

/* 1: matvecs on Q4_0, Q4_1 and Q8_0 weights go through SDOT (dispatch_init
 * sets it: the CPU has the instructions, NEON and `llm sdot` are on) */
extern int tensor_sdot;
extern int tensor_sdot_wanted;

int  tensor_sdot_detect(void);          /* ID_AA64ISAR0_EL1.DP: 1 if present */
void q8_quantize(q8_input_t *x, const float *in, uint32_t cols);
void matvec_q4_0_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols);
void matvec_q4_1_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols);
void matvec_q8_0_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols);

#endif
