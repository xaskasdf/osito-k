/* Exercise the real PS/2 decoder and shared event producer, not a keyboard
 * mock. Only interrupt masking and hardware/polling entry points are stubbed. */
#define INPUT_HOST_TEST 1
#include "../kernel/input_events.c"
#include "../kernel/keyboard.c"

extern int printf(const char *format, ...);
extern void abort(void);

static bool desktop;
static unsigned checks;
static unsigned win32_count;
static uint16_t win32_scan[4096];
static bool win32_up[4096];
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); abort(); \
} } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void serial_putdec(uint64_t n) { (void)n; }
static int serial_key = -1;
int serial_getc(void) { int c = serial_key; serial_key = -1; return c; }
void ps2_init(void) { }
void ps2_irq(void) { }
void ps2_poll(void) { }
bool compositor_is_running(void) { return desktop; }
void win32_post_keyboard_scan(uint16_t scan, int key_up)
{
    CHECK(win32_count < 4096);
    win32_scan[win32_count] = scan;
    win32_up[win32_count++] = key_up != 0;
}

static void reset(bool gui)
{
    desktop = gui;
    kb_init();
    input_events_init(1280, 800);
    win32_count = 0;
}

static void expect_event(uint8_t usage, bool pressed, uint8_t flags)
{
    input_event_t event;
    CHECK(input_pop_event(&event));
    CHECK(event.type == (pressed ? INPUT_KEY_DOWN : INPUT_KEY_UP));
    CHECK(event.scancode == usage);
    CHECK(event.flags == flags);
    CHECK(!event.buttons && !event.dx && !event.dy && !event.wheel);
}

static void key(uint8_t sc, bool extended, bool pressed)
{
    if (extended) kb_handle_hw_byte(0xE0);
    kb_handle_hw_byte(sc | (pressed ? 0 : 0x80));
}

static void mapping(void)
{
    /* Physical rows, independent of the alphabetical HID table ordering. */
    static const struct { uint8_t sc, usage; char ascii; } row[] = {
        {0x10,0x14,'q'}, {0x11,0x1A,'w'}, {0x12,0x08,'e'}, {0x13,0x15,'r'},
        {0x14,0x17,'t'}, {0x15,0x1C,'y'}, {0x16,0x18,'u'}, {0x17,0x0C,'i'},
        {0x18,0x12,'o'}, {0x19,0x13,'p'}, {0x1E,0x04,'a'}, {0x1F,0x16,'s'},
        {0x20,0x07,'d'}, {0x21,0x09,'f'}, {0x22,0x0A,'g'}, {0x23,0x0B,'h'},
        {0x24,0x0D,'j'}, {0x25,0x0E,'k'}, {0x26,0x0F,'l'}, {0x2C,0x1D,'z'},
        {0x2D,0x1B,'x'}, {0x2E,0x06,'c'}, {0x2F,0x19,'v'}, {0x30,0x05,'b'},
        {0x31,0x11,'n'}, {0x32,0x10,'m'}, {0x02,0x1E,'1'}, {0x03,0x1F,'2'},
        {0x04,0x20,'3'}, {0x05,0x21,'4'}, {0x06,0x22,'5'}, {0x07,0x23,'6'},
        {0x08,0x24,'7'}, {0x09,0x25,'8'}, {0x0A,0x26,'9'}, {0x0B,0x27,'0'},
        {0x0C,0x2D,'-'}, {0x0D,0x2E,'='}, {0x1A,0x2F,'['}, {0x1B,0x30,']'},
        {0x2B,0x31,'\\'}, {0x27,0x33,';'}, {0x28,0x34,'\''}, {0x29,0x35,'`'},
        {0x33,0x36,','}, {0x34,0x37,'.'}, {0x35,0x38,'/'}, {0x39,0x2C,' '},
        {0x01,0x29,27}, {0x0F,0x2B,'\t'}, {0x0E,0x2A,'\b'}, {0x1C,0x28,'\n'},
        {0x3B,0x3A,0}, {0x3C,0x3B,0}, {0x3D,0x3C,0}, {0x3E,0x3D,0},
        {0x3F,0x3E,0}, {0x40,0x3F,0}, {0x41,0x40,0}, {0x42,0x41,0},
        {0x43,0x42,0}, {0x44,0x43,0}, {0x57,0x44,0}, {0x58,0x45,0},
        {0x37,0x55,'*'}, {0x4A,0x56,'-'}, {0x4E,0x57,'+'},
        {0x4F,0x59,'1'}, {0x50,0x5A,'2'}, {0x51,0x5B,'3'}, {0x4B,0x5C,'4'},
        {0x4C,0x5D,'5'}, {0x4D,0x5E,'6'}, {0x47,0x5F,'7'}, {0x48,0x60,'8'},
        {0x49,0x61,'9'}, {0x52,0x62,'0'}, {0x53,0x63,'.'},
        {0x45,0x53,0}, {0x46,0x47,0}, {0x56,0x64,0},
    };
    for (unsigned gui = 0; gui < 2; gui++) {
        reset(gui);
        for (unsigned i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            key(row[i].sc, false, true);
            expect_event(row[i].usage, true, 0);
            key(row[i].sc, false, false);
            expect_event(row[i].usage, false, 0);
            CHECK(kb_trygetchar() == (gui ? 0 : row[i].ascii));
            CHECK(!kb_has_input() && !input_has_events());
            /* Native/USB producers must publish exactly the same key IDs. */
            input_post_key(row[i].usage, true, false);
            expect_event(row[i].usage, true, 0);
            input_post_key(row[i].usage, false, false);
            expect_event(row[i].usage, false, 0);
        }
    }
}

