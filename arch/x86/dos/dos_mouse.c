/*
 * OsitoK - Microsoft-compatible DOS mouse services (INT 33h)
 *
 * Mouse state is private to the DOS VM. Host input is sampled through a
 * non-destructive snapshot so DOS never drains compositor or Win32 events.
 */

#include "cpu8086.h"
#include "dos_mouse.h"
#include "dos_vbe.h"
#include "../include/input_events.h"

extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);
extern void dos_vga_mouse_update(dos_vm_t *vm);
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);

#define DOS_MOUSE_BUTTONS       3U
#define DOS_MOUSE_VIRTUAL_MAX_X 639U
#define DOS_MOUSE_VIRTUAL_MAX_Y 199U
#define DOS_MOUSE_VERSION       0x0800U
#define DOS_MOUSE_TYPE_PS2      4U

static int32_t dos_mouse_clamp_i32(int32_t value, int32_t minimum,
                                   int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static uint16_t dos_mouse_max_x(const dos_vm_t *vm)
{
    if (vm->vbe_active && vm->vbe_width)
        return (uint16_t)(vm->vbe_width - 1u);
    return DOS_MOUSE_VIRTUAL_MAX_X;
}

static uint16_t dos_mouse_max_y(const dos_vm_t *vm)
{
    if (vm->vbe_active && vm->vbe_height)
        return (uint16_t)(vm->vbe_height - 1u);
    return DOS_MOUSE_VIRTUAL_MAX_Y;
}

static uint16_t dos_mouse_axis_step(const dos_vm_t *vm, bool horizontal)
{
    if (vm->vbe_active)
        return 1U;
    if (vm->vga_mode == 0x13)
        return horizontal ? 2U : 1U;
    return 8U;
}

static uint16_t dos_mouse_quantize(int32_t value, uint16_t minimum,
                                   uint16_t maximum, uint16_t step)
{
    value = dos_mouse_clamp_i32(value, minimum, maximum);
    if (step <= 1U) return (uint16_t)value;

    uint32_t rounded = ((uint32_t)value + (step - 1U) / 2U) / step * step;
    uint32_t first = ((uint32_t)minimum + step - 1U) / step * step;
    uint32_t last = (uint32_t)maximum / step * step;
    if (first > last)
        return (uint16_t)value;
    if (rounded < first) rounded = first;
    if (rounded > last) rounded = last;
    return (uint16_t)rounded;
}

static void dos_mouse_map_host(const dos_vm_t *vm, int32_t host_x,
                               int32_t host_y, uint32_t screen_width,
                               uint32_t screen_height, int32_t *virtual_x,
                               int32_t *virtual_y)
{
    if (vm->vbe_active && vm->vbe_width && vm->vbe_height) {
        uint32_t origin_x;
        uint32_t origin_y;
        uint32_t display_width;
        uint32_t display_height;
        dos_vbe_fit_rect(vm->vbe_width, vm->vbe_height, screen_width,
                         screen_height, &origin_x, &origin_y,
                         &display_width, &display_height);
        int32_t local_x = dos_mouse_clamp_i32(
            host_x - (int32_t)origin_x, 0,
            display_width ? (int32_t)display_width - 1 : 0);
        int32_t local_y = dos_mouse_clamp_i32(
            host_y - (int32_t)origin_y, 0,
            display_height ? (int32_t)display_height - 1 : 0);
        *virtual_x = display_width > 1u && vm->vbe_width > 1u
            ? (int32_t)((uint64_t)(uint32_t)local_x *
                        (vm->vbe_width - 1u) / (display_width - 1u))
            : 0;
        *virtual_y = display_height > 1u && vm->vbe_height > 1u
            ? (int32_t)((uint64_t)(uint32_t)local_y *
                        (vm->vbe_height - 1u) / (display_height - 1u))
            : 0;
        return;
    }

    uint32_t content_width = vm->vga_mode == 0x13 ? 320U : 640U;
    uint32_t content_height = vm->vga_mode == 0x13 ? 200U : 400U;
    int32_t local_x;
    int32_t local_y;

    if (screen_width >= content_width) {
        int32_t origin = (int32_t)((screen_width - content_width) / 2U);
        local_x = host_x - origin;
    } else if (screen_width) {
        local_x = (int32_t)((int64_t)host_x * content_width / screen_width);
    } else {
        local_x = 0;
    }

    if (screen_height >= content_height) {
        int32_t origin = (int32_t)((screen_height - content_height) / 2U);
        local_y = host_y - origin;
    } else if (screen_height) {
        local_y = (int32_t)((int64_t)host_y * content_height / screen_height);
    } else {
        local_y = 0;
    }

    local_x = dos_mouse_clamp_i32(local_x, 0,
                                  (int32_t)content_width - 1);
    local_y = dos_mouse_clamp_i32(local_y, 0,
                                  (int32_t)content_height - 1);
    *virtual_x = vm->vga_mode == 0x13 ? local_x * 2 : local_x;
    *virtual_y = vm->vga_mode == 0x13 ? local_y : local_y / 2;
}

static void dos_mouse_map_event(const dos_vm_t *vm, int32_t host_x,
                                int32_t host_y, uint32_t screen_width,
                                uint32_t screen_height, uint16_t *event_x,
                                uint16_t *event_y)
{
    int32_t mapped_x;
    int32_t mapped_y;
    dos_mouse_map_host(vm, host_x, host_y, screen_width, screen_height,
                       &mapped_x, &mapped_y);
    *event_x = dos_mouse_quantize(mapped_x + vm->mouse_host_offset_x,
                                  vm->mouse_min_x, vm->mouse_max_x,
                                  dos_mouse_axis_step(vm, true));
    *event_y = dos_mouse_quantize(mapped_y + vm->mouse_host_offset_y,
                                  vm->mouse_min_y, vm->mouse_max_y,
                                  dos_mouse_axis_step(vm, false));
}

static void dos_mouse_reset(dos_vm_t *vm,
                            const input_mouse_snapshot_t *snapshot,
                            uint32_t screen_width, uint32_t screen_height)
{
    int32_t mapped_x;
    int32_t mapped_y;
    dos_mouse_map_host(vm, snapshot->x, snapshot->y, screen_width,
                       screen_height, &mapped_x, &mapped_y);

    vm->mouse_min_x = 0;
    vm->mouse_max_x = dos_mouse_max_x(vm);
    vm->mouse_min_y = 0;
    vm->mouse_max_y = dos_mouse_max_y(vm);
    vm->mouse_x = dos_mouse_quantize(
        ((uint32_t)vm->mouse_max_x + 1u) / 2u,
        vm->mouse_min_x, vm->mouse_max_x,
                                     dos_mouse_axis_step(vm, true));
    vm->mouse_y = dos_mouse_quantize(
        ((uint32_t)vm->mouse_max_y + 1u) / 2u,
        vm->mouse_min_y, vm->mouse_max_y,
                                     dos_mouse_axis_step(vm, false));
    vm->mouse_host_offset_x = (int32_t)vm->mouse_x - mapped_x;
    vm->mouse_host_offset_y = (int32_t)vm->mouse_y - mapped_y;
    vm->mouse_buttons = snapshot->buttons & 0x07U;
    vm->mouse_cursor_flag = -1;
    vm->mouse_visible = false;
    vm->mouse_motion_x = snapshot->motion_x;
    vm->mouse_motion_y = snapshot->motion_y;
    for (uint32_t i = 0; i < DOS_MOUSE_BUTTONS; i++) {
        vm->mouse_press_count[i] = snapshot->press_count[i];
        vm->mouse_release_count[i] = snapshot->release_count[i];
        vm->mouse_press_observed[i] = snapshot->press_count[i];
        vm->mouse_release_observed[i] = snapshot->release_count[i];
        vm->mouse_press_x[i] = vm->mouse_x;
        vm->mouse_press_y[i] = vm->mouse_y;
        vm->mouse_release_x[i] = vm->mouse_x;
        vm->mouse_release_y[i] = vm->mouse_y;
    }
    vm->mouse_initialized = true;
    vm->mouse_diag_mask = 0;
    dos_vga_mouse_update(vm);
}

static void dos_mouse_sync(dos_vm_t *vm,
                           const input_mouse_snapshot_t *snapshot,
                           uint32_t screen_width, uint32_t screen_height)
{
    int32_t mapped_x;
    int32_t mapped_y;
    dos_mouse_map_host(vm, snapshot->x, snapshot->y, screen_width,
                       screen_height, &mapped_x, &mapped_y);

    int32_t position_x = mapped_x + vm->mouse_host_offset_x;
    int32_t position_y = mapped_y + vm->mouse_host_offset_y;
    if (position_x < vm->mouse_min_x) {
        position_x = vm->mouse_min_x;
        vm->mouse_host_offset_x = position_x - mapped_x;
    } else if (position_x > vm->mouse_max_x) {
        position_x = vm->mouse_max_x;
        vm->mouse_host_offset_x = position_x - mapped_x;
    }
    if (position_y < vm->mouse_min_y) {
        position_y = vm->mouse_min_y;
        vm->mouse_host_offset_y = position_y - mapped_y;
    } else if (position_y > vm->mouse_max_y) {
        position_y = vm->mouse_max_y;
        vm->mouse_host_offset_y = position_y - mapped_y;
    }

    vm->mouse_x = dos_mouse_quantize(position_x, vm->mouse_min_x,
                                     vm->mouse_max_x,
                                     dos_mouse_axis_step(vm, true));
    vm->mouse_y = dos_mouse_quantize(position_y, vm->mouse_min_y,
                                     vm->mouse_max_y,
                                     dos_mouse_axis_step(vm, false));
    vm->mouse_buttons = snapshot->buttons & 0x07U;

    for (uint32_t i = 0; i < DOS_MOUSE_BUTTONS; i++) {
        if (snapshot->press_count[i] != vm->mouse_press_observed[i]) {
            dos_mouse_map_event(vm, snapshot->press_x[i],
                                snapshot->press_y[i], screen_width,
                                screen_height, &vm->mouse_press_x[i],
                                &vm->mouse_press_y[i]);
            vm->mouse_press_observed[i] = snapshot->press_count[i];
        }
        if (snapshot->release_count[i] != vm->mouse_release_observed[i]) {
            dos_mouse_map_event(vm, snapshot->release_x[i],
                                snapshot->release_y[i], screen_width,
                                screen_height, &vm->mouse_release_x[i],
                                &vm->mouse_release_y[i]);
            vm->mouse_release_observed[i] = snapshot->release_count[i];
        }
    }
    dos_vga_mouse_update(vm);
}

static void dos_mouse_set_position(dos_vm_t *vm,
                                   const input_mouse_snapshot_t *snapshot,
                                   uint32_t screen_width,
                                   uint32_t screen_height, uint16_t x,
                                   uint16_t y)
{
    int32_t mapped_x;
    int32_t mapped_y;
    dos_mouse_map_host(vm, snapshot->x, snapshot->y, screen_width,
                       screen_height, &mapped_x, &mapped_y);
    vm->mouse_x = dos_mouse_quantize(x, vm->mouse_min_x, vm->mouse_max_x,
                                     dos_mouse_axis_step(vm, true));
    vm->mouse_y = dos_mouse_quantize(y, vm->mouse_min_y, vm->mouse_max_y,
                                     dos_mouse_axis_step(vm, false));
    vm->mouse_host_offset_x = (int32_t)vm->mouse_x - mapped_x;
    vm->mouse_host_offset_y = (int32_t)vm->mouse_y - mapped_y;
    dos_vga_mouse_update(vm);
}

static void dos_mouse_set_range(dos_vm_t *vm,
                                const input_mouse_snapshot_t *snapshot,
                                uint32_t screen_width, uint32_t screen_height,
                                bool horizontal, uint16_t first,
                                uint16_t second)
{
    uint16_t minimum = first < second ? first : second;
    uint16_t maximum = first < second ? second : first;
    uint16_t absolute_max = horizontal ? dos_mouse_max_x(vm)
                                       : dos_mouse_max_y(vm);
    if (minimum > absolute_max) minimum = absolute_max;
    if (maximum > absolute_max) maximum = absolute_max;

    if (horizontal) {
        vm->mouse_min_x = minimum;
        vm->mouse_max_x = maximum;
    } else {
        vm->mouse_min_y = minimum;
        vm->mouse_max_y = maximum;
    }
    dos_mouse_set_position(vm, snapshot, screen_width, screen_height,
                           vm->mouse_x, vm->mouse_y);
}

static void dos_mouse_diag_unsupported(dos_vm_t *vm, uint16_t function)
{
    if (function >= 32U) return;
    uint32_t mask = 1U << function;
    if (vm->mouse_diag_mask & mask) return;
    vm->mouse_diag_mask |= mask;
    serial_puts("[DOS/MOUSE] unsupported INT 33h function 0x");
    serial_puthex(function, 4);
    serial_puts(" ignored\n");
}

static void dos_mouse_dispatch_snapshot(dos_vm_t *vm,
                                        const input_mouse_snapshot_t *snapshot,
                                        uint32_t screen_width,
                                        uint32_t screen_height)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t function = cpu->ax;

    if (function == 0) {
        dos_mouse_reset(vm, snapshot, screen_width, screen_height);
        cpu->ax = 0xFFFFU;
        cpu->bx = DOS_MOUSE_BUTTONS;
        return;
    }

    if (!vm->mouse_initialized)
        dos_mouse_reset(vm, snapshot, screen_width, screen_height);
    dos_mouse_sync(vm, snapshot, screen_width, screen_height);

    switch (function) {
    case 0x0001:
        if (vm->mouse_cursor_flag < 0)
            vm->mouse_cursor_flag++;
        vm->mouse_visible = vm->mouse_cursor_flag == 0;
        dos_vga_mouse_update(vm);
        break;
    case 0x0002:
        if (vm->mouse_cursor_flag > -32768)
            vm->mouse_cursor_flag--;
        vm->mouse_visible = false;
        dos_vga_mouse_update(vm);
        break;
    case 0x0003:
        cpu->bx = vm->mouse_buttons;
        cpu->cx = vm->mouse_x;
        cpu->dx = vm->mouse_y;
        break;
    case 0x0004:
        dos_mouse_set_position(vm, snapshot, screen_width, screen_height,
                               cpu->cx, cpu->dx);
        break;
    case 0x0005: {
        uint16_t button = cpu->bx;
        cpu->ax = vm->mouse_buttons;
        if (button < DOS_MOUSE_BUTTONS) {
            cpu->bx = (uint16_t)(snapshot->press_count[button] -
                                 vm->mouse_press_count[button]);
            vm->mouse_press_count[button] = snapshot->press_count[button];
            cpu->cx = vm->mouse_press_x[button];
            cpu->dx = vm->mouse_press_y[button];
        } else {
            cpu->bx = 0;
            cpu->cx = vm->mouse_x;
            cpu->dx = vm->mouse_y;
        }
        break;
    }
    case 0x0006: {
        uint16_t button = cpu->bx;
        cpu->ax = vm->mouse_buttons;
        if (button < DOS_MOUSE_BUTTONS) {
            cpu->bx = (uint16_t)(snapshot->release_count[button] -
                                 vm->mouse_release_count[button]);
            vm->mouse_release_count[button] =
                snapshot->release_count[button];
            cpu->cx = vm->mouse_release_x[button];
            cpu->dx = vm->mouse_release_y[button];
        } else {
            cpu->bx = 0;
            cpu->cx = vm->mouse_x;
            cpu->dx = vm->mouse_y;
        }
        break;
    }
    case 0x0007:
        dos_mouse_set_range(vm, snapshot, screen_width, screen_height, true,
                            cpu->cx, cpu->dx);
        break;
    case 0x0008:
        dos_mouse_set_range(vm, snapshot, screen_width, screen_height, false,
                            cpu->cx, cpu->dx);
        break;
    case 0x000B:
        cpu->cx = (uint16_t)(snapshot->motion_x - vm->mouse_motion_x);
        cpu->dx = (uint16_t)(snapshot->motion_y - vm->mouse_motion_y);
        vm->mouse_motion_x = snapshot->motion_x;
        vm->mouse_motion_y = snapshot->motion_y;
        break;
    case 0x0023:
        cpu->bx = 0; /* English */
        break;
    case 0x0024:
        cpu->bx = DOS_MOUSE_VERSION;
        cpu->cx = (DOS_MOUSE_TYPE_PS2 << 8); /* PS/2 has no legacy IRQ code */
        break;
    case 0x0026:
        cpu->bx = 0; /* driver enabled */
        cpu->cx = dos_mouse_max_x(vm);
        cpu->dx = dos_mouse_max_y(vm);
        break;
    case 0x0031:
        cpu->ax = vm->mouse_min_x;
        cpu->bx = vm->mouse_min_y;
        cpu->cx = vm->mouse_max_x;
        cpu->dx = vm->mouse_max_y;
        break;
    default:
        dos_mouse_diag_unsupported(vm, function);
        break;
    }
}

