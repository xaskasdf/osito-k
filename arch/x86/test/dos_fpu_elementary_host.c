#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mpfr.h>
#include "../dos/dos_fpu_softfloat.h"

struct extended { uint64_t sig; uint16_t exp; };
static const struct extended edges[] = {
    {0,0}, {1,0}, {0x4000000000000000ull,0},
    {0x7fffffffffffffffull,0}, {0x8000000000000000ull,0},
    {0x8000000000000000ull,1}, {0xffffffffffffffffull,0},
    {0x8000000000000000ull,2}, {0x8000000000000000ull,0x1fff},
    {0x8000000000000000ull,0x3fb8}, {0x8000000000000000ull,0x3fb9},
    {0x8000000000000000ull,0x3fba}, {0x8000000000000000ull,0x3fd6},
    {0x8000000000000000ull,0x3fd7}, {0x8000000000000000ull,0x3fd8},
    {0x8000000000000000ull,0x3ffc}, {0x8000000000000000ull,0x3ffd},
    {0xffffffffffffffffull,0x3ffe}, {0x8000000000000000ull,0x3fff},
    {0x8000000000000001ull,0x3fff}, {0x8000000000000000ull,0x4000},
    {0x8000000000000000ull,0x401f}, {0x8000000000000000ull,0x6000},
    {0xffffffffffffffffull,0x7ffe}, {0x8000000000000000ull,0x7fff},
    {0xa000000000000123ull,0x7fff}, {0xc000000000000123ull,0x7fff},
    {0xd000000000000456ull,0x7fff}, {0x4000000000000000ull,0x3fff},
    {1,0x7fff}, {0,0x7fff}, {0,1}
};
static const unsigned mods[] = {0xf0,0xf1,0xf3,0xf9}, pcs[] = {0,2,3};
static const mpfr_rnd_t rounding[] = {MPFR_RNDN,MPFR_RNDD,MPFR_RNDU,MPFR_RNDZ};
static uint64_t checks, failures, cases, exact_values, approximate_values;
static double maximum_error[4];
static unsigned input_id, current_op, current_cw;
static struct extended current_a, current_b;
static dos_fpu_numeric_result_t current_result;
static unsigned current_flags;

static void check(bool condition, const char *what)
{
    checks++;
    if (condition) return;
    failures++;
    static unsigned reported = UINT32_MAX, reports;
    if (reported != input_id && reports++ < 80) {
        reported = input_id;
        printf("FAIL input=%u op=d9%02x CW=%04x a=%04x:%016" PRIx64
               " b=%04x:%016" PRIx64 " flags=%03x/%02x %s\n", input_id, current_op, current_cw,
               current_a.exp, current_a.sig, current_b.exp, current_b.sig,
               current_result.exceptions, current_flags, what);
    }
}

static void put(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) p[i] = value >> (8 * i);
}

static struct extended decode(const uint8_t *p)
{
    struct extended v = {0, p[8] | (unsigned)p[9] << 8};
    for (unsigned i = 0; i < 8; i++) v.sig |= (uint64_t)p[i] << (8 * i);
    return v;
}

static bool same(struct extended a, struct extended b) { return a.sig == b.sig && a.exp == b.exp; }
static unsigned exponent(struct extended a) { return a.exp & 0x7fff; }
static bool negative(struct extended a) { return (a.exp >> 15) != 0; }
static bool zero(struct extended a) { return !exponent(a) && !a.sig; }
static bool unsupported(struct extended a) { return exponent(a) && !(a.sig >> 63); }
static bool nan_value(struct extended a) { return exponent(a) == 0x7fff && (a.sig << 1); }
static bool infinite(struct extended a) { return exponent(a) == 0x7fff && a.sig == (1ull << 63); }
static bool denormal(struct extended a) { return !exponent(a) && a.sig; }

static void set_extended(mpfr_t out, struct extended v)
{
    if (infinite(v)) { mpfr_set_inf(out, negative(v) ? -1 : 1); return; }
    if (zero(v)) { mpfr_set_zero(out, negative(v) ? -1 : 1); return; }
    mpfr_set_uj(out, v.sig, MPFR_RNDN);
    mpfr_mul_2si(out, out, (long)(exponent(v) ? exponent(v) : 1) - 16446, MPFR_RNDN);
    if (negative(v)) mpfr_neg(out, out, MPFR_RNDN);
}

