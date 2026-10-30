#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_io.h"
#include "dos_xms.h"

extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);

static void xms_test_move(dos_vm_t *vm, uint32_t length, uint16_t source,
                            uint32_t source_offset, uint16_t destination,
                            uint32_t destination_offset)
{
    uint32_t p = dpmi_desc_get_base(&vm->cpu->ds_cache.descriptor) + vm->cpu->si;
    dos_mem_write32(vm, p, length);
    dos_mem_write16(vm, p + 4, source);
    dos_mem_write32(vm, p + 6, source_offset);
    dos_mem_write16(vm, p + 10, destination);
    dos_mem_write32(vm, p + 12, destination_offset);
    vm->cpu->ah = 0x0B;
    dos_xms_dispatch(vm);
}

int dos_xms_selftest(void)
{
    unsigned checks = 0, failures = 0;
    const uint32_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
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
    vm->emulate_cpu = true; vm->step_limit = 1000000;
    cpu8086_init(&cpu, vm); cpu.running = true;
    dos_init_ivt(vm); dpmi_init(vm);
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DOS-XMS] failed line="); serial_putdec(__LINE__); serial_puts("\n"); } } while (0)
#define CALL(function) do { cpu.ah = (function); dos_xms_dispatch(vm); } while (0)
#define OK() CHECK(cpu.ax == 1 && cpu.bl == 0)
#define ERROR(code) CHECK(cpu.ax == 0 && cpu.bl == (code))
    CHECK(!dos_xms_init(vm));
    if (!dos_io_init(vm) || !dos_xms_init(vm)) { failures++; goto cleanup; }
    dos_io_write8(vm, 0x21, 0xFF);
    uint32_t total = dpmi_ext_total_pages(vm);
    CHECK(dpmi_ext_free_page_count(vm) == total - 16u);
    CHECK(dos_xms_init(vm) && dpmi_ext_free_page_count(vm) == total - 16u);
    cpu.eax = 0xABCD4300; cpu.ebx = 0x12345678; cpu.ecx = 0x98765432;
    cpu.edx = 0x56789ABC; cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF;
    dos_int2f_dispatch(vm);
    CHECK(cpu.eax == 0xABCD4380 && cpu.ebx == 0x12345678 &&
          cpu.ecx == 0x98765432 && cpu.edx == 0x56789ABC &&
          cpu.eflags == (FLAGS_FIXED | FLAG_CF | FLAG_DF));
    cpu.ax = 0x4310; dos_int2f_dispatch(vm);
    CHECK(cpu.es == 0xF000 && cpu.bx == DOS_XMS_ENTRY_OFF &&
          memory[DOS_ROM_BASE + DOS_XMS_ENTRY_OFF] == 0xEB);
    CALL(0);
    CHECK(cpu.eax == 0xABCD0300 && cpu.ebx == 0x12340100 && cpu.edx == 0x56780001);
    cpu.ax = 0x8800; dos_int_dispatch(vm, 0x15);
    CHECK(cpu.ax == (DOS_TOTAL_MEM - 0x100000u) / 1024u && !dos_xms_bios_hooked(vm));
    CALL(7); CHECK(cpu.ax == 1 && cpu.bl == 0 && dos_xms_bios_hooked(vm));
    cpu.ax = 0x8800; dos_int_dispatch(vm, 0x15); CHECK(cpu.ax == 0);
    CALL(4); OK(); CHECK(!dos_a20_enabled(vm));
    CALL(5); OK(); CALL(5); OK();
    CALL(6); ERROR(0x94); CHECK(dos_a20_enabled(vm));
    CALL(3); OK(); CALL(3); OK();
    CALL(6); ERROR(0x94); CALL(4); OK();
    CALL(6); ERROR(0x82); CHECK(!dos_a20_enabled(vm));
    CALL(5); OK(); dos_io_set_a20(vm, false);
    CALL(5); OK(); CHECK(dos_a20_enabled(vm));
    CALL(6); ERROR(0x94); CALL(6); OK();
    cpu.dx = 0xFFFF; CALL(1); OK(); CALL(1); ERROR(0x91);
    CALL(2); OK(); CALL(2); ERROR(0x93);
    memcpy(memory + DOS_XMS_HMA_BASE + 0x13, "VDISK", 5);
    CALL(1); ERROR(0x81);
    memset(memory + DOS_XMS_HMA_BASE + 0x13, 0, 5);
    cpu.dx = 0; CALL(1); OK(); CALL(2); OK();
    uint32_t shared = dpmi_ext_alloc_pages(vm, 4, false);
    CHECK(shared == DOS_XMS_HMA_BASE + DOS_XMS_HMA_SIZE);
    uint32_t free_before = dpmi_ext_free_page_count(vm);
    CALL(8); CHECK(cpu.ax == free_before * 4u && cpu.dx == cpu.ax && cpu.bl == 0);
    CALL(0x88); CHECK(cpu.eax == free_before * 4u && cpu.edx == cpu.eax &&
                     cpu.ecx == DOS_TOTAL_MEM - 1u && cpu.bl == 0);
    cpu.edx = UINT32_MAX; CALL(0x89); ERROR(0xA0); CHECK(cpu.dx == 0);
    cpu.dx = 1; CALL(9); OK(); uint16_t a = cpu.dx;
    cpu.dx = 6; CALL(9); OK(); uint16_t b = cpu.dx;
    CHECK(a && b && a != b && dpmi_ext_free_page_count(vm) == free_before - 3u);
    cpu.dx = a; CALL(0x0C); CHECK(cpu.ax == 1);
    uint32_t base = ((uint32_t)cpu.dx << 16) | cpu.bx;
    CHECK(base >= 0x110000 && base + 4096u <= DPMI_EXT_BASE + total * 4096u);
    cpu.dx = a; CALL(0x0D); OK();
    cpu.ds = 0; cpu.si = 0x6000; cpu8086_sync_segment(&cpu, 3);
    for (unsigned i = 0; i < 512; i++) memory[0x7000 + i] = (uint8_t)(i * 13u);
    xms_test_move(vm, 256, 0, 0x7000, a, 2); OK();
    CHECK(!memcmp(memory + base + 2, memory + 0x7000, 256) && !dos_a20_enabled(vm));
    xms_test_move(vm, 256, a, 2, 0, 0x7400); OK();
    CHECK(!memcmp(memory + 0x7400, memory + 0x7000, 256));
    xms_test_move(vm, 256, a, 2, b, 0); OK();
    xms_test_move(vm, 256, b, 0, 0, 0x7500); OK();
    CHECK(!memcmp(memory + 0x7500, memory + 0x7000, 256));
    cpu_system_segment_t saved_ds = cpu.ds_cache;
    cpu.ds_cache.valid = false; CALL(0x0B); ERROR(0xA7);
    cpu.ds_cache = saved_ds;
    dpmi_desc_set_limit(&cpu.ds_cache.descriptor, cpu.si + 14u);
    CALL(0x0B); ERROR(0xA7); cpu.ds_cache = saved_ds;
    cpu.si = 0xF000; dpmi_desc_set_base(&cpu.ds_cache.descriptor, 0xFFFF7000u);
    CALL(0x0B); ERROR(0xA7);
    cpu.ds_cache = saved_ds; cpu.si = 0x6000;
    xms_test_move(vm, 0, a, 1024, b, 6144); OK();
    xms_test_move(vm, 3, a, 0, b, 0); ERROR(0xA7);
    xms_test_move(vm, 2, 0xFFFF, 0, b, 0); ERROR(0xA3);
    xms_test_move(vm, 2, a, 1025, b, 0); ERROR(0xA4);
    xms_test_move(vm, 2, a, 0, 0xFFFF, 0); ERROR(0xA5);
    xms_test_move(vm, 2, a, 0, b, 6145); ERROR(0xA6);
    xms_test_move(vm, 4, a, 1022, b, 0); ERROR(0xA7);
    xms_test_move(vm, 4, a, 0, b, 6142); ERROR(0xA7);
    xms_test_move(vm, 2, 0, UINT32_MAX, b, 0); ERROR(0xA7);
    CHECK(!memcmp(memory + 0x7000, memory + 0x7400, 256));
    xms_test_move(vm, 32, 0, 0xFFFF0000, b, 1024); OK();
    CHECK(!dos_a20_enabled(vm));
    xms_test_move(vm, 128, 0, 0x7000, 0, 0x7002); OK();
    for (unsigned i = 0; i < 128; i++) CHECK(memory[0x7002 + i] == (uint8_t)(i * 13u));
    vm->ems_frame_bases[0] = base;
    memcpy(memory + base, memory + 0x7400, 256);
    xms_test_move(vm, 128, 0, 0xE0000000u, a, 2); OK();
    CHECK(!memcmp(memory + base + 2, memory + 0x7400, 128));
    vm->ems_frame_bases[0] = 0;
    cpu.dx = a; cpu.bx = 12; CALL(0x0F); OK();
    cpu.dx = a; CALL(0x0C); CHECK(cpu.ax == 1);
    base = ((uint32_t)cpu.dx << 16) | cpu.bx;
    CHECK(!memcmp(memory + base + 2, memory + 0x7400, 128));
    cpu.dx = a; CALL(0x0D); OK();
    cpu.dx = a; cpu.ebx = UINT32_MAX; CALL(0x8F); ERROR(0xA0);
    cpu.dx = a; CALL(0x8E); CHECK(cpu.ax == 1 && cpu.edx == 12 && cpu.bh == 0);
    CHECK(!memcmp(memory + base + 2, memory + 0x7400, 128));
    cpu.dx = a; cpu.bx = 1; CALL(0x0F); OK();
    CHECK(!memcmp(memory + base + 2, memory + 0x7400, 128));
    for (unsigned i = 0; i < 255; i++) { cpu.dx = a; CALL(0x0C); CHECK(cpu.ax == 1); }
    cpu.dx = a; CALL(0x0C); ERROR(0xAC);
    cpu.dx = a; CALL(0x0E); CHECK(cpu.ax == 1 && cpu.bh == 255 && cpu.dx == 1);
    cpu.dx = a; CALL(0x0A); ERROR(0xAB);
    cpu.dx = a; cpu.bx = 2; CALL(0x0F); ERROR(0xAB);
    for (unsigned i = 0; i < 255; i++) { cpu.dx = a; CALL(0x0D); OK(); }
    cpu.dx = a; CALL(0x0D); ERROR(0xAA);
    cpu.dx = a; cpu.ebx = 0; CALL(0x8F); OK();
    cpu.dx = a; CALL(0x8E); CHECK(cpu.ax == 1 && cpu.edx == 0);
    cpu.dx = a; CALL(0x0A); OK(); cpu.dx = a; CALL(0x0A); ERROR(0xA2);
    cpu.dx = b; CALL(0x0A); OK();
    CHECK(dpmi_ext_free_page_count(vm) == free_before);
    cpu.edx = free_before * 4u; CALL(0x89); OK(); a = cpu.dx;
    CALL(8); CHECK(cpu.ax == 0 && cpu.dx == 0 && cpu.bl == 0xA0);
    CALL(0x88); CHECK(cpu.eax == 0 && cpu.edx == 0 && cpu.ecx == DOS_TOTAL_MEM - 1u && cpu.bl == 0xA0);
    cpu.dx = 1; CALL(9); ERROR(0xA0); CHECK(cpu.dx == 0);
    cpu.dx = 0; CALL(9); OK(); b = cpu.dx;
    cpu.dx = b; CALL(0x0A); OK(); cpu.dx = a; CALL(0x0A); OK();
    CHECK(dpmi_ext_free_page_count(vm) == free_before);
    /* Force IRQ0 during a pinned copy, then grow the table from that IRQ. */
    cpu.dx = 32; CALL(9); OK(); a = cpu.dx;
    cpu.dx = 32; CALL(9); OK(); b = cpu.dx;
    cpu.dx = a; CALL(0x0C); CHECK(cpu.ax == 1);
    base = ((uint32_t)cpu.dx << 16) | cpu.bx;
    cpu.dx = a; CALL(0x0D); OK();
    for (unsigned i = 0; i < 32768; i++) memory[base + i] = (uint8_t)(i * 7u + 3u);
    const uint8_t irq[] = {
        0x1E, 0x60, 0x0E, 0x1F, 0xBA, (uint8_t)a, (uint8_t)(a >> 8),
        0xB4, 0x0A, 0xFF, 0x1E, 0x00, 0x91, 0x88, 0x1E, 0x20, 0x91,
        0xBE, 0x00, 0xB0, 0xB9, 0x90, 0x01,
        0x31, 0xD2, 0xB4, 0x09, 0xFF, 0x1E, 0x00, 0x91,
        0x89, 0x14, 0x83, 0xC6, 0x02, 0xE2, 0xF1,
        0xB0, 0x20, 0xE6, 0x20, 0x61, 0x1F, 0xCF
    };
    memcpy(memory + 0x9000, irq, sizeof(irq));
    dos_mem_write16(vm, 8u * 4u, 0x9000); dos_mem_write16(vm, 8u * 4u + 2u, 0);
    dos_mem_write32(vm, 0x9100, 0xF0000000u | DOS_XMS_ENTRY_OFF);
    cpu8086_load_real_cs(&cpu, 0); cpu.eip = 0x9800;
    cpu.ss = 0; cpu.esp = 0xD000; cpu8086_sync_segment(&cpu, 2);
    vm->software_int_return_flags = FLAGS_FIXED | FLAG_IF;
    vm->timer_irq_pending = true; dos_io_write8(vm, 0x21, 0xFE);
    xms_test_move(vm, 32768, a, 0, b, 0); OK();
    CHECK(cpu.running && cpu.cs == 0 && cpu.eip == 0x9800 && cpu.esp == 0xD000 && memory[0x9120] == 0xAB);
    vm->software_int_return_flags = 0; dos_io_write8(vm, 0x21, 0xFF);
    cpu.dx = b; CALL(0x0C); CHECK(cpu.ax == 1);
    CHECK(!memcmp(memory + (((uint32_t)cpu.dx << 16) | cpu.bx), memory + base, 32768));
    cpu.dx = b; CALL(0x0D); OK();
    for (unsigned i = 0; i < 400; i++) { cpu.dx = dos_mem_read16(vm, 0xB000 + i * 2u); CALL(0x0A); OK(); }
    cpu.dx = a; CALL(0x0A); OK(); cpu.dx = b; CALL(0x0A); OK();
    CHECK(dpmi_ext_free_page_count(vm) == free_before);
    uint16_t ids[512];
    for (unsigned i = 0; i < 512; i++) { cpu.dx = 0; CALL(9); OK(); ids[i] = cpu.dx; }
    CHECK(ids[511] > 256 && dpmi_ext_free_page_count(vm) == free_before);
    for (unsigned i = 0; i < 512; i++) { cpu.dx = ids[i]; CALL(0x0A); OK(); }
    cpu.dx = 0xFFFF; CALL(0x10); ERROR(0xB0); CHECK(cpu.dx == DOS_XMS_UMB_SIZE / 16u);
    cpu.dx = 32; CALL(0x10); CHECK(cpu.ax == 1 && cpu.bx == DOS_XMS_UMB_BASE >> 4 && cpu.dx == 32);
    uint16_t umb = cpu.bx;
    cpu.dx = 64; CALL(0x10); CHECK(cpu.ax == 1 && cpu.bx == umb + 32 && cpu.dx == 64);
    uint16_t second = cpu.bx;
    cpu.dx = umb + 1; CALL(0x11); ERROR(0xB2);
    cpu.dx = umb; cpu.bx = 33; CALL(0x12); ERROR(0xB0); CHECK(cpu.dx == 32);
    cpu.dx = second; CALL(0x11); OK();
    cpu.dx = umb; cpu.bx = 96; CALL(0x12); OK();
    cpu.dx = umb; cpu.bx = 16; CALL(0x12); OK();
    cpu.dx = umb; CALL(0x11); OK(); cpu.dx = umb; CALL(0x11); ERROR(0xB2);
    cpu.dx = DOS_XMS_UMB_SIZE / 16u; CALL(0x10); CHECK(cpu.ax == 1);
    cpu.dx = 1; CALL(0x10); ERROR(0xB1); CHECK(cpu.dx == 0);
    cpu.dx = umb; CALL(0x11); OK();
    CALL(0xFF); ERROR(0x80);
    dos_xms_cleanup(vm); dos_xms_cleanup(vm);
    CHECK(dpmi_ext_free_page_count(vm) == total - 4u);
    CHECK(dpmi_ext_free_pages(vm, shared, 4) && dpmi_ext_free_page_count(vm) == total);
    uint32_t occupied = dpmi_ext_alloc_pages(vm, 1, false);
    CHECK(occupied == DOS_XMS_HMA_BASE && !dos_xms_init(vm));
    CHECK(dpmi_ext_free_pages(vm, occupied, 1));
    uint32_t old_int19 = dos_mem_read32(vm, 0x19u * 4u);
    dos_mem_write32(vm, 0x19u * 4u, 0x80000000u);
    memcpy(memory + 0x80013, "VDISK", 5);
    CHECK(!dos_xms_init(vm) && !vm->xms && dpmi_ext_free_page_count(vm) == total);
    memset(memory + 0x80013, 0, 5); dos_mem_write32(vm, 0x19u * 4u, old_int19);
    memcpy(memory + DOS_XMS_HMA_BASE + 0x13, "VDISK", 5);
    CHECK(!dos_xms_init(vm) && !vm->xms && dpmi_ext_free_page_count(vm) == total);
    memset(memory + DOS_XMS_HMA_BASE + 0x13, 0, 5);
    vm->system_mem_size = 0x100000u;
    CHECK(dos_xms_init(vm));
    CALL(0); CHECK(cpu.ax == 0x0300 && cpu.dx == 0);
    cpu.dx = 0xFFFF; CALL(1); ERROR(0x90); CALL(2); ERROR(0x90);
    CALL(0x88); CHECK(cpu.eax == 0 && cpu.edx == 0 && cpu.ecx == 0xFFFFF && cpu.bl == 0xA0);
    dos_xms_cleanup(vm); vm->system_mem_size = DOS_TOTAL_MEM;
cleanup:
    dos_xms_cleanup(vm); dos_io_shutdown(vm);
    dos_host_free_pages(memory, DOS_VM_ADDRESS_SPACE_SIZE / 4096u);
    dos_host_free_pages(vm, vm_pages);
    serial_puts("[DOS-XMS] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CHECK
#undef CALL
#undef OK
#undef ERROR
    return (int)failures;
}
