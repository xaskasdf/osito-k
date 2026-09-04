/*
 * OsitoK — DOS Dynamic Binary Translation Engine
 *
 * Translates hot 8086 basic blocks into native x86-64 machine code.
 * Cold paths fall back to the interpreter (cpu8086.c).
 *
 * Pipeline:  8086 bytes → IR → x86-64 native → code cache
 *
 * The generated code uses System V AMD64 ABI: RDI = cpu8086_state_t*.
 * Inside each compiled block, RBX holds the cpu state pointer.
 */

#include "cpu8086.h"
#include "dos_jit.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Memory allocation */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Interrupt dispatch (dos_int.c) */
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);
extern void dos_transfer_to_native(dos_vm_t *vm);

/* ── cpu8086_state_t struct offsets (little-endian x86-64) ─────────
 *
 * union { uint32_t eax; ... };   offset  0
 * union { uint32_t ecx; ... };   offset  4
 * union { uint32_t edx; ... };   offset  8
 * union { uint32_t ebx; ... };   offset 12
 * union { uint32_t esp; ... };   offset 16
 * union { uint32_t ebp; ... };   offset 20
 * union { uint32_t esi; ... };   offset 24
 * union { uint32_t edi; ... };   offset 28
 * uint16_t cs;                   offset 32
 * uint16_t ds;                   offset 34
 * uint16_t es;                   offset 36
 * uint16_t ss;                   offset 38
 * uint16_t fs;                   offset 40
 * uint16_t gs;                   offset 42
 * union { uint32_t eip; ... };   offset 44
 * union { uint32_t eflags; ... };offset 48
 */

#define OFF_EAX     0
#define OFF_ECX     4
#define OFF_EDX     8
#define OFF_EBX     12
#define OFF_ESP     16
#define OFF_EBP     20
#define OFF_ESI     24
#define OFF_EDI     28
#define OFF_CS      32
#define OFF_DS      34
#define OFF_ES      36
#define OFF_SS      38
#define OFF_FS      40
#define OFF_GS      42
#define OFF_EIP     44
#define OFF_EFLAGS  48

/* 16-bit register offset: low 16 bits of the 32-bit register */
static const uint8_t reg16_offset[8] = {
    OFF_EAX, OFF_ECX, OFF_EDX, OFF_EBX,
    OFF_ESP, OFF_EBP, OFF_ESI, OFF_EDI
};

/* 8-bit register offsets: AL=0, CL=4, DL=8, BL=12, AH=1, CH=5, DH=9, BH=13 */
static const uint8_t reg8_offset[8] = {
    OFF_EAX + 0, OFF_ECX + 0, OFF_EDX + 0, OFF_EBX + 0,  /* AL CL DL BL */
    OFF_EAX + 1, OFF_ECX + 1, OFF_EDX + 1, OFF_EBX + 1   /* AH CH DH BH */
};

/* ── Helper: emit bytes to buffer ──────────────────────────────── */

static inline void emit8(uint8_t **buf, uint8_t b)
{
    *(*buf)++ = b;
}

static inline void emit16(uint8_t **buf, uint16_t v)
{
    *(*buf)++ = (uint8_t)(v & 0xFF);
    *(*buf)++ = (uint8_t)(v >> 8);
}

static inline void emit32(uint8_t **buf, uint32_t v)
{
    *(*buf)++ = (uint8_t)(v & 0xFF);
    *(*buf)++ = (uint8_t)((v >> 8) & 0xFF);
    *(*buf)++ = (uint8_t)((v >> 16) & 0xFF);
    *(*buf)++ = (uint8_t)((v >> 24) & 0xFF);
}

/* ── Memory helpers ────────────────────────────────────────────── */

static void jit_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = val;
}

/* ══════════════════════════════════════════════════════════════════
 * 1. jit_init — Initialize JIT state
 * ══════════════════════════════════════════════════════════════════ */

void jit_init(jit_state_t *jit)
{
    jit_memset(jit, 0, sizeof(jit_state_t));

    /* Allocate executable code cache — bare metal, all pages are RWX */
    uint64_t pages = (JIT_CACHE_SIZE + 4095) / 4096;
    jit->code_buf = (uint8_t *)mem_alloc_pages(pages);

    if (!jit->code_buf) {
        serial_puts("[JIT] FATAL: failed to allocate code cache\n");
        return;
    }

    jit->code_used = 0;
    jit->block_count = 0;
    jit->interpreted = 0;
    jit->jit_executed = 0;
    jit->jit_compiled = 0;

    serial_puts("[JIT] Initialized: code cache ");
    serial_putdec(JIT_CACHE_SIZE / 1024);
    serial_puts("KB at 0x");
    serial_puthex((uint64_t)jit->code_buf, 16);
    serial_puts("\n");
}

