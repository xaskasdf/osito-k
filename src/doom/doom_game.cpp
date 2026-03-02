/*
 * OsitoK - DOOM wireframe game loop
 *
 * Entry point: game_doom() — called from shell "doom" command.
 * Generates a procedural level, then loops: input → move → render → flush.
 *
 * Controls:
 *   WASD        Move/strafe (UART keyboard)
 *   Joystick    Left/Right = rotate, analog = forward/back
 *   Button/M    Toggle minimap
 *   Ctrl+C      Exit
 *
 * doom_state_t is static (BSS) — too large for 1.5KB task stack.
 */

#include "doom/doom.h"
#include "drivers/uart.h"
#include "drivers/video.h"
#include "drivers/input.h"
#include "kernel/task.h"

extern "C" {

/* Static game state in BSS (~1.4KB) — does NOT fit on stack */
static doom_state_t g_doom;

/* ====== Movement constants (fix16) ====== */

#define MOVE_SPEED      FIX16_C(0.15)
#define STRAFE_SPEED    FIX16_C(0.12)
#define TURN_RATE       4           /* angle units per input */
#define COLLISION_RADIUS FIX16_C(0.8)

/* Enemy AI constants */
#define ENEMY_SPEED     FIX16_C(0.06)
#define ENEMY_SIGHT     FIX16(20)   /* detection range */
#define ENEMY_ATTACK_R  FIX16(8)    /* attack range */
#define ENEMY_ATK_TIME  30          /* frames between attacks */
#define ENEMY_HURT_TIME 10          /* hurt stun frames */
#define SHOOT_COOLDOWN  6           /* frames between shots */
#define DOOM_FOCAL      48          /* must match doom_render.cpp */
#define SCREEN_CX       64

/* ====== Collision detection ====== */

/*
 * Check if point (px,py) is too close to linedef i.
 * Uses point-to-segment distance approximation.
 * Returns 1 if collision detected.
 */
static int line_collides(dlevel_t *lv, uint8_t i, fix16_t px, fix16_t py)
{
    dlinedef_t *line = &lv->lines[i];
    if (!(line->flags & DLINE_SOLID)) return 0;

    fix16_t ax = FIX16(lv->verts[line->v1].x);
    fix16_t ay = FIX16(lv->verts[line->v1].y);
    fix16_t bx = FIX16(lv->verts[line->v2].x);
    fix16_t by = FIX16(lv->verts[line->v2].y);

    /* Vector AB */
    fix16_t abx = bx - ax;
    fix16_t aby = by - ay;

    /* Vector AP */
    fix16_t apx = px - ax;
    fix16_t apy = py - ay;

    /* Project AP onto AB: t = dot(AP,AB) / dot(AB,AB) */
    fix16_t ab_dot = fix_mul(abx, abx) + fix_mul(aby, aby);
    if (ab_dot == 0) return 0;

    fix16_t t = fix_div(fix_mul(apx, abx) + fix_mul(apy, aby), ab_dot);

    /* Clamp t to [0, 1] */
    if (t < 0) t = 0;
    if (t > FIX16_ONE) t = FIX16_ONE;

    /* Closest point on segment */
    fix16_t cx = ax + fix_mul(t, abx);
    fix16_t cy = ay + fix_mul(t, aby);

    /* Distance from P to closest point */
    fix16_t dist = fix_dist_approx(px - cx, py - cy);

    return dist < COLLISION_RADIUS;
}

/* ====== Movement with collision ====== */

void doom_move(doom_state_t *st, fix16_t dx, fix16_t dy)
{
    dlevel_t *lv = &st->level;
    fix16_t nx = st->player.x + dx;
    fix16_t ny = st->player.y + dy;

    /* Check collision with all solid linedefs */
    int blocked = 0;
    for (uint8_t i = 0; i < lv->num_lines; i++) {
        if (line_collides(lv, i, nx, ny)) {
            blocked = 1;
            break;
        }
    }

    if (!blocked) {
        st->player.x = nx;
        st->player.y = ny;
        return;
    }

    /* Wall sliding: try X only */
    blocked = 0;
    nx = st->player.x + dx;
    ny = st->player.y;
    for (uint8_t i = 0; i < lv->num_lines; i++) {
        if (line_collides(lv, i, nx, ny)) {
            blocked = 1;
            break;
        }
    }
    if (!blocked) {
        st->player.x = nx;
        return;
    }

    /* Try Y only */
    blocked = 0;
    nx = st->player.x;
    ny = st->player.y + dy;
    for (uint8_t i = 0; i < lv->num_lines; i++) {
        if (line_collides(lv, i, nx, ny)) {
            blocked = 1;
            break;
        }
    }
    if (!blocked) {
        st->player.y = ny;
    }
    /* Fully blocked: don't move */
}

/* ====== Shooting (hitscan) ====== */

static void doom_shoot(doom_state_t *st)
{
    if (st->muzzle_flash > 0) return;  /* cooldown */
    st->muzzle_flash = SHOOT_COOLDOWN;

    /* Find closest enemy in crosshair */
    fix16_t best_z = 0x7FFFFFFF;
    int best_idx = -1;

    fix16_t cs = fix_cos(st->player.angle);
    fix16_t sn = fix_sin(st->player.angle);

    for (uint8_t i = 0; i < st->num_enemies; i++) {
        denemy_t *e = &st->enemies[i];
        if (e->state == DENEMY_DEAD) continue;

        /* Transform to view space */
        fix16_t dx = e->x - st->player.x;
        fix16_t dy = e->y - st->player.y;
        fix16_t vx = fix_mul(dx, cs) + fix_mul(dy, sn);
        fix16_t vz = fix_mul(dy, cs) - fix_mul(dx, sn);

        if (vz < FIX16(1)) continue;  /* behind camera */

        /* Project to screen X */
        int sx = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(vx, DOOM_FOCAL), vz));

        /* Check if within crosshair (±12 pixels from center) */
        int diff = sx - SCREEN_CX;
        if (diff < 0) diff = -diff;
        if (diff > 12) continue;

        /* Closest hit wins */
        if (vz < best_z) {
            best_z = vz;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        denemy_t *e = &st->enemies[best_idx];
        e->hp--;
        if (e->hp == 0) {
            e->state = DENEMY_DEAD;
            st->kill_count++;
        } else {
            e->state = DENEMY_HURT;
            e->timer = ENEMY_HURT_TIME;
        }
    }
}

