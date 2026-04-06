/*
 * OsitoK x86-64 — Seccomp (Secure Computing)
 *
 * Syscall filtering per-process using BPF programs.
 * Modes:
 *   SECCOMP_MODE_STRICT: only read/write/exit/sigreturn allowed
 *   SECCOMP_MODE_FILTER: BPF program decides per syscall
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint32_t proc_current_pid(void);

/* BPF execution */
typedef struct { uint8_t opcode; uint8_t regs; int16_t off; int32_t imm; } bpf_insn_t;
extern uint64_t bpf_run(int prog_idx, const void *ctx, uint32_t ctx_len)
    __attribute__((weak));

/* Seccomp return actions */
#define SECCOMP_RET_ALLOW  0x7FFF0000
#define SECCOMP_RET_KILL   0x00000000
#define SECCOMP_RET_ERRNO  0x00050000
#define SECCOMP_RET_TRAP   0x00030000
#define SECCOMP_RET_LOG    0x7FFC0000

/* Seccomp modes */
#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

/* ── Per-Process Seccomp State ───────────────────────────────── */

#define SECCOMP_MAX_PROCS 64

typedef struct {
    uint32_t pid;
    uint8_t  mode;
    int      bpf_prog;     /* BPF program index for FILTER mode */
} seccomp_state_t;

static seccomp_state_t seccomp_procs[SECCOMP_MAX_PROCS];

/* ── Syscall Data (passed to BPF program) ────────────────────── */

typedef struct {
    int32_t  nr;           /* Syscall number */
    uint32_t arch;         /* AUDIT_ARCH_X86_64 = 0xC000003E */
    uint64_t ip;           /* Instruction pointer */
    uint64_t args[6];      /* Syscall arguments */
} seccomp_data_t;

/* ── Public API ──────────────────────────────────────────────── */

int seccomp_set_mode(uint32_t pid, uint8_t mode, int bpf_prog)
{
    /* Find or allocate slot */
    int slot = -1;
    for (int i = 0; i < SECCOMP_MAX_PROCS; i++) {
        if (seccomp_procs[i].pid == pid && seccomp_procs[i].mode != SECCOMP_MODE_DISABLED) {
            slot = i; break;
        }
        if (slot < 0 && seccomp_procs[i].mode == SECCOMP_MODE_DISABLED) slot = i;
    }
    if (slot < 0) return -1;

    seccomp_procs[slot].pid = pid;
    seccomp_procs[slot].mode = mode;
    seccomp_procs[slot].bpf_prog = bpf_prog;

    serial_puts("[SECCOMP] PID ");
    serial_putdec(pid);
    serial_puts(" → mode ");
    serial_putdec(mode);
    serial_puts("\n");
    return 0;
}

/* Check if a syscall is allowed. Called from syscall dispatcher.
 * Returns: 0 = allow, -1 = kill, >0 = errno to return */
int seccomp_check(uint64_t nr, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint32_t pid = proc_current_pid();

    for (int i = 0; i < SECCOMP_MAX_PROCS; i++) {
        if (seccomp_procs[i].pid != pid) continue;

        switch (seccomp_procs[i].mode) {
        case SECCOMP_MODE_DISABLED:
            return 0;

        case SECCOMP_MODE_STRICT:
            /* Only allow: read(0), write(1), exit(60), exit_group(231), sigreturn(15) */
            if (nr == 0 || nr == 1 || nr == 60 || nr == 231 || nr == 15)
                return 0;
            serial_puts("[SECCOMP] STRICT: blocked syscall ");
            serial_putdec(nr);
            serial_puts("\n");
            return -1;  /* Kill */

        case SECCOMP_MODE_FILTER:
            if (bpf_run) {
                seccomp_data_t data = {
                    .nr = (int32_t)nr, .arch = 0xC000003E,
                    .args = {a1, a2, a3, a4, a5, 0}
                };
                uint64_t ret = bpf_run(seccomp_procs[i].bpf_prog,
                                       &data, sizeof(data));
                uint32_t action = (uint32_t)(ret & 0xFFFF0000);
                if (action == SECCOMP_RET_ALLOW) return 0;
                if (action == SECCOMP_RET_KILL)  return -1;
                if (action == SECCOMP_RET_ERRNO) return (int)(ret & 0xFFFF);
                return 0;  /* Default: allow */
            }
            return 0;
        }
        break;
    }
    return 0;  /* No seccomp filter for this process */
}

void seccomp_clear(uint32_t pid)
{
    for (int i = 0; i < SECCOMP_MAX_PROCS; i++) {
        if (seccomp_procs[i].pid == pid)
            seccomp_procs[i].mode = SECCOMP_MODE_DISABLED;
    }
}
