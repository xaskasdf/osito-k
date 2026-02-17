/*
 * OsitoK - 3D matrix and projection library
 *
 * Contains: rotation matrices, multiply, transform, project, test suite
 */

#include "math/matrix3.h"
#include "drivers/uart.h"

extern "C" {

/* ====== Rotation matrices ====== */

/*
 * | 1    0      0   |
 * | 0   cos   -sin  |
 * | 0   sin    cos  |
 */
void mat3_rotate_x(mat3_t *out, angle_t angle)
{
    fix16_t c = fix_cos(angle);
    fix16_t s = fix_sin(angle);

    out->m[0][0] = FIX16_ONE; out->m[0][1] = 0;  out->m[0][2] = 0;
    out->m[1][0] = 0;         out->m[1][1] = c;   out->m[1][2] = -s;
    out->m[2][0] = 0;         out->m[2][1] = s;   out->m[2][2] = c;
}

/*
 * |  cos  0   sin |
 * |   0   1    0  |
 * | -sin  0   cos |
 */
void mat3_rotate_y(mat3_t *out, angle_t angle)
{
    fix16_t c = fix_cos(angle);
    fix16_t s = fix_sin(angle);

    out->m[0][0] = c;  out->m[0][1] = 0;         out->m[0][2] = s;
    out->m[1][0] = 0;  out->m[1][1] = FIX16_ONE; out->m[1][2] = 0;
    out->m[2][0] = -s; out->m[2][1] = 0;         out->m[2][2] = c;
}

/*
 * | cos  -sin  0 |
 * | sin   cos  0 |
 * |  0     0   1 |
 */
void mat3_rotate_z(mat3_t *out, angle_t angle)
{
    fix16_t c = fix_cos(angle);
    fix16_t s = fix_sin(angle);

    out->m[0][0] = c;  out->m[0][1] = -s; out->m[0][2] = 0;
    out->m[1][0] = s;  out->m[1][1] = c;  out->m[1][2] = 0;
    out->m[2][0] = 0;  out->m[2][1] = 0;  out->m[2][2] = FIX16_ONE;
}

/* ====== Matrix multiply ====== */

/*
 * out = a * b (3x3 matrix multiplication)
 * out must NOT alias a or b — uses internal temp buffer.
 * 27 fix_mul operations, ~400 cycles.
 */
void mat3_multiply(mat3_t *out, const mat3_t *a, const mat3_t *b)
{
    mat3_t tmp;

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            tmp.m[i][j] = fix_mul(a->m[i][0], b->m[0][j])
                        + fix_mul(a->m[i][1], b->m[1][j])
                        + fix_mul(a->m[i][2], b->m[2][j]);
        }
    }

    /* Copy result — safe even if out aliases a or b */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out->m[i][j] = tmp.m[i][j];
}

/* ====== Matrix-vector transform ====== */

vec3_t mat3_transform(const mat3_t *m, vec3_t v)
{
    vec3_t r;
    r.x = fix_mul(m->m[0][0], v.x) + fix_mul(m->m[0][1], v.y) + fix_mul(m->m[0][2], v.z);
    r.y = fix_mul(m->m[1][0], v.x) + fix_mul(m->m[1][1], v.y) + fix_mul(m->m[1][2], v.z);
    r.z = fix_mul(m->m[2][0], v.x) + fix_mul(m->m[2][1], v.y) + fix_mul(m->m[2][2], v.z);
    return r;
}

/* ====== Perspective projection ====== */

int project(vec3_t v, fix16_t focal, int *sx, int *sy)
{
    if (v.z <= FIX16_C(0.5))
        return 0;  /* behind camera */

    fix16_t inv_z = fix_div(focal, v.z);
    *sx = 64 + FIX16_ROUND(fix_mul(v.x, inv_z));
    *sy = 32 - FIX16_ROUND(fix_mul(v.y, inv_z));
    return 1;
}

/* ====== Debug print ====== */

void vec3_print(vec3_t v)
{
    uart_putc('(');
    fix_print(v.x);
    uart_puts(", ");
    fix_print(v.y);
    uart_puts(", ");
    fix_print(v.z);
    uart_putc(')');
}

void mat3_print(const mat3_t *m)
{
    for (int i = 0; i < 3; i++) {
        uart_puts("  | ");
        fix_print(m->m[i][0]);
        uart_puts("  ");
        fix_print(m->m[i][1]);
        uart_puts("  ");
        fix_print(m->m[i][2]);
        uart_puts(" |\n");
    }
}

/* ====== Test suite ====== */