void jit_destroy(jit_state_t *jit)
{
    if (!jit) return;
    if (jit->code_buf) {
        mem_free_pages(jit->code_buf,
                       (JIT_CACHE_SIZE + 4095u) / 4096u);
        jit->code_buf = NULL;
    }
    jit->code_used = 0;
    jit->block_count = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * 2. jit_get_block — Look up or create a block for CS:IP
 * ══════════════════════════════════════════════════════════════════ */

jit_block_t *jit_get_block(jit_state_t *jit, uint16_t cs, uint16_t ip)
{
    /* Linear search for existing block */
    for (uint32_t i = 0; i < jit->block_count; i++) {
        if (jit->blocks[i].cs == cs && jit->blocks[i].ip == ip)
            return &jit->blocks[i];
    }

    /* Not found — allocate a new entry */
    if (jit->block_count >= JIT_MAX_BLOCKS) {
        serial_puts("[JIT] Block table full\n");
        return (jit_block_t *)0;
    }

    jit_block_t *blk = &jit->blocks[jit->block_count++];
    jit_memset(blk, 0, sizeof(jit_block_t));
    blk->cs = cs;
    blk->ip = ip;
    return blk;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. jit_decode_block — Decode 8086 bytes into IR
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: add an IR instruction */
static inline void ir_emit(jit_block_t *blk, ir_op_t op,
                           uint32_t a, uint32_t b, uint32_t c, uint8_t width)
{
    if (blk->ir_count >= IR_MAX_PER_BLOCK)
        return;
    ir_inst_t *inst = &blk->ir[blk->ir_count++];
    inst->op = op;
    inst->a = a;
    inst->b = b;
    inst->c = c;
    inst->width = width;
}

/* Decode a ModR/M byte and return the two register indices.
 * Only handles mod=3 (register-to-register). Returns 0 on success, -1 if mem. */
static int decode_modrm_reg(uint8_t modrm, uint8_t *reg, uint8_t *rm)
{
    uint8_t mod = (modrm >> 6) & 3;
    *reg = (modrm >> 3) & 7;
    *rm  = modrm & 7;
    if (mod != 3)
        return -1;  /* memory operand — bail out */
    return 0;
}

int jit_decode_block(dos_vm_t *vm, jit_block_t *block)
{
    uint32_t base = dos_linear(block->cs, block->ip);
    uint8_t *code = vm->mem + base;
    uint32_t max_len = 256;  /* max bytes per block */
    uint32_t pos = 0;
    uint8_t reg, rm;

    block->ir_count = 0;

    while (pos < max_len && block->ir_count < IR_MAX_PER_BLOCK) {
        uint32_t insn_start = pos;  /* byte offset of this instruction */
        uint8_t op = code[pos++];

        switch (op) {

        /* ── NOP ─────────────────────────────────────────── */
        case 0x90:
            ir_emit(block, IR_NOP, 0, 0, 0, 0);
            break;

        /* ── MOV reg8, imm8 (0xB0-0xB7) ─────────────────── */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            uint8_t r = op - 0xB0;
            uint8_t imm = code[pos++];
            ir_emit(block, IR_MOV_REG_IMM, r, imm, 0, 1);
            break;
        }

        /* ── MOV reg16, imm16 (0xB8-0xBF) ────────────────── */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            uint8_t r = op - 0xB8;
            uint16_t imm = code[pos] | ((uint16_t)code[pos + 1] << 8);
            pos += 2;
            ir_emit(block, IR_MOV_REG_IMM, r, imm, 0, 2);
            break;
        }

        /* ── MOV r/m16, r16 (0x89) ───────────────────────── */
        case 0x89: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                /* Memory operand — fall back to interpreter */
                pos -= 2;  /* rewind opcode + modrm */
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_MOV_REG_REG, rm, reg, 0, 2);
            break;
        }

        /* ── MOV r16, r/m16 (0x8B) ───────────────────────── */
        case 0x8B: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_MOV_REG_REG, reg, rm, 0, 2);
            break;
        }

        /* ── ADD r/m16, r16 (0x01) ───────────────────────── */
        case 0x01: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_ADD, rm, reg, 0, 2);
            break;
        }

        /* ── ADD r16, r/m16 (0x03) ───────────────────────── */
        case 0x03: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_ADD, reg, rm, 0, 2);
            break;
        }

        /* ── SUB r/m16, r16 (0x29) ───────────────────────── */
        case 0x29: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_SUB, rm, reg, 0, 2);
            break;
        }

        /* ── SUB r16, r/m16 (0x2B) ───────────────────────── */
        case 0x2B: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_SUB, reg, rm, 0, 2);
            break;
        }

        /* ── CMP r/m16, r16 (0x39) ───────────────────────── */
        case 0x39: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_CMP, rm, reg, 0, 2);
            break;
        }

        /* ── CMP r16, r/m16 (0x3B) ───────────────────────── */
        case 0x3B: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_CMP, reg, rm, 0, 2);
            break;
        }

        /* ── XOR r/m16, r16 (0x31) ───────────────────────── */
        case 0x31: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_XOR, rm, reg, 0, 2);
            break;
        }

        /* ── XOR r16, r/m16 (0x33) ───────────────────────── */
        case 0x33: {
            uint8_t modrm = code[pos++];
            if (decode_modrm_reg(modrm, &reg, &rm) < 0) {
                pos -= 2;
                ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
                goto done;
            }
            ir_emit(block, IR_XOR, reg, rm, 0, 2);
            break;
        }

        /* ── INC reg16 (0x40-0x47) ───────────────────────── */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            ir_emit(block, IR_INC, op - 0x40, 0, 0, 2);
            break;

        /* ── DEC reg16 (0x48-0x4F) ───────────────────────── */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            ir_emit(block, IR_DEC, op - 0x48, 0, 0, 2);
            break;

        /* ── PUSH reg16 (0x50-0x57) ──────────────────────── */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            /* c = byte offset within block (for codegen bailout IP) */
            ir_emit(block, IR_PUSH, op - 0x50, 0, insn_start, 2);
            break;

        /* ── POP reg16 (0x58-0x5F) ───────────────────────── */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            ir_emit(block, IR_POP, op - 0x58, 0, insn_start, 2);
            break;

        /* ── INT imm8 (0xCD) — terminates block ──────────── */
        case 0xCD: {
            uint8_t int_num = code[pos++];
            ir_emit(block, IR_INT, int_num, 0, 0, 0);
            goto done;
        }

        /* ── CALL near rel16 (0xE8) — terminates block ───── */
        case 0xE8: {
            int16_t rel = (int16_t)(code[pos] | ((uint16_t)code[pos + 1] << 8));
            pos += 2;
            /* Target IP = ip + pos (after this instruction) + rel */
            uint16_t target = (uint16_t)(block->ip + pos + rel);
            /* c = insn_start so codegen can bail to this instruction */
            ir_emit(block, IR_CALL_IMM, target, 0, insn_start, 2);
            goto done;
        }

        /* ── JMP near rel16 (0xE9) — terminates block ────── */
        case 0xE9: {
            int16_t rel = (int16_t)(code[pos] | ((uint16_t)code[pos + 1] << 8));
            pos += 2;
            uint16_t target = (uint16_t)(block->ip + pos + rel);
            ir_emit(block, IR_JMP_IMM, block->cs, target, 0, 2);
            goto done;
        }

        /* ── JMP short rel8 (0xEB) — terminates block ────── */
        case 0xEB: {
            int8_t rel = (int8_t)code[pos++];
            uint16_t target = (uint16_t)(block->ip + pos + rel);
            ir_emit(block, IR_JMP_IMM, block->cs, target, 0, 2);
            goto done;
        }

        /* ── RET near (0xC3) — terminates block ──────────── */
        case 0xC3:
            /* c = insn_start so codegen can bail to this instruction */
            ir_emit(block, IR_RET, 0, 0, insn_start, 0);
            goto done;

        /* ── JZ rel8 (0x74) — terminates block ───────────── */
        case 0x74: {
            int8_t rel = (int8_t)code[pos++];
            /* a = condition (0x74 = JZ), b = delta from block->ip+pos */
            ir_emit(block, IR_JCC, 0x74, (uint32_t)(uint16_t)(block->ip + pos + rel), 0, 2);
            goto done;
        }

        /* ── JNZ rel8 (0x75) — terminates block ──────────── */
        case 0x75: {
            int8_t rel = (int8_t)code[pos++];
            ir_emit(block, IR_JCC, 0x75, (uint32_t)(uint16_t)(block->ip + pos + rel), 0, 2);
            goto done;
        }

        /* ── Unknown opcode — fall back to interpreter ────── */
        default:
            pos--;  /* rewind: interpreter will re-decode */
            ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
            goto done;
        }
    }

