/* Link with section GC: only the observation helper executes on the host. */
#include "../dos/dos_io.c"

extern int puts(const char *);

#ifdef DOS_IRQ_DIAGNOSTICS
static uint64_t observed_ns = 1000;
uint64_t idt_get_monotonic_ns(void) { return observed_ns++; }

int main(void)
{
    dos_vm_t vm = {0};
    cpu8086_state_t cpu = {0};
    DOS_IO_STATE io = {0};
    dos_io_irq_trace(NULL, 1, 0, 8);
    dos_io_irq_trace(&vm, 1, 0, 8);
    vm.cpu = &cpu;
    dos_io_irq_trace(&vm, 1, 0, 8);
    if (dos_irq_trace_total || observed_ns != 1000) return 1;
    vm.io = &io;
    vm.timer_irq_pending = true;
    vm.dpmi.virtual_interrupts_enabled = true;
    vm.dpmi.interrupt_depth = 3;
    vm.dpmi.control_depth = 2;
    io.pic[0].isr = 0x21;
    io.pic[0].mask = 0x80;
    io.pit[0].reload = 12345;
    io.pit[0].irq_period = 6789;
    cpu.cs = 0x17;
    for (uint32_t i = 1; i <= 600; i++) {
        cpu.eip = 0x1000 + i;
        cpu.insn_count = i * 7u;
        dos_io_irq_trace(&vm, DOS_IRQ_TRACE_ACCEPT, 5, i);
    }
    if (dos_irq_trace_total != 600 || observed_ns != 1600) return 2;
    for (uint64_t i = 89; i <= 600; i++) {
        const volatile DOS_IRQ_RECORD *r = &dos_irq_trace_records[(i - 1) % 512u];
        if (r->sequence != i || r->ns != 999 + i || r->owner != (uintptr_t)&vm ||
            r->instructions != i * 7 || r->irq_period != 6789 ||
            r->eip != 0x1000 + i || r->value != i || r->cs != 0x17 ||
            r->reload != 12345 || r->kind != DOS_IRQ_TRACE_ACCEPT || r->irq != 5 ||
            r->depth != 3 || r->control_depth != 2 || !r->virtual_if ||
            !r->pending || r->isr != 0x21 || r->mask != 0x80 || r->reserved) return 3;
    }
    if (cpu.eip != 0x1258 || cpu.insn_count != 4200 ||
        !vm.timer_irq_pending || !vm.dpmi.virtual_interrupts_enabled ||
        vm.dpmi.interrupt_depth != 3 || vm.dpmi.control_depth != 2 ||
        io.pic[0].isr != 0x21 || io.pic[0].mask != 0x80 ||
        io.pit[0].reload != 12345 || io.pit[0].irq_period != 6789) return 4;
    puts("PASS IRQ trace: null guards, publication, 512-slot wrap and unchanged device/CPU state");
    return 0;
}
#else
int main(void)
{
    unsigned evaluated = 0;
    dos_io_irq_trace(++evaluated, ++evaluated, ++evaluated, ++evaluated);
    if (evaluated) return 1;
    puts("PASS IRQ trace disabled: no argument evaluation or observer symbols");
    return 0;
}
#endif
