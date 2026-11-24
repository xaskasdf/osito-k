/*
 * OsitoK x86-64 — Static Binary Analysis for Speculation
 *
 * Analyzes ELF .text sections at load time to build a control flow
 * graph (CFG) of basic blocks. This data enables:
 *   1. Smarter prefetch (follow branches, not just linear)
 *   2. Loop detection for future TLS parallelization
 *   3. Syscall site mapping (speculation barriers)
 *
 * The analyzer is a minimal x86-64 decoder — only identifies
 * branch/call/ret/syscall instructions to find block boundaries.
 * Not a full disassembler (~200 lines).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* ── Analysis result ────────────────────────────────────────── */

#define SPEC_MAX_BLOCKS   4096
#define SPEC_MAX_SYSCALLS  256
#define SPEC_MAX_LOOPS     128

typedef struct {
    uint64_t start;   /* VA of block entry */
    uint64_t end;     /* VA past last instruction */
    uint64_t target;  /* Branch target (0 if fallthrough or indirect) */
    uint8_t  is_call; /* 1 if block ends with CALL */
    uint8_t  is_ret;  /* 1 if block ends with RET */
} spec_block_t;

typedef struct {
    uint64_t header;    /* VA of loop entry (back-edge target) */
    uint64_t back_edge; /* VA of the backward jump */
    uint64_t exit;      /* VA of the first instruction after loop */
    uint32_t depth;     /* Nesting depth (0 = outermost) */
} spec_loop_t;

typedef struct {
    spec_block_t *blocks;
    int           block_count;

    uint64_t     *syscalls;
    int           syscall_count;

    spec_loop_t  *loops;
    int           loop_count;

    uint64_t      text_base;
    uint64_t      text_size;
} spec_analysis_t;

/* ── Minimal x86-64 instruction length decoder ──────────────── */

/* Returns approximate instruction length at `ip`. Handles the common
 * encodings that matter for branch detection. Conservative: returns
 * 1 for unknown opcodes (safe — next byte is re-examined). */
static int spec_insn_length(const uint8_t *ip, uint64_t remaining)
{
    if (remaining == 0) return 1;
    uint8_t op = ip[0];

    /* REX prefix (0x40-0x4F) — skip and decode next byte */
    int rex = 0;
    if (op >= 0x40 && op <= 0x4F && remaining > 1) {
        rex = 1;
        op = ip[1];
    }

    /* Single-byte ops */
    if (op == 0xC3 || op == 0xCB) return rex + 1;  /* RET */
    if (op == 0xCC) return rex + 1;                  /* INT3 */
    if (op == 0x90) return rex + 1;                  /* NOP */
    if (op >= 0x50 && op <= 0x5F) return rex + 1;    /* PUSH/POP reg */

    /* Short Jcc (70-7F) + JMP short (EB) */
    if ((op >= 0x70 && op <= 0x7F) || op == 0xEB) return rex + 2;

    /* CALL rel32 (E8), JMP rel32 (E9) */
    if (op == 0xE8 || op == 0xE9) return rex + 5;

    /* Two-byte opcodes (0F xx) */
    if (op == 0x0F && remaining > rex + 1) {
        uint8_t op2 = ip[rex + 1];
        if (op2 == 0x05) return rex + 2;  /* SYSCALL */
        if (op2 >= 0x80 && op2 <= 0x8F) return rex + 6;  /* Jcc near */
        if (op2 == 0x1F) {
            /* NOP with ModRM — variable length, approximate */
            if (remaining > rex + 2) {
                uint8_t modrm = ip[rex + 2];
                int mod = modrm >> 6;
                if (mod == 0) return rex + 3 + ((modrm & 7) == 4 ? 1 : 0);
                if (mod == 1) return rex + 4 + ((modrm & 7) == 4 ? 1 : 0);
                if (mod == 2) return rex + 7 + ((modrm & 7) == 4 ? 1 : 0);
            }
            return rex + 3;
        }
        return rex + 3;  /* Most 0F xx ops are 3 bytes with REX */
    }

    /* ModRM-based: need to decode addressing mode for length */
    /* Opcodes 80-83 (immediate ALU), 89/8B (MOV), etc. */
    if ((op >= 0x80 && op <= 0x83) || op == 0x89 || op == 0x8B ||
        op == 0x8D || op == 0x31 || op == 0x33 || op == 0x39 ||
        op == 0x3B || op == 0x85 || op == 0x01 || op == 0x03 ||
        op == 0x29 || op == 0x09 || op == 0x0B || op == 0x21 ||
        op == 0x23 || op == 0xFF) {
        if (remaining <= rex + 1) return rex + 2;
        uint8_t modrm = ip[rex + 1];
        int mod = modrm >> 6, rm = modrm & 7;
        int len = rex + 2;  /* opcode + modrm */
        if (rm == 4 && mod != 3) len++;  /* SIB byte */
        if (mod == 1) len += 1;          /* disp8 */
        if (mod == 2 || (mod == 0 && rm == 5)) len += 4; /* disp32 */
        /* Immediate for 80-83 */
        if (op == 0x80 || op == 0x82) len += 1;
        if (op == 0x81) len += 4;
        if (op == 0x83) len += 1;
        return len;
    }

    /* MOV imm to reg (B8-BF + imm32/64) */
    if (op >= 0xB8 && op <= 0xBF) return rex ? 10 : 5;
    /* MOV imm8 to reg (B0-B7) */
    if (op >= 0xB0 && op <= 0xB7) return rex + 2;

    /* Default: assume 3 bytes (covers most unhandled cases) */
    return rex + 3;
}

