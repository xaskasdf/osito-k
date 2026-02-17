/*
 * OsitoK - OsitoVM bytecode interpreter
 *
 * 32-bit stack machine running inside the shell task.
 * Yields every VM_YIELD_INTERVAL instructions to keep the
 * scheduler and heartbeat alive. Ctrl+C (0x03) aborts execution.
 */

#include "vm/vm.h"
#include "drivers/uart.h"
#include "drivers/gpio.h"
#include "drivers/adc.h"
#include "drivers/input.h"
#include "drivers/video.h"
#include "kernel/task.h"

extern "C" {

/* ====== Stack helpers ====== */

static inline int vm_push(vm_t *vm, uint32_t val)
{
    if (vm->sp >= VM_STACK_SIZE) return -1;
    vm->stack[vm->sp++] = val;
    return 0;
}

static inline int vm_pop(vm_t *vm, uint32_t *val)
{
    if (vm->sp == 0) return -1;
    *val = vm->stack[--vm->sp];
    return 0;
}

static inline int vm_rpush(vm_t *vm, uint32_t val)
{
    if (vm->rsp >= VM_RSTACK_SIZE) return -1;
    vm->rstack[vm->rsp++] = val;
    return 0;
}

static inline int vm_rpop(vm_t *vm, uint32_t *val)
{
    if (vm->rsp == 0) return -1;
    *val = vm->rstack[--vm->rsp];
    return 0;
}

/* Read u8 from bytecode at PC, advance PC */
static inline uint8_t fetch8(vm_t *vm)
{
    return vm->code[vm->pc++];
}

/* Read u16 LE from bytecode at PC, advance PC by 2 */
static inline uint16_t fetch16(vm_t *vm)
{
    uint16_t lo = vm->code[vm->pc++];
    uint16_t hi = vm->code[vm->pc++];
    return lo | (hi << 8);
}

/* Read u32 LE from bytecode at PC, advance PC by 4 */
static inline uint32_t fetch32(vm_t *vm)
{
    uint32_t b0 = vm->code[vm->pc++];
    uint32_t b1 = vm->code[vm->pc++];
    uint32_t b2 = vm->code[vm->pc++];
    uint32_t b3 = vm->code[vm->pc++];
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

/* Read signed i16 LE from bytecode at PC, advance PC by 2 */
static inline int16_t fetch_i16(vm_t *vm)
{
    uint16_t raw = fetch16(vm);
    return (int16_t)raw;
}

/* ====== Syscall dispatcher ====== */

static int vm_syscall(vm_t *vm, uint8_t num)
{
    uint32_t a, b;

    switch (num) {
    case SYS_PUTC:
        if (vm_pop(vm, &a) < 0) return -1;
        uart_putc((char)a);
        break;

    case SYS_GETC:
        vm_push(vm, (uint32_t)uart_getc());
        break;

    case SYS_PUT_DEC:
        if (vm_pop(vm, &a) < 0) return -1;
        uart_put_dec(a);
        break;

    case SYS_PUT_HEX:
        if (vm_pop(vm, &a) < 0) return -1;
        uart_put_hex(a);
        break;

    case SYS_YIELD:
        task_yield();
        break;

    case SYS_DELAY:
        if (vm_pop(vm, &a) < 0) return -1;
        task_delay_ticks(a);
        break;

    case SYS_TICKS:
        vm_push(vm, get_tick_count());
        break;

    case SYS_GPIO_MODE:
        if (vm_pop(vm, &b) < 0) return -1;  /* mode */
        if (vm_pop(vm, &a) < 0) return -1;  /* pin */
        gpio_mode((uint8_t)a, (uint8_t)b);
        break;

    case SYS_GPIO_WRITE:
        if (vm_pop(vm, &b) < 0) return -1;  /* val */
        if (vm_pop(vm, &a) < 0) return -1;  /* pin */
        gpio_write((uint8_t)a, (uint8_t)b);
        break;

    case SYS_GPIO_READ:
        if (vm_pop(vm, &a) < 0) return -1;  /* pin */
        vm_push(vm, gpio_read((uint8_t)a));
        break;

    case SYS_GPIO_TOGGLE:
        if (vm_pop(vm, &a) < 0) return -1;  /* pin */
        gpio_toggle((uint8_t)a);
        break;

    case SYS_INPUT_POLL:
        vm_push(vm, (uint32_t)input_poll());
        break;

    case SYS_INPUT_STATE:
        vm_push(vm, input_get_state());
        break;

    case SYS_ADC_READ:
        vm_push(vm, (uint32_t)adc_read());
        break;

    case SYS_FB_CLEAR:
        fb_clear();
        break;

    case SYS_FB_PIXEL:
        if (vm_pop(vm, &b) < 0) return -1;  /* y */
        if (vm_pop(vm, &a) < 0) return -1;  /* x */
        fb_set_pixel((int)a, (int)b);
        break;

    case SYS_FB_LINE:
        {
            uint32_t y1, x1;
            if (vm_pop(vm, &y1) < 0) return -1;
            if (vm_pop(vm, &x1) < 0) return -1;
            if (vm_pop(vm, &b) < 0) return -1;   /* y0 */
            if (vm_pop(vm, &a) < 0) return -1;   /* x0 */
            fb_line((int)a, (int)b, (int)x1, (int)y1);
        }
        break;

    case SYS_FB_FLUSH:
        fb_flush();
        break;

    default:
        uart_puts("vm: unknown syscall ");
        uart_put_dec(num);
        uart_puts("\n");
        return -1;
    }
    return 0;
}

/* ====== Public API ====== */

void vm_init(vm_t *vm)
{
    /* Zero everything */
    uint8_t *p = (uint8_t *)vm;
    for (uint32_t i = 0; i < sizeof(vm_t); i++)
        p[i] = 0;
}

int vm_load(vm_t *vm, uint8_t *buf, uint32_t size)
{
    if (size < VM_HEADER_SIZE) {
        uart_puts("vm: file too small\n");
        return -1;
    }

    /* Check magic */
    uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (magic != VM_MAGIC) {
        uart_puts("vm: bad magic (expected OSVM)\n");
        return -1;
    }

    /* Check version */
    if (buf[4] != VM_VERSION) {
        uart_puts("vm: unsupported version ");
        uart_put_dec(buf[4]);
        uart_puts("\n");
        return -1;
    }

    /* num_locals in buf[6] — informational, we always have VM_MAX_LOCALS */

    vm->code = buf + VM_HEADER_SIZE;
    vm->code_size = size - VM_HEADER_SIZE;
    vm->pc = 0;
    vm->running = 1;
    vm->exit_code = 0;
    vm->insn_count = 0;

    return 0;
}

int vm_run(vm_t *vm)
{
    uint32_t a, b;

    while (vm->running) {
        /* Periodic yield + Ctrl+C check */
        if ((vm->insn_count % VM_YIELD_INTERVAL) == 0 && vm->insn_count > 0) {
            task_yield();
            if (uart_rx_available()) {
                int ch = uart_getc();
                if (ch == 0x03) {
                    uart_puts("\n^C\n");
                    vm->running = 0;
                    vm->exit_code = -1;
                    return -1;
                }
            }
        }

        /* Bounds check */
        if (vm->pc >= vm->code_size) {
            uart_puts("vm: PC out of bounds\n");
            vm->running = 0;
            vm->exit_code = -2;
            return -2;
        }

        uint8_t op = fetch8(vm);

        switch (op) {

        /* ---- Control ---- */
        case OP_NOP:
            break;

        case OP_HALT:
            vm->running = 0;
            vm->exit_code = (vm->sp > 0) ? (int32_t)vm->stack[vm->sp - 1] : 0;
            break;

        /* ---- Stack ---- */
        case OP_PUSH8:
            if (vm_push(vm, fetch8(vm)) < 0) goto stack_overflow;
            break;

        case OP_PUSH16:
            if (vm_push(vm, fetch16(vm)) < 0) goto stack_overflow;
            break;

        case OP_PUSH32:
            if (vm_push(vm, fetch32(vm)) < 0) goto stack_overflow;
            break;

        case OP_DUP:
            if (vm->sp == 0) goto stack_underflow;
            if (vm_push(vm, vm->stack[vm->sp - 1]) < 0) goto stack_overflow;
            break;

        case OP_DROP:
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            break;

        case OP_SWAP:
            if (vm->sp < 2) goto stack_underflow;
            a = vm->stack[vm->sp - 1];
            vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
            vm->stack[vm->sp - 2] = a;
            break;

        case OP_OVER:
            if (vm->sp < 2) goto stack_underflow;
            if (vm_push(vm, vm->stack[vm->sp - 2]) < 0) goto stack_overflow;
            break;

        case OP_ROT:
            if (vm->sp < 3) goto stack_underflow;
            a = vm->stack[vm->sp - 3];
            vm->stack[vm->sp - 3] = vm->stack[vm->sp - 2];
            vm->stack[vm->sp - 2] = vm->stack[vm->sp - 1];
            vm->stack[vm->sp - 1] = a;
            break;

        /* ---- Arithmetic ---- */
        case OP_ADD:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a + b);
            break;

        case OP_SUB:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a - b);
            break;

        case OP_MUL:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a * b);
            break;

        case OP_DIV:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            if (b == 0) {
                uart_puts("vm: division by zero\n");
                vm->running = 0;
                vm->exit_code = -3;
                return -3;
            }
            vm_push(vm, (uint32_t)((int32_t)a / (int32_t)b));
            break;

        case OP_MOD:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            if (b == 0) {
                uart_puts("vm: modulo by zero\n");
                vm->running = 0;
                vm->exit_code = -3;
                return -3;
            }
            vm_push(vm, (uint32_t)((int32_t)a % (int32_t)b));
            break;

        case OP_NEG:
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (uint32_t)(-(int32_t)a));
            break;

        /* ---- Bitwise ---- */
        case OP_AND:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a & b);
            break;

        case OP_OR:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a | b);
            break;

        case OP_XOR:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a ^ b);
            break;

        case OP_NOT:
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, ~a);
            break;

        case OP_SHL:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a << b);
            break;

        case OP_SHR:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (uint32_t)((int32_t)a >> b));
            break;

        /* ---- Comparison ---- */
        case OP_EQ:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a == b ? 1 : 0);
            break;

        case OP_NE:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, a != b ? 1 : 0);
            break;

        case OP_LT:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (int32_t)a < (int32_t)b ? 1 : 0);
            break;

        case OP_GT:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (int32_t)a > (int32_t)b ? 1 : 0);
            break;

        case OP_LE:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (int32_t)a <= (int32_t)b ? 1 : 0);
            break;

        case OP_GE:
            if (vm_pop(vm, &b) < 0 || vm_pop(vm, &a) < 0) goto stack_underflow;
            vm_push(vm, (int32_t)a >= (int32_t)b ? 1 : 0);
            break;

        /* ---- Control flow ---- */
        case OP_JMP: {
            int16_t offset = fetch_i16(vm);
            /* offset is relative to PC at the start of this instruction's operand.
             * But we already advanced PC past the opcode + 2 bytes.
             * We want: PC_before_operand + offset
             * PC_before_operand = vm->pc - 2  (we fetched 2 bytes)
             * So target = (vm->pc - 2) + offset */
            vm->pc = (uint32_t)((int32_t)(vm->pc - 2) + (int32_t)offset);
            break;
        }

        case OP_JZ: {
            int16_t offset = fetch_i16(vm);
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            if (a == 0)
                vm->pc = (uint32_t)((int32_t)(vm->pc - 2) + (int32_t)offset);
            break;
        }

        case OP_JNZ: {
            int16_t offset = fetch_i16(vm);
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            if (a != 0)
                vm->pc = (uint32_t)((int32_t)(vm->pc - 2) + (int32_t)offset);
            break;
        }

        case OP_CALL: {
            int16_t offset = fetch_i16(vm);
            if (vm_rpush(vm, vm->pc) < 0) {
                uart_puts("vm: return stack overflow\n");
                vm->running = 0;
                return -4;
            }
            vm->pc = (uint32_t)((int32_t)(vm->pc - 2) + (int32_t)offset);
            break;
        }

        case OP_RET:
            if (vm_rpop(vm, &a) < 0) {
                uart_puts("vm: return stack underflow\n");
                vm->running = 0;
                return -4;
            }
            vm->pc = a;
            break;

        /* ---- Local variables ---- */
        case OP_LOAD: {
            uint8_t idx = fetch8(vm);
            if (idx >= VM_MAX_LOCALS) {
                uart_puts("vm: local index out of range\n");
                vm->running = 0;
                return -5;
            }
            vm_push(vm, vm->locals[idx]);
            break;
        }

        case OP_STORE: {
            uint8_t idx = fetch8(vm);
            if (idx >= VM_MAX_LOCALS) {
                uart_puts("vm: local index out of range\n");
                vm->running = 0;
                return -5;
            }
            if (vm_pop(vm, &a) < 0) goto stack_underflow;
            vm->locals[idx] = a;
            break;
        }

        /* ---- Syscalls ---- */
        case OP_SYSCALL: {
            uint8_t num = fetch8(vm);
            if (vm_syscall(vm, num) < 0) {
                vm->running = 0;
                return -6;
            }
            break;
        }

        default:
            uart_puts("vm: unknown opcode 0x");
            uart_put_hex(op);
            uart_puts(" at PC ");
            uart_put_dec(vm->pc - 1);
            uart_puts("\n");
            vm->running = 0;
            vm->exit_code = -7;
            return -7;
        }

        vm->insn_count++;
    }

    return vm->exit_code;

stack_overflow:
    uart_puts("vm: stack overflow at PC ");
    uart_put_dec(vm->pc);
    uart_puts("\n");
    vm->running = 0;
    return -8;

stack_underflow:
    uart_puts("vm: stack underflow at PC ");
    uart_put_dec(vm->pc);
    uart_puts("\n");
    vm->running = 0;
    return -9;
}

} /* extern "C" */