void dos_mouse_init(dos_vm_t *vm)
{
    if (!vm) return;
    input_mouse_snapshot_t snapshot;
    input_get_mouse_snapshot(&snapshot);
    dos_mouse_reset(vm, &snapshot, fb_get_width(), fb_get_height());
}

void dos_mouse_video_mode_changed(dos_vm_t *vm)
{
    dos_mouse_init(vm);
}

void dos_int33_mouse(dos_vm_t *vm)
{
    if (!vm || !vm->cpu) return;
    input_mouse_snapshot_t snapshot;
    input_get_mouse_snapshot(&snapshot);
    dos_mouse_dispatch_snapshot(vm, &snapshot, fb_get_width(), fb_get_height());
}

int dos_mouse_selftest(void)
{
    dos_vm_t vm = {0};
    cpu8086_state_t cpu = {0};
    input_mouse_snapshot_t snapshot = {0};
    int failures = 0;

    vm.cpu = &cpu;
    cpu.vm = &vm;
    vm.vga_mode = 0x13;
    snapshot.x = 320;
    snapshot.y = 200;

    cpu.ax = 0;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.ax != 0xFFFFU || cpu.bx != DOS_MOUSE_BUTTONS ||
        vm.mouse_visible || vm.mouse_cursor_flag != -1 ||
        vm.mouse_max_x != 639 || vm.mouse_max_y != 199)
        failures++;

    cpu.ax = 1;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (!vm.mouse_visible || vm.mouse_cursor_flag != 0)
        failures++;
    cpu.ax = 2;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    cpu.ax = 1;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (vm.mouse_visible || vm.mouse_cursor_flag != -1)
        failures++;
    cpu.ax = 1;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (!vm.mouse_visible || vm.mouse_cursor_flag != 0)
        failures++;

    cpu.ax = 7;
    cpu.cx = 500;
    cpu.dx = 100;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    cpu.ax = 8;
    cpu.cx = 160;
    cpu.dx = 40;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    cpu.ax = 4;
    cpu.cx = 0;
    cpu.dx = 300;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    cpu.ax = 3;
    snapshot.buttons = 5;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.bx != 5 || cpu.cx != 100 || cpu.dx != 160)
        failures++;

    snapshot.press_count[0] = 2;
    snapshot.press_x[0] = 320;
    snapshot.press_y[0] = 200;
    cpu.ax = 5;
    cpu.bx = 0;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.ax != 5 || cpu.bx != 2 || cpu.cx < 100 || cpu.cx > 500 ||
        cpu.dx < 40 || cpu.dx > 160)
        failures++;
    cpu.ax = 5;
    cpu.bx = 0;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.bx != 0)
        failures++;

    snapshot.release_count[2] = 3;
    snapshot.release_x[2] = 400;
    snapshot.release_y[2] = 220;
    cpu.ax = 6;
    cpu.bx = 2;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.bx != 3)
        failures++;

    snapshot.motion_x = 12;
    snapshot.motion_y = -7;
    cpu.ax = 0x0B;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.cx != 12 || (int16_t)cpu.dx != -7)
        failures++;
    cpu.ax = 0x0B;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.cx != 0 || cpu.dx != 0)
        failures++;

    cpu.ax = 0x24;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.bx != DOS_MOUSE_VERSION || cpu.cx != 0x0400)
        failures++;
    cpu.ax = 0x26;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.bx != 0 || cpu.cx != 639 || cpu.dx != 199)
        failures++;
    cpu.ax = 0x31;
    dos_mouse_dispatch_snapshot(&vm, &snapshot, 640, 400);
    if (cpu.ax != 100 || cpu.bx != 40 || cpu.cx != 500 || cpu.dx != 160)
        failures++;

    vm.vga_mode = 0x03;
    dos_mouse_reset(&vm, &snapshot, 640, 400);
    if (vm.mouse_visible || (vm.mouse_x & 7U) || (vm.mouse_y & 7U) ||
        vm.mouse_max_x != 639 || vm.mouse_max_y != 199)
        failures++;

    vm.vga_mode = 0xFF;
    vm.vbe_active = true;
    vm.vbe_width = 800;
    vm.vbe_height = 600;
    int32_t mapped_x;
    int32_t mapped_y;
    dos_mouse_map_host(&vm, 639, 479, 640, 480,
                       &mapped_x, &mapped_y);
    dos_mouse_reset(&vm, &snapshot, 640, 480);
    if (mapped_x != 799 || mapped_y != 599 ||
        vm.mouse_x != 400 || vm.mouse_y != 300 ||
        vm.mouse_max_x != 799 || vm.mouse_max_y != 599)
        failures++;
    return failures;
}
