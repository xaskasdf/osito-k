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
#include "dos_hostmem.h"
#include "dos_jit.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Interrupt dispatch (dos_int.c) */
extern void dos_transfer_to_native(dos_vm_t *vm);

#define JIT_EXIT_INTERPRET 0x100u
#define JIT_EXIT_INTERRUPT 0x200u

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

enum { JIT_LOOKUP_LRU, JIT_CODE_LRU };

_Static_assert((JIT_CODE_UNITS & (JIT_CODE_UNITS - 1u)) == 0,
               "JIT code arena must contain a power of two size classes");
_Static_assert(2u * JIT_CODE_UNITS <= 65536u && JIT_MAX_BLOCKS < 65536u,
               "JIT cache links use 16-bit indices");

static uint32_t jit_hash(uint16_t cs, uint32_t ip)
{
    return (ip ^ (ip >> 12) ^ ((uint32_t)cs * 0x9E37u)) &
           (JIT_MAX_BLOCKS - 1u);
}

static void jit_lru_remove(jit_state_t *jit, jit_block_t *block, unsigned list)
{
    jit_links_t *links = &block->links[list];
    if (links->prev) jit->blocks[links->prev - 1u].links[list].next = links->next;
    else jit->lru_head[list] = links->next;
    if (links->next) jit->blocks[links->next - 1u].links[list].prev = links->prev;
    else jit->lru_tail[list] = links->prev;
    links->prev = links->next = 0;
}

static void jit_lru_touch(jit_state_t *jit, jit_block_t *block, unsigned list)
{
    uint16_t index = (uint16_t)(block - jit->blocks + 1u);
    if (jit->lru_head[list] == index) return;
    jit_links_t *links = &block->links[list];
    if (links->prev || links->next || jit->lru_tail[list] == index)
        jit_lru_remove(jit, block, list);
    links->next = jit->lru_head[list];
    if (links->next) jit->blocks[links->next - 1u].links[list].prev = index;
    else jit->lru_tail[list] = index;
    jit->lru_head[list] = index;
}

static uint8_t jit_code_order(uint32_t size)
{
    uint8_t order = 1;
    for (uint32_t capacity = JIT_CODE_MIN; capacity < size; capacity <<= 1)
        order++;
    return order;
}

/* Each node records the largest free buddy order in its subtree, plus one.
 * Zero marks an unavailable subtree. Metadata never resides in executable code. */
static void jit_code_update(jit_state_t *jit, uint16_t node, uint8_t order)
{
    while (node > 1u) {
        node >>= 1;
        uint8_t left = jit->code_tree[node * 2u];
        uint8_t right = jit->code_tree[node * 2u + 1u];
        jit->code_tree[node] = left == order && right == order ? order + 1u :
                              left > right ? left : right;
        order++;
    }
}

static void jit_release_code(jit_block_t *block)
{
    jit_state_t *jit = block->owner;
    if (jit && block->native_capacity) {
        jit_lru_remove(jit, block, JIT_CODE_LRU);
        uint8_t order = jit_code_order(block->native_capacity);
        jit->code_tree[block->code_node] = order;
        jit_code_update(jit, block->code_node, order);
        jit->code_used -= block->native_capacity;
        jit->code_payload -= block->native_size;
    }
    block->compiled = false;
    block->native_code = NULL;
    block->native_size = block->native_capacity = block->code_node = 0;
}

static bool jit_allocate_code(jit_state_t *jit, jit_block_t *block, uint32_t size)
{
    if (!size || size > JIT_CACHE_SIZE) return false;
    uint8_t order = jit_code_order(size);
    while (jit->code_tree[1] < order) {
        uint16_t victim = jit->lru_tail[JIT_CODE_LRU];
        if (!victim) return false;
        jit_release_code(&jit->blocks[victim - 1u]);
        jit->code_evictions++;
    }
    uint32_t capacity = JIT_CACHE_SIZE, offset = 0;
    uint16_t node = 1;
    uint8_t level = jit_code_order(capacity);
    while (level > order) {
        node *= 2u;
        capacity >>= 1;
        level--;
        if (jit->code_tree[node] < order) {
            node++;
            offset += capacity;
        }
    }
    jit->code_tree[node] = 0;
    jit_code_update(jit, node, order);
    block->code_node = node;
    block->native_code = jit->code_buf + offset;
    block->native_capacity = capacity;
    block->native_size = size;
    jit->code_used += capacity;
    jit->code_payload += size;
    jit_lru_touch(jit, block, JIT_CODE_LRU);
    return true;
}