static struct extended encode(mpfr_srcptr x)
{
    struct extended out = {0, mpfr_signbit(x) ? 0x8000u : 0};
    if (mpfr_inf_p(x)) { out.exp |= 0x7fff; out.sig = 1ull << 63; return out; }
    if (mpfr_zero_p(x)) return out;
    mpfr_t scaled;
    mpfr_init2(scaled, 64);
    long exp = mpfr_get_exp(x);
    mpfr_abs(scaled, x, MPFR_RNDN);
    mpfr_mul_2si(scaled, scaled, exp < -16381 ? 16445 : 64 - exp, MPFR_RNDN);
    out.sig = mpfr_get_uj(scaled, MPFR_RNDN);
    out.exp |= exp < -16381 ? 0 : exp + 16382;
    mpfr_clear(scaled);
    return out;
}

/* Class handling is independent of the imported math. Finite-domain limits
 * come from SDM Vol.2 F2XM1/FYL2XP1, not extrapolation of their formulae. */
static bool reference(mpfr_t out, struct extended a, struct extended b, unsigned *flags,
                      struct extended *special, bool *exact)
{
    bool unary = current_op == 0xf0;
    if (unary) b = (struct extended){0,0};
    *flags = 0; *exact = true;
    *special = (struct extended){0xc000000000000000ull,0xffff};
    if (unsupported(a) || unsupported(b)) { *flags = 1; return true; }
    bool na = nan_value(a), nb = nan_value(b);
    if (na || nb) {
        bool sa = na && !(a.sig & (1ull << 62)), sb = nb && !(b.sig & (1ull << 62));
        *flags = sa || sb ? 1 : 0;
        if (!na) *special = b;
        else if (!nb) *special = a;
        else if (sa != sb) *special = sa ? b : a;
        else *special = a.sig > b.sig || (a.sig == b.sig && a.exp < b.exp) ? a : b;
        special->sig |= 1ull << 62;
        return true;
    }
    mpfr_t x, y, limit;
    mpfr_inits2(mpfr_get_prec(out), x, y, limit, (mpfr_ptr)0);
    set_extended(x, a); set_extended(y, b);
    bool defined = true;
    *flags = denormal(a) || denormal(b) ? 2 : 0;
    if (unary) {
        if (!infinite(a) && mpfr_cmpabs_ui(x, 1) > 0) defined = false;
        else {
            mpfr_exp2m1(out, x, MPFR_RNDN);
            if (!zero(a) && !infinite(a)) *flags |= 32;
        }
    } else if (current_op == 0xf1) {
        if ((negative(a) && !zero(a)) || (zero(a) && zero(b)) ||
            (infinite(a) && zero(b)) || (mpfr_cmp_ui(x, 1) == 0 && infinite(b))) {
            *flags = 1;
            mpfr_set_nan(out);
        } else if (zero(a)) {
            *flags = infinite(b) ? 0 : 4;
            mpfr_set_inf(out, negative(b) ? 1 : -1);
        } else if (mpfr_cmp_ui(x, 1) == 0) mpfr_set_zero(out, negative(b) ? -1 : 1);
        else {
            mpfr_log2(out, x, MPFR_RNDN);
            mpfr_mul(out, out, y, MPFR_RNDN);
            if (!infinite(a) && !infinite(b) && !zero(b)) *flags |= 32;
        }
    } else if (current_op == 0xf9) {
        mpfr_sqrt_ui(limit, 2, MPFR_RNDN);
        mpfr_div_2ui(limit, limit, 1, MPFR_RNDN);
        mpfr_ui_sub(limit, 1, limit, MPFR_RNDN);
        if (mpfr_cmpabs(x, limit) > 0) defined = false;
        else if (zero(a) && infinite(b)) { *flags = 1; mpfr_set_nan(out); }
        else {
            mpfr_log2p1(out, x, MPFR_RNDN);
            mpfr_mul(out, out, y, MPFR_RNDN);
            if (!zero(a) && !zero(b) && !infinite(b)) *flags |= 32;
        }
    } else {
        mpfr_atan2(out, y, x, MPFR_RNDN);
        if (!mpfr_zero_p(out)) *flags |= 32;
    }
    if (defined) {
        if (mpfr_nan_p(out)) *flags = 1;
        else if (mpfr_zero_p(out) || mpfr_inf_p(out)) *special = encode(out);
        else *exact = false;
    }
    mpfr_clears(x, y, limit, (mpfr_ptr)0);
    return defined;
}

