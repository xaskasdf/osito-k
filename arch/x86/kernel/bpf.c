/*
 * OsitoK x86-64 — BPF/eBPF Virtual Machine
 *
 * Extended BPF bytecode interpreter for packet filtering and tracing.
 * Supports: ALU ops, memory access, jumps, function calls.
 * Programs loaded via bpf_load(), attached to network hooks.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* BPF instruction */
typedef struct {
    uint8_t  opcode;
    uint8_t  regs;      /* dst:4 | src:4 */
    int16_t  off;
    int32_t  imm;
} bpf_insn_t;

/* Instruction classes */
#define BPF_ALU64 0x07
#define BPF_JMP   0x05
#define BPF_LDX   0x01

/* ALU ops */
#define BPF_ADD  0x00
#define BPF_SUB  0x10
#define BPF_MOV  0xB0
#define BPF_AND  0x50
#define BPF_OR   0x40
#define BPF_XOR  0xA0
#define BPF_LSH  0x60
#define BPF_RSH  0x70

/* Jump ops */
#define BPF_JA   0x00
#define BPF_JEQ  0x10
#define BPF_JGT  0x20
#define BPF_JNE  0x50
#define BPF_EXIT 0x90
#define BPF_CALL 0x80

#define BPF_MAX_PROGS  8
#define BPF_MAX_INSNS  256
#define BPF_STACK_SIZE 512

typedef struct {
    bool       active;
    bpf_insn_t insns[BPF_MAX_INSNS];
    uint32_t   count;
    char       name[32];
} bpf_prog_t;

static bpf_prog_t bpf_progs[BPF_MAX_PROGS];

/* Run BPF program, return r0 */
uint64_t bpf_run(int idx, const void *ctx, uint32_t ctx_len)
{
    if (idx < 0 || idx >= BPF_MAX_PROGS || !bpf_progs[idx].active) return 0;
    bpf_prog_t *p = &bpf_progs[idx];

    uint64_t r[11] = {0};
    uint8_t stack[BPF_STACK_SIZE] = {0};
    r[1] = (uint64_t)ctx;
    r[10] = (uint64_t)(stack + BPF_STACK_SIZE);

    uint32_t pc = 0;
    uint32_t limit = 100000;

    while (pc < p->count && limit-- > 0) {
        bpf_insn_t *i = &p->insns[pc];
        uint8_t dst = i->regs & 0x0F;
        uint8_t src = (i->regs >> 4) & 0x0F;
        uint8_t cls = i->opcode & 0x07;
        uint8_t op  = i->opcode & 0xF0;
        bool use_src = (i->opcode & 0x08) != 0;
        uint64_t sv = use_src ? r[src] : (uint64_t)(int64_t)i->imm;

        if (cls == BPF_ALU64) {
            switch (op) {
            case BPF_ADD: r[dst] += sv; break;
            case BPF_SUB: r[dst] -= sv; break;
            case BPF_AND: r[dst] &= sv; break;
            case BPF_OR:  r[dst] |= sv; break;
            case BPF_XOR: r[dst] ^= sv; break;
            case BPF_LSH: r[dst] <<= sv; break;
            case BPF_RSH: r[dst] >>= sv; break;
            case BPF_MOV: r[dst] = sv; break;
            }
        } else if (cls == BPF_JMP) {
            switch (op) {
            case BPF_JA:  pc += i->off; break;
            case BPF_JEQ: if (r[dst] == sv) pc += i->off; break;
            case BPF_JGT: if (r[dst] > sv)  pc += i->off; break;
            case BPF_JNE: if (r[dst] != sv) pc += i->off; break;
            case BPF_EXIT: return r[0];
            case BPF_CALL:
                if (i->imm == 6) {
                    serial_puts("[BPF] trace: 0x");
                    serial_puthex(r[1], 16);
                    serial_puts("\n");
                }
                r[0] = 0;
                break;
            }
        } else if (cls == BPF_LDX) {
            uint64_t addr = r[src] + i->off;
            (void)ctx_len;
            r[dst] = *(uint64_t *)addr;
        }
        pc++;
    }
    return r[0];
}

int bpf_load(const bpf_insn_t *insns, uint32_t count, const char *name)
{
    if (count > BPF_MAX_INSNS) return -1;
    for (int i = 0; i < BPF_MAX_PROGS; i++) {
        if (!bpf_progs[i].active) {
            bpf_progs[i].active = true;
            memcpy(bpf_progs[i].insns, insns, count * sizeof(bpf_insn_t));
            bpf_progs[i].count = count;
            int j = 0;
            while (name[j] && j < 31) { bpf_progs[i].name[j] = name[j]; j++; }
            bpf_progs[i].name[j] = '\0';
            serial_puts("[BPF] Loaded: ");
            serial_puts(name);
            serial_puts("\n");
            return i;
        }
    }
    return -1;
}

void bpf_unload(int idx)
{
    if (idx >= 0 && idx < BPF_MAX_PROGS) bpf_progs[idx].active = false;
}

void bpf_list(void)
{
    serial_puts("[BPF] Programs:\n");
    int n = 0;
    for (int i = 0; i < BPF_MAX_PROGS; i++) {
        if (!bpf_progs[i].active) continue;
        serial_puts("  "); serial_puts(bpf_progs[i].name);
        serial_puts(" ("); serial_putdec(bpf_progs[i].count);
        serial_puts(" insns)\n");
        n++;
    }
    if (n == 0) serial_puts("  (none)\n");
}
