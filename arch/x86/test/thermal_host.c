#define THERMAL_HOST_TEST
#include "../kernel/thermal.c"

extern int printf(const char *, ...);
static unsigned checks, failures, reads;
static bool other_vendor, no_msr, fail_target, fail_core, fail_package, fail_hwp;
static uint32_t max_leaf = 6, thermal_features = 0xc1;
static uint64_t target = 100ULL << 16, core = (1ULL << 31) | (35ULL << 16);
static uint64_t package = 30ULL << 16, hwp_enabled;
const uint8_t x86_read_msr_fault[1] = {0}, x86_read_msr_fixup[1] = {0};
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

void thermal_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    if (!leaf) {
        *a = max_leaf; *b = other_vendor ? 0 : 0x756e6547;
        *d = 0x49656e69; *c = 0x6c65746e;
    } else if (leaf == 1) {
        *b = 7U << 24; *d = no_msr ? 0 : (1U << 5) | (1U << 29);
        *c = 1U << 7;
    } else if (leaf == 6) *a = thermal_features;
    else CHECK(false);
}

bool x86_read_msr_safe(uint32_t msr, uint64_t *value)
{
    reads++;
    switch (msr) {
    case MSR_TEMPERATURE_TARGET:
        if (fail_target) return false;
        *value = target; return true;
    case IA32_THERM_STATUS:
        if (fail_core) return false;
        *value = core; return true;
    case IA32_PACKAGE_THERM_STATUS:
        if (fail_package) return false;
        *value = package; return true;
    case IA32_MISC_ENABLE: *value = (1ULL << 3) | (1ULL << 16); return true;
    case IA32_PM_ENABLE:
        if (fail_hwp) return false;
        *value = hwp_enabled; return true;
    case IA32_HWP_REQUEST:
        CHECK(hwp_enabled & 1);
        *value = 0x8000ff08; return true;
    default: CHECK(false); return false;
    }
}

void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
volatile uint32_t *idt_get_apic_base(void) { return NULL; }

int main(void)
{
    thermal_snapshot_t s;
    thermal_read(NULL); CHECK(reads == 0);
    thermal_read(&s);
    CHECK(s.intel && s.msr && s.apic_id == 7);
    CHECK(s.target_valid && s.target_c == 100);
    CHECK(s.core.valid && s.core.delta_c == 35 && s.target_c-s.core.delta_c == 65);
    CHECK(s.pkg.valid && s.pkg.delta_c == 30 && !(s.pkg.raw & (1ULL << 31)));
    CHECK(s.misc_valid && s.hwp_valid && !s.hwp_request_valid);
    CHECK(reads == 5);
    hwp_enabled = 1; thermal_read(&s);
    CHECK(s.hwp_request_valid && s.hwp_request == 0x8000ff08);
    fail_hwp = true; thermal_read(&s);
    CHECK(!s.hwp_valid && !s.hwp_request_valid);
    fail_hwp = false;
    core = (40ULL << 16) | 0x33; thermal_read(&s);
    CHECK(s.core.readable && !s.core.valid && s.core.raw == core);
    CHECK(s.pkg.valid);
    fail_core = true; fail_package = true; thermal_read(&s);
    CHECK(!s.core.valid && !s.core.readable && !s.pkg.valid && !s.pkg.readable);
    fail_core = fail_package = false;
    fail_target = true; thermal_read(&s);
    CHECK(!s.target_valid && s.target_c == 0 && s.pkg.valid);
    fail_target = false; target = 0; thermal_read(&s);
    CHECK(!s.target_valid);
    thermal_features = 0; reads = 0; thermal_read(&s);
    CHECK(!s.dts && !s.package && !s.hwp && reads == 1);
    CHECK(!s.core.readable && !s.pkg.readable && !s.target_valid);
    other_vendor = true; reads = 0; thermal_read(&s);
    CHECK(!s.intel && reads == 0);
    other_vendor = false; max_leaf = 1; thermal_read(&s);
    CHECK(!s.msr && reads == 0);
    max_leaf = 6; no_msr = true; thermal_read(&s);
    CHECK(!s.msr && reads == 0);
    uint64_t rip = (uintptr_t)x86_read_msr_fault;
    CHECK(!x86_msr_fixup(14, 8, 0, &rip));
    CHECK(!x86_msr_fixup(13, 0x1b, 0, &rip));
    CHECK(!x86_msr_fixup(13, 0x3b, 0, &rip));
    CHECK(!x86_msr_fixup(13, 8, 1, &rip));
    CHECK(rip == (uintptr_t)x86_read_msr_fault);
    CHECK(!x86_msr_fixup(13, 8, 0, NULL));
    rip++; CHECK(!x86_msr_fixup(13, 8, 0, &rip)); rip--;
    CHECK(x86_msr_fixup(13, 8, 0, &rip));
    CHECK(rip == (uintptr_t)x86_read_msr_fixup);
    rip = (uintptr_t)x86_read_msr_fault;
    CHECK(x86_msr_fixup(13, 0x38, 0, &rip));
    CHECK(rip == (uintptr_t)x86_read_msr_fixup);
    rip = 0x400000;
    CHECK(!x86_msr_fixup(13, 0x38, 0, &rip) && rip == 0x400000);
    printf("Thermal read-only: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
