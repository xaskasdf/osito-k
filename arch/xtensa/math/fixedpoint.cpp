/*
 * OsitoK - Fixed-point 16.16 math library (non-inline functions)
 *
 * Contains: sine table, fix_div, fix_sqrt, fix_print, fix_test
 */

#include "math/fixedpoint.h"
#include "drivers/uart.h"

extern "C" {

/* ====== Sine table: 256 entries, full circle ====== */

/*
 * sin_table[i] = fix16(sin(i * 2π / 256))
 * Full circle stored (no quarter-wave tricks) for zero-branch lookup.
 * 256 × 4 = 1024 bytes DRAM.
 */
static const fix16_t sin_table[256] = {
         0,   1608,   3216,   4821,   6424,   8022,   9616,  11204,
     12785,  14359,  15924,  17479,  19024,  20557,  22078,  23586,
     25080,  26558,  28020,  29466,  30893,  32303,  33692,  35062,
     36410,  37736,  39040,  40320,  41576,  42806,  44011,  45190,
     46341,  47464,  48559,  49624,  50660,  51665,  52639,  53581,
     54491,  55368,  56212,  57022,  57798,  58538,  59244,  59914,
     60547,  61145,  61705,  62228,  62714,  63162,  63572,  63944,
     64277,  64571,  64827,  65043,  65220,  65358,  65457,  65516,
     65536,  65516,  65457,  65358,  65220,  65043,  64827,  64571,
     64277,  63944,  63572,  63162,  62714,  62228,  61705,  61145,
     60547,  59914,  59244,  58538,  57798,  57022,  56212,  55368,
     54491,  53581,  52639,  51665,  50660,  49624,  48559,  47464,
     46341,  45190,  44011,  42806,  41576,  40320,  39040,  37736,
     36410,  35062,  33692,  32303,  30893,  29466,  28020,  26558,
     25080,  23586,  22078,  20557,  19024,  17479,  15924,  14359,
     12785,  11204,   9616,   8022,   6424,   4821,   3216,   1608,
         0,  -1608,  -3216,  -4821,  -6424,  -8022,  -9616, -11204,
    -12785, -14359, -15924, -17479, -19024, -20557, -22078, -23586,
    -25080, -26558, -28020, -29466, -30893, -32303, -33692, -35062,
    -36410, -37736, -39040, -40320, -41576, -42806, -44011, -45190,
    -46341, -47464, -48559, -49624, -50660, -51665, -52639, -53581,
    -54491, -55368, -56212, -57022, -57798, -58538, -59244, -59914,
    -60547, -61145, -61705, -62228, -62714, -63162, -63572, -63944,
    -64277, -64571, -64827, -65043, -65220, -65358, -65457, -65516,
    -65536, -65516, -65457, -65358, -65220, -65043, -64827, -64571,
    -64277, -63944, -63572, -63162, -62714, -62228, -61705, -61145,
    -60547, -59914, -59244, -58538, -57798, -57022, -56212, -55368,
    -54491, -53581, -52639, -51665, -50660, -49624, -48559, -47464,
    -46341, -45190, -44011, -42806, -41576, -40320, -39040, -37736,
    -36410, -35062, -33692, -32303, -30893, -29466, -28020, -26558,
    -25080, -23586, -22078, -20557, -19024, -17479, -15924, -14359,
    -12785, -11204,  -9616,  -8022,  -6424,  -4821,  -3216,  -1608,
};

/* ====== Trigonometry ====== */

fix16_t fix_sin(angle_t angle)
{
    return sin_table[angle];
}

fix16_t fix_cos(angle_t angle)
{
    return sin_table[(uint8_t)(angle + 64)];
}

/* ====== Division ====== */

/*
 * fix_div: (a << 16) / b using 64-bit arithmetic.
 * ~200 cycles (libgcc __divdi3).
 */
fix16_t fix_div(fix16_t a, fix16_t b)
{
    if (b == 0) return (a >= 0) ? FIX16_MAX : FIX16_MIN;

    int64_t num = (int64_t)a << 16;
    return (fix16_t)(num / (int64_t)b);
}

/* ====== Square root ====== */

/*
 * fix_sqrt: binary search on 32-bit result.
 * For input x in fix16, we want r such that r*r = x (both fix16).
 * Equivalent to: result = sqrt(x * 65536) as integer, then that IS the fix16 result.
 *
 * Uses bit-by-bit method: ~16 iterations, ~300 cycles.
 */
fix16_t fix_sqrt(fix16_t x)
{
    if (x <= 0) return 0;

    /* We need sqrt of x as a 16.16 number.
     * r² = x in fix16 means (r_raw)² / 65536 = x_raw / 65536
     * So r_raw = sqrt(x_raw * 65536) = sqrt(x_raw) * 256
     *
     * Work with 64-bit: val = (uint64_t)x << 16, find integer sqrt of val.
     */
    uint64_t val = (uint64_t)(uint32_t)x << 16;
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 46;  /* Start high enough for 48-bit values */

    /* Find starting bit position */
    while (bit > val) bit >>= 2;

    while (bit != 0) {
        uint64_t trial = root + bit;
        if (val >= trial) {
            val -= trial;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }

    return (fix16_t)root;
}

/* ====== Print ====== */

/*
 * fix_print: output fix16 as decimal string via UART.
 * Format: [-]integer.fraction (3 decimal places)
 * Example: -3.141
 */
void fix_print(fix16_t x)
{
    if (x < 0) {
        uart_putc('-');
        x = -x;
    }

    /* Integer part */
    uint32_t ipart = (uint32_t)x >> 16;
    uart_put_dec(ipart);

    uart_putc('.');

    /* Fractional part: multiply fraction by 1000, round */
    uint32_t frac = (uint32_t)(x & 0xFFFF);
    /* frac * 1000 / 65536 — use 32-bit: frac max is 65535, *1000 = 65535000 fits uint32 */
    uint32_t dec = (frac * 1000 + 32768) / 65536;

    /* Print with leading zeros (3 digits) */
    if (dec < 100) uart_putc('0');
    if (dec < 10)  uart_putc('0');
    uart_put_dec(dec);
}

/* ====== Validation test ====== */

static void test_case(const char *label, fix16_t got, fix16_t expected, fix16_t tolerance)
{
    uart_puts(label);
    uart_puts(" = ");
    fix_print(got);

    fix16_t diff = fix_abs(got - expected);
    if (diff <= tolerance) {
        uart_puts("  OK\n");
    } else {
        uart_puts("  FAIL (expected ");
        fix_print(expected);
        uart_puts(")\n");
    }
}

void fix_test(void)
{
    uart_puts("=== Fixed-point 16.16 test ===\n");

    /* Multiplication */
    test_case("1.5 * 2.0",
        fix_mul(FIX16_C(1.5), FIX16(2)),
        FIX16(3), 1);

    test_case("-3 * 2.5",
        fix_mul(FIX16(-3), FIX16_C(2.5)),
        FIX16_C(-7.5), 1);

    /* Division */
    test_case("10 / 3",
        fix_div(FIX16(10), FIX16(3)),
        FIX16_C(3.333), FIX16_C(0.002));

    test_case("1 / 4",
        fix_div(FIX16(1), FIX16(4)),
        FIX16_C(0.25), 1);

    /* Trigonometry */
    test_case("sin(0)",
        fix_sin(0),
        0, 1);

    test_case("sin(64) [90deg]",
        fix_sin(64),
        FIX16_ONE, 1);

    test_case("sin(128) [180deg]",
        fix_sin(128),
        0, 1);

    test_case("cos(0)",
        fix_cos(0),
        FIX16_ONE, 1);

    test_case("cos(128) [180deg]",
        fix_cos(128),
        FIX16_NEG_ONE, 1);

    /* Square root */
    test_case("sqrt(4)",
        fix_sqrt(FIX16(4)),
        FIX16(2), 2);

    test_case("sqrt(2)",
        fix_sqrt(FIX16(2)),
        FIX16_C(1.414), FIX16_C(0.002));

    test_case("sqrt(0.25)",
        fix_sqrt(FIX16_C(0.25)),
        FIX16_C(0.5), FIX16_C(0.002));

    /* Distance approximation */
    fix16_t d = fix_dist_approx(FIX16(3), FIX16(4));
    test_case("dist(3,4)",
        d,
        FIX16(5), FIX16(1));  /* ~7% error = ±1 on magnitude 5 */

    /* Lerp */
    test_case("lerp(0,10,0.5)",
        fix_lerp(FIX16(0), FIX16(10), FIX16_C(0.5)),
        FIX16(5), 1);

    uart_puts("=== done ===\n");
}

} /* extern "C" */
