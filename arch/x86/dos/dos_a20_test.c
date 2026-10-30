#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_io.h"
#include "dos_jit.h"

extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);

int dos_a20_selftest(void)
{
    dos_vm_t vm = {0}, other = {0};
    cpu8086_state_t cpu = {0};
    const unsigned pages = DOS_VM_ADDRESS_SPACE_SIZE / DOS_PAGE_SIZE;
    uint8_t *memory = dos_host_alloc_pages(pages);
    const unsigned block_pages = (sizeof(jit_block_t) + 4095u) / 4096u;
    jit_block_t *block = dos_host_alloc_pages(block_pages);
    if (!memory || !block || !dos_io_init(&vm) || !dos_io_init(&other)) {
        dos_io_shutdown(&vm); dos_io_shutdown(&other);
        if (memory) dos_host_free_pages(memory, pages);
        if (block) dos_host_free_pages(block, block_pages);
        return 1;
    }
    memset(memory, 0, pages * DOS_PAGE_SIZE);
    vm.cpu = &cpu; vm.mem = memory; vm.total_mem_size = pages * DOS_PAGE_SIZE;
    vm.emulate_cpu = true; vm.step_limit = 100000;
    cpu8086_init(&cpu, &vm);
    unsigned checks = 0, failures = 0;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DOS-A20] failed line="); serial_putdec(__LINE__); serial_puts("\n"); } } while (0)
    CHECK(dos_a20_enabled(&vm) && dos_io_read8(&vm, 0x92) == 0);
    /* Every gate combination, both command paths and independent VMs. */
    for (unsigned kbc = 0; kbc < 2; kbc++) {
        for (unsigned fast = 0; fast < 2; fast++) {
            dos_io_write8(&vm, 0x64, 0xd1);
            dos_io_write8(&vm, 0x60, 0xcdu | (kbc << 1));
            dos_io_write8(&vm, 0x92, fast << 1);
            CHECK(dos_a20_enabled(&vm) == (kbc || fast));
            CHECK(!other.a20_masked && dos_io_read8(&other, 0x92) == 0);
            dos_io_write8(&vm, 0x64, 0xd0);
            CHECK(dos_io_read8(&vm, 0x60) == (0xcdu | (kbc << 1)));
            CHECK(dos_io_read8(&vm, 0x92) == fast << 1);
        }
    }
    dos_io_write8(&vm, 0x92, 0);
    dos_io_write8(&vm, 0x64, 0xdd);
    CHECK(vm.a20_masked);
    dos_io_write8(&vm, 0x64, 0xdf);
    CHECK(!vm.a20_masked);

    for (unsigned function = 0; function < 5; function++) {
        cpu.eax = 0xa5a52400u | function;
        cpu.ebx = 0xb6b61234; cpu.ecx = 0xc7c75678; cpu.edx = 0xd8d89abc;
        cpu.esi = 0x12345678; cpu.edi = 0x87654321; cpu.ebp = 0xfeed1234;
        cpu.eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF | FLAG_ZF;
        dos_int_dispatch(&vm, 0x15);
        uint32_t expected_ax = 0xa5a50000u | (function == 2 ? 1u : function);
        if (function == 4) expected_ax |= 0x8600u;
        CHECK(cpu.eax == expected_ax && cpu.ebx == (function == 3 ? 0xb6b60003u : 0xb6b61234u));
        CHECK(cpu.ecx == 0xc7c75678 && cpu.edx == 0xd8d89abc &&
              cpu.esi == 0x12345678 && cpu.edi == 0x87654321 && cpu.ebp == 0xfeed1234);
        CHECK(cpu.eflags == (uint32_t)(FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_ZF |
                                      (function == 4 ? FLAG_CF : 0)));
    }
    /* BIOS changes must not consume a pending keyboard command/data byte. */
    dos_io_write8(&vm, 0x64, 0xd1);
    CHECK(dos_io_set_a20(&vm, false));
    dos_io_write8(&vm, 0x60, 3);
    CHECK(!vm.a20_masked);
    dos_io_write8(&vm, 0x92, 2);
    CHECK(dos_io_set_a20(&vm, false) && vm.a20_masked && !dos_io_read8(&vm, 0x92));

    for (unsigned high = 0; high < 4; high++) {
        uint32_t low = high * 0x200000u + 0x8123u, alias = low | 0x100000u;
        memory[low] = 0x35; memory[alias] = 0xa7;
        CHECK(dos_mem_read8(&vm, alias) == 0x35);
        dos_mem_write8(&vm, alias, 0x58);
        CHECK(memory[low] == 0x58 && memory[alias] == 0xa7);
        CHECK(dos_a20_address(&vm, alias) == low);
    }
    dos_mem_write32(&vm, 0xfffff, 0x44332211);
    CHECK(memory[0xfffff] == 0x11 && memory[0] == 0x22 && memory[1] == 0x33 && memory[2] == 0x44);
    CHECK(dos_mem_read32(&vm, 0xfffff) == 0x44332211);
    memory[0x100000] = 0xc1;
    CHECK(dos_phys_read8(&vm, 0x100000) == 0xc1);
    dos_phys_write8(&vm, 0x100000, 0xc2);
    CHECK(memory[0x100000] == 0xc2 && memory[0] == 0x22 && vm.a20_masked);

    uint8_t *direct = NULL;
    CHECK(dos_mem_guest_span(&vm, 0x108000, 32, &direct) && direct == memory + 0x8000);
    CHECK(dos_mem_guest_span(&vm, 0xffff0, 32, &direct) && direct == NULL);
    vm.total_mem_size = 0x100000;
    CHECK(dos_mem_guest_span(&vm, 0x108000, 32, &direct) && direct == memory + 0x8000);
    CHECK(dos_mem_guest_span(&vm, 0xffff0, 32, &direct) && direct == NULL);
    CHECK(!dos_mem_guest_span(&vm, 0x208000, 1, NULL));
    CHECK(!dos_mem_guest_span(&vm, UINT32_MAX, 2, NULL));
    vm.total_mem_size = pages * DOS_PAGE_SIZE;

    /* Mask bus addresses, never the backing page selected by EMS/VBE. */
    vm.ems_frame_bases[0] = 0x300000;
    memory[0x300010] = 0x73; memory[0x200010] = 0x21;
    CHECK(dos_mem_read8(&vm, 0x1e0010) == 0x73);
    dos_mem_write8(&vm, 0x1e0010, 0x74);
    CHECK(memory[0x300010] == 0x74 && memory[0x200010] == 0x21);
    vm.ems_frame_bases[0] = 0;
    vm.vbe_active = true; vm.vbe_bank = 16;
    memory[DOS_VBE_FB_BASE + 0x100021] = 0x95;
    CHECK(dos_mem_read8(&vm, 0x1a0021) == 0x95);
    dos_mem_write8(&vm, 0x1a0021, 0x96);
    CHECK(memory[DOS_VBE_FB_BASE + 0x100021] == 0x96);
    CHECK(dos_mem_guest_span(&vm, 0x1a0021, 1, &direct) && direct == NULL);
    vm.vbe_active = false;
    dos_io_vga_set_mode(&vm, 0x13, true); vm.vga_mode = 0x13;
    dos_mem_write8(&vm, 0x1a0010, 0x63);
    CHECK(dos_mem_read8(&vm, 0xa0010) == 0x63);
    vm.vga_mode = 3;

    /* Translation masks page-table cycles and physical results, not CR2. */
    cpu.cr0 = DOS_CR0_PG | 1; cpu.cr3 = 0x104000;
    dos_phys_write8(&vm, 0x4000, 7);
    dos_phys_write8(&vm, 0x4001, 0x50); dos_phys_write8(&vm, 0x4002, 0x10);
    dos_phys_write8(&vm, 0x5000, 7);
    dos_phys_write8(&vm, 0x5001, 0x80); dos_phys_write8(&vm, 0x5002, 0x10);
    dos_page_translation_t page;
    dos_page_fault_t fault;
    CHECK(dos_page_probe(&vm, 0x123, DOS_PAGE_WRITE | DOS_PAGE_USER, &page, &fault));
    CHECK(page.pde == 0x4000 && page.pte == 0x5000 && page.physical == 0x8123);
    dos_page_commit(&vm, &page, true);
    CHECK(memory[0x4000] == 0x27 && memory[0x5000] == 0x67 && !memory[0x104000]);
    CHECK(!dos_page_probe(&vm, 0x401000, DOS_PAGE_USER, &page, &fault) &&
          fault.linear == 0x401000 && fault.error == DOS_PAGE_USER);
    cpu.cr0 = 0;

    vm.dpmi.active = true; cpu.protected_mode = true; cpu.eflags = FLAGS_FIXED;
    CHECK(dos_a20_enabled(&vm) && dos_mem_read8(&vm, 0x100000) == 0xc2);
    cpu.eax = 0x2402; dos_int_dispatch(&vm, 0x15);
    CHECK(cpu.ax == 1 && !(cpu.flags & FLAG_CF));
    cpu.guest_idt = true;
    CHECK(!dos_a20_enabled(&vm));
    cpu.guest_idt = false; cpu.eflags |= FLAG_VM;
    CHECK(!dos_a20_enabled(&vm));
    cpu.eflags = FLAGS_FIXED; cpu.protected_mode = false;
    CHECK(vm.a20_masked && !dos_a20_enabled(&vm));
    vm.dpmi.active = false;

    /* BIOS 87h enables A20 for both descriptors and payload, then restores it. */
    uint32_t table = 0x100100;
    dpmi_descriptor_t source = { .limit_lo = 1, .access = 0x93 };
    dpmi_descriptor_t target = source;
    dpmi_desc_set_base(&source, 0x100200); dpmi_desc_set_base(&target, 0x100300);
    memcpy(memory + table + 16, &source, 8); memcpy(memory + table + 24, &target, 8);
    memory[0x100200] = 0x57; memory[0x100201] = 0x13;
    memory[0x200] = 0x68; memory[0x300] = 0xaa;
    cpu.es = 0xffff; cpu.si = 0x110; cpu.cx = 1; cpu.ax = 0x8700;
    dos_int_dispatch(&vm, 0x15);
    CHECK(cpu.ah == 0 && !(cpu.flags & FLAG_CF) && vm.a20_masked);
    CHECK(memory[0x100300] == 0x57 && memory[0x100301] == 0x13 && memory[0x300] == 0xaa);
    memory[table + 21] = 0; cpu.ax = 0x8700;
    dos_int_dispatch(&vm, 0x15);
    CHECK(cpu.ah == 2 && (cpu.flags & FLAG_CF) && vm.a20_masked);

    /* Fetch and decoded-block identity must track the effective bus bytes. */
    cpu8086_init(&cpu, &vm); cpu.eflags = FLAGS_FIXED;
    cpu8086_reset_real_cs(&cpu, 0xffff); cpu.ip = 0x4010;
    const uint8_t code[] = {0xbb, 0x34, 0x12, 0xeb, 0xfb};
    memcpy(memory + 0x4000, code, sizeof(code));
    memcpy(memory + 0x104000, code, sizeof(code)); memory[0x104001] = 0x78;
    memset(block, 0, sizeof(*block)); block->cs = cpu.cs; block->ip = cpu.ip;
    CHECK(jit_decode_block(&vm, block) > 0 && jit_block_current(&vm, block));
    CHECK(cpu8086_run_one(&vm) && cpu.bx == 0x1234);
    CHECK(dos_io_set_a20(&vm, true) && !jit_block_current(&vm, block));
    cpu.ip = 0x4010;
    CHECK(cpu8086_run_one(&vm) && cpu.bx == 0x1278);
    CHECK(dos_io_set_a20(&vm, false));
    vm.total_mem_size = 0x100000;
    CHECK(jit_decode_block(&vm, block) > 0 && jit_block_current(&vm, block));
    cpu.ip = 0x4010;
    CHECK(cpu8086_run_one(&vm) && cpu.bx == 0x1234);
#undef CHECK
    vm.total_mem_size = pages * DOS_PAGE_SIZE;
    /* Controller response tests latch IRQ1, but this memory fixture has no
     * installed BIOS IVT. Console tests below supply their own cooked queue. */
    dos_io_clear_irq(&vm, 1);
    dos_io_write8(&vm, 0x21, 0xff);
    failures += dos_a20_buffer_selftest(&vm);
    dos_io_shutdown(&vm); dos_io_shutdown(&other);
    dos_host_free_pages(memory, pages); dos_host_free_pages(block, block_pages);
    serial_puts("[DOS-A20] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}
