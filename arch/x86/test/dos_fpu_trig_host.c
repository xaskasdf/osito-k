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
    {0x8000000000000000ull,0x3fb9}, {0x8000000000000000ull,0x3fba},
    {0xffffffffffffffffull,0x3fba}, {0x8000000000000000ull,0x3fbb},
    {0x8000000000000000ull,0x3fbe}, {0x8000000000000000ull,0x3fc0},
    {0x8000000000000000ull,0x3ffd}, {0x8000000000000000ull,0x3ffe},
    {0xc90fdaa22168c235ull,0x3ffe}, {0x8000000000000000ull,0x3fff},
    {0xc90fdaa22168c234ull,0x3fff}, {0xc90fdaa22168c235ull,0x3fff},
    {0xc90fdaa22168c236ull,0x3fff}, {0x8000000000000000ull,0x4000},
    {0xc90fdaa22168c235ull,0x4000}, {0x8000000000000000ull,0x401f},
    {0xffffffffffffffffull,0x403d}, {0x8000000000000000ull,0x403e},
    {0x8000000000000000ull,0x7ffe}, {0x8000000000000000ull,0x7fff},
    {0xa000000000000123ull,0x7fff}, {0xc000000000000123ull,0x7fff},
    {0x4000000000000000ull,0x3fff}, {1,0x7fff}, {0,0x7fff}, {0,1}
};
static const unsigned mods[] = {0xf2,0xfb,0xfe,0xff}, pcs[] = {0,2,3};
static const mpfr_rnd_t rounding[] = {MPFR_RNDN,MPFR_RNDD,MPFR_RNDU,MPFR_RNDZ};
static uint64_t checks, failures, cases, exact_values, approximate_values;
static double maximum_error[4];
static unsigned input_id, current_op, current_cw;

