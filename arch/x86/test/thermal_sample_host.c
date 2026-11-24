#define THERMAL_SAMPLE_HOST_TEST
#include "../kernel/thermal_sample.c"

extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern char *strstr(const char *, const char *);
static unsigned checks, failures, reads, sleeps, phase, cpu_reads;
static uint32_t signature = 0x906ea, max_leaf = 0x16, max_extended = 0x80000008;
static bool intel = true, msr = true, tsc = true, invariant = true, perf = true;
static bool migrate, sleep_fail;
static uint32_t fail_msr;
static uint64_t hz = 2800000000ULL;
static sample_counters_t mock[2];
static char output[8192];
static size_t output_len;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

void sample_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    if (!leaf) {
        *a = max_leaf; *b = intel ? 0x756e6547 : 0;
        *d = 0x49656e69; *c = 0x6c65746e;
    } else if (leaf == 1) {
        *a = signature; *b = (migrate ? cpu_reads++ & 1 : phase ? mock[1].apic_id : mock[0].apic_id) << 24;
        *d = (msr ? 32 : 0) | (tsc ? 16 : 0);
    } else if (leaf == 6) *c = perf ? 1 : 0;
    else if (leaf == 0x80000000) *a = max_extended;
    else if (leaf == 0x80000007) *d = invariant ? 256 : 0;
    else CHECK(false);
}

uint64_t idt_get_tsc_freq(void) { return hz; }
uint64_t sample_tsc(void) { return mock[phase].tsc; }
int sched_sleep_ticks(uint64_t ticks)
{
    CHECK(ticks == 100);
    sleeps++; phase = 1;
    return sleep_fail ? -1 : 0;
}

bool x86_read_msr_safe(uint32_t id, uint64_t *value)
{
    reads++;
    if (id == fail_msr) return false;
    sample_counters_t *s = &mock[phase];
    switch (id) {
    case IA32_MPERF: *value = s->mperf; return true;
    case IA32_APERF: *value = s->aperf; return true;
    case MSR_RAPL_POWER_UNIT: *value = s->units; return true;
    case MSR_PKG_ENERGY_STATUS: *value = s->energy; return true;
    case IA32_PERF_CTL: *value = s->perf_ctl; return true;
    case MSR_PKG_CST_CONFIG: *value = s->cstate_config; return true;
    case MSR_POWER_CTL: *value = s->power_ctl; return true;
    default:
        for (unsigned i = 0; i < RESIDENCY_COUNT; i++)
            if (id == residency[i].msr) { *value = s->state[i]; return true; }
        CHECK(false); return false;
    }
}

void serial_puts(const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof(output) - output_len) { CHECK(false); return; }
    memcpy(output + output_len, s, n + 1); output_len += n;
}
void serial_putdec(uint64_t n)
{
    char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)n); serial_puts(b);
}
void serial_puthex(uint64_t n, int digits)
{
    char b[32]; snprintf(b, sizeof(b), "0x%0*llX", digits, (unsigned long long)n); serial_puts(b);
}

static void fixture(void)
{
    signature = 0x906ea; max_leaf = 0x16; max_extended = 0x80000008;
    intel = msr = tsc = invariant = perf = true;
    migrate = sleep_fail = false;
    hz = 2800000000ULL; fail_msr = phase = reads = sleeps = cpu_reads = 0;
    output_len = 0; output[0] = 0;
    memset(mock, 0, sizeof(mock));
    for (unsigned i = 0; i < 2; i++) {
        mock[i].cpu_valid = mock[i].perf_valid = mock[i].energy_valid = true;
        mock[i].tsc = 10000000000ULL + hz * i;
        mock[i].apic_id = 2;
        mock[i].aperf = 2000000000ULL + (hz / 4 * 3 / 2) * i;
        mock[i].mperf = 1000000000ULL + hz / 4 * i;
        mock[i].units = 14U << 8;
        mock[i].energy = 1000 + 163840 * i;
        for (unsigned j = 0; j < RESIDENCY_COUNT; j++) {
            mock[i].state_valid[j] = true;
            mock[i].state[j] = 500 + (hz / 10 * i);
        }
    }
}