/* ====== Enemy AI ====== */

void doom_update_enemies(doom_state_t *st)
{
    for (uint8_t i = 0; i < st->num_enemies; i++) {
        denemy_t *e = &st->enemies[i];
        if (e->state == DENEMY_DEAD) continue;

        fix16_t dx = st->player.x - e->x;
        fix16_t dy = st->player.y - e->y;
        fix16_t dist = fix_dist_approx(dx, dy);

        switch (e->state) {
        case DENEMY_IDLE:
            if (dist < ENEMY_SIGHT)
                e->state = DENEMY_CHASE;
            break;

        case DENEMY_CHASE:
            if (dist < ENEMY_ATTACK_R) {
                e->state = DENEMY_ATTACK;
                e->timer = ENEMY_ATK_TIME;
            } else if (dist > 0) {
                /* Move toward player */
                fix16_t inv = fix_div(ENEMY_SPEED, dist);
                e->x += fix_mul(dx, inv);
                e->y += fix_mul(dy, inv);
            }
            break;

        case DENEMY_ATTACK:
            if (e->timer > 0) {
                e->timer--;
            } else {
                /* Deal damage to player */
                if (st->player_hp > 0) {
                    st->player_hp--;
                    st->damage_flash = 4;
                }
                if (st->player_hp == 0)
                    st->game_over = 1;
                e->timer = ENEMY_ATK_TIME;
                e->state = DENEMY_CHASE;
            }
            /* Stay close to player during attack */
            if (dist > ENEMY_ATTACK_R + FIX16(2)) {
                e->state = DENEMY_CHASE;
            }
            break;

        case DENEMY_HURT:
            if (e->timer > 0) {
                e->timer--;
            } else {
                e->state = DENEMY_CHASE;
            }
            break;
        }
    }
}