done:
    block->length = (uint16_t)pos;
    return block->ir_count;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. jit_compile_block — Translate IR to x86-64 machine code
 * ══════════════════════════════════════════════════════════════════
 *
 * Generated function signature (System V ABI):
 *   uint32_t block_fn(cpu8086_state_t *cpu);  // cpu in RDI
 *
 * Returns: 0 = block done, continue dispatching
 *          >0 = INT number to handle
 *
 * Register convention inside generated code:
 *   RBX = cpu8086_state_t * (callee-saved, persists through block)
 */

/* Emit: mov dword ptr [rbx + off8], imm32
 * Encoding: C7 43 <off8> <imm32>  (if off8 fits in signed byte)
 *       or: C7 83 <off32> <imm32> (if larger offset) */
static void __attribute__((unused)) emit_mov_mem_imm32(uint8_t **buf, uint8_t off, uint32_t imm)
{
    emit8(buf, 0xC7);
    if (off < 128) {
        emit8(buf, 0x43);  /* ModR/M: [rbx + disp8], reg=0 */
        emit8(buf, off);
    } else {
        emit8(buf, 0x83);  /* ModR/M: [rbx + disp32], reg=0 */
        emit32(buf, off);
    }
    emit32(buf, imm);
}

/* Emit: mov word ptr [rbx + off8], imm16
 * Prefix 66h + C7 43 <off8> <imm16> */
