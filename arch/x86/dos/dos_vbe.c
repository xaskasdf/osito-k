/*
 * VESA BIOS Extensions 2.0 for the DOS virtual machine.
 *
 * A 4 MB guest-physical framebuffer follows system RAM. Windowed clients
 * reach it through A000:0000; linear clients map PhysBasePtr through DPMI.
 */

#include "cpu8086.h"
#include "dos_io.h"
#include "dos_mouse.h"
#include "dos_vbe.h"

extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *address, uint64_t count);
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);
extern void dos_int10_video(dos_vm_t *vm);
extern void dos_vga_flush(dos_vm_t *vm);
extern void dos_vga_invalidate_text(dos_vm_t *vm);
extern void dos_vga_vbe_update(dos_vm_t *vm);
extern void dos_native_map_vbe_window(dos_vm_t *vm);

#define VBE_SUCCESS             0x004Fu
#define VBE_FAILED              0x014Fu
#define VBE_NOT_SUPPORTED       0x024Fu
#define VBE_INVALID_MODE        0x034Fu

#define VBE_ROM_SEGMENT         0xF000u
#define VBE_ROM_LINEAR          0x000F0000u
#define VBE_WINDOW_FUNC_OFFSET  0x8000u
#define VBE_MODE_LIST_OFFSET    0x8020u
#define VBE_OEM_OFFSET          0x8060u
#define VBE_VENDOR_OFFSET       0x8080u
#define VBE_PRODUCT_OFFSET      0x8090u
#define VBE_REVISION_OFFSET     0x80B0u
#define VBE_PM_TABLE_OFFSET     0x8200u
#define VBE_PM_TABLE_SIZE       23u

#define VBE_MODE_LINEAR         0x4000u
#define VBE_MODE_NO_CLEAR       0x8000u
#define VBE_MODE_RESERVED_MASK  0x3E00u
#define VBE_MODE_NUMBER_MASK    0x01FFu

#define VBE_MODE_ATTR_SUPPORTED 0x0001u
#define VBE_MODE_ATTR_INFO      0x0002u
#define VBE_MODE_ATTR_COLOR     0x0008u
#define VBE_MODE_ATTR_GRAPHICS  0x0010u
#define VBE_MODE_ATTR_NON_VGA   0x0020u
#define VBE_MODE_ATTR_LFB       0x0080u

#define VBE_STATE_MAGIC         0x53454256u /* "VBES" */
#define VBE_STATE_VERSION       1u

typedef struct {
    uint16_t mode;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
} vbe_mode_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t requested;
    uint16_t mode;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t bank;
    uint16_t display_x;
    uint16_t display_y;
    uint8_t bpp;
    uint8_t bytes_per_pixel;
    uint8_t dac_width;
    uint8_t vga_mode;
    uint8_t vga_page;
    uint8_t text_attr;
    uint8_t cursor_start;
    uint8_t cursor_end;
    uint8_t active;
    uint8_t linear;
    uint8_t no_clear;
    uint8_t reserved[2];
    uint16_t cursor_positions[8];
    uint8_t palette[256][3];
} vbe_saved_state_t;

static const vbe_mode_t vbe_modes[] = {
    { 0x100,  640,  400,  8 },
    { 0x101,  640,  480,  8 },
    { 0x103,  800,  600,  8 },
    { 0x105, 1024,  768,  8 },
    { 0x107, 1280, 1024,  8 },
    { 0x10D,  320,  200, 15 },
    { 0x10E,  320,  200, 16 },
    { 0x10F,  320,  200, 24 },
    { 0x110,  640,  480, 15 },
    { 0x111,  640,  480, 16 },
    { 0x112,  640,  480, 24 },
    { 0x113,  800,  600, 15 },
    { 0x114,  800,  600, 16 },
    { 0x115,  800,  600, 24 },
    { 0x116, 1024,  768, 15 },
    { 0x117, 1024,  768, 16 },
    { 0x118, 1024,  768, 24 },
    { 0x119, 1280, 1024, 15 },
    { 0x11A, 1280, 1024, 16 },
    { 0x11B, 1280, 1024, 24 },
};

static const char vbe_oem[] = "OsitoK VBE 2.0";
static const char vbe_vendor[] = "OsitoK";
static const char vbe_product[] = "Virtual GOP Adapter";
static const char vbe_revision[] = "2.0";

static void vbe_zero(uint8_t *destination, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) destination[i] = 0;
}

static void vbe_copy_string(uint8_t *destination, const char *source)
{
    do {
        *destination++ = (uint8_t)*source;
    } while (*source++);
}

static void vbe_put16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1u] = (uint8_t)(value >> 8);
}

static void vbe_put32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
    vbe_put16(buffer, offset, (uint16_t)value);
    vbe_put16(buffer, offset + 2u, (uint16_t)(value >> 16));
}

static uint32_t vbe_far_pointer(uint16_t offset)
{
    return ((uint32_t)VBE_ROM_SEGMENT << 16) | offset;
}

static uint32_t vbe_linear_far_pointer(uint32_t linear)
{
    if (linear > 0x000FFFFFu) return 0;
    return ((linear >> 4) << 16) | (linear & 0xFu);
}

static uint32_t vbe_buffer_pointer(uint32_t linear, uint16_t rom_offset)
{
    uint32_t pointer = vbe_linear_far_pointer(linear);
    return pointer ? pointer : vbe_far_pointer(rom_offset);
}

