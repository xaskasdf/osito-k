#include "thermal.h"
#include "../include/msr_safe.h"

#define IA32_THERM_STATUS        0x19c
#define IA32_MISC_ENABLE         0x1a0
#define MSR_TEMPERATURE_TARGET   0x1a2
#define IA32_PACKAGE_THERM_STATUS 0x1b1
#define IA32_PM_ENABLE           0x770
#define IA32_HWP_REQUEST         0x774

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t n);
extern void serial_puthex(uint64_t n, int digits);
extern volatile uint32_t *idt_get_apic_base(void);

#ifdef THERMAL_HOST_TEST
extern void thermal_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                           uint32_t *c, uint32_t *d);
#else
static void thermal_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                           uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}
#endif

static void thermal_sensor_read(thermal_sensor_t *s, uint32_t msr, bool package)
{
    s->readable = x86_read_msr_safe(msr, &s->raw);
    /* Package status has no bit-31 Reading Valid field (Intel SDM 16.9). */
    s->valid = s->readable && (package || (s->raw & (1ULL << 31)));
    s->delta_c = (s->raw >> 16) & 0x7f;
}

void thermal_read(thermal_snapshot_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    uint32_t a, b, c, d;
    thermal_cpuid(0, &a, &b, &c, &d);
    s->intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
    if (!s->intel || a < 6) return;
    thermal_cpuid(1, &a, &b, &c, &d);
    s->apic_id = b >> 24;
    s->msr = (d & (1U << 5)) != 0;
    s->est = (c & (1U << 7)) != 0;
    s->tm = (d & (1U << 29)) != 0;
    if (!s->msr) return;
    thermal_cpuid(6, &a, &b, &c, &d);
    s->dts = (a & 1U) != 0;
    s->package = (a & (1U << 6)) != 0;
    s->hwp = (a & (1U << 7)) != 0;
    if (s->dts || s->package) {
        if (x86_read_msr_safe(MSR_TEMPERATURE_TARGET, &s->target_raw)) {
            s->target_c = (s->target_raw >> 16) & 0xff;
            s->target_valid = s->target_c != 0;
        }
    }
    if (s->dts) thermal_sensor_read(&s->core, IA32_THERM_STATUS, false);
    if (s->package) thermal_sensor_read(&s->pkg, IA32_PACKAGE_THERM_STATUS, true);
    if (s->est || s->tm)
        s->misc_valid = x86_read_msr_safe(IA32_MISC_ENABLE, &s->misc);
    if (s->hwp) {
        s->hwp_valid = x86_read_msr_safe(IA32_PM_ENABLE, &s->hwp_enable);
        /* HWP request registers may only be accessed after HWP is enabled. */
        if (s->hwp_valid && (s->hwp_enable & 1))
            s->hwp_request_valid = x86_read_msr_safe(IA32_HWP_REQUEST,
                                                   &s->hwp_request);
    }
}

static void thermal_print_sensor(const char *name, const thermal_sensor_t *s,
                                  const thermal_snapshot_t *cpu)
{
    serial_puts(name); serial_puts(": ");
    if (!s->readable) { serial_puts("unavailable\n"); return; }
    if (!s->valid) serial_puts("reading invalid");
    else {
        if (cpu->target_valid) {
            int degrees = (int)cpu->target_c - s->delta_c;
            if (degrees < 0) { serial_puts("-"); degrees = -degrees; }
            serial_putdec((uint64_t)degrees); serial_puts(" C; ");
        }
        serial_puts("DTS delta="); serial_putdec(s->delta_c); serial_puts(" C");
    }
    serial_puts("; thermal="); serial_putdec(s->raw & 1);
    serial_puts(" critical="); serial_putdec((s->raw >> 4) & 1);
    serial_puts(" external-PROCHOT="); serial_putdec((s->raw >> 2) & 1);
    serial_puts("; logged thermal/critical=");
    serial_putdec((s->raw >> 1) & 1); serial_puts("/");
    serial_putdec((s->raw >> 5) & 1);
    serial_puts("; raw="); serial_puthex(s->raw, 16); serial_puts("\n");
    if (s->raw & (1ULL << 4))
        serial_puts("THERMAL CRITICAL: stop workloads and power down now.\n");
}

void thermal_dump(void)
{
    thermal_snapshot_t s;
    thermal_read(&s);
    serial_puts("Thermal diagnostics (read-only; current CPU/package)\n");
    if (!s.intel || !s.msr) {
        serial_puts("Intel thermal MSRs unavailable on this CPU\n");
        return;
    }
    serial_puts("CPU APIC "); serial_putdec(s.apic_id);
    serial_puts("; temperature target: ");
    if (s.target_valid) { serial_putdec(s.target_c); serial_puts(" C"); }
    else serial_puts("unavailable (no assumed TjMax)");
    serial_puts("; raw="); serial_puthex(s.target_raw, 16); serial_puts("\n");
    thermal_print_sensor("Core", &s.core, &s);
    thermal_print_sensor("Package", &s.pkg, &s);
    if (s.misc_valid) {
        if (s.tm) {
            serial_puts("Automatic thermal control: ");
            serial_puts(s.misc & (1ULL << 3) ? "enabled\n" : "disabled\n");
        }
        if (s.est) {
            serial_puts("SpeedStep: ");
            serial_puts(s.misc & (1ULL << 16) ? "enabled\n" : "disabled\n");
        }
    }
    serial_puts("HWP: ");
    serial_puts(!s.hwp ? "unsupported\n" : !s.hwp_valid ? "unavailable\n" :
                (s.hwp_enable & 1) ? "enabled\n" : "disabled\n");
    if (s.hwp_request_valid) {
        serial_puts("HWP request: "); serial_puthex(s.hwp_request, 16);
        serial_puts("\n");
    }
    volatile uint32_t *apic = idt_get_apic_base();
    serial_puts("Local APIC timer: ");
    serial_puts(!apic ? "unavailable\n" :
        (apic[0x320 / 4] & 0x10000U) ? "masked\n" : "unmasked\n");
    serial_puts("Fan RPM/control: unavailable (no ACPI EC fan driver); unchanged\n");
}

void thermal_msr_selftest(void)
{
    uint64_t value = 0x12345678ULL;
    if (x86_read_msr_safe(0xffffffffU, &value)) {
        serial_puts("MSR probe: unsupported register accepted by platform\n");
        return;
    }
    serial_puts(value == 0x12345678ULL ? "MSR probe: fault recovered; output unchanged PASS\n" :
                                      "MSR probe: output changed FAIL\n");
}
