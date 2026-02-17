/*
 * OsitoK - Elite flight demo
 *
 * Wireframe 3D flight with starfield, HUD, and joystick controls.
 * Renders ship models rotating in front of the camera.
 */

#include "game/game.h"
#include "drivers/uart.h"
#include "drivers/video.h"
#include "drivers/input.h"
#include "gfx/wire3d.h"
#include "gfx/ships.h"
#include "math/matrix3.h"
#include "kernel/task.h"

extern "C" {

/* ====== PRNG ====== */

static uint16_t game_rand(game_state_t *g)
{
    g->rng_seed = g->rng_seed * 1103515245 + 12345;
    return (uint16_t)((g->rng_seed >> 16) & 0x7FFF);
}

/* ====== Starfield ====== */

static void stars_init(game_state_t *g)
{
    for (int i = 0; i < STAR_COUNT; i++) {
        g->star_x[i] = (int8_t)(game_rand(g) % 128);
        g->star_y[i] = (int8_t)(VIEW_Y_MIN + game_rand(g) % (VIEW_Y_MAX - VIEW_Y_MIN));
    }
}

static void stars_update(game_state_t *g)
{
    /* Compute horizontal drift from yaw change */
    /* Use speed as scroll factor: higher speed = more star drift */
    int drift = (int)(int8_t)(g->yaw) >> 5;  /* +-4 pixels based on yaw angle */

    for (int i = 0; i < STAR_COUNT; i++) {
        int sx = g->star_x[i] - drift;

        /* Wrap horizontally */
        if (sx < 0) {
            sx = 127;
            g->star_y[i] = (int8_t)(VIEW_Y_MIN + game_rand(g) % (VIEW_Y_MAX - VIEW_Y_MIN));
        } else if (sx > 127) {
            sx = 0;
            g->star_y[i] = (int8_t)(VIEW_Y_MIN + game_rand(g) % (VIEW_Y_MAX - VIEW_Y_MIN));
        }

        g->star_x[i] = (int8_t)sx;
        fb_set_pixel(g->star_x[i], g->star_y[i]);
    }
}

/* ====== HUD ====== */

static const char *compass_dirs[8] = {"N ","NE","E ","SE","S ","SW","W ","NW"};

static void hud_draw(const game_state_t *g)
{
    /* Separator line */
    fb_line(0, HUD_Y, 127, HUD_Y);

    /* Speed bar: "SPD:===---" at row 8 */
    char spd[11];
    spd[0] = 'S'; spd[1] = 'P'; spd[2] = 'D'; spd[3] = ':';
    for (int i = 0; i < 6; i++)
        spd[4 + i] = (i < g->speed) ? '=' : '-';
    spd[10] = '\0';
    fb_text_puts(0, 8, spd);

    /* Compass: direction + angle */
    uint8_t dir_idx = (uint8_t)((g->yaw + 16) >> 5) & 7;  /* 256/8 = 32 */
    uint16_t deg = ((uint16_t)g->yaw * 360) >> 8;

    char comp[7];
    const char *d = compass_dirs[dir_idx];
    comp[0] = d[0];
    comp[1] = d[1];
    comp[2] = '0' + (deg / 100) % 10;
    comp[3] = '0' + (deg / 10) % 10;
    comp[4] = '0' + deg % 10;
    comp[5] = '\0';
    fb_text_puts(12, 8, comp);

    /* Ship name */
    fb_text_puts(20, 8, ship_names[g->ship_idx]);

    /* Mini radar box (x 0-15, y 54-62) */
    fb_line(0, 54, 15, 54);
    fb_line(0, 62, 15, 62);
    fb_line(0, 54, 0, 62);
    fb_line(15, 54, 15, 62);

    /* Center cross */
    fb_set_pixel(7, 58);
    fb_set_pixel(8, 58);

    /* Ship dot based on yaw offset */
    int rx = 8 + (((int8_t)g->yaw) >> 5);
    if (rx < 1) rx = 1;
    if (rx > 14) rx = 14;
    fb_set_pixel(rx, 57);
    fb_set_pixel(rx, 58);
}

/* ====== Game loop ====== */

void game_elite(void)
{
    game_state_t g;

    /* Zero-init */
    {
        char *p = (char *)&g;
        for (unsigned i = 0; i < sizeof(g); i++) p[i] = 0;
    }

    g.speed = 3;
    g.ship_idx = 0;
    g.rng_seed = get_tick_count();
    stars_init(&g);

    uart_puts("elite: a/d=yaw w/s=pitch n=ship Ctrl+C=exit\n");

    for (;;) {
        /* 1. Input — consume events */
        input_event_t ev;
        while ((ev = input_poll()) != INPUT_NONE) {
            if (ev == INPUT_LEFT)  g.yaw -= 4;
            if (ev == INPUT_RIGHT) g.yaw += 4;
            if (ev == INPUT_PRESS) {
                /* Cycle ship on button press */
                g.ship_idx = (g.ship_idx + 1) % SHIP_COUNT;
            }
        }

        /* Pitch: button held → pitch up, else slow auto-drift */
        uint32_t state = input_get_state();
        uint8_t btn = (state >> 16) & 1;
        if (btn)
            g.pitch -= 2;
        else if ((g.frame_count & 3) == 0)
            g.pitch += 1;  /* slow auto-drift every 4 frames */

        /* UART keyboard controls */
        while (uart_rx_available()) {
            int ch = uart_getc();
            switch (ch) {
            case 0x03: goto done;       /* Ctrl+C */
            case 'a':  g.yaw -= 4;  break;
            case 'd':  g.yaw += 4;  break;
            case 'w':  g.pitch -= 3; break;
            case 's':  g.pitch += 3; break;
            case 'n':  g.ship_idx = (g.ship_idx + 1) % SHIP_COUNT; break;
            }
        }

        /* 2. Render */
        fb_clear();

        /* Starfield */
        stars_update(&g);

        /* Ship wireframe */
        mat3_t rx, ry, rot;
        mat3_rotate_x(&rx, g.pitch);
        mat3_rotate_y(&ry, g.yaw);
        mat3_multiply(&rot, &ry, &rx);

        fix16_t z = (g.ship_idx == 3) ? FIX16(8) : GAME_SHIP_Z;
        vec3_t pos = vec3(0, 0, z);
        wire_render(ship_list[g.ship_idx], &rot, pos, GAME_FOCAL);

        /* HUD */
        hud_draw(&g);

        /* 3. Flush + yield */
        fb_flush();
        g.frame_count++;
        task_yield();
    }

done:
    uart_puts("elite: ");
    uart_put_dec(g.frame_count);
    uart_puts(" frames\n");
}

} /* extern "C" */
