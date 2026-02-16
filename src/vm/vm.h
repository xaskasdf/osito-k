/*
 * OsitoK - OsitoVM bytecode stack machine
 *
 * 32-bit stack machine with:
 *   - Operand stack (64 entries)
 *   - Return stack for CALL/RET (16 entries)
 *   - Local variables (32 slots)
 *   - Syscalls for I/O, GPIO, timing
 *
 * Binary format (.vm):
 *   Offset 0: Magic "OSVM" (0x4F53564D LE)
 *   Offset 4: Version (1), Flags (0), Num locals, Reserved
 *   Offset 8: Bytecode start
 */
#ifndef OSITO_VM_H
#define OSITO_VM_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Constants ====== */

#define VM_STACK_SIZE   64    /* operand stack entries (256 bytes) */
#define VM_RSTACK_SIZE  16    /* return stack entries (64 bytes) */
#define VM_MAX_LOCALS   32    /* local variable slots (128 bytes) */

#define VM_MAGIC        0x4D56534F  /* "OSVM" little-endian */
#define VM_VERSION      1
#define VM_HEADER_SIZE  8

#define VM_YIELD_INTERVAL 1000  /* yield every N instructions */

/* ====== Opcodes ====== */

typedef enum {
    /* Control */
    OP_NOP      = 0x00,
    OP_HALT     = 0x01,

    /* Stack */
    OP_PUSH8    = 0x10,
    OP_PUSH16   = 0x11,
    OP_PUSH32   = 0x12,
    OP_DUP      = 0x13,
    OP_DROP     = 0x14,
    OP_SWAP     = 0x15,
    OP_OVER     = 0x16,
    OP_ROT      = 0x17,

    /* Arithmetic */
    OP_ADD      = 0x20,
    OP_SUB      = 0x21,
    OP_MUL      = 0x22,
    OP_DIV      = 0x23,
    OP_MOD      = 0x24,
    OP_NEG      = 0x25,

    /* Bitwise */
    OP_AND      = 0x28,
    OP_OR       = 0x29,
    OP_XOR      = 0x2A,
    OP_NOT      = 0x2B,
    OP_SHL      = 0x2C,
    OP_SHR      = 0x2D,

    /* Comparison */
    OP_EQ       = 0x30,
    OP_NE       = 0x31,
    OP_LT       = 0x32,
    OP_GT       = 0x33,
    OP_LE       = 0x34,
    OP_GE       = 0x35,

    /* Control flow (i16 relative offset) */
    OP_JMP      = 0x40,
    OP_JZ       = 0x41,
    OP_JNZ      = 0x42,
    OP_CALL     = 0x43,
    OP_RET      = 0x44,

    /* Local variables */
    OP_LOAD     = 0x50,
    OP_STORE    = 0x51,

    /* Syscalls */
    OP_SYSCALL  = 0x60,
} vm_opcode_t;

/* ====== Syscall numbers ====== */

#define SYS_PUTC        0
#define SYS_GETC        1
#define SYS_PUT_DEC     2
#define SYS_PUT_HEX     3
#define SYS_YIELD       4
#define SYS_DELAY       5
#define SYS_TICKS       6
#define SYS_GPIO_MODE   7
#define SYS_GPIO_WRITE  8
#define SYS_GPIO_READ   9
#define SYS_GPIO_TOGGLE 10

/* ====== VM state ====== */

typedef struct {
    /* Operand stack */
    uint32_t stack[VM_STACK_SIZE];
    uint16_t sp;

    /* Return stack (CALL/RET) */
    uint32_t rstack[VM_RSTACK_SIZE];
    uint16_t rsp;

    /* Local variables */
    uint32_t locals[VM_MAX_LOCALS];

    /* Program */
    uint8_t  *code;
    uint32_t code_size;
    uint32_t pc;

    /* State */
    uint8_t  running;
    int32_t  exit_code;
    uint32_t insn_count;
} vm_t;

/* ====== API ====== */

/* Initialize VM state to zeroes */
void vm_init(vm_t *vm);

/*
 * Load a .vm binary into the VM.
 * buf/size: raw file contents (header + bytecode).
 * Returns 0 on success, -1 on error (bad magic/version).
 * Note: buf must remain valid for the lifetime of vm_run.
 */
int vm_load(vm_t *vm, uint8_t *buf, uint32_t size);

/*
 * Run the loaded program until HALT or error.
 * Returns exit code (TOS at HALT) or negative on error.
 */
int vm_run(vm_t *vm);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_VM_H */
