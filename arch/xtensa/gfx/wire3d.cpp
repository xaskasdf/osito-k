/*
 * OsitoK - Wireframe 3D renderer
 *
 * Render pipeline: rotate vertices → project to 2D → draw edges.
 * Includes built-in cube model and test/spin commands.
 */

#include "gfx/wire3d.h"
#include "drivers/video.h"
#include "drivers/uart.h"
#include "kernel/task.h"

extern "C" {

/* ====== Render pipeline ====== */

void wire_render(const wire_model_t *model, const mat3_t *rot,
                 vec3_t pos, fix16_t focal)
{
    uint8_t nv = model->nv;
    if (nv > WIRE_MAX_VERTS)
        nv = WIRE_MAX_VERTS;

    /* Stack buffers for projected 2D coordinates */
    int sx[WIRE_MAX_VERTS];
    int sy[WIRE_MAX_VERTS];
    uint8_t visible[WIRE_MAX_VERTS];

    /* Transform + project each vertex once */
    for (int i = 0; i < nv; i++) {
        vec3_t world = vec3_add(mat3_transform(rot, model->verts[i]), pos);
        visible[i] = (uint8_t)project(world, focal, &sx[i], &sy[i]);
    }

    /* Draw edges where both endpoints are visible */
    for (int i = 0; i < model->ne; i++) {
        uint8_t a = model->edges[i * 2];
        uint8_t b = model->edges[i * 2 + 1];
        if (a < nv && b < nv && visible[a] && visible[b])
            fb_line(sx[a], sy[a], sx[b], sy[b]);
    }
}

/* ====== Built-in cube model ====== */

/*
 * Unit cube: vertices at ±1 on each axis.
 *
 *     3------2       Y
 *    /|     /|       |
 *   7------6 |       +--X
 *   | 0----|-1      /
 *   |/     |/      Z
 *   4------5
 */
static const vec3_t cube_verts[8] = {
    { FIX16(-1), FIX16(-1), FIX16(-1) },  /* 0 */
    { FIX16( 1), FIX16(-1), FIX16(-1) },  /* 1 */
    { FIX16( 1), FIX16( 1), FIX16(-1) },  /* 2 */
    { FIX16(-1), FIX16( 1), FIX16(-1) },  /* 3 */
    { FIX16(-1), FIX16(-1), FIX16( 1) },  /* 4 */
    { FIX16( 1), FIX16(-1), FIX16( 1) },  /* 5 */
    { FIX16( 1), FIX16( 1), FIX16( 1) },  /* 6 */
    { FIX16(-1), FIX16( 1), FIX16( 1) },  /* 7 */
};

static const uint8_t cube_edges[12 * 2] = {
    /* Front face */   0,1,  1,2,  2,3,  3,0,
    /* Back face */    4,5,  5,6,  6,7,  7,4,
    /* Connectors */   0,4,  1,5,  2,6,  3,7,
};

const wire_model_t wire_cube = {
    cube_verts, cube_edges, 8, 12
};

/* ====== Test: static cube ====== */

void wire_test(void)
{
    mat3_t rot;
    mat3_identity(&rot);

    /* Position cube at z=5 (in front of camera) */
    vec3_t pos = vec3(0, 0, FIX16(5));

    fb_clear();
    wire_render(&wire_cube, &rot, pos, FIX16(64));
    fb_text_puts(0, 0, "wiretest");
    fb_flush();

    uart_puts("wiretest: cube 8v 12e at z=5\n");
}

/* ====== Test: spinning cube ====== */

void wire_spin(void)
{
    uart_puts("wirespin: spinning cube (Ctrl+C to stop)\n");

    vec3_t pos = vec3(0, 0, FIX16(5));
    angle_t ay = 0;
    angle_t ax = 0;
    uint32_t frames = 0;
    uint32_t t_start = get_tick_count();

    for (int i = 0; i < 500; i++) {
        /* Check Ctrl+C */
        if (uart_rx_available()) {
            int ch = uart_getc();
            if (ch == 0x03)
                break;
        }

        /* Build combined rotation: X then Y */
        mat3_t rx, ry, rot;
        mat3_rotate_x(&rx, ax);
        mat3_rotate_y(&ry, ay);
        mat3_multiply(&rot, &ry, &rx);

        fb_clear();
        wire_render(&wire_cube, &rot, pos, FIX16(64));
        fb_flush();

        ay += 3;  /* ~4.2° per frame */
        ax += 1;  /* ~1.4° per frame */
        frames++;

        task_yield();
    }

    uint32_t elapsed = get_tick_count() - t_start;
    uart_puts("wirespin: ");
    uart_put_dec(frames);
    uart_puts(" frames in ");
    uart_put_dec(elapsed);
    uart_puts(" ticks");
    if (elapsed > 0) {
        uart_puts(" (");
        uart_put_dec(frames * TICK_HZ / elapsed);
        uart_puts(" fps)");
    }
    uart_puts("\n");
}

} /* extern "C" */