/* ====== Game over screen ====== */

static void draw_game_over(doom_state_t *st)
{
    fb_text_puts(10, 3, "GAME  OVER");
    char buf[12];
    buf[0] = 'K'; buf[1] = 'I'; buf[2] = 'L'; buf[3] = 'L';
    buf[4] = 'S'; buf[5] = ':';
    buf[6] = '0' + (st->kill_count / 10) % 10;
    buf[7] = '0' + st->kill_count % 10;
    buf[8] = '\0';
    fb_text_puts(12, 5, buf);
}

/* ====== HUD ====== */

static void doom_hud(doom_state_t *st)
{
    /* HP in top-left */
    char hp_buf[6];
    hp_buf[0] = 'H'; hp_buf[1] = 'P';
    hp_buf[2] = '0' + (st->player_hp / 10) % 10;
    hp_buf[3] = '0' + st->player_hp % 10;
    hp_buf[4] = '\0';
    fb_text_puts(0, 0, hp_buf);

    /* Kill count */
    char kbuf[5];
    kbuf[0] = 'K';
    kbuf[1] = '0' + (st->kill_count / 10) % 10;
    kbuf[2] = '0' + st->kill_count % 10;
    kbuf[3] = '\0';
    fb_text_puts(0, 9, kbuf);

    /* Angle in top-right corner */
    uint16_t deg = ((uint16_t)st->player.angle * 360) >> 8;
    char buf[5];
    buf[0] = '0' + (deg / 100) % 10;
    buf[1] = '0' + (deg / 10) % 10;
    buf[2] = '0' + deg % 10;
    buf[3] = '\0';
    fb_text_puts(28, 0, buf);
}

/* ====== Game loop ====== */

