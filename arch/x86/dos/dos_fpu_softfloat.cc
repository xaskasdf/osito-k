#include "dos_fpu_softfloat.h"
#include "../lib/softfloat3e/include/softfloat.h"
#include "../lib/bochs-transcendental/fpu_trans.h"

static uint64_t read_bits(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static extFloat80_t read_extended(const uint8_t *p)
{
    return {read_bits(p, 8), (uint16_t)read_bits(p + 8, 2)};
}

static void write_extended(uint8_t *p, extFloat80_t value)
{
    for (unsigned i = 0; i < 8; i++) p[i] = value.signif >> (8 * i);
    p[8] = value.signExp;
    p[9] = value.signExp >> 8;
}

static bool memory_transfer(const uint8_t *image, const uint8_t *operand,
                            uint8_t opcode, uint8_t modrm, dos_fpu_numeric_result_t *result)
{
    if (modrm >= 0xc0 || !(opcode & 1u)) return false;
    unsigned group = (modrm >> 3) & 7u;
    if (opcode == 0xd9 && group != 0 && group != 2 && group != 3) return false;
    if (opcode == 0xdd && group > 3) return false;
    if (opcode == 0xdb && (group == 4 || group == 6)) return false;
    if (opcode < 0xd9 || opcode > 0xdf) return false;
    enum { REAL32, REAL64, REAL80, INTEGER, BCD };
    unsigned type = opcode == 0xd9 ? REAL32 : opcode == 0xdd && group != 1 ? REAL64 :
                    opcode == 0xdb && group >= 5 ? REAL80 :
                    opcode == 0xdf && (group == 4 || group == 6) ? BCD : INTEGER;
    unsigned bytes = type == REAL32 ? 4 : type == REAL64 ? 8 : type != INTEGER ? 10 :
                     opcode == 0xdb ? 4 : opcode == 0xdd || group >= 5 ? 8 : 2;
    bool load = group == 0 || group == 5 || (type == BCD && group == 4);
    *result = {};
    result->condition_clear = 0x0200u;
    uint16_t control = read_bits(image, 2);
    softfloat_status_t status = {};
    status.softfloat_roundingMode = group == 1 ? (unsigned)softfloat_round_minMag : (control >> 10) & 3u;
    status.softfloat_exceptionMasks = control & 0x3fu;
    status.extF80_roundingPrecision = 80;
    extFloat80_t value;
    if (load) {
        if (type == REAL32) value = f32_to_extF80(read_bits(operand, 4), &status);
        else if (type == REAL64) value = f64_to_extF80(read_bits(operand, 8), &status);
        else if (type == REAL80) value = read_extended(operand);
        else if (type == INTEGER) value = bytes == 8 ? i64_to_extF80((int64_t)read_bits(operand, 8)) :
            i32_to_extF80(bytes == 4 ? (int32_t)read_bits(operand, 4) : (int16_t)read_bits(operand, 2));
        else {
            uint64_t magnitude = 0;
            // FBLD assumes valid digits. Invalid nibbles do not raise #IA;
            // their weighted-digit result is an explicit implementation policy.
            for (unsigned i = 9; i-- > 0;)
                magnitude = magnitude * 100 + (operand[i] >> 4) * 10 + (operand[i] & 15u);
            value = i64_to_extF80((int64_t)magnitude);
            value.signExp |= (operand[9] & 0x80u) << 8;
        }
        result->push = true;
        write_extended(result->pushed_value, value);
    } else {
        value = read_extended(image + 32);
        result->memory_size = bytes;
        result->pops = group != 2;
        uint64_t bits = 0;
        if (type == REAL32) bits = extF80_to_f32(value, &status);
        else if (type == REAL64) bits = extF80_to_f64(value, &status);
        else if (type == REAL80) write_extended(result->value, value);
        else if (type == INTEGER) bits = bytes == 8 ? (uint64_t)extF80_to_i64(value, &status) :
            bytes == 4 ? (uint32_t)extF80_to_i32(value, &status) : (uint16_t)extF80_to_i16(value, &status);
        else {
            int64_t integer = extF80_to_i64(value, &status);
            uint64_t magnitude = integer < 0 ? 0 - (uint64_t)integer : (uint64_t)integer;
            if ((status.softfloat_exceptionFlags & softfloat_flag_invalid) ||
                magnitude > UINT64_C(999999999999999999)) {
                status.softfloat_exceptionFlags = softfloat_flag_invalid;
                result->value[7] = 0xc0; result->value[8] = result->value[9] = 0xff;
            } else {
                for (unsigned i = 0; i < 9; i++) {
                    unsigned pair = magnitude % 100;
                    result->value[i] = ((pair / 10) << 4) | (pair % 10);
                    magnitude /= 100;
                }
                result->value[9] = value.signExp >> 8 & 0x80u;
            }
        }
        if (type != REAL80 && type != BCD)
            for (unsigned i = 0; i < bytes; i++) result->value[i] = bits >> (8 * i);
    }
    unsigned raised = status.softfloat_exceptionFlags & 0x23fu;
    if (raised & 1u) raised = 1u;
    // Unlike register results, inhibited real stores report neither #P nor C1.
    else if (!load && (raised & ~control & 0x18u)) raised &= ~0x220u;
    result->exceptions = raised;
    return true;
}

bool dos_fpu_soft_evaluate(const uint8_t *image, const uint8_t *operand,
                           uint8_t opcode, uint8_t modrm,
                           dos_fpu_numeric_result_t *result)
{
    if (memory_transfer(image, operand, opcode, modrm, result)) return true;
    unsigned group = (modrm >> 3) & 7u, index = modrm & 7u;
    bool memory = modrm < 0xc0;
    bool arithmetic = memory ? !(opcode & 1u) && opcode >= 0xd8 && opcode <= 0xde :
                      opcode == 0xd8 || opcode == 0xdc || opcode == 0xde;
    arithmetic = arithmetic && group != 2 && group != 3;
    bool unary = opcode == 0xd9 && (modrm == 0xfa || modrm == 0xfc || modrm == 0xfd);
    bool trig = opcode == 0xd9 && (modrm == 0xf2 || modrm == 0xfb || modrm >= 0xfe);
    bool elementary = opcode == 0xd9 && (modrm == 0xf0 || modrm == 0xf1 ||
                                       modrm == 0xf3 || modrm == 0xf9);
    bool remainder = opcode == 0xd9 && (modrm == 0xf5 || modrm == 0xf8);
    bool extract = opcode == 0xd9 && modrm == 0xf4;
    bool integer_flags = !memory && (opcode == 0xdb || opcode == 0xdf) &&
                         modrm >= 0xe8 && modrm <= 0xf7;
    bool double_pop = !memory && ((opcode == 0xda && modrm == 0xe9) ||
                                 (opcode == 0xde && modrm == 0xd9));
    bool test_zero = opcode == 0xd9 && modrm == 0xe4;
    bool examine = opcode == 0xd9 && modrm == 0xe5;
    bool compare = integer_flags || double_pop || test_zero || (memory ?
        !(opcode & 1u) && opcode >= 0xd8 && opcode <= 0xde && (group == 2 || group == 3) :
        (opcode == 0xd8 && (group == 2 || group == 3)) ||
        (opcode == 0xdd && modrm >= 0xe0 && modrm < 0xf0));
    if (!arithmetic && !unary && !trig && !elementary && !remainder && !extract &&
        !compare && !examine) return false;

    *result = {};
    result->write = !compare && !examine;
    result->condition_clear = trig || remainder ? 0x0600u : 0x0200u;
    uint16_t control = read_bits(image, 2);
    softfloat_status_t status = {};
    status.softfloat_roundingMode = (control >> 10) & 3u;
    status.softfloat_exceptionMasks = control & 0x3fu;
    unsigned precision = (control >> 8) & 3u;
    status.extF80_roundingPrecision = precision == 0 ? 32 : precision == 2 ? 64 : 80;
    extFloat80_t a = read_extended(image + 32), b = {};
    if (examine) {
        unsigned top = (read_bits(image + 2, 2) >> 11) & 7u;
        unsigned classification;
        if (!(image[4] & (1u << top))) classification = 0x4100u;
        else if (extF80_isUnsupported(a)) classification = 0;
        else {
            static const uint16_t codes[] = {0x4000, 0x0100, 0x0100, 0x0500, 0x0500, 0x4400, 0x0400};
            classification = codes[extF80_class(a)];
        }
        result->condition_clear = 0x4700u;
        result->conditions = classification | ((a.signExp & 0x8000u) >> 6);
        return true;
    }
    if (memory) {
        if (opcode == 0xd8) b = f32_to_extF80(read_bits(operand, 4), &status);
        else if (opcode == 0xdc) b = f64_to_extF80(read_bits(operand, 8), &status);
        else if (opcode == 0xda) b = i32_to_extF80((int32_t)read_bits(operand, 4));
        else b = i32_to_extF80((int16_t)read_bits(operand, 2));
        // Widening quiets SNaN. Retain its class until the arithmetic NaN
        // selection, where an existing QNaN takes precedence over an SNaN.
        if (status.softfloat_exceptionFlags & softfloat_flag_invalid)
            b.signif &= ~UINT64_C(0x4000000000000000);
    } else if (!test_zero)
        b = read_extended(image + 32 + (unary || elementary || remainder || double_pop ? 1 : index) * 16);
    // NaN propagation precedes a denormal found while widening memory input.
    if (extF80_isNaN(a) || extF80_isNaN(b))
        status.softfloat_exceptionFlags &= ~softfloat_flag_denormal;

    result->destination = !memory && arithmetic && opcode != 0xd8 ? index : 0;
    result->pops = !memory && arithmetic && opcode == 0xde;
    extFloat80_t value = a;
    if (compare) {
        bool quiet = !memory && (opcode == 0xdd || opcode == 0xda || (integer_flags && group == 5));
        int relation = extF80_compare(a, b, quiet, &status);
        result->pops = double_pop ? 2 : memory ? group == 3 :
            opcode == 0xdf || (opcode == 0xd8 && group == 3) || (opcode == 0xdd && group == 5);
        unsigned flags = relation == softfloat_relation_equal ? 0x4000u :
                         relation == softfloat_relation_less ? 0x0100u :
                         relation == softfloat_relation_unordered ? 0x4500u : 0;
        if (integer_flags)
            result->integer_flags = (flags >> 8) & 0x45u;
        else if (!(status.softfloat_exceptionFlags & ~control & 1u)) {
            result->condition_clear = 0x4700u;
            result->conditions = flags;
        }
    } else if (extract) {
        extFloat80_t significand = a;
        value = extF80_extract(&significand, &status);
        result->push = true;
        write_extended(result->pushed_value, significand);
    } else if (remainder) {
        Bit64u quotient;
        int partial = modrm == 0xf5 ?
            floatx80_ieee754_remainder(a, b, value, quotient, &status) :
            floatx80_remainder(a, b, value, quotient, &status);
        if (partial >= 0 && !(status.softfloat_exceptionFlags & ~control & 7u)) {
            result->condition_clear = 0x4700u;
            result->conditions = partial ? 0x0400u :
                ((quotient & 1u) << 9) | ((quotient & 2u) << 13) | ((quotient & 4u) << 6);
        }
    } else if (elementary) {
        status.extF80_roundingPrecision = 80;
        result->destination = modrm == 0xf0 ? 0 : 1;
        result->pops = modrm != 0xf0;
        if (modrm == 0xf0) value = f2xm1(a, status);
        else if (modrm == 0xf1) value = fyl2x(a, b, status);
        else if (modrm == 0xf3) value = fpatan(a, b, status);
        else value = fyl2xp1(a, b, status);
    } else if (trig) {
        status.extF80_roundingPrecision = 80;
        value = a;
        extFloat80_t pushed = {};
        int outside;
        if (modrm == 0xf2) {
            outside = fptan(value, status);
            pushed = extF80_isNaN(value) ? value :
                     extFloat80_t{UINT64_C(0x8000000000000000), 0x3fff};
        } else if (modrm == 0xfb) outside = fsincos(a, &value, &pushed, status);
        else outside = modrm == 0xfe ? fsin(value, status) : fcos(value, status);
        result->write = !outside;
        result->conditions = outside ? 0x0400u : 0;
        result->push = !outside && (modrm == 0xf2 || modrm == 0xfb);
        write_extended(result->pushed_value, pushed);
    } else if (unary) {
        if (modrm == 0xfa) value = extF80_sqrt(a, &status);
        else if (modrm == 0xfc) value = extF80_roundToInt(a, &status);
        else value = extF80_scale(a, b, &status);
    } else {
        // The group selects the numerical order, independently of destination.
        bool reverse = group == 5 || group == 7;
        if (reverse && group >= 4) { extFloat80_t swap = a; a = b; b = swap; }
        if (group == 0) value = extF80_add(a, b, &status);
        else if (group == 1) value = extF80_mul(a, b, &status);
        else if (group < 6) value = extF80_sub(a, b, &status);
        else value = extF80_div(a, b, &status);
    }

    unsigned raised = status.softfloat_exceptionFlags & 0x23fu;
    // A fault in operand evaluation inhibits later numerical exceptions.
    if (raised & 1u) raised = 1u;
    else if (raised & 4u) raised = 4u;
    else if ((raised & 2u) && !(control & 2u)) raised = 2u;
    result->exceptions = raised;
    write_extended(result->value, value);
    return true;
}