static bool vbe_available(const dos_vm_t *vm)
{
    return vm && vm->mem &&
           vm->total_mem_size >= DOS_VBE_FB_BASE + DOS_VBE_FB_SIZE;
}

static bool vbe_buffer(dos_vm_t *vm, uint16_t selector, uint32_t offset,
                       uint32_t size, bool write, uint32_t *linear_out)
{
    if (!vm || !vm->cpu || !linear_out || size > vm->total_mem_size)
        return false;

    uint64_t linear;
    if (vm->cpu->protected_mode && vm->cpu->pm_cs_loaded) {
        dpmi_descriptor_t descriptor;
        if (!dpmi_guest_descriptor(vm, selector, &descriptor) ||
            (descriptor.access & (DESC_PRESENT | DESC_SEGMENT)) !=
                (DESC_PRESENT | DESC_SEGMENT))
            return false;
        if ((descriptor.access & DESC_CODE) != 0) {
            if (write || !(descriptor.access & DESC_READABLE))
                return false;
        } else if (write && !(descriptor.access & DESC_WRITABLE)) {
            return false;
        }
        uint32_t limit = dpmi_desc_get_limit(&descriptor);
        if (offset > limit || (size && size - 1u > limit - offset))
            return false;
        linear = (uint64_t)dpmi_desc_get_base(&descriptor) + offset;
    } else {
        linear = ((uint32_t)selector << 4) + (uint16_t)offset;
    }

    if (linear > vm->total_mem_size ||
        size > vm->total_mem_size - (uint32_t)linear)
        return false;
    *linear_out = (uint32_t)linear;
    return true;
}

static const vbe_mode_t *vbe_find_mode(uint16_t mode)
{
    for (uint32_t i = 0; i < sizeof(vbe_modes) / sizeof(vbe_modes[0]); i++)
        if (vbe_modes[i].mode == mode) return &vbe_modes[i];
    return NULL;
}

static uint8_t vbe_bytes_per_pixel(uint8_t bpp)
{
    return (uint8_t)((bpp + 7u) / 8u);
}

static uint32_t vbe_pitch_alignment(uint8_t bytes_per_pixel)
{
    /* Scan lines are both pixel- and dword-aligned. */
    return bytes_per_pixel == 3u ? 12u : 4u;
}

void dos_vbe_fit_rect(uint32_t source_width, uint32_t source_height,
                      uint32_t target_width, uint32_t target_height,
                      uint32_t *origin_x, uint32_t *origin_y,
                      uint32_t *display_width, uint32_t *display_height)
{
    uint32_t width = 0;
    uint32_t height = 0;

    if (source_width && source_height && target_width && target_height) {
        if (source_width <= target_width && source_height <= target_height) {
            width = source_width;
            height = source_height;
        } else if ((uint64_t)target_width * source_height <=
                   (uint64_t)target_height * source_width) {
            width = target_width;
            height = (uint32_t)((uint64_t)source_height * target_width /
                                source_width);
        } else {
            height = target_height;
            width = (uint32_t)((uint64_t)source_width * target_height /
                               source_height);
        }
        if (!width) width = 1;
        if (!height) height = 1;
    }

    if (origin_x) *origin_x = (target_width - width) / 2u;
    if (origin_y) *origin_y = (target_height - height) / 2u;
    if (display_width) *display_width = width;
    if (display_height) *display_height = height;
}

static uint32_t vbe_frame_size(const vbe_mode_t *mode)
{
    return (uint32_t)mode->width * mode->height *
           vbe_bytes_per_pixel(mode->bpp);
}

static uint16_t vbe_default_pitch(const vbe_mode_t *mode)
{
    return (uint16_t)((uint32_t)mode->width *
                      vbe_bytes_per_pixel(mode->bpp));
}

static void vbe_set_status(cpu8086_state_t *cpu, uint16_t status)
{
    cpu->ax = status;
}

static void vbe_write_mode_list(uint8_t *destination)
{
    uint32_t count = sizeof(vbe_modes) / sizeof(vbe_modes[0]);
    for (uint32_t i = 0; i < count; i++)
        vbe_put16(destination, i * 2u, vbe_modes[i].mode);
    vbe_put16(destination, count * 2u, 0xFFFFu);
}

static void vbe_write_rom_data(dos_vm_t *vm)
{
    uint8_t *rom = vm->mem + VBE_ROM_LINEAR;

    /* VBE 1.x direct bank function: INT 10h followed by a far return. */
    rom[VBE_WINDOW_FUNC_OFFSET] = 0xCD;
    rom[VBE_WINDOW_FUNC_OFFSET + 1u] = 0x10;
    rom[VBE_WINDOW_FUNC_OFFSET + 2u] = 0xCB;

    vbe_write_mode_list(rom + VBE_MODE_LIST_OFFSET);
    vbe_copy_string(rom + VBE_OEM_OFFSET, vbe_oem);
    vbe_copy_string(rom + VBE_VENDOR_OFFSET, vbe_vendor);
    vbe_copy_string(rom + VBE_PRODUCT_OFFSET, vbe_product);
    vbe_copy_string(rom + VBE_REVISION_OFFSET, vbe_revision);

    /* Protected-mode table. Set-window and palette calls retain the INT 10h
     * ABI. Display-start marks BH=FF so the handler treats CX:DX as a byte
     * offset, as required by the VBE 2.0 protected-mode ABI. */
    uint8_t *table = rom + VBE_PM_TABLE_OFFSET;
    vbe_put16(table, 0, 8);
    vbe_put16(table, 2, 11);
    vbe_put16(table, 4, 16);
    vbe_put16(table, 6, 19);
    table[8] = 0xCD; table[9] = 0x10; table[10] = 0xC3;
    table[11] = 0xB7; table[12] = 0xFF;
    table[13] = 0xCD; table[14] = 0x10; table[15] = 0xC3;
    table[16] = 0xCD; table[17] = 0x10; table[18] = 0xC3;
    table[19] = 0xFF; table[20] = 0xFF;
    table[21] = 0xFF; table[22] = 0xFF;
}