static void check(bool condition, const char *what)
{
    checks++;
    if (condition) return;
    if (++failures <= 40)
        printf("FAIL input=%u op=d9%02x CW=%04x %s\n", input_id, current_op, current_cw, what);
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

static void set_extended(mpfr_t out, struct extended v)
{
    unsigned exp = v.exp & 0x7fffu;
    mpfr_set_uj(out, v.sig, MPFR_RNDN);
    mpfr_mul_2si(out, out, (long)(exp ? exp : 1) - 16383 - 63, MPFR_RNDN);
    if (v.exp & 0x8000u) mpfr_neg(out, out, MPFR_RNDN);
}

/* Encode an already rounded, finite extended-format result. */
static struct extended encode(mpfr_srcptr x)
{
    struct extended out = {0, mpfr_signbit(x) ? 0x8000u : 0};
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

struct reference { struct extended value; unsigned flags; };

/* Tininess is detected after unbounded 64-bit rounding. Underflow either
 * narrows to the subnormal quantum or applies the x87 +24576 exponent bias. */
static struct reference exact_reference(mpfr_srcptr x, unsigned rc, bool masked)
{
    mpfr_t rounded, exact;
    mpfr_init2(rounded, 64);
    mpfr_init2(exact, mpfr_get_prec(x));
    mpfr_set(exact, x, MPFR_RNDN);
    mpfr_set(rounded, x, rounding[rc]);
    bool tiny = !mpfr_zero_p(rounded) && mpfr_get_exp(rounded) < -16381;
    if (tiny && !masked) {
        mpfr_mul_2si(exact, exact, 24576, MPFR_RNDN);
        mpfr_set(rounded, exact, rounding[rc]);
    } else if (!mpfr_zero_p(x) && mpfr_get_exp(x) < -16381) {
        mpfr_mul_2si(exact, exact, 16445, MPFR_RNDN);
        mpfr_rint(exact, exact, rounding[rc]);
        mpfr_mul_2si(exact, exact, -16445, MPFR_RNDN);
        mpfr_set(rounded, exact, MPFR_RNDN);
        mpfr_set(exact, x, MPFR_RNDN);
    }
    unsigned flags = tiny ? 16 : 0;
    if (mpfr_cmpabs(rounded, exact) > 0) flags |= 0x200u;
    struct reference ref = {encode(rounded), flags};
    mpfr_clears(rounded, exact, (mpfr_ptr)0);
    return ref;
}

/* Independent fixed-pi reduction model. Do not compare large x87 arguments
 * against sin(x) with an infinitely accurate pi: that is a different contract.
 * The divisor is the SDM's 66-significant-bit pi/2, expressed exactly. */
static void references(mpfr_t sine, mpfr_t cosine, mpfr_t tangent, mpfr_srcptr x, bool reduce)
{
    if (!reduce) {
        mpfr_sin_cos(sine, cosine, x, MPFR_RNDN);
        mpfr_tan(tangent, x, MPFR_RNDN);
        return;
    }
    mpfr_t r, pi2, s, c;
    mpfr_inits2(mpfr_get_prec(x), r, pi2, s, c, (mpfr_ptr)0);
    mpfr_set_str(pi2, "0x1.921fb54442d184698p+0", 0, MPFR_RNDN);
    long quotient;
    mpfr_remquo(r, &quotient, x, pi2, MPFR_RNDN);
    mpfr_sin_cos(s, c, r, MPFR_RNDN);
    unsigned q = (unsigned)quotient & 3u;
    mpfr_set(sine, q & 1 ? c : s, MPFR_RNDN);
    mpfr_set(cosine, q & 1 ? s : c, MPFR_RNDN);
    if (q & 2) mpfr_neg(sine, sine, MPFR_RNDN);
    if (q == 1 || q == 2) mpfr_neg(cosine, cosine, MPFR_RNDN);
    mpfr_div(tangent, sine, cosine, MPFR_RNDN);
    mpfr_clears(r, pi2, s, c, (mpfr_ptr)0);
}

static bool bounded(const uint8_t *value, mpfr_srcptr ref, unsigned rc)
{
    struct extended v = decode(value);
    if ((v.exp & 0x7fffu) == 0x7fff || !v.sig || !(v.sig >> 63)) return false;
    mpfr_t error, actual;
    mpfr_inits2(mpfr_get_prec(ref), error, actual, (mpfr_ptr)0);
    set_extended(actual, v);
    mpfr_sub(error, actual, ref, MPFR_RNDU);
    mpfr_abs(error, error, MPFR_RNDU);
    mpfr_mul_2si(error, error, 64 - mpfr_get_exp(ref), MPFR_RNDU);
    double ulps = mpfr_get_d(error, MPFR_RNDU);
    if (ulps > maximum_error[rc]) maximum_error[rc] = ulps;
    /* SDM Vol.1 8.3.10: <1 ulp nearest, <1.5 ulp directed after reduction. */
    bool ok = mpfr_cmp_d(error, rc ? 1.5 : 1.0) < 0;
    mpfr_clears(error, actual, (mpfr_ptr)0);
    return ok;
}

static void test_value(struct extended a, bool exhaustive)
{
    unsigned exp = a.exp & 0x7fffu;
    bool unsupported = exp && !(a.sig >> 63);
    bool special = unsupported || exp == 0x7fff;
    bool nan = !unsupported && exp == 0x7fff && (a.sig << 1);
    bool invalid = special && (!nan || !(a.sig & (1ull << 62)));
    bool outside = !special && exp >= 0x403e;
    bool zero = !exp && !a.sig;
    bool denormal = !exp && a.sig;
    bool tiny = !special && !zero && exp < 0x3fbb;
    long unbiased = (long)(exp ? exp : 1) - 16383;
    mpfr_prec_t precision = tiny ? -2 * (unbiased - 63) + 256 : 320;
    mpfr_t x, sinx, cosx, tanx, one;
    mpfr_inits2(precision, x, sinx, cosx, tanx, one, (mpfr_ptr)0);
    mpfr_set_ui(one, 1, MPFR_RNDN);
    if (!special && !outside) {
        set_extended(x, a);
        /* Within |x| <= 1 the reference is the mathematical function. Beyond
         * that this is explicitly a fixed-pi model, not a libm accuracy claim. */
        references(sinx, cosx, tanx, x, mpfr_cmpabs(x, one) > 0);
    }
    for (unsigned oi = 0; oi < 4; oi++) for (unsigned rc = 0; rc < 4; rc++) {
        current_op = mods[oi];
        bool pair = oi < 2;
        mpfr_srcptr ref = oi == 0 ? tanx : oi == 3 ? cosx : sinx;
        struct reference normal[2] = {0}, pushed[2] = {0};
        if (!special && !outside && (tiny || zero)) {
            for (unsigned u = 0; u < 2; u++) {
                normal[u] = exact_reference(ref, rc, u);
                pushed[u] = exact_reference(oi == 0 ? one : cosx, rc, u);
            }
        }
        for (unsigned mask = 0; mask < 64; mask++) {
            if (!exhaustive && mask != 63 && mask != 31) continue;
            dos_fpu_numeric_result_t baseline = {0};
            for (unsigned pc = 0; pc < 3; pc++) {
                current_cw = 0x40u | mask | (pcs[pc] << 8) | (rc << 10);
                uint8_t image[160] = {0}, operand[16] = {0};
                put(image, current_cw, 2);
                put(image + 32, a.sig, 8); put(image + 40, a.exp, 2);
                /* Irrelevant registers must not affect unary classification. */
                for (unsigned i = 1; i < 8; i++) {
                    put(image + 32 + 16 * i, 0xc000000000000123ull, 8);
                    put(image + 40 + 16 * i, 0x7fff, 2);
                }
                uint8_t before[160]; memcpy(before, image, sizeof(before));
                dos_fpu_numeric_result_t got;
                check(dos_fpu_soft_evaluate(image, operand, 0xd9, current_op, &got), "accepted");
                cases++;
                check(!memcmp(image, before, sizeof(image)), "input immutable");
                check(got.write == !outside && got.push == (pair && !outside) &&
                      !got.pops && !got.destination && got.condition_clear == 0x600 &&
                      got.conditions == (outside ? 0x400 : 0), "result contract/C2");
                if (pc) check(got.exceptions == baseline.exceptions &&
                    !memcmp(got.value, baseline.value, 10) &&
                    !memcmp(got.pushed_value, baseline.pushed_value, 10), "PC must not affect trig");
                else baseline = got;
                unsigned flags = invalid ? 1u : denormal ? 2u : 0;
                if (!special && !outside && !zero) {
                    flags |= 32;
                    if (tiny) flags |= normal[(mask >> 4) & 1].flags;
                }
                bool inhibited = (flags & ~mask & 3u) != 0;
                if (denormal && !(mask & 2)) flags = 2;
                unsigned compared = tiny || special || outside || zero ? 0x23fu : 0x3fu;
                check((got.exceptions & compared) == flags, "exceptions/round direction");
                if (outside) {
                    check(same(decode(got.value), a), "range rejection preserves input");
                } else if (special) {
                    struct extended expected = nan ? a :
                        (struct extended){0xc000000000000000ull, 0xffff};
                    expected.sig |= 1ull << 62;
                    check(same(decode(got.value), expected), "NaN payload/indefinite");
                    if (pair) check(same(decode(got.pushed_value), expected), "NaN pair");
                } else if (!inhibited) {
                    if (tiny || zero) {
                        check(same(decode(got.value), normal[(mask >> 4) & 1].value), "exact tiny/zero");
                        exact_values++;
                        if (pair) {
                            check(same(decode(got.pushed_value), pushed[(mask >> 4) & 1].value), "exact pushed");
                            exact_values++;
                        }
                    } else {
                        check(bounded(got.value, ref, rc), "approximation bound");
                        approximate_values++;
                        if (pair) {
                            if (!oi) check(same(decode(got.pushed_value),
                                (struct extended){0x8000000000000000ull,0x3fff}), "tan pushes one");
                            else check(bounded(got.pushed_value, cosx, rc), "cosine bound");
                            approximate_values += oi == 1;
                        }
                    }
                }
            }
        }
    }
    mpfr_clears(x, sinx, cosx, tanx, one, (mpfr_ptr)0);
    input_id++;
}

int main(void)
{
    printf("TRIG MPFR version=%s seed=9e3779b97f4a7c15\n", mpfr_get_version());
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
        for (unsigned sign = 0; sign < 2; sign++) {
            struct extended a = edges[i]; a.exp ^= sign << 15;
            test_value(a, true);
        }
    uint64_t state = 0x9e3779b97f4a7c15ull;
    /* Deterministic samples across the polynomial/table/reduction boundaries. */
    for (int exp = -68; exp < 63; exp++) for (unsigned i = 0; i < 8; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        struct extended a = {state | (1ull << 63), (exp + 16383) | ((i & 1) << 15)};
        test_value(a, false);
    }
    mpfr_t grid;
    mpfr_init2(grid, 64);
    for (unsigned i = 1; i <= 512; i++) {
        mpfr_set_ui(grid, i, MPFR_RNDN);
        mpfr_div_2ui(grid, grid, 8, MPFR_RNDN);
        for (unsigned neighbor = 0; neighbor < 3; neighbor++) {
            mpfr_t value;
            mpfr_init2(value, 64);
            mpfr_set(value, grid, MPFR_RNDN);
            if (!neighbor) mpfr_nextbelow(value);
            if (neighbor == 2) mpfr_nextabove(value);
            struct extended a = encode(value);
            test_value(a, false);
            a.exp |= 0x8000u;
            test_value(a, false);
            mpfr_clear(value);
        }
    }
    mpfr_clear(grid);
    mpfr_free_cache();
    printf("TRIG MPFR inputs=%u cases=%" PRIu64 " checks=%" PRIu64
           " exact=%" PRIu64 " approximate=%" PRIu64 " failures=%" PRIu64 "\n",
           input_id, cases, checks, exact_values, approximate_values, failures);
    printf("TRIG MPFR max-ulp nearest=%.9f down=%.9f up=%.9f zero=%.9f\n",
           maximum_error[0],maximum_error[1],maximum_error[2],maximum_error[3]);
    return failures != 0;
}
