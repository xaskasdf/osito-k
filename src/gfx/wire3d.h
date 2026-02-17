/*
 * OsitoK - Wireframe 3D renderer
 *
 * Takes a model (vertices + edge indices), applies rotation matrix,
 * projects to 2D, and draws wireframe lines to the framebuffer.
 *
 * Designed for Elite-style 3D: ships, stations, asteroids.
 */
#ifndef OSITO_WIRE3D_H
#define OSITO_WIRE3D_H

#include "math/matrix3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum vertices per model (for stack-allocated projection buffers) */
#define WIRE_MAX_VERTS  64

/* Wireframe model: vertex array + edge index pairs */
typedef struct {
    const vec3_t  *verts;    /* array of 3D vertices */
    const uint8_t *edges;    /* pairs of vertex indices [a,b, a,b, ...] */
    uint8_t        nv;       /* number of vertices (max WIRE_MAX_VERTS) */
    uint8_t        ne;       /* number of edges */
} wire_model_t;

/*
 * Render wireframe model to framebuffer.
 * rot:   3x3 rotation matrix (object orientation)
 * pos:   object position in world space (camera at origin)
 * focal: focal length for perspective (typical: FIX16(64))
 *
 * Does NOT call fb_clear or fb_flush — caller controls those.
 */
void wire_render(const wire_model_t *model, const mat3_t *rot,
                 vec3_t pos, fix16_t focal);

/* Built-in model: unit cube (8 vertices, 12 edges) */
extern const wire_model_t wire_cube;

/* Test: draw static cube to framebuffer */
void wire_test(void);

/* Test: spinning cube animation (~5 seconds, Ctrl+C to stop) */
void wire_spin(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_WIRE3D_H */