void dos_vbe_init(dos_vm_t *vm)
{
    if (!vm) return;
    vm->vbe_active = false;
    vm->vbe_linear = false;
    vm->vbe_no_clear = false;
    vm->vbe_mode = 0;
    vm->vbe_width = 0;
    vm->vbe_height = 0;
    vm->vbe_pitch = 0;
    vm->vbe_bank = 0;
    vm->vbe_display_x = 0;
    vm->vbe_display_y = 0;
    vm->vbe_bpp = 0;
    vm->vbe_bytes_per_pixel = 0;
    vm->vbe_dac_width = 6;
    if (!vm->system_mem_size)
        vm->system_mem_size = vm->total_mem_size < DOS_TOTAL_MEM
                            ? vm->total_mem_size : DOS_TOTAL_MEM;
    if (vbe_available(vm)) vbe_write_rom_data(vm);
}

void dos_vbe_leave_mode(dos_vm_t *vm)
{
    if (!vm) return;
    vm->vbe_active = false;
    vm->vbe_linear = false;
    vm->vbe_no_clear = false;
    vm->vbe_mode = 0;
    vm->vbe_width = 0;
    vm->vbe_height = 0;
    vm->vbe_pitch = 0;
    vm->vbe_bank = 0;
    vm->vbe_display_x = 0;
    vm->vbe_display_y = 0;
    vm->vbe_bpp = 0;
    vm->vbe_bytes_per_pixel = 0;
    dos_native_map_vbe_window(vm);
}

static bool vbe_apply_mode(dos_vm_t *vm, const vbe_mode_t *mode,
                           bool linear, bool no_clear)
{
    if (!vbe_available(vm) || !mode) return false;

    uint32_t frame_size = vbe_frame_size(mode);
    if (!frame_size || frame_size > DOS_VBE_FB_SIZE) return false;

    vm->vbe_mode = mode->mode;
    vm->vbe_width = mode->width;
    vm->vbe_height = mode->height;
    vm->vbe_pitch = vbe_default_pitch(mode);
    vm->vbe_bank = 0;
    vm->vbe_display_x = 0;
    vm->vbe_display_y = 0;
    vm->vbe_bpp = mode->bpp;
    vm->vbe_bytes_per_pixel = vbe_bytes_per_pixel(mode->bpp);
    vm->vbe_dac_width = 6;
    vm->vbe_active = true;
    vm->vbe_linear = linear;
    vm->vbe_no_clear = no_clear;
    vm->vga_mode = 0xFFu;

    if (!no_clear) {
        vbe_zero(vm->mem + DOS_VBE_FB_BASE, DOS_VBE_FB_SIZE);
    }

    vm->mem[0x449] = 0xFFu;
    if (no_clear) vm->mem[0x487] |= 0x80u;
    else vm->mem[0x487] &= 0x7Fu;
    dos_native_map_vbe_window(vm);
    dos_vga_vbe_update(vm);
    dos_mouse_video_mode_changed(vm);
    return true;
}

