#define LOCKUP_HOST_TEST
#include "../kernel/lockup.c"

extern int printf(const char *, ...);
extern char *strstr(const char *, const char *);
static unsigned checks, failures, sends, mode, cpu, cpu_count, submit_failure;
static unsigned no_ack, send_failure, stop_after_sample, view_failure;
static uint64_t fake_tsc, hz;
static uint32_t apic[256];
static uint32_t pixels[1280 * 1024 + 2];
static screen_view_t view;
static smp_work_func_t queued;
static char output[4096];
static unsigned output_size;
const uint8_t gui_font8x16[95][16] = { [0 ... 94] = { [0 ... 15] = 0xaa } };
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

uint64_t lockup_tsc(void) { fake_tsc += 1000000; return fake_tsc; }
uint64_t idt_get_ticks(void) { return mode == 0 ? 1 : fake_tsc / 10000; }
uint64_t sched_get_switches(void) { return mode < 2 ? 1 : fake_tsc / 10000; }
uint64_t idt_get_tsc_freq(void) { return hz; }
uint32_t smp_current_cpu(void) { return cpu; }
uint32_t smp_cpu_count(void) { return cpu_count; }
volatile uint32_t *idt_get_apic_base(void) { return apic; }
int sched_sleep_ticks(uint64_t value) { (void)value; return 0; }
int smp_submit_ff(smp_work_func_t fn, void *arg, void *res)
{
    CHECK(!arg && !res);
    if (submit_failure) return -1;
    queued = fn;
    state = WD_ARMED; /* The fixture drives the asynchronous AP explicitly. */
    return 0;
}
bool lockup_send_nmi(void)
{
    sends++;
    CHECK(lockup_nmi_pending == 1);
    if (send_failure) return false;
    if (!no_ack) {
        lockup_nmi_frame.rip = 0xffff800002001234ULL;
        lockup_nmi_frame.rflags = mode ? 0x202 : 2;
        lockup_nmi_captured++;
        lockup_nmi_pending = 0;
    }
    if (stop_after_sample) stop_requested = 1;
    return true;
}
static int get_view(screen_view_t *out)
{
    *out = view;
    return view_failure ? SCREEN_UNAVAILABLE : SCREEN_OK;
}
const screen_capture_ops_t screen_capture_gop = { .view = get_view };
void sh_puts(const char *s)
{
    while (*s && output_size < sizeof(output) - 1) output[output_size++] = *s++;
    output[output_size] = 0;
}
static void reset(void)
{
    running = stop_requested = sample_requested = state = reason = 0;
    lockup_nmi_pending = lockup_nmi_captured = 0;
    fake_tsc = sends = mode = submit_failure = no_ack = send_failure = 0;
    stop_after_sample = view_failure = output_size = 0;
    output[0] = 0;
    cpu = 1;
    cpu_count = 2;
    hz = 1000000;
    queued = NULL;
    for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++)
        pixels[i] = 0x12345678;
    view = (screen_view_t){pixels + 1, 336 * 160 * 4, 320, 160, 336, 1};
}
static void bounds_check(void)
{
    CHECK(pixels[0] == 0x12345678);
    CHECK(pixels[1 + view.pitch * view.height] == 0x12345678);
    unsigned changed = 0;
    for (unsigned y = 0; y < view.height; y++) {
        for (unsigned x = 0; x < view.width; x++)
            changed += pixels[1 + y * view.pitch + x] != 0x12345678;
        for (unsigned x = view.width; x < view.pitch; x++)
            CHECK(pixels[1 + y * view.pitch + x] == 0x12345678);
    }
    CHECK(changed > 0);
}
int main(void)
{
    reset();
    CHECK(!arm(0) && !arm(1) && !arm(61));
    cpu_count = 1; CHECK(!arm(2)); cpu_count = 2;
    hz = 0; CHECK(!arm(2)); hz = 1000000;
    view_failure = 1; CHECK(!arm(2)); view_failure = 0;
    view.bytes--; CHECK(!arm(2)); view.bytes++;
    submit_failure = 1; CHECK(!arm(2) && !running); submit_failure = 0;
    CHECK(arm(2) && running && queued);
    CHECK(!arm(2));
    queued(NULL, NULL);
    CHECK(state == WD_CAPTURED && reason == WHY_TIMER && !running);
    CHECK(sends == 1 && lockup_nmi_captured == 1 && !lockup_nmi_pending);
    bounds_check();
    report(false);
    CHECK(strstr(output, "RIP=FFFF800002001234") != NULL);

    reset(); mode = 1;
    CHECK(arm(2)); queued(NULL, NULL);
    CHECK(state == WD_CAPTURED && reason == WHY_SCHED && sends == 1);
    reset(); mode = 2;
    CHECK(arm(2)); queued(NULL, NULL);
    CHECK(state == WD_EXPIRED && !running && !sends);
    reset();
    CHECK(arm(2)); stop_requested = 1; queued(NULL, NULL);
    CHECK(state == WD_OFF && !running && !sends);
    reset();
    CHECK(arm(2)); cpu = 0; queued(NULL, NULL);
    CHECK(state == WD_FAILED && !running && !sends);
    reset(); mode = 2;
    CHECK(arm(2)); sample_requested = stop_after_sample = 1; queued(NULL, NULL);
    CHECK(reason == WHY_SAMPLE && sends == 1 && lockup_nmi_captured == 1);
    reset(); no_ack = 1;
    CHECK(arm(2)); queued(NULL, NULL);
    CHECK(state == WD_FAILED && reason == WHY_NO_NMI && lockup_nmi_pending);
    CHECK(!running && !arm(2));
    bounds_check();
    reset(); send_failure = 1;
    CHECK(arm(2)); queued(NULL, NULL);
    CHECK(state == WD_FAILED && !lockup_nmi_pending && !running);

    reset();
    view = (screen_view_t){pixels + 1, 1280 * 1024 * 4, 1280, 1024, 1280, 0};
    CHECK(arm(2)); queued(NULL, NULL); bounds_check();
    char *bad[] = {"lockup", "arm", "99999999999999999999"};
    lockup_command(3, bad);
    CHECK(strstr(output, "cannot arm") != NULL);
    printf("lockup host: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