static void modifiers(void)
{
    static const struct { uint8_t lsc, rsc, left, right, flag; bool ext; } mods[] = {
        {0x2A,0x36,0xE1,0xE5,KEY_FLAG_SHIFT,false},
        {0x1D,0x1D,0xE0,0xE4,KEY_FLAG_CTRL,true},
        {0x38,0x38,0xE2,0xE6,KEY_FLAG_ALT,true},
    };
    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        reset(true);
        uint8_t ext = mods[i].ext ? KEY_FLAG_EXTENDED : 0;
        key(mods[i].lsc, false, true);
        expect_event(mods[i].left, true, mods[i].flag);
        key(mods[i].rsc, mods[i].ext, true);
        expect_event(mods[i].right, true, mods[i].flag | ext);
        key(mods[i].lsc, false, false);
        expect_event(mods[i].left, false, mods[i].flag);
        key(0x10, false, true);
        expect_event(0x14, true, mods[i].flag);
        key(0x10, false, false);
        expect_event(0x14, false, mods[i].flag);
        key(mods[i].rsc, mods[i].ext, false);
        expect_event(mods[i].right, false, ext);
        CHECK(input_get_modifiers() == 0);

        input_post_key(mods[i].left, true, false);
        expect_event(mods[i].left, true, mods[i].flag);
        input_post_key(mods[i].right, true, false);
        expect_event(mods[i].right, true, mods[i].flag);
        input_post_key(mods[i].left, false, false);
        expect_event(mods[i].left, false, mods[i].flag);
        input_post_key(mods[i].right, false, false);
        expect_event(mods[i].right, false, 0);
    }
    reset(true);
    key(0x3A, false, true);
    expect_event(0x39, true, KEY_FLAG_CAPS);
    key(0x3A, false, true); /* typematic must not toggle twice */
    expect_event(0x39, true, KEY_FLAG_CAPS);
    key(0x3A, false, false);
    expect_event(0x39, false, KEY_FLAG_CAPS);
    key(0x2A, false, true);
    expect_event(0xE1, true, KEY_FLAG_SHIFT | KEY_FLAG_CAPS);
    key(0x10, false, true);
    expect_event(0x14, true, KEY_FLAG_SHIFT | KEY_FLAG_CAPS);
    key(0x2A, false, false);
    expect_event(0xE1, false, KEY_FLAG_CAPS);
    key(0x3A, false, true);
    expect_event(0x39, true, 0);
    key(0x3A, false, false);
    expect_event(0x39, false, 0);
}