static void emit_mov_mem_imm16(uint8_t **buf, uint8_t off, uint16_t imm)
{
    emit8(buf, 0x66);  /* operand size prefix */
    emit8(buf, 0xC7);
    if (off < 128) {
        emit8(buf, 0x43);
        emit8(buf, off);
    } else {
        emit8(buf, 0x83);
        emit32(buf, off);
    }
    emit16(buf, imm);
}

/* Emit: mov byte ptr [rbx + off8], imm8
 * Encoding: C6 43 <off8> <imm8> */
static void emit_mov_mem_imm8(uint8_t **buf, uint8_t off, uint8_t imm)
{
    emit8(buf, 0xC6);
    emit8(buf, 0x43);  /* ModR/M: [rbx + disp8] */
    emit8(buf, off);
    emit8(buf, imm);
}

/* Emit: mov eax, dword ptr [rbx + off8]
 * Encoding: 8B 43 <off8> */
static void emit_load_eax(uint8_t **buf, uint8_t off)
{
    emit8(buf, 0x8B);
    emit8(buf, 0x43);  /* ModR/M: eax, [rbx + disp8] */
    emit8(buf, off);
}

/* Emit: mov dword ptr [rbx + off8], eax
 * Encoding: 89 43 <off8> */
static void __attribute__((unused)) emit_store_eax(uint8_t **buf, uint8_t off)
{
    emit8(buf, 0x89);
    emit8(buf, 0x43);
    emit8(buf, off);
}

/* Emit: mov ax, word ptr [rbx + off8]  (with 66h prefix)
 * Encoding: 66 8B 43 <off8> */
static void emit_load_ax(uint8_t **buf, uint8_t off)
{
    emit8(buf, 0x66);
    emit8(buf, 0x8B);
    emit8(buf, 0x43);
    emit8(buf, off);
}

/* Emit: mov word ptr [rbx + off8], ax  (with 66h prefix)
 * Encoding: 66 89 43 <off8> */
static void emit_store_ax(uint8_t **buf, uint8_t off)
{
    emit8(buf, 0x66);
    emit8(buf, 0x89);
    emit8(buf, 0x43);
    emit8(buf, off);
}