void game_doom(void)
{
    doom_state_t *st = &g_doom;

    /* Zero-init */
    {
        char *p = (char *)st;
        for (unsigned i = 0; i < sizeof(doom_state_t); i++) p[i] = 0;
    }

    /* Generate level */
    st->rng_seed = get_tick_count();
    doom_generate(st, st->rng_seed);

    /* Place player at spawn */
    st->player.x = FIX16(st->level.spawn_x);
    st->player.y = FIX16(st->level.spawn_y);
    st->player.angle = st->level.spawn_angle;
    st->show_map = 0;
    st->player_hp = 10;
    st->kill_count = 0;
    st->muzzle_flash = 0;
    st->damage_flash = 0;
    st->game_over = 0;

    uart_puts("doom: WASD=move f=shoot m=map Ctrl+C=exit\n");
    uart_puts("  verts=");
    uart_put_dec(st->level.num_verts);
    uart_puts(" lines=");
    uart_put_dec(st->level.num_lines);
    uart_puts(" sectors=");
    uart_put_dec(st->level.num_sectors);
    uart_puts(" enemies=");
    uart_put_dec(st->num_enemies);
    uart_puts("\n");

    for (;;) {
        /* 1. Input — joystick events */
        input_event_t ev;
        while ((ev = input_poll()) != INPUT_NONE) {
            if (ev == INPUT_LEFT)  st->player.angle += TURN_RATE;
            if (ev == INPUT_RIGHT) st->player.angle -= TURN_RATE;
            if (ev == INPUT_PRESS) st->show_map ^= 1;
        }

        /* UART keyboard
         * Forward direction in world = (-sin(angle), cos(angle))
         * Right direction in world  = (cos(angle), sin(angle))
         * angle increases counterclockwise
         */
        while (uart_rx_available()) {
            int ch = uart_getc();
            switch (ch) {
            case 0x03: goto done;   /* Ctrl+C */

            case 'w': {
                /* Move forward: (-sin, +cos) */
                fix16_t sn = fix_sin(st->player.angle);
                fix16_t cs = fix_cos(st->player.angle);
                doom_move(st, -fix_mul(sn, MOVE_SPEED), fix_mul(cs, MOVE_SPEED));
                break;
            }
            case 's': {
                /* Move backward: (+sin, -cos) */
                fix16_t sn = fix_sin(st->player.angle);
                fix16_t cs = fix_cos(st->player.angle);
                doom_move(st, fix_mul(sn, MOVE_SPEED), -fix_mul(cs, MOVE_SPEED));
                break;
            }
            case 'a':
                /* Turn left (counterclockwise) */
                st->player.angle += TURN_RATE;
                break;
            case 'd':
                /* Turn right (clockwise) */
                st->player.angle -= TURN_RATE;
                break;
            case 'q': {
                /* Strafe left: (-cos, -sin) */
                fix16_t cs = fix_cos(st->player.angle);
                fix16_t sn = fix_sin(st->player.angle);
                doom_move(st, -fix_mul(cs, STRAFE_SPEED), -fix_mul(sn, STRAFE_SPEED));
                break;
            }
            case 'e': {
                /* Strafe right: (+cos, +sin) */
                fix16_t cs = fix_cos(st->player.angle);
                fix16_t sn = fix_sin(st->player.angle);
                doom_move(st, fix_mul(cs, STRAFE_SPEED), fix_mul(sn, STRAFE_SPEED));
                break;
            }
            case 'f':
            case ' ':
                if (!st->game_over)
                    doom_shoot(st);
                break;
            case 'm':
                st->show_map ^= 1;
                break;
            case 'r':
                /* Regenerate level with new seed */
                st->rng_seed = get_tick_count();
                doom_generate(st, st->rng_seed);
                st->player.x = FIX16(st->level.spawn_x);
                st->player.y = FIX16(st->level.spawn_y);
                st->player.angle = st->level.spawn_angle;
                st->player_hp = 10;
                st->kill_count = 0;
                st->game_over = 0;
                st->muzzle_flash = 0;
                st->damage_flash = 0;
                uart_puts("doom: new level\n");
                break;
            }
        }

        /* Joystick analog → forward/backward */
        uint32_t state = input_get_state();
        uint16_t jx = state & 0xFFFF;
        if (jx < 300) {
            fix16_t sn = fix_sin(st->player.angle);
            fix16_t cs = fix_cos(st->player.angle);
            doom_move(st, -fix_mul(sn, MOVE_SPEED), fix_mul(cs, MOVE_SPEED));
        } else if (jx > 700) {
            fix16_t sn = fix_sin(st->player.angle);
            fix16_t cs = fix_cos(st->player.angle);
            doom_move(st, fix_mul(sn, MOVE_SPEED), -fix_mul(cs, MOVE_SPEED));
        }

        /* 1.5. Update AI + flash timers */
        if (!st->game_over) {
            doom_update_enemies(st);
        }
        if (st->muzzle_flash > 0) st->muzzle_flash--;
        if (st->damage_flash > 0) st->damage_flash--;

        /* 2. Render */
        fb_clear();

        if (st->game_over) {
            draw_game_over(st);
        } else if (st->show_map) {
            doom_minimap(st);
        } else {
            doom_render(st);
            doom_hud(st);
        }

        /* 3. Flush + yield */
        fb_flush();
        st->frame_count++;
        task_yield();
    }

done:
    uart_puts("doom: ");
    uart_put_dec(st->frame_count);
    uart_puts(" frames\n");
}

} /* extern "C" */
