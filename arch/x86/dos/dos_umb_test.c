#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_io.h"
#include "dos_loader.h"
#include "dos_mem.h"
#include "dos_xms.h"

extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);

int dos_umb_selftest(void)
{
    unsigned checks = 0, failures = 0;
    const unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(DOS_VM_ADDRESS_SPACE_SIZE / 4096u);
    cpu8086_state_t cpu = {0};
    if (!vm || !memory) {
        dos_host_free_pages(vm, vm_pages);
        dos_host_free_pages(memory, DOS_VM_ADDRESS_SPACE_SIZE / 4096u);
        return 1;
    }
    memset(vm, 0, sizeof(*vm)); memset(memory, 0, DOS_VM_ADDRESS_SPACE_SIZE);
    vm->mem = memory; vm->total_mem_size = DOS_VM_ADDRESS_SPACE_SIZE;
    vm->system_mem_size = DOS_TOTAL_MEM; vm->cpu = &cpu;
    vm->emulate_cpu = true; vm->current_psp = 0x1234;
    cpu8086_init(&cpu, vm); dos_init_ivt(vm); dpmi_init(vm); dos_mem_init(vm);
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DOS-UMB] failed line="); serial_putdec(__LINE__); serial_puts("\n"); } } while (0)
#define API(function, value) do { cpu.ax = (function); cpu.bx = (value); \
    cpu.flags = FLAGS_FIXED | FLAG_CF; dos_int21_dispatch(vm); } while (0)
