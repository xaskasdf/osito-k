/*
 * OsitoK - 3D vector and matrix library
 *
 * vec3_t:  3D vector (3 x fix16_t = 12 bytes)
 * mat3_t:  3x3 rotation matrix (9 x fix16_t = 36 bytes)
 *
 * Uses fix16_t from fixedpoint.h for all arithmetic.
 * Designed for Elite-style 3D: rotate ships/stations, project to 128x64 FB.
 */
#ifndef OSITO_MATRIX3_H
#define OSITO_MATRIX3_H

#include "math/fixedpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Types ====== */

typedef struct { fix16_t x, y, z; } vec3_t;   /* 12 bytes */
typedef struct { fix16_t m[3][3]; } mat3_t;    /* 36 bytes */

/* ====== Vector operations (inline) ====== */

INLINE vec3_t vec3(fix16_t x, fix16_t y, fix16_t z)
{
    vec3_t v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

INLINE vec3_t vec3_add(vec3_t a, vec3_t b)
{
    return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

INLINE vec3_t vec3_sub(vec3_t a, vec3_t b)
{
    return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

INLINE vec3_t vec3_neg(vec3_t v)
{
    return vec3(-v.x, -v.y, -v.z);
}

INLINE vec3_t vec3_scale(vec3_t v, fix16_t s)
{
    return vec3(fix_mul(v.x, s), fix_mul(v.y, s), fix_mul(v.z, s));
}

INLINE fix16_t vec3_dot(vec3_t a, vec3_t b)
{
    return fix_mul(a.x, b.x) + fix_mul(a.y, b.y) + fix_mul(a.z, b.z);
}

INLINE fix16_t vec3_length(vec3_t v)
{
    return fix_sqrt(vec3_dot(v, v));
}

/* ====== Matrix operations (inline simple ones) ====== */

INLINE void mat3_identity(mat3_t *out)
{
    out->m[0][0] = FIX16_ONE; out->m[0][1] = 0;         out->m[0][2] = 0;
    out->m[1][0] = 0;         out->m[1][1] = FIX16_ONE; out->m[1][2] = 0;
    out->m[2][0] = 0;         out->m[2][1] = 0;         out->m[2][2] = FIX16_ONE;
}

/* ====== Matrix operations (in matrix3.cpp) ====== */

void mat3_rotate_x(mat3_t *out, angle_t angle);
void mat3_rotate_y(mat3_t *out, angle_t angle);
void mat3_rotate_z(mat3_t *out, angle_t angle);
void mat3_multiply(mat3_t *out, const mat3_t *a, const mat3_t *b);
vec3_t mat3_transform(const mat3_t *m, vec3_t v);

/* ====== Projection ====== */

/*
 * Project 3D point to 2D screen coordinates.
 * focal: focal length in fix16 (e.g. FIX16(64)).
 * sx, sy: output screen coordinates, centered at (64, 32).
 * Returns 1 if visible, 0 if behind camera (z <= 0.5).
 */
int project(vec3_t v, fix16_t focal, int *sx, int *sy);

/* ====== Debug ====== */

void vec3_print(vec3_t v);
void mat3_print(const mat3_t *m);
void mat3_test(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_MATRIX3_H */