int main(void)
{
    fixture();
    sample_caps_t caps;
    sample_counters_t a, b;
    sample_result_t r;
    sample_caps(&caps); CHECK(caps.supported && caps.aperf && caps.signature == signature);
    sample_read(&caps, &a); CHECK(reads == 14 && a.cpu_valid && a.energy_valid && a.perf_valid);
    phase = 1; sample_read(&caps, &b);
    sample_calculate(&caps, &a, &b, &r);
    CHECK(r.valid && r.elapsed_us == 1000000);
    CHECK(r.perf_valid && r.c0_bp == 2500 && r.mean_mhz == 1050);
    CHECK(r.active_valid && r.active_mhz == 4200);
    CHECK(r.energy_valid && r.package_mw == 10000);
    for (unsigned i = 0; i < RESIDENCY_COUNT; i++) CHECK(r.state_valid[i] && r.state_bp[i] == 1000);
    a.energy = 0xfffffff0U; b.energy = (uint32_t)(a.energy + 163840);
    sample_calculate(&caps, &a, &b, &r); CHECK(r.energy_valid && r.package_mw == 10000);
    b.units++; sample_calculate(&caps, &a, &b, &r); CHECK(!r.energy_valid); b.units--;
    a.energy_valid = false; sample_calculate(&caps, &a, &b, &r); CHECK(!r.energy_valid); a.energy_valid = true;
    b.energy_valid = false; sample_calculate(&caps, &a, &b, &r); CHECK(!r.energy_valid); b.energy_valid = true;
    a.energy = 0; b.energy = 2; a.units = b.units = 0;
    sample_calculate(&caps, &a, &b, &r); CHECK(r.energy_valid && r.package_mw == 2000);
    b.energy = 0x80000000U; a.units = b.units = 31U << 8;
    sample_calculate(&caps, &a, &b, &r); CHECK(r.energy_valid && r.package_mw == 1000);
    b.aperf = a.aperf; b.mperf = a.mperf;
    sample_calculate(&caps, &a, &b, &r); CHECK(r.perf_valid && !r.active_valid && !r.c0_bp && !r.mean_mhz);
    b.aperf++; sample_calculate(&caps, &a, &b, &r); CHECK(!r.perf_valid && !r.active_valid);
    b = mock[1]; b.mperf = a.mperf - 1;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.perf_valid);
    b = mock[1]; b.aperf = a.aperf - 1;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.perf_valid);
    b = mock[1]; b.mperf = a.mperf + hz + 1;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.perf_valid);
    b = mock[1]; b.aperf = ~0ULL;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.perf_valid && !r.active_valid);
    b = mock[1]; b.state[0] = a.state[0] - 1;
    b.state[1] = a.state[1] + hz + 1; b.state_valid[2] = false; a.state_valid[3] = false;
    sample_calculate(&caps, &a, &b, &r);
    CHECK(!r.state_valid[0] && !r.state_valid[1] && !r.state_valid[2] && !r.state_valid[3] && r.state_valid[4]);
    a = mock[0]; b = mock[1]; b.apic_id++;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b = mock[1]; b.cpu_valid = false;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b = mock[1]; b.tsc = a.tsc;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b.tsc = a.tsc - 1; sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b.tsc = a.tsc + hz / 4; sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b.tsc = a.tsc + hz * 6; sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b.tsc = ~0ULL; sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b = mock[1]; b.span = hz / 1000 + 1;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    b = mock[1]; a.span = hz / 1000 + 1;
    sample_calculate(&caps, &a, &b, &r); CHECK(!r.valid);
    uint64_t out;
    CHECK(!sample_scale(1, 1, 0, &out));
    CHECK(!sample_scale(~0ULL, 2, 1, &out));
    CHECK(sample_scale(0, 1000, 1, &out) && out == 0);
    uint32_t good[] = {0x406e3, 0x506e3, 0x806e9, 0x906ec};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(sample_model_supported(good[i]));
    CHECK(!sample_model_supported(0x50654));
    CHECK(!sample_model_supported(0x90672));
    CHECK(!sample_model_supported(0x90fea));

    fixture(); intel = false; sample_caps(&caps); sample_read(&caps, &a);
    CHECK(!caps.supported && !reads);
    fixture(); signature = 0xb0671; sample_caps(&caps); sample_read(&caps, &a);
    CHECK(!caps.supported && !reads);
    fixture(); max_leaf = 1; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); max_extended = 0x80000001; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); msr = false; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); tsc = false; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); invariant = false; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); hz = 0; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); hz = ~0ULL; sample_caps(&caps); CHECK(!caps.supported);
    fixture(); perf = false; sample_caps(&caps); sample_read(&caps, &a);
    CHECK(caps.supported && !caps.aperf && !a.perf_valid && reads == 12);
    fixture(); fail_msr = IA32_MPERF; sample_caps(&caps); sample_read(&caps, &a);
    CHECK(!a.perf_valid && a.energy_valid);
    fail_msr = IA32_APERF; sample_read(&caps, &a); CHECK(!a.perf_valid);
    fail_msr = MSR_RAPL_POWER_UNIT; reads = 0; sample_read(&caps, &a);
    CHECK(!a.energy_valid && reads == 13);
    fail_msr = MSR_PKG_ENERGY_STATUS; sample_read(&caps, &a); CHECK(!a.energy_valid);
    fail_msr = residency[0].msr; sample_read(&caps, &a); CHECK(!a.state_valid[0] && a.state_valid[1]);
    fail_msr = IA32_PERF_CTL; sample_read(&caps, &a); CHECK(!a.perf_ctl_valid && a.power_ctl_valid);
    fail_msr = MSR_PKG_CST_CONFIG; sample_read(&caps, &a); CHECK(!a.cstate_config_valid);
    fail_msr = MSR_POWER_CTL; sample_read(&caps, &a); CHECK(!a.power_ctl_valid);
    fixture(); sample_caps(&caps); migrate = true; sample_read(&caps, &a); CHECK(!a.cpu_valid);
    fixture(); thermal_sample_dump();
    CHECK(sleeps == 1 && reads == 28);
    CHECK(strstr(output, "CPU C0 (MPERF/TSC): 25.00%; active MHz: 4200; wall-average MHz: 1050"));
    CHECK(strstr(output, "Package RAPL average: 10000 mW"));
    CHECK(strstr(output, "Core C6: 10.00%") && !strstr(output, "0x0x"));
    fixture(); sleep_fail = true; thermal_sample_dump();
    CHECK(sleeps == 1 && reads == 14 && strstr(output, "scheduler could not sleep"));
    fixture(); intel = false; thermal_sample_dump();
    CHECK(!sleeps && !reads && strstr(output, "Sampling unavailable:"));
    fixture(); mock[1].apic_id++; thermal_sample_dump();
    CHECK(strstr(output, "Sampling invalid:") && !strstr(output, "RAPL average:"));
    printf("Thermal sample: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