int jit_compile_block(jit_state_t *jit, jit_block_t *block)
{
    if (!jit->code_buf)
        return -1;

    /* Check space in code cache — generous estimate: 64 bytes per IR inst */
    uint32_t est = block->ir_count * 64 + 32;  /* +32 for prologue/epilogue */
    if (jit->code_used + est > JIT_CACHE_SIZE) {
        serial_puts("[JIT] Code cache full, invalidating\n");
        jit_invalidate_all(jit);
    }

    uint8_t *start = jit->code_buf + jit->code_used;
    uint8_t *buf = start;

    /* ── Prologue ──────────────────────────────────────────────
     * push rbx
     * push r12
     * mov rbx, rdi           ; RBX = cpu state pointer
     */
    emit8(&buf, 0x53);                   /* push rbx */
    emit8(&buf, 0x41); emit8(&buf, 0x54); /* push r12 */
    emit8(&buf, 0x48); emit8(&buf, 0x89); emit8(&buf, 0xFB); /* mov rbx, rdi */

    /* Control-flow IR instructions (IR_INT, IR_EXIT_BLOCK, IR_JMP_IMM, etc.)
     * store the target/next IP in their fields at decode time.
     * For non-control IR, the exact per-instruction IP is not needed.
     * The block terminator always sets EIP before returning. */

    for (uint16_t i = 0; i < block->ir_count; i++) {
        ir_inst_t *ir = &block->ir[i];

        switch (ir->op) {

        /* ── IR_NOP ─────────────────────────────────────── */
        case IR_NOP:
            /* Emit nothing (or a real NOP if desired for debugging) */
            break;

        /* ── IR_MOV_REG_IMM ─────────────────────────────── */
        case IR_MOV_REG_IMM:
            if (ir->width == 1) {
                /* MOV byte ptr [rbx + reg8_off], imm8 */
                emit_mov_mem_imm8(&buf, reg8_offset[ir->a], (uint8_t)ir->b);
            } else {
                /* MOV word ptr [rbx + reg16_off], imm16
                 * We zero the upper 16 bits by writing a dword if needed,
                 * but for 8086 compat we just write the 16-bit value. */
                emit_mov_mem_imm16(&buf, reg16_offset[ir->a], (uint16_t)ir->b);
            }
            break;

        /* ── IR_MOV_REG_REG ─────────────────────────────── */
        case IR_MOV_REG_REG: {
            uint8_t dst_off = reg16_offset[ir->a];
            uint8_t src_off = reg16_offset[ir->b];
            /* mov ax, [rbx + src]; mov [rbx + dst], ax */
            emit_load_ax(&buf, src_off);
            emit_store_ax(&buf, dst_off);
            break;
        }

        /* ── IR_ADD ─────────────────────────────────────── */
        case IR_ADD: {
            uint8_t dst_off = reg16_offset[ir->a];
            uint8_t src_off = reg16_offset[ir->b];
            /* mov ax, [rbx + src_off] */
            emit_load_ax(&buf, src_off);
            /* add [rbx + dst_off], ax → 66 01 43 <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0x01);
            emit8(&buf, 0x43);
            emit8(&buf, dst_off);
            /* Save flags: pushfq; pop rax; mov [rbx + OFF_EFLAGS], eax */
            emit8(&buf, 0x9C);           /* pushfq */
            emit8(&buf, 0x58);           /* pop rax */
            /* Mask to keep only OF/SF/ZF/AF/PF/CF (low 12 bits) and merge */
            /* and eax, 0x08D5 — keeps OF SF ZF AF PF CF */
            emit8(&buf, 0x25);           /* and eax, imm32 */
            emit32(&buf, 0x08D5);
            /* Load existing eflags, clear those bits, OR in new ones */
            /* mov ecx, [rbx + OFF_EFLAGS] */
            emit8(&buf, 0x8B);
            emit8(&buf, 0x4B);
            emit8(&buf, OFF_EFLAGS);
            /* and ecx, ~0x08D5 */
            emit8(&buf, 0x81);
            emit8(&buf, 0xE1);           /* and ecx, imm32 */
            emit32(&buf, ~(uint32_t)0x08D5);
            /* or ecx, eax */
            emit8(&buf, 0x09);
            emit8(&buf, 0xC1);           /* or ecx, eax */
            /* mov [rbx + OFF_EFLAGS], ecx */
            emit8(&buf, 0x89);
            emit8(&buf, 0x4B);
            emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_SUB ─────────────────────────────────────── */
        case IR_SUB: {
            uint8_t dst_off = reg16_offset[ir->a];
            uint8_t src_off = reg16_offset[ir->b];
            emit_load_ax(&buf, src_off);
            /* sub [rbx + dst_off], ax → 66 29 43 <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0x29);
            emit8(&buf, 0x43);
            emit8(&buf, dst_off);
            /* Save flags */
            emit8(&buf, 0x9C);
            emit8(&buf, 0x58);
            emit8(&buf, 0x25); emit32(&buf, 0x08D5);
            emit8(&buf, 0x8B); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            emit8(&buf, 0x81); emit8(&buf, 0xE1); emit32(&buf, ~(uint32_t)0x08D5);
            emit8(&buf, 0x09); emit8(&buf, 0xC1);
            emit8(&buf, 0x89); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_CMP (flags only, no writeback) ──────────── */
        case IR_CMP: {
            uint8_t dst_off = reg16_offset[ir->a];
            uint8_t src_off = reg16_offset[ir->b];
            /* mov ax, [rbx + dst] */
            emit_load_ax(&buf, dst_off);
            /* cmp ax, [rbx + src] → 66 3B 43 <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0x3B);
            emit8(&buf, 0x43);
            emit8(&buf, src_off);
            /* Save flags */
            emit8(&buf, 0x9C);
            emit8(&buf, 0x58);
            emit8(&buf, 0x25); emit32(&buf, 0x08D5);
            emit8(&buf, 0x8B); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            emit8(&buf, 0x81); emit8(&buf, 0xE1); emit32(&buf, ~(uint32_t)0x08D5);
            emit8(&buf, 0x09); emit8(&buf, 0xC1);
            emit8(&buf, 0x89); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_XOR ─────────────────────────────────────── */
        case IR_XOR: {
            uint8_t dst_off = reg16_offset[ir->a];
            uint8_t src_off = reg16_offset[ir->b];
            emit_load_ax(&buf, src_off);
            /* xor [rbx + dst_off], ax → 66 31 43 <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0x31);
            emit8(&buf, 0x43);
            emit8(&buf, dst_off);
            /* Save flags */
            emit8(&buf, 0x9C);
            emit8(&buf, 0x58);
            emit8(&buf, 0x25); emit32(&buf, 0x08D5);
            emit8(&buf, 0x8B); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            emit8(&buf, 0x81); emit8(&buf, 0xE1); emit32(&buf, ~(uint32_t)0x08D5);
            emit8(&buf, 0x09); emit8(&buf, 0xC1);
            emit8(&buf, 0x89); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_INC ─────────────────────────────────────── */
        case IR_INC: {
            uint8_t off = reg16_offset[ir->a];
            /* inc word ptr [rbx + off] → 66 FF 43 <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0xFF);
            emit8(&buf, 0x43);  /* ModR/M: /0 [rbx + disp8] */
            emit8(&buf, off);
            /* Save flags (INC doesn't affect CF, but pushfq captures all) */
            emit8(&buf, 0x9C);
            emit8(&buf, 0x58);
            emit8(&buf, 0x25); emit32(&buf, 0x08D5);
            emit8(&buf, 0x8B); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            emit8(&buf, 0x81); emit8(&buf, 0xE1); emit32(&buf, ~(uint32_t)0x08D5);
            emit8(&buf, 0x09); emit8(&buf, 0xC1);
            emit8(&buf, 0x89); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_DEC ─────────────────────────────────────── */
        case IR_DEC: {
            uint8_t off = reg16_offset[ir->a];
            /* dec word ptr [rbx + off] → 66 FF 4B <off> */
            emit8(&buf, 0x66);
            emit8(&buf, 0xFF);
            emit8(&buf, 0x4B);  /* ModR/M: /1 [rbx + disp8] */
            emit8(&buf, off);
            /* Save flags */
            emit8(&buf, 0x9C);
            emit8(&buf, 0x58);
            emit8(&buf, 0x25); emit32(&buf, 0x08D5);
            emit8(&buf, 0x8B); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            emit8(&buf, 0x81); emit8(&buf, 0xE1); emit32(&buf, ~(uint32_t)0x08D5);
            emit8(&buf, 0x09); emit8(&buf, 0xC1);
            emit8(&buf, 0x89); emit8(&buf, 0x4B); emit8(&buf, OFF_EFLAGS);
            break;
        }

        /* ── IR_PUSH ────────────────────────────────────── */
        case IR_PUSH: {
            /* PUSH requires writing to emulated SS:SP memory.
             * Bail to interpreter — set IP to this PUSH instruction
             * so the interpreter re-executes it. ir->c = byte offset. */
            emit_mov_mem_imm16(&buf, OFF_EIP,
                               (uint16_t)(block->ip + ir->c));
            emit8(&buf, 0x31); emit8(&buf, 0xC0);  /* xor eax, eax */
            emit8(&buf, 0x41); emit8(&buf, 0x5C);  /* pop r12 */
            emit8(&buf, 0x5B);                       /* pop rbx */
            emit8(&buf, 0xC3);                       /* ret */
            goto compile_done;
        }

        /* ── IR_POP ─────────────────────────────────────── */
        case IR_POP: {
            /* POP requires reading from emulated SS:SP memory.
             * Bail to interpreter — set IP to this POP instruction. */
            emit_mov_mem_imm16(&buf, OFF_EIP,
                               (uint16_t)(block->ip + ir->c));
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }

        /* ── IR_INT ─────────────────────────────────────── */
        case IR_INT: {
            /* Update IP to point past the INT instruction.
             * block->ip + block->length is the end of the block,
             * and INT always terminates the block so that is correct. */
            emit_mov_mem_imm16(&buf, OFF_EIP, block->ip + block->length);
            /* Return int number in EAX */
            emit8(&buf, 0xB8);              /* mov eax, imm32 */
            emit32(&buf, ir->a);
            /* Epilogue */
            emit8(&buf, 0x41); emit8(&buf, 0x5C);  /* pop r12 */
            emit8(&buf, 0x5B);                       /* pop rbx */
            emit8(&buf, 0xC3);                       /* ret */
            goto compile_done;
        }

        /* ── IR_JMP_IMM ─────────────────────────────────── */
        case IR_JMP_IMM: {
            /* Set CS:IP and return 0 */
            emit_mov_mem_imm16(&buf, OFF_CS, (uint16_t)ir->a);
            emit_mov_mem_imm16(&buf, OFF_EIP, (uint16_t)ir->b);
            emit8(&buf, 0x31); emit8(&buf, 0xC0);  /* xor eax, eax */
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }

        /* ── IR_JCC (JZ/JNZ) ───────────────────────────── */
        case IR_JCC: {
            /* Load emulated flags into native flags via:
             * mov eax, [rbx + OFF_EFLAGS]; push rax; popfq */
            emit_load_eax(&buf, OFF_EFLAGS);
            emit8(&buf, 0x50);              /* push rax */
            emit8(&buf, 0x9D);              /* popfq */

            /* Taken path: set IP to branch target, return 0 */
            /* Not-taken path: set IP to fall-through, return 0 */
            uint16_t taken_ip = (uint16_t)ir->b;
            uint16_t fallthru_ip = block->ip + block->length;

            if (ir->a == 0x74) {
                /* JZ: jnz over_taken (skip taken path if NZ) */
                emit8(&buf, 0x75);          /* JNZ rel8 */
                /* Skip distance = taken path bytes:
                 * emit_mov_mem_imm16(OFF_EIP, taken_ip) = 6 bytes
                 * xor eax,eax = 2, pop r12 = 2, pop rbx = 1, ret = 1 → 12 */
                emit8(&buf, 12);
            } else {
                /* JNZ: jz over_taken (skip taken path if Z) */
                emit8(&buf, 0x74);          /* JZ rel8 */
                emit8(&buf, 12);
            }

            /* Taken path */
            emit_mov_mem_imm16(&buf, OFF_EIP, taken_ip);
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);

            /* Not-taken (fall through) */
            emit_mov_mem_imm16(&buf, OFF_EIP, fallthru_ip);
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }

        /* ── IR_CALL_IMM ────────────────────────────────── */
        case IR_CALL_IMM: {
            /* CALL needs to push return address onto 8086 stack.
             * That requires memory write — bail to interpreter.
             * Set IP to the CALL instruction itself (ir->c = offset). */
            emit_mov_mem_imm16(&buf, OFF_EIP,
                               (uint16_t)(block->ip + ir->c));
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }

        /* ── IR_RET ─────────────────────────────────────── */
        case IR_RET: {
            /* RET pops IP from 8086 stack — needs memory read. Bail.
             * Set IP to the RET instruction (ir->c = offset). */
            emit_mov_mem_imm16(&buf, OFF_EIP,
                               (uint16_t)(block->ip + ir->c));
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }

        /* ── IR_EXIT_BLOCK ──────────────────────────────── */
        case IR_EXIT_BLOCK: {
            /* Set IP to current position, return 0 to resume interpreter */
            /* The decoder set pos to point at the unrecognized opcode,
             * so block->ip + block->length is the right restart point. */
            emit_mov_mem_imm16(&buf, OFF_EIP, block->ip + block->length);
            emit8(&buf, 0x31); emit8(&buf, 0xC0);  /* xor eax, eax */
            emit8(&buf, 0x41); emit8(&buf, 0x5C);  /* pop r12 */
            emit8(&buf, 0x5B);                       /* pop rbx */
            emit8(&buf, 0xC3);                       /* ret */
            goto compile_done;
        }

        default:
            /* Unknown IR op — shouldn't happen. Bail. */
            emit_mov_mem_imm16(&buf, OFF_EIP, block->ip + block->length);
            emit8(&buf, 0x31); emit8(&buf, 0xC0);
            emit8(&buf, 0x41); emit8(&buf, 0x5C);
            emit8(&buf, 0x5B);
            emit8(&buf, 0xC3);
            goto compile_done;
        }
    }

    /* If we ran through all IR without a terminating instruction,
     * emit a default epilogue: update IP, return 0. */
    emit_mov_mem_imm16(&buf, OFF_EIP, block->ip + block->length);
    emit8(&buf, 0x31); emit8(&buf, 0xC0);  /* xor eax, eax */
    emit8(&buf, 0x41); emit8(&buf, 0x5C);  /* pop r12 */
    emit8(&buf, 0x5B);                       /* pop rbx */
    emit8(&buf, 0xC3);                       /* ret */

compile_done:
    block->native_code = start;
    block->native_size = (uint32_t)(buf - start);
    block->compiled = true;

    jit->code_used += block->native_size;
    jit->jit_compiled++;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * 5. jit_exec_block — Execute a compiled block
 * ══════════════════════════════════════════════════════════════════ */

typedef uint32_t (*jit_func_t)(cpu8086_state_t *cpu);

void jit_exec_block(dos_vm_t *vm, jit_block_t *block)
{
    if (!block->compiled || !block->native_code)
        return;

    jit_func_t fn = (jit_func_t)block->native_code;
    uint32_t result = fn(vm->cpu);

    block->exec_count++;

    if (result > 0) {
        /* result is an INT number — dispatch it */
        cpu8086_state_t *cpu = vm->cpu;
        uint16_t saved_cs = cpu->cs;
        uint32_t saved_eip = cpu->eip;
        uint32_t saved_eflags = cpu->eflags;

        if (cpu_deliver_pm_software_interrupt(vm, (uint8_t)result,
                                              saved_eip)) {
            if (cpu->op_size_32)
                dos_transfer_to_native(vm);
            return;
        }

        /* Mirror the real-mode CPU frame built by the interpreter. */
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
        cpu_push16(cpu, cpu->cs);
        cpu_push16(cpu, cpu->ip);
        cpu->flags &= ~(FLAG_IF | FLAG_TF);

        uint8_t previous_frame_bytes = vm->software_int_frame_bytes;
        uint32_t previous_return_flags = vm->software_int_return_flags;
        vm->software_int_frame_bytes = 6;
        vm->software_int_return_flags = saved_eflags;
        dos_int_dispatch(vm, (uint8_t)result);
        vm->software_int_frame_bytes = previous_frame_bytes;
        vm->software_int_return_flags = previous_return_flags;

        if (cpu->cs == saved_cs && cpu->eip == saved_eip) {
            cpu_stack_adjust(cpu, 6);
            const uint32_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF |
                                          FLAG_ZF | FLAG_SF | FLAG_OF;
            cpu->eflags = (saved_eflags & ~status_flags) |
                          (cpu->eflags & status_flags) | FLAGS_FIXED;
            if (cpu->protected_mode && vm->dpmi.active)
                cpu->flags |= FLAG_IF;
        } else {
            cpu8086_sync_cs(cpu);
            if (cpu->protected_mode && cpu->op_size_32)
                dos_transfer_to_native(vm);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 6. jit_invalidate_all — Flush all cached blocks
 * ══════════════════════════════════════════════════════════════════ */

void jit_invalidate_all(jit_state_t *jit)
{
    jit->block_count = 0;
    jit->code_used = 0;

    for (uint32_t i = 0; i < 65536; i++)
        jit->hit_count[i] = 0;

    serial_puts("[JIT] Cache invalidated\n");
}

/* ══════════════════════════════════════════════════════════════════
 * 7. jit_print_stats — Print JIT statistics
 * ══════════════════════════════════════════════════════════════════ */

void jit_print_stats(jit_state_t *jit)
{
    serial_puts("[JIT] Stats:\n");
    serial_puts("  Blocks compiled: ");
    serial_putdec(jit->jit_compiled);
    serial_puts("\n");
    serial_puts("  Blocks in cache: ");
    serial_putdec(jit->block_count);
    serial_puts("\n");
    serial_puts("  Code cache used: ");
    serial_putdec(jit->code_used);
    serial_puts(" / ");
    serial_putdec(JIT_CACHE_SIZE);
    serial_puts(" bytes\n");
    serial_puts("  Interpreted:     ");
    serial_putdec(jit->interpreted);
    serial_puts("\n");
    serial_puts("  JIT executed:    ");
    serial_putdec(jit->jit_executed);
    serial_puts("\n");
}