struct rounded_reference { struct extended value; unsigned flags, direction; int bias; bool clipped; };

/* Transcendental execution raises P even for exact mathematical powers.
 * Its tiny results therefore raise U, including masked subnormals. */
static struct rounded_reference round_reference(mpfr_srcptr x, unsigned rc, unsigned mask)
{
    mpfr_t rounded, work;
    mpfr_init2(rounded, 64);
    mpfr_init2(work, mpfr_get_prec(x));
    mpfr_set(work, x, MPFR_RNDN);
    mpfr_set(rounded, x, rounding[rc]);
    long exp = mpfr_zero_p(rounded) ? 0 : mpfr_get_exp(rounded);
    bool tiny = exp < -16381, overflow = exp > 16384;
    struct rounded_reference r = {0};
    if (overflow) {
        r.flags = 8;
        if (!(mask & 8)) r.bias = -24576;
        else {
            r.clipped = true;
            bool inf = rc == 0 || rc == (mpfr_signbit(x) ? 1u : 2u);
            r.value = (struct extended){inf ? 1ull << 63 : UINT64_MAX,
                (mpfr_signbit(x) ? 0x8000u : 0) | (inf ? 0x7fffu : 0x7ffeu)};
        }
    } else if (tiny && !(mask & 16)) { r.flags = 16; r.bias = 24576; }
    else if (mpfr_get_exp(x) < -16381) {
        mpfr_mul_2si(work, work, 16445, MPFR_RNDN);
        mpfr_rint(work, work, rounding[rc]);
        mpfr_mul_2si(work, work, -16445, MPFR_RNDN);
        mpfr_set(rounded, work, MPFR_RNDN);
        if (tiny) r.flags = 16;
    }
    if (r.bias) {
        mpfr_mul_2si(work, x, r.bias, MPFR_RNDN);
        mpfr_set(rounded, work, rounding[rc]);
    }
    if (!r.clipped) {
        r.value = encode(rounded);
        mpfr_mul_2si(work, x, r.bias, MPFR_RNDN);
        if (mpfr_cmpabs(rounded, work) > 0) r.direction = 0x200;
    } else if (exponent(r.value) == 0x7fff) r.direction = 0x200;
    mpfr_clears(rounded, work, (mpfr_ptr)0);
    return r;
}

static bool bounded(struct extended value, mpfr_srcptr ref, unsigned rc, int bias)
{
    if (exponent(value) == 0x7fff || unsupported(value)) return false;
    mpfr_t error, actual, target;
    mpfr_inits2(mpfr_get_prec(ref), error, actual, target, (mpfr_ptr)0);
    set_extended(actual, value);
    mpfr_mul_2si(target, ref, bias, MPFR_RNDN);
    mpfr_sub(error, actual, target, MPFR_RNDU);
    mpfr_abs(error, error, MPFR_RNDU);
    long quantum = mpfr_get_exp(target) - 64;
    if (quantum < -16445) quantum = -16445;
    mpfr_mul_2si(error, error, -quantum, MPFR_RNDU);
    double ulps = mpfr_get_d(error, MPFR_RNDU);
    if (ulps > maximum_error[rc]) maximum_error[rc] = ulps;
    bool ok = mpfr_cmp_d(error, rc ? 1.5 : 1.0) < 0;
    mpfr_clears(error, actual, target, (mpfr_ptr)0);
    return ok;
}