static void extended_keys(void)
{
    static const struct { uint8_t sc, usage; const char *vt; } keys[] = {
        {0x48,0x52,"A"}, {0x50,0x51,"B"}, {0x4D,0x4F,"C"}, {0x4B,0x50,"D"},
        {0x47,0x4A,"H"}, {0x4F,0x4D,"F"}, {0x52,0x49,"2~"}, {0x53,0x4C,"3~"},
        {0x49,0x4B,"5~"}, {0x51,0x4E,"6~"},
        {0x1C,0x58,NULL}, {0x35,0x54,NULL}, {0x5B,0xE3,NULL},
        {0x5C,0xE7,NULL}, {0x5D,0x65,NULL},
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        reset(false);
        kb_handle_hw_byte(0xE0);
        CHECK(!input_has_events() && !kb_has_input());
        kb_handle_hw_byte(keys[i].sc);
        expect_event(keys[i].usage, true, KEY_FLAG_EXTENDED);
        key(keys[i].sc, true, false);
        expect_event(keys[i].usage, false, KEY_FLAG_EXTENDED);
        if (keys[i].vt) {
            CHECK(kb_trygetchar() == 27);
            CHECK(kb_trygetchar() == '[');
            for (const char *p = keys[i].vt; *p; p++) CHECK(kb_trygetchar() == *p);
        }
        CHECK(!kb_has_input());
    }
    reset(true);
    static const uint8_t print[] = {0xE0,0x2A,0xE0,0x37,0xE0,0xB7,0xE0,0xAA};
    for (unsigned i = 0; i < sizeof(print); i++) kb_handle_hw_byte(print[i]);
    expect_event(0x46, true, KEY_FLAG_EXTENDED);
    expect_event(0x46, false, KEY_FLAG_EXTENDED);
    CHECK(!input_has_events() && !input_get_modifiers());

    key(0x1D, false, true);
    expect_event(0xE0, true, KEY_FLAG_CTRL);
    static const uint8_t pause[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
    for (unsigned i = 0; i < sizeof(pause); i++) kb_handle_hw_byte(pause[i]);
    expect_event(0x48, true, KEY_FLAG_CTRL);
    expect_event(0x48, false, KEY_FLAG_CTRL);
    CHECK(kb_get_bios_shift_flags() & 4); /* Pause must not release real Ctrl. */
    key(0x1D, false, false);
    expect_event(0xE0, false, 0);
    CHECK(!input_has_events());
    kb_handle_hw_byte(0xE1); kb_handle_hw_byte(0x1D);
    key(0x10, false, true); /* recover from a truncated Pause */
    expect_event(0x14, true, 0);
    static const uint8_t noise[] = {0,0xFA,0xFE,0xFF,0xE0,0x2A,0xE0,0xAA};
    for (unsigned i = 0; i < sizeof(noise); i++) kb_handle_hw_byte(noise[i]);
    CHECK(!input_has_events() && !input_get_modifiers());
}

static void raw_and_injected(void)
{
    reset(true);
    int owner;
    static const uint8_t bytes[] = {
        0x10,0x90,0xE0,0x48,0xE0,0xC8,
        0xE1,0x1D,0x45,0xE1,0x9D,0xC5,
        0xE0,0x2A,0xE0,0x37,0xE0,0xB7,0xE0,0xAA,
    };
    CHECK(input_keyboard_acquire(&owner));
    for (unsigned i = 0; i < sizeof(bytes); i++) kb_handle_hw_byte(bytes[i]);
    CHECK(!input_has_events() && !kb_has_input());
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        uint8_t byte;
        CHECK(input_keyboard_read_set1(&owner, &byte));
        CHECK(byte == bytes[i]);
    }
    input_keyboard_release(&owner);
    key(0x10, false, true);
    expect_event(0x14, true, 0);
    key(0x10, false, false);
    expect_event(0x14, false, 0);
    kb_inject_scancode(0x10);
    kb_inject_scancode(0x90);
    CHECK(kb_trygetchar() == 'q');
    CHECK(!input_has_events() && !kb_has_input());
    reset(false);
    key(0x2A, false, true); key(0x10, false, true);
    CHECK(kb_trygetchar() == 'Q');
    key(0x2A, false, false); key(0x1D, false, true); key(0x2E, false, true);
    CHECK(kb_trygetchar() == 3);
    CHECK(!kb_has_input());
}

static bool cancel_wait(void *context)
{
    unsigned *calls = context;
    (*calls)++;
    return true;
}

static void interrupted_input(void)
{
    reset(false);
    unsigned calls = 0;
    CHECK(kb_getchar_interruptible(cancel_wait, &calls) == -1 && calls == 1);
    kb_inject_scancode(0x10);
    CHECK(kb_getchar_interruptible(cancel_wait, &calls) == 'q' && calls == 1);
    serial_key = 's';
    CHECK(kb_getchar_interruptible(cancel_wait, &calls) == 's' && calls == 1);
    serial_key = 't';
    CHECK(kb_getchar() == 't');
    serial_key = 255;
    CHECK(kb_getchar_interruptible(cancel_wait, &calls) == 255 && calls == 1);
    CHECK(kb_getchar_interruptible(cancel_wait, &calls) == -1 && calls == 2);
}

static void win32_routing(void)
{
    for (unsigned gui = 0; gui < 2; gui++) {
        reset(gui);
        key(0x10, false, true); key(0x10, false, false);
        key(0x48, true, true); key(0x48, true, false);
        key(0x1D, true, true); key(0x1D, true, false);
        CHECK(win32_count == 6);
        for (unsigned i=0;i<6;i++) {
            CHECK(win32_scan[i] == (i<2 ? 0x10 : i<4 ? 0xE048 : 0xE01D));
            CHECK(win32_up[i] == ((i&1)!=0));
        }
        const uint8_t pause[]={0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
        for(unsigned i=0;i<sizeof(pause);i++) kb_handle_hw_byte(pause[i]);
        CHECK(win32_count==8 && win32_scan[6]==0xE145 && win32_scan[7]==0xE145);
        CHECK(!win32_up[6] && win32_up[7]);
        kb_handle_hw_byte(0xFA); kb_handle_hw_byte(0xFE);
        kb_handle_hw_byte(0xE0); kb_handle_hw_byte(0x2A);
        kb_inject_scancode(0x10); kb_inject_scancode(0x90);
        CHECK(win32_count==8);
        int owner;
        CHECK(input_keyboard_acquire(&owner));
        key(0x10,false,true); key(0x10,false,false);
        CHECK(win32_count==8);
        input_keyboard_release(&owner);
    }
}

int main(void)
{
    mapping();
    modifiers();
    extended_keys();
    raw_and_injected();
    interrupted_input();
    win32_routing();
    printf("PASS keyboard set-1/HID contract: %u checks\n", checks);
    return 0;
}
