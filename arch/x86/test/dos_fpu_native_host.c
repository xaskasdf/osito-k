#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Linux/WSL observation independent of Osito-K. Compile with
 * -mgeneral-regs-only so only the explicit assembly touches x87 state.
 * Loss is reported verbatim, not converted into a conformance pass. */
static void probe(uint8_t *before, uint8_t *after, unsigned pending,
                  unsigned iterations, unsigned yield)
{
    uint8_t env[28] = {0};
    uint16_t cw = pending ? 0x037e : 0x037f;
    __asm__ volatile("fninit; fldcw %1; fldz; fldz; .byte 0xde, 0xf9; fnstenv %0"
                     : "=m"(env) : "m"(cw) : "memory");
    uint32_t fip = 0x12345678, fdp = 0x23456789;
    uint16_t fcs = 0x137, fds = 0x147;
    memcpy(env + 12, &fip, 4);
    memcpy(env + 16, &fcs, 2);
    memcpy(env + 20, &fdp, 4);
    memcpy(env + 24, &fds, 2);
    __asm__ volatile("fnclex; fldenv (%0); fnstenv (%1); fldcw (%0)"
                     : : "r"(env), "r"(before) : "memory");
    for (unsigned i = 0; i < iterations; ++i) {
        if (yield) {
            unsigned long result = 24;
            __asm__ volatile("syscall" : "+a"(result) : : "rcx", "r11", "memory");
        } else {
            unsigned result = 0;
            __asm__ volatile("cpuid" : "+a"(result) : : "rbx", "rcx", "rdx", "memory");
        }
    }
    __asm__ volatile("fnstenv (%0); fninit" : : "r"(after) : "memory");
}

int main(void)
{
    for (unsigned pending = 0; pending < 2; ++pending)
        for (unsigned trial = 0; trial < 4; ++trial) {
            uint8_t before[28] = {0}, after[28] = {0};
            unsigned iterations = trial < 2 ? trial : 100000;
            unsigned yield = trial == 3;
            probe(before, after, pending, iterations, yield);
            printf("pending=%u iterations=%u operation=%s", pending, iterations,
                   yield ? "yield" : "cpuid");
            const unsigned offsets[] = {4, 12, 16, 20, 24};
            for (unsigned i = 0; i < 5; ++i) {
                uint32_t a, b;
                memcpy(&a, before + offsets[i], 4);
                memcpy(&b, after + offsets[i], 4);
                printf(" %u:%08x/%08x", offsets[i], a, b);
            }
            putchar('\n');
        }
    return 0;
}