static void test_pair(unsigned op, struct extended a, struct extended b, bool exhaustive)
{
    current_op = op; current_a = a; current_b = b;
    mpfr_prec_t precision = 320;
    if (op == 0xf3 && !negative(a) && !zero(a) && !zero(b) &&
        !unsupported(a) && !unsupported(b) && exponent(a) < 0x7fff && exponent(b) < 0x7fff) {
        long delta = (long)exponent(a) - exponent(b) + 64;
        /* Retain atan(t)'s cubic correction even at the normal/subnormal
         * boundary, where 320 bits would incorrectly make it exactly t. */
        if (2 * delta + 256 > precision) precision = 2 * delta + 256;
    }
    mpfr_t ref;
    mpfr_init2(ref, precision);
    unsigned flags;
    struct extended special;
    bool exact;
    if (!reference(ref, a, b, &flags, &special, &exact)) { mpfr_clear(ref); return; }
    bool power_log = op == 0xf1 && a.sig && !(a.sig & (a.sig - 1));
    bool tiny_atan = false;
    if (op == 0xf3 && !negative(a) && !exact && !infinite(a) && !infinite(b) && !zero(a)) {
        mpfr_t x, y, ratio;
        mpfr_inits2(precision, x, y, ratio, (mpfr_ptr)0);
        set_extended(x, a); set_extended(y, b);
        long scale = mpfr_get_exp(y) - mpfr_get_exp(x);
        bool exact_ratio = mpfr_div(ratio, y, x, MPFR_RNDN) == 0;
        tiny_atan = scale <= -40 && (exact_ratio || scale <= -70);
        mpfr_clears(x, y, ratio, (mpfr_ptr)0);
    }
    for (unsigned rc = 0; rc < 4; rc++) for (unsigned mask = 0; mask < 64; mask++) {
        if (!exhaustive && mask != 63 && mask != 31 && mask != 39 && mask != 47 && mask != 55) continue;
        unsigned expected_flags = flags;
        struct rounded_reference expected = {0};
        if (!exact) { expected = round_reference(ref, rc, mask); expected_flags |= expected.flags; }
        if ((expected_flags & 2) && !(mask & 2)) expected_flags = 2;
        dos_fpu_numeric_result_t baseline = {0};
        for (unsigned pc = 0; pc < 3; pc++) {
            current_cw = 0x40u | mask | (pcs[pc] << 8) | (rc << 10);
            uint8_t image[160] = {0}, operand[16] = {0}, before[160];
            put(image, current_cw, 2);
            put(image + 32, a.sig, 8); put(image + 40, a.exp, 2);
            put(image + 48, b.sig, 8); put(image + 56, b.exp, 2);
            for (unsigned i = 2; i < 8; i++) {
                put(image + 32 + 16 * i, 0xc000000000000123ull, 8);
                put(image + 40 + 16 * i, 0x7fff, 2);
            }
            memcpy(before, image, sizeof(image));
            dos_fpu_numeric_result_t got;
            bool accepted = dos_fpu_soft_evaluate(image, operand, 0xd9, op, &got);
            current_result = got; current_flags = expected_flags;
            check(accepted, "accepted");
            cases++;
            check(!memcmp(image, before, sizeof(image)), "immutable input");
            check(got.write && !got.push && got.pops == (op != 0xf0) &&
                got.destination == (op != 0xf0) && got.condition_clear == 0x200 &&
                !got.conditions, "destination/pop/conditions");
            if (pc) check(got.exceptions == baseline.exceptions &&
                !memcmp(got.value, baseline.value, 10), "precision control ignored");
            else baseline = got;
            check((got.exceptions & 0x3f) == expected_flags, "exception priority/range");
            if (exact) {
                check(same(decode(got.value), special), "exact class/sign/NaN payload");
                check(!(got.exceptions & 0x200), "exact special C1 clear");
                exact_values++;
            } else if (!(expected_flags & ~mask & 7)) {
                if (tiny_atan || power_log) {
                    check(same(decode(got.value), expected.value), "exact tiny atan/power log rounding");
                    check((got.exceptions & 0x200) == expected.direction, "exact final C1");
                    exact_values++;
                } else {
                    if (expected.clipped) check(same(decode(got.value), expected.value), "masked overflow");
                    else check(bounded(decode(got.value), ref, rc, expected.bias), "approximation bound");
                    approximate_values++;
                }
                check(negative(decode(got.value)) == (mpfr_signbit(ref) != 0), "result sign including zero");
            }
        }
    }
    mpfr_clear(ref);
    input_id++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ELEMENTARY MPFR version=%s seed=9e3779b97f4a7c15\n", mpfr_get_version());
    for (unsigned oi = 0; oi < 4; oi++) {
        for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
            for (unsigned j = 0; j < (oi ? sizeof(edges) / sizeof(edges[0]) : 1); j++)
                for (unsigned signs = 0; signs < 4; signs++) {
                    struct extended a = edges[i], b = oi ? edges[j] :
                        (struct extended){signs & 1 ? 0xa000000000000123ull : 0xc000000000000123ull,0x7fff};
                    a.exp ^= (signs & 1) << 15; b.exp ^= (signs & 2) << 14;
                    test_pair(mods[oi], a, b, true);
                }
        printf("ELEMENTARY edge op=d9%02x inputs=%u failures=%" PRIu64 "\n", mods[oi], input_id, failures);
    }
    uint64_t state = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < 1024; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        struct extended a = {state | (1ull << 63), 1 + state % 0x7ffe};
        struct extended b = {(state * 0xd1342543de82ef95ull) | (1ull << 63),
            (1 + (state >> 16) % 0x7ffe) | ((i & 1) << 15)};
        test_pair(0xf1, a, b, false);
        a.exp |= (i & 2) << 14;
        test_pair(0xf3, a, b, false);
        a.exp = (1 + exponent(a) % 0x3ffd) | (a.exp & 0x8000);
        test_pair(0xf0, a, b, false);
        a.exp = (1 + exponent(a) % 0x3ffc) | (a.exp & 0x8000);
        test_pair(0xf9, a, b, false);
    }
    mpfr_t grid;
    mpfr_init2(grid, 64);
    for (unsigned i = 1; i <= 512; i++) for (unsigned n = 0; n < 3; n++) {
        mpfr_set_ui(grid, i, MPFR_RNDN);
        mpfr_div_2ui(grid, grid, 8, MPFR_RNDN);
        if (!n) mpfr_nextbelow(grid);
        if (n == 2) mpfr_nextabove(grid);
        for (unsigned signs = 0; signs < 4; signs++) {
            struct extended a = encode(grid), b = {1ull << 63, 0x3fff | ((signs & 2) << 14)};
            a.exp |= (signs & 1) << 15;
            for (unsigned oi = 0; oi < 4; oi++) test_pair(mods[oi], a, b, false);
        }
    }
    /* The domain endpoints are irrational; check adjacent extended values
     * rather than treating a decimal approximation as the true boundary. */
    mpfr_t boundary;
    mpfr_init2(boundary, 320);
    mpfr_sqrt_ui(boundary, 2, MPFR_RNDN);
    mpfr_div_2ui(boundary, boundary, 1, MPFR_RNDN);
    mpfr_ui_sub(boundary, 1, boundary, MPFR_RNDN);
    for (unsigned n = 0; n < 3; n++) {
        mpfr_set(grid, boundary, MPFR_RNDN);
        if (!n) mpfr_nextbelow(grid);
        if (n == 2) mpfr_nextabove(grid);
        for (unsigned signs = 0; signs < 4; signs++) {
            struct extended a = encode(grid), b = {UINT64_MAX,0x7ffe | ((signs & 2) << 14)};
            a.exp |= (signs & 1) << 15;
            test_pair(0xf9, a, b, true);
        }
    }
    mpfr_clear(boundary);
    mpfr_clear(grid);
    mpfr_free_cache();
    printf("ELEMENTARY MPFR inputs=%u cases=%" PRIu64 " checks=%" PRIu64
        " exact=%" PRIu64 " approximate=%" PRIu64 " failures=%" PRIu64 "\n",
        input_id, cases, checks, exact_values, approximate_values, failures);
    printf("ELEMENTARY MPFR max-ulp nearest=%.9f down=%.9f up=%.9f zero=%.9f\n",
        maximum_error[0],maximum_error[1],maximum_error[2],maximum_error[3]);
    return failures != 0;
}