static void vbe_controller_info(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t linear;
    if (!vbe_buffer(vm, cpu->es, cpu->edi, 4, true, &linear)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    bool extended = vm->mem[linear] == 'V' && vm->mem[linear + 1u] == 'B' &&
                    vm->mem[linear + 2u] == 'E' && vm->mem[linear + 3u] == '2';
    uint32_t size = extended ? 512u : 256u;
    if (!vbe_buffer(vm, cpu->es, cpu->edi, size, true, &linear)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    uint8_t *info = vm->mem + linear;
    vbe_zero(info, size);
    info[0] = 'V'; info[1] = 'E'; info[2] = 'S'; info[3] = 'A';
    vbe_put16(info, 4, 0x0200);
    vbe_put32(info, 10, 0); /* fixed 6-bit DAC, VGA-compatible controller */
    vbe_put16(info, 18, (uint16_t)(DOS_VBE_FB_SIZE / 0x10000u));

    if (extended) {
        uint32_t mode_linear = linear + 34u;
        uint32_t oem_linear = linear + 256u;
        uint32_t vendor_linear = oem_linear + sizeof(vbe_oem);
        uint32_t product_linear = vendor_linear + sizeof(vbe_vendor);
        uint32_t revision_linear = product_linear + sizeof(vbe_product);
        vbe_write_mode_list(vm->mem + mode_linear);
        vbe_copy_string(vm->mem + oem_linear, vbe_oem);
        vbe_copy_string(vm->mem + vendor_linear, vbe_vendor);
        vbe_copy_string(vm->mem + product_linear, vbe_product);
        vbe_copy_string(vm->mem + revision_linear, vbe_revision);
        vbe_put32(info, 6,
                  vbe_buffer_pointer(oem_linear, VBE_OEM_OFFSET));
        vbe_put32(info, 14,
                  vbe_buffer_pointer(mode_linear, VBE_MODE_LIST_OFFSET));
        vbe_put16(info, 20, 0x0100);
        vbe_put32(info, 22,
                  vbe_buffer_pointer(vendor_linear, VBE_VENDOR_OFFSET));
        vbe_put32(info, 26,
                  vbe_buffer_pointer(product_linear, VBE_PRODUCT_OFFSET));
        vbe_put32(info, 30,
                  vbe_buffer_pointer(revision_linear, VBE_REVISION_OFFSET));
    } else {
        vbe_put32(info, 6, vbe_far_pointer(VBE_OEM_OFFSET));
        vbe_put32(info, 14, vbe_far_pointer(VBE_MODE_LIST_OFFSET));
    }
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_mode_info(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    const vbe_mode_t *mode = vbe_find_mode(cpu->cx & VBE_MODE_NUMBER_MASK);
    uint32_t linear;
    if (!mode || !vbe_buffer(vm, cpu->es, cpu->edi, 256, true, &linear)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    uint8_t *info = vm->mem + linear;
    vbe_zero(info, 256);
    vbe_put16(info, 0, VBE_MODE_ATTR_SUPPORTED | VBE_MODE_ATTR_INFO |
                           VBE_MODE_ATTR_COLOR | VBE_MODE_ATTR_GRAPHICS |
                           VBE_MODE_ATTR_NON_VGA | VBE_MODE_ATTR_LFB);
    info[2] = 0x07; /* relocatable, readable, writable window A */
    vbe_put16(info, 4, 64);
    vbe_put16(info, 6, 64);
    vbe_put16(info, 8, 0xA000);
    vbe_put32(info, 12, vbe_far_pointer(VBE_WINDOW_FUNC_OFFSET));
    vbe_put16(info, 16, vbe_default_pitch(mode));
    vbe_put16(info, 18, mode->width);
    vbe_put16(info, 20, mode->height);
    info[22] = 8;
    info[23] = 16;
    info[24] = 1;
    info[25] = mode->bpp;
    info[26] = 1;
    info[27] = mode->bpp == 8 ? 4 : 6;
    info[28] = 0;

    uint32_t frame_size = vbe_frame_size(mode);
    uint32_t images = DOS_VBE_FB_SIZE / frame_size;
    if (images > 256u) images = 256u;
    info[29] = (uint8_t)(images ? images - 1u : 0u);
    info[30] = 1;

    if (mode->bpp == 15) {
        info[31] = 5; info[32] = 10;
        info[33] = 5; info[34] = 5;
        info[35] = 5; info[36] = 0;
        info[37] = 1; info[38] = 15;
    } else if (mode->bpp == 16) {
        info[31] = 5; info[32] = 11;
        info[33] = 6; info[34] = 5;
        info[35] = 5; info[36] = 0;
    } else if (mode->bpp == 24) {
        info[31] = 8; info[32] = 16;
        info[33] = 8; info[34] = 8;
        info[35] = 8; info[36] = 0;
    }
    vbe_put32(info, 40, DOS_VBE_FB_BASE);
    uint32_t used = images * frame_size;
    vbe_put32(info, 44, used);
    vbe_put16(info, 48, (uint16_t)((DOS_VBE_FB_SIZE - used) / 1024u));
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_set_mode(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t request = cpu->bx;
    if (request & VBE_MODE_RESERVED_MASK) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    const vbe_mode_t *mode = vbe_find_mode(request & VBE_MODE_NUMBER_MASK);
    if (!vbe_apply_mode(vm, mode, (request & VBE_MODE_LINEAR) != 0,
                        (request & VBE_MODE_NO_CLEAR) != 0)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    serial_puts("[DOS/VBE] mode 0x");
    serial_puthex(mode->mode, 4);
    serial_puts(vm->vbe_linear ? " linear\n" : " banked\n");
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_current_mode(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (vm->vbe_active) {
        cpu->bx = vm->vbe_mode |
                  (vm->vbe_linear ? VBE_MODE_LINEAR : 0) |
                  (vm->vbe_no_clear ? VBE_MODE_NO_CLEAR : 0);
    } else {
        cpu->bx = vm->vga_mode;
    }
    vbe_set_status(cpu, VBE_SUCCESS);
}

static uint16_t vbe_state_blocks(void)
{
    return (uint16_t)((sizeof(vbe_saved_state_t) + 63u) / 64u);
}

static void vbe_restore_standard_mode(dos_vm_t *vm,
                                      const vbe_saved_state_t *state)
{
    uint8_t mode = state->vga_mode;
    if (mode != 0x03 && mode != 0x13) mode = 0x03;
    vm->cpu->ax = (uint16_t)(0x0080u | mode);
    dos_int10_video(vm);

    vm->vga_page = mode == 0x03 ? state->vga_page & 7u : 0;
    vm->text_attr = state->text_attr;
    vm->cursor_start = state->cursor_start;
    vm->cursor_end = state->cursor_end;
    for (uint32_t page = 0; page < 8u; page++)
        dos_mem_write16(vm, 0x450u + page * 2u,
                        state->cursor_positions[page]);
    uint16_t cursor = state->cursor_positions[vm->vga_page];
    vm->cursor_row = (uint8_t)(cursor >> 8);
    vm->cursor_col = (uint8_t)cursor;
    vm->mem[0x462] = vm->vga_page;
    dos_mem_write16(vm, 0x44E,
                    mode == 0x03 ? (uint16_t)(vm->vga_page * 4096u) : 0);
    dos_mem_write16(vm, 0x460,
                    (uint16_t)(((uint16_t)vm->cursor_start << 8) |
                               vm->cursor_end));
    if (mode == 0x03) {
        dos_vga_invalidate_text(vm);
        dos_vga_flush(vm);
    }
}

static bool vbe_restore_mode_state(dos_vm_t *vm,
                                   const vbe_saved_state_t *state)
{
    if (!state->active) {
        vbe_restore_standard_mode(vm, state);
        return true;
    }
    const vbe_mode_t *mode = vbe_find_mode(state->mode);
    if (!vbe_apply_mode(vm, mode, state->linear != 0, true)) return false;

    uint32_t minimum_pitch = (uint32_t)mode->width *
                             vbe_bytes_per_pixel(mode->bpp);
    uint32_t maximum_pitch = DOS_VBE_FB_SIZE / mode->height;
    uint32_t alignment = vbe_pitch_alignment(vm->vbe_bytes_per_pixel);
    if (state->pitch >= minimum_pitch && state->pitch <= maximum_pitch &&
        state->pitch % alignment == 0)
        vm->vbe_pitch = state->pitch;

    uint32_t max_lines = DOS_VBE_FB_SIZE / vm->vbe_pitch;
    uint32_t logical_width = vm->vbe_pitch / vm->vbe_bytes_per_pixel;
    if ((uint32_t)state->display_x + vm->vbe_width <= logical_width &&
        (uint32_t)state->display_y + vm->vbe_height <= max_lines) {
        vm->vbe_display_x = state->display_x;
        vm->vbe_display_y = state->display_y;
    }
    if ((uint32_t)state->bank * DOS_VBE_WINDOW_SIZE < DOS_VBE_FB_SIZE)
        vm->vbe_bank = state->bank;
    vm->vbe_no_clear = state->no_clear != 0;
    dos_native_map_vbe_window(vm);
    dos_vga_vbe_update(vm);
    return true;
}

static void vbe_save_restore(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->dl == 0) {
        cpu->bx = vbe_state_blocks();
        vbe_set_status(cpu, VBE_SUCCESS);
        return;
    }
    if (cpu->dl != 1 && cpu->dl != 2) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    uint32_t linear;
    if (!vbe_buffer(vm, cpu->es, cpu->bx, sizeof(vbe_saved_state_t),
                    cpu->dl == 1, &linear)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    vbe_saved_state_t *state =
        (vbe_saved_state_t *)(void *)(vm->mem + linear);

    if (cpu->dl == 1) {
        vbe_zero((uint8_t *)state, sizeof(*state));
        state->magic = VBE_STATE_MAGIC;
        state->version = VBE_STATE_VERSION;
        state->requested = cpu->cx & 0x000Fu;
        state->mode = vm->vbe_mode;
        state->width = vm->vbe_width;
        state->height = vm->vbe_height;
        state->pitch = vm->vbe_pitch;
        state->bank = vm->vbe_bank;
        state->display_x = vm->vbe_display_x;
        state->display_y = vm->vbe_display_y;
        state->bpp = vm->vbe_bpp;
        state->bytes_per_pixel = vm->vbe_bytes_per_pixel;
        state->dac_width = vm->vbe_dac_width;
        state->vga_mode = vm->vga_mode;
        state->vga_page = vm->vga_page;
        state->text_attr = vm->text_attr;
        state->cursor_start = vm->cursor_start;
        state->cursor_end = vm->cursor_end;
        state->active = vm->vbe_active;
        state->linear = vm->vbe_linear;
        state->no_clear = vm->vbe_no_clear;
        for (uint32_t page = 0; page < 8u; page++)
            state->cursor_positions[page] =
                dos_mem_read16(vm, 0x450u + page * 2u);
        (void)dos_io_copy_dac(vm, state->palette);
        vbe_set_status(cpu, VBE_SUCCESS);
        return;
    }

    if (state->magic != VBE_STATE_MAGIC ||
        state->version != VBE_STATE_VERSION) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    uint16_t requested = cpu->cx & 0x000Fu;
    if ((requested & 0x000Bu) && !vbe_restore_mode_state(vm, state)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    if (requested & 0x0004u) {
        dos_io_write8(vm, 0x3C8, 0);
        for (uint32_t i = 0; i < 256u; i++) {
            dos_io_write8(vm, 0x3C9, state->palette[i][0]);
            dos_io_write8(vm, 0x3C9, state->palette[i][1]);
            dos_io_write8(vm, 0x3C9, state->palette[i][2]);
        }
    }
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_window_control(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->vbe_active || vm->vbe_linear) {
        vbe_set_status(cpu, VBE_INVALID_MODE);
        return;
    }
    if (cpu->bl != 0 || (cpu->bh != 0 && cpu->bh != 1)) {
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
        return;
    }
    if (cpu->bh == 1) {
        cpu->dx = vm->vbe_bank;
        vbe_set_status(cpu, VBE_SUCCESS);
        return;
    }
    if ((uint32_t)cpu->dx * DOS_VBE_WINDOW_SIZE >= DOS_VBE_FB_SIZE) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    vm->vbe_bank = cpu->dx;
    dos_native_map_vbe_window(vm);
    dos_vga_vbe_update(vm);
    vbe_set_status(cpu, VBE_SUCCESS);
}

static uint32_t vbe_maximum_pitch(const dos_vm_t *vm)
{
    uint32_t maximum = DOS_VBE_FB_SIZE / vm->vbe_height;
    if (maximum > 65532u) maximum = 65532u;
    maximum -= maximum % vbe_pitch_alignment(vm->vbe_bytes_per_pixel);
    return maximum;
}

static void vbe_return_scanline(dos_vm_t *vm, uint32_t pitch)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu->bx = (uint16_t)pitch;
    cpu->cx = (uint16_t)(pitch / vm->vbe_bytes_per_pixel);
    uint32_t lines = DOS_VBE_FB_SIZE / pitch;
    cpu->dx = (uint16_t)(lines > 0xFFFFu ? 0xFFFFu : lines);
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_scanline_control(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->vbe_active || !vm->vbe_bytes_per_pixel) {
        vbe_set_status(cpu, VBE_INVALID_MODE);
        return;
    }
    if (cpu->bl == 1) {
        vbe_return_scanline(vm, vm->vbe_pitch);
        return;
    }
    if (cpu->bl == 3) {
        vbe_return_scanline(vm, vbe_maximum_pitch(vm));
        return;
    }
    if (cpu->bl != 0 && cpu->bl != 2) {
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
        return;
    }

    uint32_t requested = cpu->bl == 0
                       ? (uint32_t)cpu->cx * vm->vbe_bytes_per_pixel
                       : cpu->cx;
    uint32_t alignment = vbe_pitch_alignment(vm->vbe_bytes_per_pixel);
    requested = (requested + alignment - 1u) / alignment * alignment;
    uint32_t minimum = (uint32_t)vm->vbe_width * vm->vbe_bytes_per_pixel;
    uint32_t maximum = vbe_maximum_pitch(vm);
    if (requested < minimum || requested > maximum) {
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
        return;
    }
    vm->vbe_pitch = (uint16_t)requested;
    vm->vbe_display_x = 0;
    vm->vbe_display_y = 0;
    dos_vga_vbe_update(vm);
    vbe_return_scanline(vm, requested);
}

static bool vbe_display_start_valid(const dos_vm_t *vm, uint32_t x,
                                    uint32_t y)
{
    uint32_t logical_width = vm->vbe_pitch / vm->vbe_bytes_per_pixel;
    uint32_t maximum_lines = DOS_VBE_FB_SIZE / vm->vbe_pitch;
    return x + vm->vbe_width <= logical_width &&
           y + vm->vbe_height <= maximum_lines;
}

static void vbe_display_start(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->vbe_active || !vm->vbe_bytes_per_pixel) {
        vbe_set_status(cpu, VBE_INVALID_MODE);
        return;
    }
    if (cpu->bh == 0 && cpu->bl == 1) {
        cpu->bh = 0;
        cpu->cx = vm->vbe_display_x;
        cpu->dx = vm->vbe_display_y;
        vbe_set_status(cpu, VBE_SUCCESS);
        return;
    }

    uint32_t x;
    uint32_t y;
    if (cpu->bh == 0xFFu && (cpu->bl == 0 || cpu->bl == 0x80u)) {
        uint32_t byte_offset = ((uint32_t)cpu->dx << 16) | cpu->cx;
        y = byte_offset / vm->vbe_pitch;
        uint32_t row_offset = byte_offset % vm->vbe_pitch;
        if (row_offset % vm->vbe_bytes_per_pixel) {
            vbe_set_status(cpu, VBE_FAILED);
            return;
        }
        x = row_offset / vm->vbe_bytes_per_pixel;
    } else if (cpu->bh == 0 && (cpu->bl == 0 || cpu->bl == 0x80u)) {
        x = cpu->cx;
        y = cpu->dx;
    } else {
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
        return;
    }

    if (!vbe_display_start_valid(vm, x, y)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    vm->vbe_display_x = (uint16_t)x;
    vm->vbe_display_y = (uint16_t)y;
    dos_vga_vbe_update(vm);
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_dac_format(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->vbe_active || vm->vbe_bpp != 8) {
        vbe_set_status(cpu, VBE_INVALID_MODE);
        return;
    }
    if (cpu->bl == 1) {
        cpu->bh = vm->vbe_dac_width;
        vbe_set_status(cpu, VBE_SUCCESS);
    } else if (cpu->bl == 0 && cpu->bh >= 6) {
        vm->vbe_dac_width = 6;
        cpu->bh = vm->vbe_dac_width;
        vbe_set_status(cpu, VBE_SUCCESS);
    } else {
        cpu->bh = vm->vbe_dac_width;
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
    }
}

static void vbe_palette_data(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->vbe_active || vm->vbe_bpp != 8) {
        vbe_set_status(cpu, VBE_INVALID_MODE);
        return;
    }
    if ((uint32_t)cpu->dx + cpu->cx > 256u) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    if (cpu->bl == 2 || cpu->bl == 3) {
        vbe_set_status(cpu, VBE_NOT_SUPPORTED);
        return;
    }
    if (cpu->bl != 0 && cpu->bl != 1 && cpu->bl != 0x80u) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    uint32_t linear;
    uint32_t byte_count = (uint32_t)cpu->cx * 4u;
    if (!vbe_buffer(vm, cpu->es, cpu->edi, byte_count,
                    cpu->bl == 1, &linear)) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }

    uint8_t *entries = vm->mem + linear;
    if (cpu->bl == 0 || cpu->bl == 0x80u) {
        dos_io_write8(vm, 0x3C8, (uint8_t)cpu->dx);
        for (uint32_t i = 0; i < cpu->cx; i++) {
            dos_io_write8(vm, 0x3C9, entries[i * 4u + 1u]);
            dos_io_write8(vm, 0x3C9, entries[i * 4u + 2u]);
            dos_io_write8(vm, 0x3C9, entries[i * 4u + 3u]);
        }
    } else {
        uint8_t palette[256][3];
        if (!dos_io_copy_dac(vm, palette)) {
            vbe_set_status(cpu, VBE_FAILED);
            return;
        }
        for (uint32_t i = 0; i < cpu->cx; i++) {
            uint32_t index = (uint32_t)cpu->dx + i;
            entries[i * 4u] = 0;
            entries[i * 4u + 1u] = palette[index][0];
            entries[i * 4u + 2u] = palette[index][1];
            entries[i * 4u + 3u] = palette[index][2];
        }
    }
    vbe_set_status(cpu, VBE_SUCCESS);
}

static void vbe_protected_mode_interface(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->bl != 0) {
        vbe_set_status(cpu, VBE_FAILED);
        return;
    }
    cpu->es = VBE_ROM_SEGMENT;
    cpu->di = VBE_PM_TABLE_OFFSET;
    cpu->cx = VBE_PM_TABLE_SIZE;
    vbe_set_status(cpu, VBE_SUCCESS);
}

void dos_int10_vbe(dos_vm_t *vm)
{
    if (!vbe_available(vm)) {
        vbe_set_status(vm->cpu, VBE_FAILED);
        return;
    }

    switch (vm->cpu->al) {
    case 0x00: vbe_controller_info(vm); break;
    case 0x01: vbe_mode_info(vm); break;
    case 0x02: vbe_set_mode(vm); break;
    case 0x03: vbe_current_mode(vm); break;
    case 0x04: vbe_save_restore(vm); break;
    case 0x05: vbe_window_control(vm); break;
    case 0x06: vbe_scanline_control(vm); break;
    case 0x07: vbe_display_start(vm); break;
    case 0x08: vbe_dac_format(vm); break;
    case 0x09: vbe_palette_data(vm); break;
    case 0x0A: vbe_protected_mode_interface(vm); break;
    default: vbe_set_status(vm->cpu, VBE_FAILED); break;
    }
}

static uint32_t vbe_far_to_linear(uint32_t pointer)
{
    return (pointer >> 16) * 16u + (pointer & 0xFFFFu);
}

int dos_vbe_selftest(void)
{
    uint64_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    uint64_t memory_pages =
        (DOS_VM_ADDRESS_SPACE_SIZE + 4095u) / 4096u;
    dos_vm_t *vm = (dos_vm_t *)mem_alloc_pages(vm_pages);
    uint8_t *memory = (uint8_t *)mem_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) mem_free_pages(vm, vm_pages);
        if (memory) mem_free_pages(memory, memory_pages);
        return 1;
    }
    vbe_zero((uint8_t *)vm, vm_pages * 4096u);
    vbe_zero(memory, memory_pages * 4096u);
    cpu8086_state_t cpu;
    vbe_zero((uint8_t *)&cpu, sizeof(cpu));
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = DOS_VM_ADDRESS_SPACE_SIZE;
    vm->system_mem_size = DOS_TOTAL_MEM;
    vm->vga_mode = 0x03;
    cpu8086_init(&cpu, vm);
    int failures = 0;
    if (!dos_io_init(vm)) {
        failures++;
        goto done;
    }
    dos_vbe_init(vm);

    uint32_t controller = 0x20000u;
    memory[controller] = 'V'; memory[controller + 1u] = 'B';
    memory[controller + 2u] = 'E'; memory[controller + 3u] = '2';
    cpu.ax = 0x4F00; cpu.es = 0x2000; cpu.di = 0;
    dos_int10_vbe(vm);
    uint32_t mode_list = vbe_far_to_linear(
        dos_mem_read32(vm, controller + 14u));
    if (cpu.ax != VBE_SUCCESS || memory[controller] != 'V' ||
        memory[controller + 1u] != 'E' ||
        memory[controller + 2u] != 'S' ||
        memory[controller + 3u] != 'A' ||
        dos_mem_read16(vm, controller + 4u) != 0x0200u ||
        dos_mem_read16(vm, controller + 18u) != 64u ||
        mode_list < controller + 34u || mode_list >= controller + 256u ||
        dos_mem_read16(vm, mode_list) != vbe_modes[0].mode)
        failures++;

    uint32_t mode_info = 0x22000u;
    cpu.ax = 0x4F01; cpu.cx = 0x111; cpu.es = 0x2200; cpu.di = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS ||
        !(dos_mem_read16(vm, mode_info) & VBE_MODE_ATTR_LFB) ||
        dos_mem_read16(vm, mode_info + 18u) != 640u ||
        dos_mem_read16(vm, mode_info + 20u) != 480u ||
        memory[mode_info + 25u] != 16u ||
        memory[mode_info + 27u] != 6u ||
        dos_mem_read32(vm, mode_info + 40u) != DOS_VBE_FB_BASE)
        failures++;

    cpu.ax = 0x4F02; cpu.bx = 0x101;
    dos_int10_vbe(vm);
    dos_mem_write8(vm, DOS_VBE_WINDOW_BASE, 0x12);
    if (cpu.ax != VBE_SUCCESS || !vm->vbe_active || vm->vbe_linear ||
        memory[DOS_VBE_FB_BASE] != 0x12)
        failures++;

    cpu.ax = 0x4F05; cpu.bh = 0; cpu.bl = 0; cpu.dx = 1;
    dos_int10_vbe(vm);
    dos_mem_write8(vm, DOS_VBE_WINDOW_BASE + 7u, 0x34);
    if (cpu.ax != VBE_SUCCESS || vm->vbe_bank != 1 ||
        memory[DOS_VBE_FB_BASE + DOS_VBE_WINDOW_SIZE + 7u] != 0x34)
        failures++;
    cpu.ax = 0x4F05; cpu.bh = 1; cpu.bl = 0; cpu.dx = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || cpu.dx != 1) failures++;

    cpu.ax = 0x4F06; cpu.bl = 0; cpu.cx = 672;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || cpu.bx != 672 || vm->vbe_pitch != 672)
        failures++;
    cpu.ax = 0x4F07; cpu.bh = 0; cpu.bl = 0; cpu.cx = 16; cpu.dx = 1;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || vm->vbe_display_x != 16 ||
        vm->vbe_display_y != 1)
        failures++;

    uint32_t palette_entry = 0x24000u;
    memory[palette_entry] = 0;
    memory[palette_entry + 1u] = 1;
    memory[palette_entry + 2u] = 2;
    memory[palette_entry + 3u] = 3;
    cpu.ax = 0x4F09; cpu.bl = 0; cpu.cx = 1; cpu.dx = 7;
    cpu.es = 0x2400; cpu.di = 0;
    dos_int10_vbe(vm);
    vbe_zero(memory + palette_entry, 4);
    cpu.ax = 0x4F09; cpu.bl = 1; cpu.cx = 1; cpu.dx = 7;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || memory[palette_entry] != 0 ||
        memory[palette_entry + 1u] != 1 ||
        memory[palette_entry + 2u] != 2 ||
        memory[palette_entry + 3u] != 3)
        failures++;

    uint32_t state_buffer = 0x26000u;
    cpu.ax = 0x4F04; cpu.cx = 0xF; cpu.dl = 1;
    cpu.es = 0x2600; cpu.bx = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS ||
        dos_mem_read32(vm, state_buffer) != VBE_STATE_MAGIC)
        failures++;

    cpu.ax = 0x4F02; cpu.bx = VBE_MODE_LINEAR | 0x111u;
    dos_int10_vbe(vm);
    dos_mem_write16(vm, DOS_VBE_FB_BASE, 0xF81Fu);
    if (cpu.ax != VBE_SUCCESS || !vm->vbe_linear ||
        dos_mem_read16(vm, DOS_VBE_FB_BASE) != 0xF81Fu)
        failures++;
    cpu.ax = 0x4F05; cpu.bh = 0; cpu.bl = 0; cpu.dx = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_INVALID_MODE) failures++;

    cpu.ax = 0x4F04; cpu.cx = 0xF; cpu.dl = 2;
    cpu.es = 0x2600; cpu.bx = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || !vm->vbe_active || vm->vbe_linear ||
        vm->vbe_mode != 0x101 || vm->vbe_pitch != 672 ||
        vm->vbe_bank != 1 || vm->vbe_display_x != 16 ||
        vm->vbe_display_y != 1)
        failures++;

    cpu.ax = 0x4F02; cpu.bx = 0x115;
    dos_int10_vbe(vm);
    cpu.ax = 0x4F06; cpu.bl = 2; cpu.cx = 2401;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || cpu.bx != 2412 || cpu.cx != 804 ||
        vm->vbe_pitch != 2412)
        failures++;

    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t display_width;
    uint32_t display_height;
    dos_vbe_fit_rect(1280, 1024, 1024, 768, &origin_x, &origin_y,
                     &display_width, &display_height);
    if (origin_x != 32 || origin_y != 0 || display_width != 960 ||
        display_height != 768)
        failures++;

    cpu.ax = 0x0083;
    dos_int10_video(vm);
    vm->vga_page = 2;
    vm->text_attr = 0x1E;
    vm->cursor_start = 5;
    vm->cursor_end = 7;
    vm->cursor_row = 4;
    vm->cursor_col = 5;
    memory[0x462] = 2;
    dos_mem_write16(vm, 0x44E, 8192);
    dos_mem_write16(vm, 0x454, 0x0405);
    dos_mem_write16(vm, 0x460, 0x0507);
    cpu.ax = 0x4F04; cpu.cx = 0xF; cpu.dl = 1;
    cpu.es = 0x2800; cpu.bx = 0;
    dos_int10_vbe(vm);
    cpu.ax = 0x4F02; cpu.bx = 0x101;
    dos_int10_vbe(vm);
    cpu.ax = 0x4F04; cpu.cx = 0xF; cpu.dl = 2;
    cpu.es = 0x2800; cpu.bx = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || vm->vbe_active || vm->vga_mode != 0x03 ||
        vm->vga_page != 2 || vm->text_attr != 0x1E ||
        vm->cursor_start != 5 || vm->cursor_end != 7 ||
        vm->cursor_row != 4 || vm->cursor_col != 5 ||
        memory[0x462] != 2 || dos_mem_read16(vm, 0x44E) != 8192 ||
        dos_mem_read16(vm, 0x454) != 0x0405 ||
        dos_mem_read16(vm, 0x460) != 0x0507)
        failures++;

    cpu.ax = 0x4F0A; cpu.bl = 0;
    dos_int10_vbe(vm);
    if (cpu.ax != VBE_SUCCESS || cpu.es != VBE_ROM_SEGMENT ||
        cpu.di != VBE_PM_TABLE_OFFSET || cpu.cx != VBE_PM_TABLE_SIZE ||
        memory[VBE_ROM_LINEAR + VBE_PM_TABLE_OFFSET + 8u] != 0xCD)
        failures++;

done:
    dos_io_shutdown(vm);
    mem_free_pages(memory, memory_pages);
    mem_free_pages(vm, vm_pages);
    return failures;
}