static void jit_cache_clear(jit_state_t *jit)
{
    for (uint32_t i = 0; i < jit->block_count; i++) {
        jit_block_t *block = &jit->blocks[i];
        block->owner = NULL;
        block->compiled = block->source_valid = false;
        block->native_code = NULL;
        block->native_size = block->native_capacity = block->code_node = 0;
    }
    jit_memset(jit->buckets, 0, sizeof(jit->buckets));
    jit_memset(jit->lru_head, 0, sizeof(jit->lru_head));
    jit_memset(jit->lru_tail, 0, sizeof(jit->lru_tail));
    jit_memset(jit->hit_count, 0, sizeof(jit->hit_count));
    jit->block_count = jit->code_used = jit->code_payload = 0;
    jit->code_tree[0] = 0;
    jit->code_tree[1] = jit_code_order(JIT_CACHE_SIZE);
    for (unsigned i = 2; i < 2u * JIT_CODE_UNITS; i++)
        jit->code_tree[i] = jit->code_tree[i >> 1] - 1u;
}

static void jit_remove_entry(jit_state_t *jit, jit_block_t *block)
{
    uint16_t index = (uint16_t)(block - jit->blocks + 1u);
    uint16_t *link = &jit->buckets[jit_hash(block->cs, block->ip)];
    while (*link && *link != index) link = &jit->blocks[*link - 1u].hash_next;
    if (*link) *link = block->hash_next;
    jit_lru_remove(jit, block, JIT_LOOKUP_LRU);
    jit_release_code(block);
}