/* ── Analyze .text section ──────────────────────────────────── */

int spec_analyze_text(uint64_t text_base, uint64_t text_size,
                      spec_analysis_t *out)
{
    memset(out, 0, sizeof(*out));
    out->text_base = text_base;
    out->text_size = text_size;

    out->blocks   = (spec_block_t *)kmalloc(SPEC_MAX_BLOCKS * sizeof(spec_block_t));
    out->syscalls = (uint64_t *)kmalloc(SPEC_MAX_SYSCALLS * sizeof(uint64_t));
    out->loops    = (spec_loop_t *)kmalloc(SPEC_MAX_LOOPS * sizeof(spec_loop_t));
    if (!out->blocks || !out->syscalls || !out->loops) return -1;

    const uint8_t *code = (const uint8_t *)text_base;
    uint64_t pos = 0;
    uint64_t block_start = text_base;
    int bc = 0, sc = 0;

    while (pos < text_size && bc < SPEC_MAX_BLOCKS - 1) {
        const uint8_t *ip = &code[pos];
        uint64_t va = text_base + pos;
        uint64_t rem = text_size - pos;
        int ilen = spec_insn_length(ip, rem);
        uint64_t next_va = va + ilen;

        bool is_branch = false;
        uint64_t target = 0;
        bool is_call = false;
        bool is_ret = false;

        uint8_t op = ip[0];
        /* Skip REX */
        int rex = 0;
        if (op >= 0x40 && op <= 0x4F && rem > 1) { rex = 1; op = ip[1]; }

        /* SYSCALL (0F 05) */
        if (op == 0x0F && rem > rex + 1 && ip[rex + 1] == 0x05) {
            if (sc < SPEC_MAX_SYSCALLS)
                out->syscalls[sc++] = va;
            is_branch = true;  /* syscall = block boundary */
        }
        /* RET */
        else if (op == 0xC3 || op == 0xCB) {
            is_ret = true;
            is_branch = true;
        }
        /* Short Jcc (70-7F) */
        else if (op >= 0x70 && op <= 0x7F) {
            int8_t disp = (int8_t)ip[rex + 1];
            target = next_va + disp;
            is_branch = true;
        }
        /* JMP short (EB) */
        else if (op == 0xEB) {
            int8_t disp = (int8_t)ip[rex + 1];
            target = next_va + disp;
            is_branch = true;
        }
        /* CALL rel32 (E8) */
        else if (op == 0xE8) {
            int32_t disp = *(int32_t *)&ip[rex + 1];
            target = next_va + disp;
            is_call = true;
            is_branch = true;
        }
        /* JMP rel32 (E9) */
        else if (op == 0xE9) {
            int32_t disp = *(int32_t *)&ip[rex + 1];
            target = next_va + disp;
            is_branch = true;
        }
        /* Near Jcc (0F 80-8F) */
        else if (op == 0x0F && rem > rex + 5 &&
                 ip[rex + 1] >= 0x80 && ip[rex + 1] <= 0x8F) {
            int32_t disp = *(int32_t *)&ip[rex + 2];
            target = next_va + disp;
            is_branch = true;
        }

        if (is_branch) {
            /* End current block */
            out->blocks[bc].start   = block_start;
            out->blocks[bc].end     = next_va;
            out->blocks[bc].target  = target;
            out->blocks[bc].is_call = is_call;
            out->blocks[bc].is_ret  = is_ret;
            bc++;
            block_start = next_va;
        }

        pos += ilen;
    }

    /* Close last block if non-empty */
    if (block_start < text_base + pos && bc < SPEC_MAX_BLOCKS) {
        out->blocks[bc].start  = block_start;
        out->blocks[bc].end    = text_base + pos;
        out->blocks[bc].target = 0;
        bc++;
    }

    out->block_count   = bc;
    out->syscall_count = sc;

    /* ── Loop detection: find back-edges ────────────────────── */
    /* A back-edge is a branch whose target is BEFORE the branch
     * (i.e., target < branch_va). This indicates a loop. */
    int lc = 0;
    for (int i = 0; i < bc && lc < SPEC_MAX_LOOPS; i++) {
        uint64_t t = out->blocks[i].target;
        if (t && t < out->blocks[i].end && t >= text_base &&
            !out->blocks[i].is_call && !out->blocks[i].is_ret) {
            out->loops[lc].header    = t;
            out->loops[lc].back_edge = out->blocks[i].start;
            out->loops[lc].exit      = out->blocks[i].end;
            out->loops[lc].depth     = 0;
            lc++;
        }
    }
    out->loop_count = lc;

    return bc;
}

/* ── Free analysis data ─────────────────────────────────────── */

void spec_analysis_free(spec_analysis_t *a)
{
    if (a->blocks)   kfree(a->blocks);
    if (a->syscalls) kfree(a->syscalls);
    if (a->loops)    kfree(a->loops);
    memset(a, 0, sizeof(*a));
}