#define OK() CHECK(!(cpu.flags & FLAG_CF))
#define ERROR(code) CHECK((cpu.flags & FLAG_CF) && cpu.ax == (code))
#define XMS(function) do { cpu.ah = (function); dos_xms_dispatch(vm); } while (0)
    API(0x5803, 1); ERROR(1);
    if (!dos_io_init(vm) || !dos_xms_init(vm)) { failures++; goto cleanup; }
    const uint16_t policies[] = {0, 1, 2, 0x40, 0x41, 0x42, 0x80, 0x81, 0x82};
    for (unsigned i = 0; i < sizeof(policies) / sizeof(policies[0]); i++) {
        dos_mem_init(vm);
        API(0x5801, policies[i]); OK(); API(0x5800, 0); OK(); CHECK(cpu.ax == policies[i]);
        API(0x4800, 0x10); OK(); uint16_t low = cpu.ax;
        CHECK(low < 0xA000); cpu.es = low; API(0x4900, 0); OK();
        API(0x5803, 1); OK(); API(0x5802, 0); OK(); CHECK(cpu.al == 1);
        API(0x5200, 0); CHECK(cpu.es == 0x50 && cpu.bx == 0x80);
        uint32_t lol = dos_linear(cpu.es, cpu.bx);
        CHECK(dos_mem_read16(vm, lol + 0x66) == 0x9FFF &&
              dos_mem_read8(vm, lol + 0x63) == 1);
        API(0x4800, 0x10); OK(); uint16_t block = cpu.ax;
        CHECK((block >= 0xD001) == (policies[i] >= 0x40));
        if ((policies[i] & 3u) == 2u)
            CHECK(block == (policies[i] >= 0x40 ? 0xDFF0 : 0x9FEF));
        API(0x5803, 0); OK();
        cpu.es = block; API(0x4A00, 8); OK(); API(0x4900, 0); OK();
        API(0x4800, 0x10); OK(); CHECK(cpu.ax < 0xA000);
        cpu.es = cpu.ax; API(0x4900, 0); OK();
    }
    const uint16_t invalid[] = {3, 0x43, 0x83, 0xC0, 0x100, 0x180, 0xFFFF};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint8_t policy = vm->allocation_strategy;
        API(0x5801, invalid[i]); ERROR(1); CHECK(vm->allocation_strategy == policy);
        API(0x5803, invalid[i]); ERROR(1); CHECK(!vm->uppermem_link);
    }
    dos_mem_init(vm); API(0x5803, 1); OK(); API(0x5801, 0x40); OK();
    API(0x4800, 0xFFFF); ERROR(8); CHECK(cpu.bx == 0xFFF);
    uint16_t largest = 0; CHECK(dos_mem_largest_available(vm, &largest) == 0 && largest == 0xFFF);
    API(0x4800, 0xFFF); OK(); uint16_t high = cpu.ax;
    API(0x4800, 1); ERROR(8); CHECK(cpu.bx == 0);
    API(0x5801, 0x80); OK(); API(0x4800, 1); OK(); CHECK(cpu.ax < 0xA000);
    cpu.es = cpu.ax; API(0x4900, 0); OK();
    cpu.es = high; API(0x4A00, 0x1000); ERROR(8); CHECK(cpu.bx == 0xFFF);
    CHECK(((dos_mcb_t *)(memory + ((uint32_t)(high - 1u) << 4)))->size == 0xFFF);
    API(0x4900, 0); OK();
    cpu.es = 0xA000; API(0x4900, 0); ERROR(9); API(0x4A00, 1); ERROR(9);
    cpu.es = 0xFFFF; API(0x4900, 0); ERROR(9); API(0x4A00, 1); ERROR(9);
    cpu.dx = 0xD000; XMS(0x11); CHECK(cpu.ax == 0 && cpu.bl == 0xB2);
    cpu.dx = 1; XMS(0x10); CHECK(cpu.ax == 0 && cpu.bl == 0xB1);

    /* MCB corruption must fail before modifying either arena. */
    for (unsigned fault = 0; fault < 6; fault++) {
        uint16_t segment = fault < 2 ? vm->first_mcb : fault < 4 ? vm->uppermem_root : 0xD000;
        dos_mcb_t *mcb = (dos_mcb_t *)(memory + ((uint32_t)segment << 4));
        dos_mcb_t saved = *mcb;
        if (fault == 0 || fault == 4) mcb->type = 'X';
        else if (fault == 3) mcb->owner = 0;
        else mcb->size = 0xFFFF;
        dos_mcb_t damaged = *mcb;
        cpu.es = high;
        API(0x4800, 1); ERROR(7); API(0x4900, 0); ERROR(7);
        API(0x4A00, 1); ERROR(7); API(0x5803, 0); ERROR(7);
        CHECK(vm->uppermem_link == 1 && memcmp(mcb, &damaged, sizeof(*mcb)) == 0);
        *mcb = saved;
    }

    /* Unlinked cleanup includes a child's upper blocks but not its parent's. */
    API(0x5801, 0x40); OK();
    uint16_t parent = dos_mem_alloc_owned(vm, 0x20, NULL, 0x1234);
    uint16_t child = dos_mem_alloc_owned(vm, 0x30, NULL, 0x5678);
    CHECK(parent >= 0xD001 && child > parent);
    memory[(uint32_t)parent << 4] = 0xA7;
    API(0x5803, 0); OK();
    CHECK(dos_mem_free_owner(vm, 0x5678) == 0 && memory[(uint32_t)parent << 4] == 0xA7);
    CHECK(dos_mem_query_block(vm, child, NULL, NULL) == DOS_MEM_ERR_BLOCK);
    CHECK(dos_mem_query_block(vm, parent, NULL, NULL) == 0);
    CHECK(dos_mem_free_owner(vm, 0x1234) == 0);
    API(0x5803, 1); OK(); API(0x4800, 0xFFFF); ERROR(8); CHECK(cpu.bx == 0xFFF);

    /* Foreign XMS allocations split DOS into separate, protected regions. */
    const uint16_t sizes[] = {0x20, 0x41, 0x10, 0x21, 0x10, 0xF5E};
    for (unsigned fit = 0; fit < 3; fit++) {
        dos_mem_init(vm);
        uint16_t blocks[6] = {0};
        for (unsigned i = 0; i < 6; i++) {
            cpu.dx = sizes[i]; XMS(0x10); CHECK(cpu.ax == 1);
            blocks[i] = cpu.bx;
            if (!(i & 1u)) memset(memory + ((uint32_t)blocks[i] << 4), 0xA5, sizes[i] * 16u);
        }
        for (unsigned i = 1; i < 6; i += 2) { cpu.dx = blocks[i]; XMS(0x11); CHECK(cpu.ax == 1); }
        API(0x5803, 1); OK(); API(0x5801, 0x40 | fit); OK();
        API(0x4800, 0x10); OK();
        CHECK(cpu.ax == (fit == 0 ? blocks[1] + 1u : fit == 1 ? blocks[3] + 1u : 0xDFF0));
        cpu.es = cpu.ax; API(0x4900, 0); OK();
        for (unsigned i = 0; i < 6; i += 2) {
            bool same = true;
            for (unsigned p = 0; p < sizes[i] * 16u; p++)
                if (memory[((uint32_t)blocks[i] << 4) + p] != 0xA5) same = false;
            CHECK(same);
            cpu.es = blocks[i] + 1u; API(0x4900, 0); ERROR(9);
        }
        uint16_t bridge = blocks[1] + sizes[1] - 1u;
        cpu.es = bridge + 1u; API(0x4900, 0); ERROR(9);
        dos_mcb_t *mcb = (dos_mcb_t *)(memory + ((uint32_t)bridge << 4));
        uint16_t saved = mcb->size; mcb->size--;
        API(0x4800, 1); ERROR(7); mcb->size = saved;
        API(0x5803, 0); OK();
        CHECK(dos_mem_free_owner(vm, 8) == 0);
        for (unsigned i = 0; i < 6; i += 2) { cpu.dx = blocks[i]; XMS(0x11); CHECK(cpu.ax == 1); }
    }
    dos_mem_init(vm);
    cpu.dx = 1; XMS(0x10); CHECK(cpu.ax == 1); uint16_t one = cpu.bx;
    cpu.dx = 0xFFE; XMS(0x10); CHECK(cpu.ax == 1); uint16_t middle = cpu.bx;
    cpu.dx = one; XMS(0x11); CHECK(cpu.ax == 1);
    API(0x5803, 1); OK(); API(0x5801, 0x40); OK();
    API(0x4800, 1); ERROR(8); CHECK(cpu.bx == 0);
    API(0x4800, 0); OK(); CHECK(cpu.ax == 0xE000);
    cpu.es = cpu.ax; API(0x4900, 0); OK();
    cpu.dx = middle; XMS(0x11); CHECK(cpu.ax == 1);

    /* Both DOS and DPMI use the same high-block lifetime and descriptors. */
    dos_mem_init(vm); API(0x5803, 1); OK(); API(0x5801, 0x40); OK();
    vm->dpmi.active = true; vm->dpmi.is_32bit = true;
    cpu.ax = 0x0100; cpu.bx = 0x20; dos_int31_dpmi(vm);
    uint16_t selector = cpu.dx; high = cpu.ax;
    CHECK(!(cpu.flags & FLAG_CF) && high >= 0xD001 &&
          dpmi_desc_get_base(&vm->dpmi.ldt[dpmi_sel_to_index(selector)]) == (uint32_t)high * 16u);
    CHECK(dos_mem_set_umb_link(vm, 0) == 0);
    cpu.ax = 0x0102; cpu.bx = 0x10; cpu.dx = selector; dos_int31_dpmi(vm); OK();
    cpu.ax = 0x0101; cpu.dx = selector; dos_int31_dpmi(vm); OK();
    vm->dpmi.active = false;

    /* A short COM's initial return word must stay inside its actual block. */
    dos_mem_init(vm); CHECK(dos_mem_set_umb_link(vm, 1) == 0);
    vm->allocation_strategy = 0x40;
    const uint8_t program[] = {0xC3};
    dos_load_spec_t spec = {.initialize_cpu = true};
    uint16_t psp = 0;
    memset(memory + 0xE0000, 0xA6, 0x10000);
    CHECK(dos_load_com_process(vm, program, sizeof(program), "UMB.COM", &spec, &psp) == 0);
    uint16_t paragraphs = 0;
    CHECK(psp >= 0xD001 && dos_mem_query_block(vm, psp, &paragraphs, NULL) == 0);
    CHECK(cpu.ss == psp && cpu.sp == paragraphs * 16u - 2u &&
          dos_mem_read16(vm, dos_linear(cpu.ss, cpu.sp)) == 0);
    bool same = true;
    for (unsigned i = 0; i < 0x10000; i++) if (memory[0xE0000 + i] != 0xA6) same = false;
    CHECK(same && dos_mem_set_umb_link(vm, 0) == 0 && dos_mem_free_owner(vm, psp) == 0);
    dos_mem_init(vm);
    cpu.dx = 0x1000; XMS(0x10); CHECK(cpu.ax == 1 && cpu.bx == 0xD000);
    API(0x5803, 1); ERROR(1);
    cpu.dx = 0xD000; XMS(0x11); CHECK(cpu.ax == 1);
cleanup:
    dos_xms_cleanup(vm); dos_io_shutdown(vm);
    dos_host_free_pages(memory, DOS_VM_ADDRESS_SPACE_SIZE / 4096u);
    dos_host_free_pages(vm, vm_pages);
    serial_puts("[DOS-UMB] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CHECK
#undef API
#undef OK
#undef ERROR
#undef XMS
    return (int)failures;
}
