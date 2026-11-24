/* USB wheel transport into native input and the optional Win32 bridge. */
#define XHCI_HOST_TEST 1
#include "../drivers/xhci.c"
extern int printf(const char *, ...);

static unsigned checks, failures, moves, absolute_moves, button_reports, wheel_reports;
static unsigned bridge_positions, bridge_wheels;
static int native_wheel, bridge_wheel, cursor_x, cursor_y;
static uint8_t current_buttons;
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL %d: %s\n", __LINE__, #test); failures++; } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t v) { (void)v; }
void serial_puthex(uint64_t v, int d) { (void)v; (void)d; }
void input_post_mouse_move(int16_t x, int16_t y)
{ moves++; cursor_x += x; cursor_y += y; }
void input_set_mouse_abs(int32_t x, int32_t y)
{ absolute_moves++; cursor_x = x; cursor_y = y; }
void input_post_mouse_button(uint8_t buttons)
{ button_reports++; current_buttons = buttons; }
void input_post_mouse_wheel(int16_t delta)
{
    CHECK(moves + absolute_moves == 1 && button_reports == 1);
    wheel_reports++; native_wheel = delta;
}
void input_get_cursor(int32_t *x, int32_t *y) { *x=cursor_x; *y=cursor_y; }

#ifndef OMIT_WIN32_BRIDGE
void win32_post_mouse_screen(int x, int y, int dx, int dy, uint32_t buttons, short wheel)
{
    CHECK(x == cursor_x && y == cursor_y && dx == 4 && dy == -3);
    CHECK(buttons == current_buttons && !wheel); bridge_positions++;
}
void win32_post_mouse_abs(int x, int y, int lmin, int lmax, uint32_t buttons)
{
    (void)lmin; (void)lmax;
    CHECK(x == cursor_x && y == cursor_y && buttons == current_buttons);
    bridge_positions++;
}
void win32_post_mouse_event(int dx, int dy, uint32_t buttons, short wheel)
{
    CHECK(!dx && !dy && buttons == current_buttons && wheel);
    CHECK(bridge_positions == 1); bridge_wheels++; bridge_wheel = wheel;
}
#endif

static void check_report(xhci_device_t *device, uint8_t *report, int expected)
{
    moves=absolute_moves=button_reports=wheel_reports=bridge_positions=bridge_wheels=0;
    native_wheel=bridge_wheel=0; cursor_x=cursor_y=50;
    hid_process_mouse(device, report);
    CHECK(moves + absolute_moves == 1 && button_reports == 1);
    CHECK(wheel_reports == (unsigned)(expected != 0) && native_wheel == expected);
#ifndef OMIT_WIN32_BRIDGE
    int scaled = expected * 120;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    CHECK(bridge_positions == 1);
    CHECK(bridge_wheels == wheel_reports && bridge_wheel == scaled);
#else
    CHECK(!bridge_positions && !bridge_wheels);
#endif
}

int main(void)
{
    static const uint8_t descriptor[] = {
        0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,
        0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
        0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x01,
        0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,
        0x75,0x08,0x95,0x02,0x81,0x06,0x09,0x38,0x95,0x01,0x81,0x06,
        0xC0,0xC0
    };
    xhci_device_t device = {0};
    CHECK(hid_parse(descriptor, sizeof(descriptor), &device.hid_caps) == 0);
    hid_caps_t *caps = &device.hid_caps;
    CHECK(caps->has_mouse && caps->mouse_wheel_field >= 0);
    if (!caps->has_mouse || caps->mouse_wheel_field < 0) return 1;
    uint8_t report[16] = {1, 4, 0xFD, 0};
    for (int absolute = 0; absolute < 2; absolute++) {
        if (absolute) caps->fields[caps->mouse_x_field].flags &= ~HID_INPUT_REL;
        const int deltas[] = {0, 1, -1, 2, -2, 127, -127};
        for (unsigned i = 0; i < sizeof(deltas)/sizeof(deltas[0]); i++) {
            report[3] = (uint8_t)deltas[i];
            check_report(&device, report, deltas[i]);
        }
    }
    hid_field_t *wheel = &caps->fields[caps->mouse_wheel_field];
    wheel->report_id=1; report[3]=1;
    check_report(&device, report, 0); /* A different report's field is not read. */
    wheel->report_id=0; wheel->flags &= ~HID_INPUT_REL;
    check_report(&device, report, 0); /* Absolute scroll positions are not deltas. */
    wheel->flags |= HID_INPUT_REL;
    wheel->bit_offset=8; caps->mouse_wheel_elem=8;
    report[9]=0xFE;
    check_report(&device, report, -2); /* Element offset in a shared field. */
    wheel->bit_offset=24; wheel->bit_size=32; caps->mouse_wheel_elem=0;
    report[3]=report[4]=report[5]=0xFF; report[6]=0x7F;
    check_report(&device, report, 32767);
    report[3]=report[4]=report[5]=0; report[6]=0x80;
    check_report(&device, report, -32768);
    caps->mouse_wheel_field=-1;
    check_report(&device, report, 0);
    printf("GUI USB wheel: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