void jit_init(jit_state_t *jit)
{
    jit_memset(jit, 0, sizeof(jit_state_t));
    jit_cache_clear(jit);

    /* The kernel direct map currently permits execution of this code cache. */
    uint64_t pages = (JIT_CACHE_SIZE + 4095) / 4096;
    jit->code_buf = (uint8_t *)dos_host_alloc_pages(pages);

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
    jit_cache_clear(jit);
    if (jit->code_buf) {
        dos_host_free_pages(jit->code_buf,
                             (JIT_CACHE_SIZE + 4095u) / 4096u);
        jit->code_buf = NULL;
    }
    jit->code_used = 0;
    jit->block_count = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * 2. jit_get_block — Look up or create a block for CS:IP
 * ══════════════════════════════════════════════════════════════════ */

jit_block_t *jit_get_block(jit_state_t *jit, uint16_t cs, uint32_t ip)
{
    uint32_t hash = jit_hash(cs, ip);
    for (uint16_t link = jit->buckets[hash]; link; ) {
        jit_block_t *block = &jit->blocks[link - 1u];
        if (block->cs == cs && block->ip == ip) {
            jit_lru_touch(jit, block, JIT_LOOKUP_LRU);
            if (block->native_capacity) jit_lru_touch(jit, block, JIT_CODE_LRU);
            return block;
        }
        link = block->hash_next;
    }

    jit_block_t *blk;
    if (jit->block_count >= JIT_MAX_BLOCKS) {
        blk = &jit->blocks[jit->lru_tail[JIT_LOOKUP_LRU] - 1u];
        jit_remove_entry(jit, blk);
        jit->lookup_evictions++;
    } else {
        blk = &jit->blocks[jit->block_count++];
    }

    jit_memset(blk, 0, sizeof(jit_block_t));
    blk->owner = jit;
    blk->cs = cs;
    blk->ip = ip;
    blk->hash_next = jit->buckets[hash];
    jit->buckets[hash] = (uint16_t)(blk - jit->blocks + 1u);
    jit_lru_touch(jit, blk, JIT_LOOKUP_LRU);
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

static bool jit_source_region(dos_vm_t *vm, uint16_t cs, uint32_t ip,
                               uint32_t *base, uint32_t *limit, bool *code32)
{
    if (!vm || !vm->cpu || !vm->mem || (vm->cpu->cr0 & 0x80000000u))
        return false;
    const cpu8086_state_t *cpu = vm->cpu;
    const dpmi_descriptor_t *descriptor = &cpu->cs_cache.descriptor;
    if (cs != cpu->cs || !cpu->cs_cache.valid || (descriptor->flags_lim & 0x20u))
        return false;
    *base = dpmi_desc_get_base(descriptor);
    *limit = dpmi_desc_get_limit(descriptor);
    *code32 = cpu->op_size_32;
    /* Do not batch across the interpreter's 16-bit fetch boundary. */
    if (!*code32 && *limit > 0xFFFFu) *limit = 0xFFFFu;
    uint64_t linear = (uint64_t)*base + ip;
    /* Read-ahead must not load VGA latches or cross a banked EMS window.
     * These sources keep the interpreter's per-access device semantics. */
    static const uint32_t windows[][2] = {
        { DOS_VGA_APERTURE_BASE, DOS_VGA_APERTURE_SIZE },
        { DOS_EMS_PAGE_FRAME_BASE, DOS_EMS_FRAME_PAGES * DOS_EMS_PAGE_SIZE }
    };
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        if (linear >= windows[i][0] &&
            linear < (uint64_t)windows[i][0] + windows[i][1]) return false;
        if (linear < windows[i][0]) {
            uint32_t last = windows[i][0] - *base - 1u;
            if (*limit > last) *limit = last;
        }
    }
    return ip <= *limit && linear < vm->total_mem_size;
}

bool jit_block_current(dos_vm_t *vm, const jit_block_t *block)
{
    /* Hand-built IR used by the kernel tests has no guest source. */
    if (!block->source_valid) return true;
    uint32_t base, limit;
    bool code32;
    if (block->protected_mode != vm->cpu->protected_mode ||
        !jit_source_region(vm, block->cs, block->ip, &base, &limit, &code32) ||
        base != block->source_base || limit != block->source_limit ||
        code32 != block->code32 || !block->source_size ||
        (uint64_t)block->ip + block->source_size - 1u > limit ||
        (uint64_t)base + block->ip + block->source_size > vm->total_mem_size)
        return false;
    for (unsigned i = 0; i < block->source_size; i++)
        if (block->source[i] != dos_mem_read8(vm, base + block->ip + i))
            return false;
    return true;
}

int jit_decode_block(dos_vm_t *vm, jit_block_t *block)
{
    uint32_t base, limit;
    bool code32;
    jit_release_code(block);
    block->source_valid = false;
    block->ir_count = block->instruction_count = block->length = 0;
    if (!jit_source_region(vm, block->cs, block->ip, &base, &limit, &code32))
        return -1;

    uint64_t available = (uint64_t)limit - block->ip + 1u;
    uint64_t backing = vm->total_mem_size - ((uint64_t)base + block->ip);
    if (available > backing) available = backing;
    if (available > sizeof(block->source)) available = sizeof(block->source);
    for (unsigned i = 0; i < available; i++)
        block->source[i] = dos_mem_read8(vm, base + block->ip + i);
    block->source_base = base;
    block->source_limit = limit;
    block->code32 = code32;
    block->protected_mode = vm->cpu->protected_mode;
    const uint8_t *code = block->source;
    uint32_t pos = 0, inspected = 0, insn_start = 0;

    while (pos < available && block->ir_count < IR_MAX_PER_BLOCK - 1u) {
        insn_start = pos;
        bool operand32 = code32, terminate = false;
        uint8_t op, reg, rm;
#define JIT_NEED(n) do { \
    if ((n) > available - pos || pos - insn_start + (n) > 15u) \
        goto fallback; \
} while (0)
        for (;;) {
            JIT_NEED(1u);
            op = code[pos++];
            if (op == 0x66) operand32 = !code32;
            else if (op != 0x67 && op != 0x26 && op != 0x2E &&
                     op != 0x36 && op != 0x3E && op != 0x64 && op != 0x65)
                break;
        }
        uint8_t width = operand32 ? 4 : 2;
        switch (op) {
        case 0x90:
            ir_emit(block, IR_NOP, 0, 0, 0, 0);
            break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            JIT_NEED(1u);
            ir_emit(block, IR_MOV_REG_IMM, op - 0xB0, code[pos++], 0, 1);
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            JIT_NEED(width);
            uint32_t imm = 0;
            for (unsigned i = 0; i < width; i++)
                imm |= (uint32_t)code[pos++] << (8u * i);
            ir_emit(block, IR_MOV_REG_IMM, op - 0xB8, imm, 0, width);
            break;
        }
        case 0x89: case 0x8B: case 0x01: case 0x03:
        case 0x29: case 0x2B: case 0x39: case 0x3B:
        case 0x31: case 0x33: case 0x21: case 0x23:
        case 0x09: case 0x0B: {
            JIT_NEED(1u);
            if (decode_modrm_reg(code[pos++], &reg, &rm) < 0) goto fallback;
            ir_op_t operation;
            switch (op & ~2u) {
            case 0x89: operation = IR_MOV_REG_REG; break;
            case 0x01: operation = IR_ADD; break;
            case 0x29: operation = IR_SUB; break;
            case 0x39: operation = IR_CMP; break;
            case 0x31: operation = IR_XOR; break;
            case 0x21: operation = IR_AND; break;
            default: operation = IR_OR; break;
            }
            ir_emit(block, operation, (op & 2u) ? reg : rm,
                    (op & 2u) ? rm : reg, 0, width);
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            ir_emit(block, op < 0x48 ? IR_INC : IR_DEC, op & 7u, 0, 0, width);
            break;
        case 0xE9: case 0xEB: case 0x74: case 0x75: {
            /* Prefixed transfers remain in the interpreter. */
            if (pos != insn_start + 1u) goto fallback;
            unsigned size = op == 0xE9 ? width : 1u;
            JIT_NEED(size);
            uint32_t raw = 0;
            for (unsigned i = 0; i < size; i++)
                raw |= (uint32_t)code[pos++] << (8u * i);
            int32_t rel = size == 1 ? (int8_t)raw :
                          size == 2 ? (int16_t)raw : (int32_t)raw;
            uint32_t target = block->ip + pos + (uint32_t)rel;
            if (!code32) target = (uint16_t)target;
            if (target > limit) goto fallback;
            ir_emit(block, op == 0x74 || op == 0x75 ? IR_JCC : IR_JMP_IMM,
                    op == 0x74 || op == 0x75 ? op : block->cs,
                    target, 0, width);
            terminate = true;
            break;
        }
        case 0xCD:
            if (block->protected_mode || pos != insn_start + 1u) goto fallback;
            JIT_NEED(1u);
            ir_emit(block, IR_INT, code[pos++], 0, 0, 0);
            terminate = true;
            break;
        default:
            /* Stack, FLAGS, memory, I/O and mode changes keep their handlers. */
            goto fallback;
        }
        block->instruction_count++;
        inspected = pos;
        if (terminate) goto done;
    }
    goto done;

fallback:
    /* Preserve prefixes for the interpreter, but include inspected bytes in
     * the cache key so a modified terminator cannot reuse stale IR. */
    inspected = pos;
    if (inspected < available) inspected++;
    pos = insn_start;
    ir_emit(block, IR_EXIT_BLOCK, 0, 0, 0, 0);
done:
    block->length = (uint16_t)pos;
    block->source_size = (uint16_t)inspected;
    block->source_valid = inspected != 0;
#undef JIT_NEED
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
static void __attribute__((unused)) emit_load_eax(uint8_t **buf, uint8_t off)
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

static void emit_guest_ip(uint8_t **buf, const jit_block_t *block, uint32_t ip)
{
    if (block->protected_mode) emit_mov_mem_imm32(buf, OFF_EIP, ip);
    else emit_mov_mem_imm16(buf, OFF_EIP, (uint16_t)ip);
}

static void emit_block_return(uint8_t **buf, uint32_t result)
{
    emit8(buf, 0xB8); emit32(buf, result);
    emit8(buf, 0x41); emit8(buf, 0x5C); /* pop r12 */
    emit8(buf, 0x5B);                  /* pop rbx */
    emit8(buf, 0xC3);
}

static void emit_status_flags(uint8_t **buf, uint32_t mask)
{
    /* Only arithmetic status is imported; guest IF/DF/TF/IOPL remain data. */
    emit8(buf, 0x9C); emit8(buf, 0x58); /* pushfq; pop rax */
    emit8(buf, 0x25); emit32(buf, mask);
    emit8(buf, 0x8B); emit8(buf, 0x4B); emit8(buf, OFF_EFLAGS);
    emit8(buf, 0x81); emit8(buf, 0xE1); emit32(buf, ~mask);
    emit8(buf, 0x09); emit8(buf, 0xC1);
    emit8(buf, 0x89); emit8(buf, 0x4B); emit8(buf, OFF_EFLAGS);
}

int jit_compile_block(jit_state_t *jit, jit_block_t *block)
{
    if (block->owner != jit) return -1;
    jit_release_code(block);
    if (!jit->code_buf || !block->ir_count || block->ir_count > IR_MAX_PER_BLOCK)
        return -1;

    /* Emit before allocating, so a generous size estimate cannot evict code.
     * Generated blocks contain no host calls or links to other cached blocks. */
    uint8_t *start = jit->emit_buf, *buf = start;
    uint32_t next = block->ip + block->length;
    if (!block->code32) next = (uint16_t)next;
    emit8(&buf, 0x53);
    emit8(&buf, 0x41); emit8(&buf, 0x54);
    emit8(&buf, 0x48); emit8(&buf, 0x89); emit8(&buf, 0xFB);

    for (uint16_t i = 0; i < block->ir_count; i++) {
        const ir_inst_t *ir = &block->ir[i];
        switch (ir->op) {
        case IR_NOP:
            break;
        case IR_MOV_REG_IMM:
            if (ir->width == 1)
                emit_mov_mem_imm8(&buf, reg8_offset[ir->a], (uint8_t)ir->b);
            else if (ir->width == 4)
                emit_mov_mem_imm32(&buf, reg16_offset[ir->a], ir->b);
            else
                emit_mov_mem_imm16(&buf, reg16_offset[ir->a], (uint16_t)ir->b);
            break;
        case IR_MOV_REG_REG:
            if (ir->width == 4) {
                emit_load_eax(&buf, reg16_offset[ir->b]);
                emit_store_eax(&buf, reg16_offset[ir->a]);
            } else {
                emit_load_ax(&buf, reg16_offset[ir->b]);
                emit_store_ax(&buf, reg16_offset[ir->a]);
            }
            break;
        case IR_ADD: case IR_SUB: case IR_CMP:
        case IR_XOR: case IR_AND: case IR_OR: {
            uint8_t operation = ir->op == IR_ADD ? 0x01 :
                                ir->op == IR_SUB ? 0x29 :
                                ir->op == IR_CMP ? 0x39 :
                                ir->op == IR_XOR ? 0x31 :
                                ir->op == IR_AND ? 0x21 : 0x09;
            if (ir->width == 4) emit_load_eax(&buf, reg16_offset[ir->b]);
            else emit_load_ax(&buf, reg16_offset[ir->b]);
            if (ir->width == 2) emit8(&buf, 0x66);
            emit8(&buf, operation);
            emit8(&buf, 0x43); emit8(&buf, reg16_offset[ir->a]);
            emit_status_flags(&buf, 0x08D5u);
            break;
        }
        case IR_INC: case IR_DEC:
            if (ir->width == 2) emit8(&buf, 0x66);
            emit8(&buf, 0xFF);
            emit8(&buf, ir->op == IR_INC ? 0x43 : 0x4B);
            emit8(&buf, reg16_offset[ir->a]);
            emit_status_flags(&buf, 0x08D4u); /* Preserve guest CF. */
            break;
        case IR_PUSH: case IR_POP: case IR_CALL_IMM: case IR_RET:
            emit_guest_ip(&buf, block, block->ip + ir->c);
            emit_block_return(&buf, JIT_EXIT_INTERPRET);
            goto compile_done;
        case IR_INT:
            emit_guest_ip(&buf, block, next);
            emit_block_return(&buf, JIT_EXIT_INTERRUPT | ir->a);
            goto compile_done;
        case IR_JMP_IMM:
            emit_mov_mem_imm16(&buf, OFF_CS, (uint16_t)ir->a);
            emit_guest_ip(&buf, block, ir->b);
            emit_block_return(&buf, 0);
            goto compile_done;
        case IR_JCC: {
            emit8(&buf, 0xF7); emit8(&buf, 0x43); emit8(&buf, OFF_EFLAGS);
            emit32(&buf, FLAG_ZF);
            emit8(&buf, ir->a == 0x74 ? 0x74 : 0x75);
            uint8_t *skip = buf++;
            emit_guest_ip(&buf, block, ir->b);
            emit_block_return(&buf, 0);
            *skip = (uint8_t)(buf - (skip + 1));
            emit_guest_ip(&buf, block, next);
            emit_block_return(&buf, 0);
            goto compile_done;
        }
        case IR_EXIT_BLOCK:
            emit_guest_ip(&buf, block, next);
            emit_block_return(&buf, JIT_EXIT_INTERPRET);
            goto compile_done;
        default:
            return -1;
        }
    }
    emit_guest_ip(&buf, block, next);
    emit_block_return(&buf, 0);

compile_done: {
    uint32_t size = (uint32_t)(buf - start);
    if (!jit_allocate_code(jit, block, size)) return -1;
    for (uint32_t i = 0; i < size; i++) block->native_code[i] = start[i];
    block->compiled = true;
    jit->jit_compiled++;
    return 0;
}
}

/* ══════════════════════════════════════════════════════════════════
 * 5. jit_exec_block — Execute a compiled block
 * ══════════════════════════════════════════════════════════════════ */

typedef uint32_t (*jit_func_t)(cpu8086_state_t *cpu);

bool jit_exec_block(dos_vm_t *vm, jit_block_t *block)
{
    if (!block->compiled || !block->native_code || !jit_block_current(vm, block))
        return false;

    if (block->owner) {
        jit_lru_touch(block->owner, block, JIT_LOOKUP_LRU);
        if (block->native_capacity) jit_lru_touch(block->owner, block, JIT_CODE_LRU);
    }
    uint16_t instruction_count = block->instruction_count;
    jit_func_t fn = (jit_func_t)block->native_code;
    uint32_t result = fn(vm->cpu);

    block->exec_count++;
    vm->cpu->insn_count += instruction_count;
    if (vm->jit) {
        ((jit_state_t *)vm->jit)->jit_instructions += instruction_count;
        if (block->protected_mode)
            ((jit_state_t *)vm->jit)->jit_pm_instructions += instruction_count;
    }

    if (result == JIT_EXIT_INTERPRET) return false;

    if ((result & ~0xFFu) == JIT_EXIT_INTERRUPT) {
        /* IR_INT only encodes real/v86, unprefixed INT imm8. Mode-changing
         * instructions end a block before their interpreted execution. */
        cpu8086_state_t *cpu = vm->cpu;
        uint32_t saved_eip = cpu->eip;
        uint32_t fault_eip = block->code32 ? saved_eip - 2u : (uint16_t)(saved_eip - 2u);
        (void)cpu8086_deliver_guest_interrupt(vm, (uint8_t)result, CPU_EVENT_SOFTWARE,
                                               saved_eip, fault_eip, 0, false);
        if (cpu->protected_mode && cpu->op_size_32 && vm->dpmi.active &&
            !cpu8086_uses_guest_idt(cpu)) dos_transfer_to_native(vm);
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════
 * 6. jit_invalidate_all — Flush all cached blocks
 * ══════════════════════════════════════════════════════════════════ */

void jit_invalidate_all(jit_state_t *jit)
{
    jit_cache_clear(jit);
    jit->cache_resets++;

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
    serial_puts("  Code payload:    "); serial_putdec(jit->code_payload);
    serial_puts(" bytes\n  Lookup evictions: "); serial_putdec(jit->lookup_evictions);
    serial_puts("\n  Code evictions:   "); serial_putdec(jit->code_evictions);
    serial_puts("\n  Cache resets:     "); serial_putdec(jit->cache_resets);
    serial_puts("\n  Interpreter backoffs: "); serial_putdec(jit->interpreter_backoffs);
    serial_puts("\n");
    serial_puts("  Interpreted:     ");
    serial_putdec(jit->interpreted);
    serial_puts("\n");
    serial_puts("  JIT executed:    ");
    serial_putdec(jit->jit_executed);
    serial_puts("\n");
    serial_puts("  JIT instructions: ");
    serial_putdec(jit->jit_instructions);
    serial_puts("\n");
    serial_puts("  JIT PM instructions: ");
    serial_putdec(jit->jit_pm_instructions);
    serial_puts("\n");
}

/* Independently reconstruct occupancy and list membership from live entries. */
static bool jit_cache_consistent(const jit_state_t *jit)
{
    if (jit->block_count > JIT_MAX_BLOCKS) return false;
    uint8_t seen[JIT_MAX_BLOCKS] = {0};
    uint8_t occupied[JIT_CODE_UNITS] = {0};
    uint8_t expected[2u * JIT_CODE_UNITS] = {0};
    unsigned counts[2] = {0};
    uint32_t used = 0, payload = 0, allocations = 0;
    for (unsigned list = 0; list < 2; list++) {
        uint16_t previous = 0;
        for (uint16_t index = jit->lru_head[list]; index; ) {
            if (index > jit->block_count || (seen[index - 1u] & (1u << list)))
                return false;
            const jit_block_t *block = &jit->blocks[index - 1u];
            if (block->owner != jit || block->links[list].prev != previous ||
                (list == JIT_CODE_LRU && !block->native_capacity)) return false;
            seen[index - 1u] |= 1u << list;
            previous = index;
            index = block->links[list].next;
            counts[list]++;
        }
        if (previous != jit->lru_tail[list]) return false;
    }
    for (unsigned bucket = 0; bucket < JIT_MAX_BLOCKS; bucket++) {
        for (uint16_t index = jit->buckets[bucket]; index; ) {
            if (index > jit->block_count || (seen[index - 1u] & 4u)) return false;
            const jit_block_t *block = &jit->blocks[index - 1u];
            if (jit_hash(block->cs, block->ip) != bucket) return false;
            seen[index - 1u] |= 4u;
            index = block->hash_next;
        }
    }
    for (unsigned i = 0; i < jit->block_count; i++) {
        const jit_block_t *block = &jit->blocks[i];
        uint32_t capacity = block->native_capacity;
        if ((seen[i] & 5u) != 5u || !!(seen[i] & 2u) != !!capacity) return false;
        if (!capacity) continue;
        uint64_t offset = (uintptr_t)block->native_code - (uintptr_t)jit->code_buf;
        if (capacity < JIT_CODE_MIN || (capacity & (capacity - 1u)) ||
            capacity > JIT_CACHE_SIZE || offset % capacity ||
            offset + capacity > JIT_CACHE_SIZE || !block->native_size ||
            block->native_size > capacity ||
            block->code_node != JIT_CACHE_SIZE / capacity + offset / capacity)
            return false;
        for (unsigned j = offset / JIT_CODE_MIN;
             j < (offset + capacity) / JIT_CODE_MIN; j++) {
            if (occupied[j]) return false;
            occupied[j] = 1;
        }
        used += capacity;
        payload += block->native_size;
        allocations++;
    }
    if (counts[JIT_LOOKUP_LRU] != jit->block_count ||
        counts[JIT_CODE_LRU] != allocations ||
        used != jit->code_used || payload != jit->code_payload) return false;
    for (unsigned i = 0; i < JIT_CODE_UNITS; i++)
        expected[JIT_CODE_UNITS + i] = occupied[i] ? 0 : 1;
    for (unsigned first = JIT_CODE_UNITS / 2u, order = 1; first; first >>= 1, order++)
        for (unsigned i = first; i < first * 2u; i++) {
            unsigned left = expected[2u * i], right = expected[2u * i + 1u];
            expected[i] = left == order && right == order ? order + 1u :
                          left > right ? left : right;
        }
    for (unsigned i = 1; i < 2u * JIT_CODE_UNITS; i++) {
        bool hidden = false;
        for (unsigned parent = i >> 1; parent; parent >>= 1)
            if (!jit->code_tree[parent]) { hidden = true; break; }
        if (!hidden && jit->code_tree[i] != expected[i]) return false;
    }
    return true;
}

int jit_cache_selftest(dos_vm_t *vm, jit_state_t *jit)
{
    unsigned checks = 0, failures = 0;
    cpu8086_state_t saved = *vm->cpu;
    void *saved_jit = vm->jit;
#define CACHE_CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; serial_puts("[DOS-JIT-CACHE] FAIL: " #condition "\n"); \
    } \
} while (0)
    jit_invalidate_all(jit);
    CACHE_CHECK(jit_cache_consistent(jit));

    /* A single cold entry, not the entire colliding bucket, is replaced. */
    jit_block_t *hot = NULL;
    for (unsigned i = 0; i < JIT_MAX_BLOCKS; i++) {
        jit_block_t *entry = jit_get_block(jit, 0, (i << 12) | i);
        CACHE_CHECK(entry->ip == ((i << 12) | i));
        if (i == 17) hot = entry;
    }
    hot->length = 3;
    hot->instruction_count = 1;
    hot->ir_count = 2;
    hot->ir[0] = (ir_inst_t){ .op = IR_MOV_REG_IMM, .a = REG_AX,
                             .b = 0x1234, .width = 2 };
    hot->ir[1] = (ir_inst_t){ .op = IR_EXIT_BLOCK };
    CACHE_CHECK(jit_compile_block(jit, hot) == 0);
    uint8_t *hot_code = hot->native_code;
    jit->hit_count[42] = 123;
    uint64_t resets = jit->cache_resets, evictions = jit->lookup_evictions;
    for (unsigned i = 0; i < 256; i++) {
        CACHE_CHECK(jit_get_block(jit, 0, (17u << 12) | 17u) == hot);
        (void)jit_get_block(jit, 0, 0x10000000u + i);
        CACHE_CHECK(hot->compiled && hot->native_code == hot_code &&
                    jit->block_count == JIT_MAX_BLOCKS &&
                    jit->hit_count[42] == 123 && jit->cache_resets == resets);
    }
    CACHE_CHECK(jit->lookup_evictions == evictions + 256u &&
                jit_cache_consistent(jit));
    vm->cpu->eax = 0xCAFE0000u;
    CACHE_CHECK(!jit_exec_block(vm, hot) && vm->cpu->eax == 0xCAFE1234u);

    /* Code pressure keeps the hot range and the evicted block's IR. */
    jit_invalidate_all(jit);
    jit_block_t *chunks[4];
    for (unsigned i = 0; i < 4; i++) {
        chunks[i] = jit_get_block(jit, 0, i);
        chunks[i]->ir_count = 1;
        chunks[i]->ir[0] = (ir_inst_t){ .op = IR_NOP };
        CACHE_CHECK(jit_allocate_code(jit, chunks[i], JIT_CACHE_SIZE / 4u));
        if (chunks[i]->native_code) chunks[i]->native_code[0] = (uint8_t)i;
    }
    CACHE_CHECK(jit->code_used == JIT_CACHE_SIZE && jit_cache_consistent(jit));
    jit_block_t *warm = jit_get_block(jit, 0, 0);
    uint8_t *warm_code = warm->native_code;
    evictions = jit->code_evictions;
    resets = jit->cache_resets;
    jit_block_t *small = jit_get_block(jit, 0, 4);
    CACHE_CHECK(jit_allocate_code(jit, small, 1));
    CACHE_CHECK(jit->code_evictions == evictions + 1u &&
                jit->cache_resets == resets && !chunks[1]->native_code &&
                chunks[1]->ir_count == 1 && chunks[1]->ir[0].op == IR_NOP &&
                warm->native_code == warm_code && warm_code[0] == 0 &&
                chunks[2]->native_code[0] == 2 && chunks[3]->native_code[0] == 3 &&
                jit_cache_consistent(jit));
    for (unsigned i = 0; i < 4; i++) jit_release_code(chunks[i]);
    jit_release_code(small);
    CACHE_CHECK(!jit->code_used && !jit->code_payload && jit_cache_consistent(jit));
    CACHE_CHECK(jit_allocate_code(jit, small, JIT_CACHE_SIZE));
    CACHE_CHECK(small->native_capacity == JIT_CACHE_SIZE &&
                small->native_code == jit->code_buf && jit_cache_consistent(jit));
    jit_release_code(small);

    /* Mixed size classes repeatedly split, evict, release and coalesce. */
    jit_invalidate_all(jit);
    uint32_t random = 0xC0DEC0DEu;
    for (unsigned iteration = 0; iteration < 2048; iteration++) {
        random = random * 1664525u + 1013904223u;
        unsigned id = (random >> 16) & 127u;
        jit_block_t *entry = jit_get_block(jit, 0, id);
        jit_release_code(entry);
        if (random & 3u) {
            uint32_t size = (JIT_CODE_MIN << ((random >> 24) % 10u)) - (random & 31u);
            CACHE_CHECK(jit_allocate_code(jit, entry, size));
            if (entry->native_code) {
                entry->exec_count = id ^ 0xA5u;
                entry->native_code[0] = (uint8_t)entry->exec_count;
                entry->native_code[entry->native_size - 1u] = (uint8_t)entry->exec_count;
            }
        }
        if ((iteration & 31u) == 0) {
            CACHE_CHECK(jit_cache_consistent(jit));
            for (unsigned i = 0; i < jit->block_count; i++) {
                jit_block_t *live = &jit->blocks[i];
                if (live->native_code)
                    CACHE_CHECK(live->native_code[0] == (uint8_t)live->exec_count &&
                                live->native_code[live->native_size - 1u] ==
                                    (uint8_t)live->exec_count);
            }
        }
    }
    for (unsigned i = 0; i < jit->block_count; i++) jit_release_code(&jit->blocks[i]);
    CACHE_CHECK(!jit->code_used && jit_cache_consistent(jit));

    /* Replacing a compiled block does not consume new space indefinitely. */
    jit_invalidate_all(jit);
    hot = jit_get_block(jit, 0, 0x1000);
    hot->length = 3;
    hot->instruction_count = 1;
    hot->ir_count = 2;
    hot->ir[1] = (ir_inst_t){ .op = IR_EXIT_BLOCK };
    for (unsigned i = 0; i < 1024; i++) {
        hot->ir[0] = (ir_inst_t){ .op = IR_MOV_REG_IMM, .a = REG_AX,
                                 .b = i, .width = 2 };
        CACHE_CHECK(jit_compile_block(jit, hot) == 0);
        vm->cpu->eax = 0xABCD0000u;
        CACHE_CHECK(!jit_exec_block(vm, hot) && vm->cpu->eax == (0xABCD0000u | i) &&
                    jit->code_used == JIT_CODE_MIN &&
                    jit->code_payload == hot->native_size);
    }
    CACHE_CHECK(jit_cache_consistent(jit));

    /* Actual dispatcher re-entry recompiles retained, validated protected IR. */
    jit_invalidate_all(jit);
    *vm->cpu = saved;
    vm->cpu->eip = 0x1800;
    vm->mem[0x1800] = 0x40;
    vm->mem[0x1801] = 0xEB; vm->mem[0x1802] = 0x0D;
    hot = jit_get_block(jit, vm->cpu->cs, 0x1800);
    CACHE_CHECK(jit_decode_block(vm, hot) > 0 && jit_compile_block(jit, hot) == 0);
    jit_release_code(hot);
    CACHE_CHECK(hot->source_valid && !hot->compiled && hot->instruction_count == 2);
    jit->hit_count[0x1800] = JIT_HOT_THRESHOLD;
    vm->jit = jit;
    uint64_t compiled = jit->jit_compiled;
    CACHE_CHECK(cpu8086_run_until(vm, true, vm->cpu->cs, 0x1810) &&
                jit->jit_compiled == compiled + 1u && hot->exec_count == 1 &&
                vm->cpu->eax == saved.eax + 1u &&
                vm->cpu->insn_count == saved.insn_count + 2u &&
                jit_cache_consistent(jit));
    jit_invalidate_all(jit);
    CACHE_CHECK(!hot->compiled && !hot->owner && jit_cache_consistent(jit));

    /* Unsupported starts cool down without suppressing interpreter execution.
     * A later code write takes effect before the next translation attempt. */
    *vm->cpu = saved;
    vm->cpu->eip = 0x1800;
    vm->mem[0x1800] = 0x05; /* ADD EAX, imm32 stays in the interpreter. */
    vm->mem[0x1801] = 1;
    vm->mem[0x1802] = vm->mem[0x1803] = vm->mem[0x1804] = 0;
    jit->hit_count[0x1800] = JIT_HOT_THRESHOLD;
    uint64_t backoffs = jit->interpreter_backoffs;
    compiled = jit->jit_compiled;
    CACHE_CHECK(cpu8086_run_until(vm, true, vm->cpu->cs, 0x1805) &&
                vm->cpu->eax == saved.eax + 1u && !jit->hit_count[0x1800] &&
                jit->interpreter_backoffs == backoffs + 1u &&
                jit->jit_compiled == compiled && jit_cache_consistent(jit));
    vm->mem[0x1800] = 0x40;
    vm->mem[0x1801] = 0xEB; vm->mem[0x1802] = 0x0D;
    vm->cpu->eip = 0x1800;
    CACHE_CHECK(cpu8086_run_until(vm, true, vm->cpu->cs, 0x1810) &&
                vm->cpu->eax == saved.eax + 2u &&
                jit->jit_compiled == compiled);
    vm->cpu->eip = 0x1800;
    jit->hit_count[0x1800] = JIT_HOT_THRESHOLD;
    CACHE_CHECK(cpu8086_run_until(vm, true, vm->cpu->cs, 0x1810) &&
                vm->cpu->eax == saved.eax + 3u &&
                jit->jit_compiled == compiled + 1u && jit_cache_consistent(jit));
    jit_invalidate_all(jit);

    vm->jit = saved_jit;
    *vm->cpu = saved;
    serial_puts("[DOS-JIT-CACHE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CACHE_CHECK
    return (int)failures;
}
