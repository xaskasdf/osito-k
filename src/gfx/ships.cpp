/*
 * OsitoK - Elite ship models (wireframe data)
 *
 * Vertex/edge data from BBC Micro Elite (bbcelite.com).
 * Scaled to fix16: original coords / 80 => range ~+-2.0
 */

#include "gfx/ships.h"
#include "drivers/video.h"
#include "drivers/uart.h"
#include "kernel/task.h"

extern "C" {

/* ====== Cobra Mk III — 28 vertices, 38 edges ====== */

static const vec3_t cobra_verts[28] = {
    { SHIP_SCALE( 32), SHIP_SCALE(  0), SHIP_SCALE( 76) },  /*  0 */
    { SHIP_SCALE(-32), SHIP_SCALE(  0), SHIP_SCALE( 76) },  /*  1 */
    { SHIP_SCALE(  0), SHIP_SCALE( 26), SHIP_SCALE( 24) },  /*  2 */
    { SHIP_SCALE(-120),SHIP_SCALE( -3), SHIP_SCALE( -8) },  /*  3 */
    { SHIP_SCALE( 120),SHIP_SCALE( -3), SHIP_SCALE( -8) },  /*  4 */
    { SHIP_SCALE(-88), SHIP_SCALE( 16), SHIP_SCALE(-40) },  /*  5 */
    { SHIP_SCALE( 88), SHIP_SCALE( 16), SHIP_SCALE(-40) },  /*  6 */
    { SHIP_SCALE( 128),SHIP_SCALE( -8), SHIP_SCALE(-40) },  /*  7 */
    { SHIP_SCALE(-128),SHIP_SCALE( -8), SHIP_SCALE(-40) },  /*  8 */
    { SHIP_SCALE(  0), SHIP_SCALE( 26), SHIP_SCALE(-40) },  /*  9 */
    { SHIP_SCALE(-32), SHIP_SCALE(-24), SHIP_SCALE(-40) },  /* 10 */
    { SHIP_SCALE( 32), SHIP_SCALE(-24), SHIP_SCALE(-40) },  /* 11 */
    { SHIP_SCALE(-36), SHIP_SCALE(  8), SHIP_SCALE(-40) },  /* 12 */
    { SHIP_SCALE( -8), SHIP_SCALE( 12), SHIP_SCALE(-40) },  /* 13 */
    { SHIP_SCALE(  8), SHIP_SCALE( 12), SHIP_SCALE(-40) },  /* 14 */
    { SHIP_SCALE( 36), SHIP_SCALE(  8), SHIP_SCALE(-40) },  /* 15 */
    { SHIP_SCALE( 36), SHIP_SCALE(-12), SHIP_SCALE(-40) },  /* 16 */
    { SHIP_SCALE(  8), SHIP_SCALE(-16), SHIP_SCALE(-40) },  /* 17 */
    { SHIP_SCALE( -8), SHIP_SCALE(-16), SHIP_SCALE(-40) },  /* 18 */
    { SHIP_SCALE(-36), SHIP_SCALE(-12), SHIP_SCALE(-40) },  /* 19 */
    { SHIP_SCALE(  0), SHIP_SCALE(  0), SHIP_SCALE( 76) },  /* 20 */
    { SHIP_SCALE(  0), SHIP_SCALE(  0), SHIP_SCALE( 90) },  /* 21 */
    { SHIP_SCALE(-80), SHIP_SCALE( -6), SHIP_SCALE(-40) },  /* 22 */
    { SHIP_SCALE(-80), SHIP_SCALE(  6), SHIP_SCALE(-40) },  /* 23 */
    { SHIP_SCALE(-88), SHIP_SCALE(  0), SHIP_SCALE(-40) },  /* 24 */
    { SHIP_SCALE( 80), SHIP_SCALE(  6), SHIP_SCALE(-40) },  /* 25 */
    { SHIP_SCALE( 88), SHIP_SCALE(  0), SHIP_SCALE(-40) },  /* 26 */
    { SHIP_SCALE( 80), SHIP_SCALE( -6), SHIP_SCALE(-40) },  /* 27 */
};

static const uint8_t cobra_edges[38 * 2] = {
     0, 1,   0, 4,   1, 3,   3, 8,   4, 7,   6, 7,   6, 9,   5, 9,
     5, 8,   2, 5,   2, 6,   3, 5,   4, 6,   1, 2,   0, 2,   8,10,
    10,11,   7,11,   1,10,   0,11,   1, 5,   0, 6,  20,21,  12,13,
    18,19,  14,15,  16,17,  15,16,  14,17,  13,18,  12,19,   2, 9,
    22,24,  23,24,  22,23,  25,26,  26,27,  25,27,
};

const wire_model_t ship_cobra = {
    cobra_verts, cobra_edges, 28, 38
};

/* ====== Sidewinder — 10 vertices, 15 edges ====== */

static const vec3_t sidewinder_verts[10] = {
    { SHIP_SCALE(-32), SHIP_SCALE(  0), SHIP_SCALE( 36) },  /*  0 */
    { SHIP_SCALE( 32), SHIP_SCALE(  0), SHIP_SCALE( 36) },  /*  1 */
    { SHIP_SCALE( 64), SHIP_SCALE(  0), SHIP_SCALE(-28) },  /*  2 */
    { SHIP_SCALE(-64), SHIP_SCALE(  0), SHIP_SCALE(-28) },  /*  3 */
    { SHIP_SCALE(  0), SHIP_SCALE( 16), SHIP_SCALE(-28) },  /*  4 */
    { SHIP_SCALE(  0), SHIP_SCALE(-16), SHIP_SCALE(-28) },  /*  5 */
    { SHIP_SCALE(-12), SHIP_SCALE(  6), SHIP_SCALE(-28) },  /*  6 */
    { SHIP_SCALE( 12), SHIP_SCALE(  6), SHIP_SCALE(-28) },  /*  7 */
    { SHIP_SCALE( 12), SHIP_SCALE( -6), SHIP_SCALE(-28) },  /*  8 */
    { SHIP_SCALE(-12), SHIP_SCALE( -6), SHIP_SCALE(-28) },  /*  9 */
};

static const uint8_t sidewinder_edges[15 * 2] = {
    0,1,  1,2,  1,4,  0,4,  0,3,  3,4,  2,4,  3,5,  2,5,  1,5,  0,5,
    6,7,  7,8,  6,9,  8,9,
};

const wire_model_t ship_sidewinder = {
    sidewinder_verts, sidewinder_edges, 10, 15
};

/* ====== Viper — 15 vertices, 20 edges ====== */

static const vec3_t viper_verts[15] = {
    { SHIP_SCALE(  0), SHIP_SCALE(  0), SHIP_SCALE( 72) },  /*  0 */
    { SHIP_SCALE(  0), SHIP_SCALE( 16), SHIP_SCALE( 24) },  /*  1 */
    { SHIP_SCALE(  0), SHIP_SCALE(-16), SHIP_SCALE( 24) },  /*  2 */
    { SHIP_SCALE( 48), SHIP_SCALE(  0), SHIP_SCALE(-24) },  /*  3 */
    { SHIP_SCALE(-48), SHIP_SCALE(  0), SHIP_SCALE(-24) },  /*  4 */
    { SHIP_SCALE( 24), SHIP_SCALE(-16), SHIP_SCALE(-24) },  /*  5 */
    { SHIP_SCALE(-24), SHIP_SCALE(-16), SHIP_SCALE(-24) },  /*  6 */
    { SHIP_SCALE( 24), SHIP_SCALE( 16), SHIP_SCALE(-24) },  /*  7 */
    { SHIP_SCALE(-24), SHIP_SCALE( 16), SHIP_SCALE(-24) },  /*  8 */
    { SHIP_SCALE(-32), SHIP_SCALE(  0), SHIP_SCALE(-24) },  /*  9 */
    { SHIP_SCALE( 32), SHIP_SCALE(  0), SHIP_SCALE(-24) },  /* 10 */
    { SHIP_SCALE(  8), SHIP_SCALE(  8), SHIP_SCALE(-24) },  /* 11 */
    { SHIP_SCALE( -8), SHIP_SCALE(  8), SHIP_SCALE(-24) },  /* 12 */
    { SHIP_SCALE( -8), SHIP_SCALE( -8), SHIP_SCALE(-24) },  /* 13 */
    { SHIP_SCALE(  8), SHIP_SCALE( -8), SHIP_SCALE(-24) },  /* 14 */
};

static const uint8_t viper_edges[20 * 2] = {
    0,3,  0,1,  0,2,  0,4,  1,7,  1,8,  2,5,  2,6,  7,8,  5,6,
    4,8,  4,6,  3,7,  3,5,  9,12,  9,13,  10,11,  10,14,  11,14,  12,13,
};

const wire_model_t ship_viper = {
    viper_verts, viper_edges, 15, 20
};

/* ====== Coriolis Station — 16 vertices, 28 edges ====== */

static const vec3_t coriolis_verts[16] = {
    { SHIP_SCALE( 160), SHIP_SCALE(   0), SHIP_SCALE( 160) },  /*  0 */
    { SHIP_SCALE(   0), SHIP_SCALE( 160), SHIP_SCALE( 160) },  /*  1 */
    { SHIP_SCALE(-160), SHIP_SCALE(   0), SHIP_SCALE( 160) },  /*  2 */
    { SHIP_SCALE(   0), SHIP_SCALE(-160), SHIP_SCALE( 160) },  /*  3 */
    { SHIP_SCALE( 160), SHIP_SCALE(-160), SHIP_SCALE(   0) },  /*  4 */
    { SHIP_SCALE( 160), SHIP_SCALE( 160), SHIP_SCALE(   0) },  /*  5 */
    { SHIP_SCALE(-160), SHIP_SCALE( 160), SHIP_SCALE(   0) },  /*  6 */
    { SHIP_SCALE(-160), SHIP_SCALE(-160), SHIP_SCALE(   0) },  /*  7 */
    { SHIP_SCALE( 160), SHIP_SCALE(   0), SHIP_SCALE(-160) },  /*  8 */
    { SHIP_SCALE(   0), SHIP_SCALE( 160), SHIP_SCALE(-160) },  /*  9 */
    { SHIP_SCALE(-160), SHIP_SCALE(   0), SHIP_SCALE(-160) },  /* 10 */
    { SHIP_SCALE(   0), SHIP_SCALE(-160), SHIP_SCALE(-160) },  /* 11 */
    { SHIP_SCALE(  10), SHIP_SCALE( -30), SHIP_SCALE( 160) },  /* 12 — docking port */
    { SHIP_SCALE(  10), SHIP_SCALE(  30), SHIP_SCALE( 160) },  /* 13 */
    { SHIP_SCALE( -10), SHIP_SCALE(  30), SHIP_SCALE( 160) },  /* 14 */
    { SHIP_SCALE( -10), SHIP_SCALE( -30), SHIP_SCALE( 160) },  /* 15 */
};

static const uint8_t coriolis_edges[28 * 2] = {
    /* Front face */   0, 3,   0, 1,   1, 2,   2, 3,
    /* Front-mid */    3, 4,   0, 4,   0, 5,   5, 1,   1, 6,   2, 6,   2, 7,   3, 7,
    /* Back face */    8,11,   8, 9,   9,10,  10,11,
    /* Back-mid */     4,11,   4, 8,   5, 8,   5, 9,   6, 9,   6,10,   7,10,   7,11,
    /* Docking port */ 12,13,  13,14,  14,15,  15,12,
};

const wire_model_t ship_coriolis = {
    coriolis_verts, coriolis_edges, 16, 28
};

/* ====== Ship list ====== */

const wire_model_t *ship_list[SHIP_COUNT] = {
    &ship_cobra,
    &ship_sidewinder,
    &ship_viper,
    &ship_coriolis,
};

const char *ship_names[SHIP_COUNT] = {
    "cobra",
    "sidewinder",
    "viper",
    "coriolis",
};

/* ====== Shell: ship [name] ====== */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

void cmd_ship(const char *args)
{
    while (*args == ' ') args++;

    /* No argument: list ships */
    if (*args == '\0') {
        uart_puts("ships:");
        for (int i = 0; i < SHIP_COUNT; i++) {
            uart_puts(" ");
            uart_puts(ship_names[i]);
        }
        uart_puts("\n");
        return;
    }

    /* Find ship by name */
    for (int i = 0; i < SHIP_COUNT; i++) {
        if (str_eq(args, ship_names[i])) {
            const wire_model_t *m = ship_list[i];

            uart_puts("ship: ");
            uart_puts(ship_names[i]);
            uart_puts(" ");
            uart_put_dec(m->nv);
            uart_puts("v ");
            uart_put_dec(m->ne);
            uart_puts("e\n");

            /* Render: slight rotation so it's not face-on */
            mat3_t rx, ry, rot;
            mat3_rotate_x(&rx, 20);   /* ~28 deg tilt */
            mat3_rotate_y(&ry, 200);  /* ~282 deg — show from front-ish */
            mat3_multiply(&rot, &ry, &rx);

            /* Coriolis bigger → farther; others close up */
            fix16_t z = (i == 3) ? FIX16(8) : FIX16(4);
            vec3_t pos = vec3(0, 0, z);

            fb_clear();
            wire_render(m, &rot, pos, FIX16(64));
            fb_text_puts(0, 0, ship_names[i]);
            fb_flush();
            return;
        }
    }

    uart_puts("unknown ship: ");
    uart_puts(args);
    uart_puts("\n");
}

/* ====== Shell: shipspin ====== */

void cmd_shipspin(void)
{
    uart_puts("shipspin: ");
    uart_put_dec(SHIP_COUNT);
    uart_puts(" ships (Ctrl+C to stop)\n");

    for (int s = 0; s < SHIP_COUNT; s++) {
        const wire_model_t *m = ship_list[s];

        uart_puts("  ");
        uart_puts(ship_names[s]);
        uart_puts("...\n");

        /* Coriolis bigger → farther; others close up */
        fix16_t z = (s == 3) ? FIX16(8) : FIX16(4);
        vec3_t pos = vec3(0, 0, z);

        angle_t ay = 0;
        angle_t ax = 0;
        int stop = 0;

        for (int f = 0; f < 100; f++) {
            /* Check Ctrl+C */
            if (uart_rx_available()) {
                int ch = uart_getc();
                if (ch == 0x03) { stop = 1; break; }
            }

            mat3_t rx, ry, rot;
            mat3_rotate_x(&rx, ax);
            mat3_rotate_y(&ry, ay);
            mat3_multiply(&rot, &ry, &rx);

            fb_clear();
            wire_render(m, &rot, pos, FIX16(64));
            fb_text_puts(0, 0, ship_names[s]);
            fb_flush();

            ay += 3;
            ax += 1;
            task_yield();
        }

        if (stop) break;
    }

    uart_puts("shipspin: done\n");
}

} /* extern "C" */
