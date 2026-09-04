/*
 * OsitoK — DOS Dynamic Binary Translation (DBT) Engine
 *
 * Translates hot 8086/386 basic blocks to native x86-64 code.
 * Cold paths fall back to the interpreter (cpu8086.c).
 *
 * Architecture:
 *   8086 bytes → IR (lightweight) → x86-64 machine code → code cache
 */

#ifndef DOS_JIT_H
#define DOS_JIT_H

#include "dos_types.h"

/* ── IR opcodes ─────────────────────────────────────────────────── */

typedef enum {
    /* Data movement */
    IR_MOV_REG_IMM,      /* a=dst_reg, b=imm32 */
    IR_MOV_REG_REG,      /* a=dst_reg, b=src_reg */
    IR_MOV_REG_MEM,      /* a=dst_reg, b=seg_reg, c=off_reg_or_imm */
    IR_MOV_MEM_REG,      /* a=seg_reg, b=off, c=src_reg */
    IR_MOV_REG_SREG,     /* a=dst_reg, b=sreg_id */
    IR_MOV_SREG_REG,     /* a=sreg_id, b=src_reg */

    /* Arithmetic */
    IR_ADD,              /* a=dst, b=src (dst += src) */
    IR_SUB,
    IR_CMP,              /* flags only, no write */
    IR_AND,
    IR_OR,
    IR_XOR,
    IR_INC,              /* a=reg */
    IR_DEC,

    /* Shifts */
    IR_SHL,              /* a=reg, b=count */
    IR_SHR,
    IR_SAR,

    /* Stack */
    IR_PUSH,             /* a=reg_or_imm */
    IR_POP,              /* a=dst_reg */

    /* Control flow */
    IR_JMP_IMM,          /* a=target_cs, b=target_ip */
    IR_JCC,              /* a=condition, b=target_ip_delta */
    IR_CALL_IMM,         /* a=target_ip */
    IR_RET,
    IR_INT,              /* a=int_number → exit JIT, call handler */

    /* Special */
    IR_LOAD_FLAGS,       /* sync x86-64 flags → emulated flags */
    IR_STORE_FLAGS,      /* sync emulated flags → x86-64 flags */
    IR_EXIT_BLOCK,       /* return to dispatcher with CS:IP updated */
    IR_NOP,
} ir_op_t;

/* ── IR instruction ─────────────────────────────────────────────── */

typedef struct {
    ir_op_t op;
    uint32_t a, b, c;
    uint8_t  width;      /* 1=8-bit, 2=16-bit, 4=32-bit */
} ir_inst_t;

#define IR_MAX_PER_BLOCK  256

/* ── Basic block ────────────────────────────────────────────────── */

typedef struct {
    /* Source location */
    uint16_t cs;
    uint16_t ip;
    uint16_t length;         /* bytes of 8086 code */

    /* IR */
    ir_inst_t ir[IR_MAX_PER_BLOCK];
    uint16_t  ir_count;

    /* Native code */
    uint8_t  *native_code;   /* pointer into code cache */
    uint32_t  native_size;   /* bytes of x86-64 code */

    /* Profiling */
    uint32_t  exec_count;    /* times executed (for hotness) */
    bool      compiled;      /* true if JIT-compiled */
} jit_block_t;

/* ── Code cache ─────────────────────────────────────────────────── */

#define JIT_CACHE_SIZE      (256 * 1024)   /* 256KB for generated code */
#define JIT_MAX_BLOCKS      4096           /* max cached blocks */
#define JIT_HOT_THRESHOLD   50             /* interpret N times before JIT */

typedef struct {
    /* Block table (hash by CS:IP) */
    jit_block_t blocks[JIT_MAX_BLOCKS];
    uint32_t    block_count;

    /* Executable code cache */
    uint8_t    *code_buf;        /* RWX memory for generated code */
    uint32_t    code_used;       /* bytes used in code_buf */

    /* Hit counters for hot path detection */
    uint16_t    hit_count[65536]; /* indexed by IP (simplified) */

    /* Stats */
    uint64_t    interpreted;     /* blocks interpreted */
    uint64_t    jit_executed;    /* blocks run from JIT cache */
    uint64_t    jit_compiled;    /* blocks compiled */
} jit_state_t;

/* ── Register mapping ───────────────────────────────────────────── */

/* Emulated 8086 registers mapped to indices for IR */
#define REG_AX  0
#define REG_CX  1
#define REG_DX  2
#define REG_BX  3
#define REG_SP  4
#define REG_BP  5
#define REG_SI  6
#define REG_DI  7

/* 8-bit register indices */
#define REG_AL  0
#define REG_CL  1
#define REG_DL  2
#define REG_BL  3
#define REG_AH  4
#define REG_CH  5
#define REG_DH  6
#define REG_BH  7

/* Segment register indices */
#define SREG_ES 0
#define SREG_CS 1
#define SREG_SS 2
#define SREG_DS 3

/* ── Public API ─────────────────────────────────────────────────── */

/* Initialize JIT engine */
void jit_init(jit_state_t *jit);

/* Release allocations owned by an initialized JIT state. */
void jit_destroy(jit_state_t *jit);

/* Look up or create a block for CS:IP */
jit_block_t *jit_get_block(jit_state_t *jit, uint16_t cs, uint16_t ip);

/* Decode 8086 bytes into IR for a basic block */
int jit_decode_block(dos_vm_t *vm, jit_block_t *block);

/* Compile IR to x86-64 native code */
int jit_compile_block(jit_state_t *jit, jit_block_t *block);

/* Execute a compiled block (returns next CS:IP) */
void jit_exec_block(dos_vm_t *vm, jit_block_t *block);

/* Invalidate all cached blocks (e.g., on PM switch) */
void jit_invalidate_all(jit_state_t *jit);

/* Print stats */
void jit_print_stats(jit_state_t *jit);

#endif /* DOS_JIT_H */