static void mat3_test_case(const char *label, vec3_t got, vec3_t expected, fix16_t tol)
{
    uart_puts(label);
    uart_puts(" = ");
    vec3_print(got);

    fix16_t dx = fix_abs(got.x - expected.x);
    fix16_t dy = fix_abs(got.y - expected.y);
    fix16_t dz = fix_abs(got.z - expected.z);

    if (dx <= tol && dy <= tol && dz <= tol) {
        uart_puts("  OK\n");
    } else {
        uart_puts("  FAIL (expected ");
        vec3_print(expected);
        uart_puts(")\n");
    }
}

static void mat3_test_scalar(const char *label, fix16_t got, fix16_t expected, fix16_t tol)
{
    uart_puts(label);
    uart_puts(" = ");
    fix_print(got);

    if (fix_abs(got - expected) <= tol) {
        uart_puts("  OK\n");
    } else {
        uart_puts("  FAIL (expected ");
        fix_print(expected);
        uart_puts(")\n");
    }
}

void mat3_test(void)
{
    mat3_t m, rx, ry, combined;
    vec3_t v, r;
    fix16_t tol = FIX16_C(0.02);  /* tolerance for trig rounding */

    uart_puts("=== Matrix3 test ===\n");

    /* Identity * v = v */
    mat3_identity(&m);
    v = vec3(FIX16(1), FIX16(2), FIX16(3));
    r = mat3_transform(&m, v);
    mat3_test_case("identity * v(1,2,3)", r, v, 1);

    /* rotZ(64) * (1,0,0) = (0,1,0)  — 90 degrees, X becomes Y */
    mat3_rotate_z(&m, 64);
    v = vec3(FIX16(1), 0, 0);
    r = mat3_transform(&m, v);
    mat3_test_case("rotZ(64) * (1,0,0)", r, vec3(0, FIX16(1), 0), tol);

    /* rotX(64) * (0,1,0) = (0,0,1)  — 90 degrees, Y becomes Z */
    mat3_rotate_x(&m, 64);
    v = vec3(0, FIX16(1), 0);
    r = mat3_transform(&m, v);
    mat3_test_case("rotX(64) * (0,1,0)", r, vec3(0, 0, FIX16(1)), tol);

    /* rotY(64) * (0,0,1) = (1,0,0)  — 90 degrees, Z becomes X */
    mat3_rotate_y(&m, 64);
    v = vec3(0, 0, FIX16(1));
    r = mat3_transform(&m, v);
    mat3_test_case("rotY(64) * (0,0,1)", r, vec3(FIX16(1), 0, 0), tol);

    /* Combined: rotX * rotY applied to (1,0,0) */
    mat3_rotate_x(&rx, 64);
    mat3_rotate_y(&ry, 64);
    mat3_multiply(&combined, &rx, &ry);
    v = vec3(FIX16(1), 0, 0);
    r = mat3_transform(&combined, v);
    uart_puts("combined rotXY * (1,0,0) = ");
    vec3_print(r);
    uart_puts("\n");

    /* Projection: center point */
    int sx, sy;
    v = vec3(0, 0, FIX16(10));
    int vis = project(v, FIX16(64), &sx, &sy);
    uart_puts("project(0,0,10) = (");
    uart_put_dec((uint32_t)sx);
    uart_puts(", ");
    uart_put_dec((uint32_t)sy);
    uart_puts(") ");
    uart_puts((vis && sx == 64 && sy == 32) ? "OK" : "FAIL");
    uart_puts("\n");

    /* Projection: offset point */
    v = vec3(FIX16(5), 0, FIX16(10));
    vis = project(v, FIX16(64), &sx, &sy);
    uart_puts("project(5,0,10) = (");
    uart_put_dec((uint32_t)sx);
    uart_puts(", ");
    uart_put_dec((uint32_t)sy);
    uart_puts(") ");
    uart_puts((vis && sx == 96 && sy == 32) ? "OK" : "FAIL");
    uart_puts("\n");

    /* Dot product: perpendicular = 0 */
    mat3_test_scalar("dot((1,0,0),(0,1,0))",
        vec3_dot(vec3(FIX16(1), 0, 0), vec3(0, FIX16(1), 0)),
        0, 1);

    /* Length: (3,4,0) = 5 */
    mat3_test_scalar("length(3,4,0)",
        vec3_length(vec3(FIX16(3), FIX16(4), 0)),
        FIX16(5), FIX16_C(0.01));

    uart_puts("=== done ===\n");
}

} /* extern "C" */
