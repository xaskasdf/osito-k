/*
 * OsitoK - DOOM 2.5D wireframe renderer
 *
 * Wall-segment projection (like id DOOM, not raycasting):
 * 1. Transform each linedef's vertices to view-space
 * 2. Clip to near plane
 * 3. Project X and Y to screen coordinates
 * 4. Draw wireframe: top edge, bottom edge, 2 vertical edges
 *
 * Also includes a top-down minimap renderer.
 */

#include "doom/doom.h"
#include "drivers/video.h"

extern "C" {

/* Rendering constants */
#define DOOM_FOCAL    48        /* focal length (pixels) */
#define NEAR_CLIP     1         /* near plane (map units, integer) */
#define SCREEN_CX     64        /* screen center X */
#define SCREEN_CY     32        /* screen center Y */

/* ====== View-space transform ====== */

/*
 * Transform world point (wx,wy) to view-space (vx,vz) relative to player.
 * vx = right, vz = forward (into screen).
 * Uses fix16 trig for rotation.
 */
static void world_to_view(const dplayer_t *pl,
                           int16_t wx, int16_t wy,
                           fix16_t *vx, fix16_t *vz)
{
    fix16_t dx = FIX16(wx) - pl->x;
    fix16_t dy = FIX16(wy) - pl->y;

    fix16_t cs = fix_cos(pl->angle);
    fix16_t sn = fix_sin(pl->angle);

    *vx = fix_mul(dx, cs) + fix_mul(dy, sn);
    *vz = fix_mul(dy, cs) - fix_mul(dx, sn);
}

/* fix16 overload for enemy positions */
static void world_to_view_f(const dplayer_t *pl,
                             fix16_t wx, fix16_t wy,
                             fix16_t *vx, fix16_t *vz)
{
    fix16_t dx = wx - pl->x;
    fix16_t dy = wy - pl->y;

    fix16_t cs = fix_cos(pl->angle);
    fix16_t sn = fix_sin(pl->angle);

    *vx = fix_mul(dx, cs) + fix_mul(dy, sn);
    *vz = fix_mul(dy, cs) - fix_mul(dx, sn);
}

/* ====== Wall rendering ====== */

/*
 * Project and draw a single wall segment.
 * v1x,v1z and v2x,v2z are view-space endpoints (fix16).
 * floor_h, ceil_h are the wall's floor/ceiling in map units.
 */
static void draw_wall(fix16_t v1x, fix16_t v1z,
                      fix16_t v2x, fix16_t v2z,
                      int8_t floor_h, int8_t ceil_h)
{
    fix16_t near = FIX16(NEAR_CLIP);

    /* Both behind camera? Skip. */
    if (v1z < near && v2z < near)
        return;

    /* Clip to near plane */
    if (v1z < near) {
        /* Interpolate v1 toward v2 */
        fix16_t t = fix_div(near - v1z, v2z - v1z);
        v1x = v1x + fix_mul(t, v2x - v1x);
        v1z = near;
    } else if (v2z < near) {
        fix16_t t = fix_div(near - v2z, v1z - v2z);
        v2x = v2x + fix_mul(t, v1x - v2x);
        v2z = near;
    }

    /* Project X: sx = CX + (vx * FOCAL) / vz */
    int sx1 = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(v1x, DOOM_FOCAL), v1z));
    int sx2 = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(v2x, DOOM_FOCAL), v2z));

    /* Reject if entirely off screen horizontally */
    if ((sx1 < 0 && sx2 < 0) || (sx1 >= 128 && sx2 >= 128))
        return;

    /* Project Y for each endpoint.
     * Floor and ceiling heights are relative to player eye (height 5).
     * sy = CY - (h_rel * FOCAL) / vz
     * Player eye is at height 5 map units.
     */
    int eye_h = 5;

    int floor_rel = floor_h - eye_h;
    int ceil_rel  = ceil_h - eye_h;

    /* Endpoint 1: sy = CY - (h_rel * FOCAL) / vz */
    int y1_top = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(ceil_rel * DOOM_FOCAL), v1z));
    int y1_bot = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(floor_rel * DOOM_FOCAL), v1z));

    /* Endpoint 2 */
    int y2_top = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(ceil_rel * DOOM_FOCAL), v2z));
    int y2_bot = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(floor_rel * DOOM_FOCAL), v2z));

    /* Draw wireframe quad: top, bottom, left vertical, right vertical */
    fb_line(sx1, y1_top, sx2, y2_top);  /* ceiling edge */
    fb_line(sx1, y1_bot, sx2, y2_bot);  /* floor edge */
    fb_line(sx1, y1_top, sx1, y1_bot);  /* left vertical */
    fb_line(sx2, y2_top, sx2, y2_bot);  /* right vertical */
}

/* ====== Portal step rendering ====== */

/*
 * Draw a portal (opening between sectors with different heights).
 * Shows step/ceiling transitions as additional wireframe lines.
 */
static void draw_portal(fix16_t v1x, fix16_t v1z,
                        fix16_t v2x, fix16_t v2z,
                        const dsector_t *front, const dsector_t *back)
{
    fix16_t near = FIX16(NEAR_CLIP);

    if (v1z < near && v2z < near)
        return;

    if (v1z < near) {
        fix16_t t = fix_div(near - v1z, v2z - v1z);
        v1x = v1x + fix_mul(t, v2x - v1x);
        v1z = near;
    } else if (v2z < near) {
        fix16_t t = fix_div(near - v2z, v1z - v2z);
        v2x = v2x + fix_mul(t, v1x - v2x);
        v2z = near;
    }

    int sx1 = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(v1x, DOOM_FOCAL), v1z));
    int sx2 = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(v2x, DOOM_FOCAL), v2z));

    if ((sx1 < 0 && sx2 < 0) || (sx1 >= 128 && sx2 >= 128))
        return;

    int eye_h = 5;

    /* Draw floor step if back sector floor is higher */
    if (back->floor_h > front->floor_h) {
        int step_rel = back->floor_h - eye_h;
        int y1 = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(step_rel * DOOM_FOCAL), v1z));
        int y2 = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(step_rel * DOOM_FOCAL), v2z));
        fb_line(sx1, y1, sx2, y2);
    }

    /* Draw ceiling step if back sector ceiling is lower */
    if (back->ceil_h < front->ceil_h) {
        int step_rel = back->ceil_h - eye_h;
        int y1 = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(step_rel * DOOM_FOCAL), v1z));
        int y2 = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(step_rel * DOOM_FOCAL), v2z));
        fb_line(sx1, y1, sx2, y2);
    }
}

/* ====== Enemy sprite rendering ====== */

static void draw_enemy(const dplayer_t *pl, const denemy_t *e)
{
    if (e->state == DENEMY_DEAD) {
        /* Dead: just a flat line on the ground */
        fix16_t vx, vz;
        world_to_view_f(pl, e->x, e->y, &vx, &vz);
        if (vz < FIX16(NEAR_CLIP)) return;

        int sx = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(vx, DOOM_FOCAL), vz));
        int size = FIX16_TO_INT(fix_div(FIX16(DOOM_FOCAL * 3), vz));
        if (size < 1) size = 1;

        /* Floor-level Y: eye_h=5, floor=0 → rel=-5 */
        int sy = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(-5 * DOOM_FOCAL), vz));
        fb_line(sx - size, sy, sx + size, sy);
        return;
    }

    fix16_t vx, vz;
    world_to_view_f(pl, e->x, e->y, &vx, &vz);
    if (vz < FIX16(NEAR_CLIP)) return;

    int sx = SCREEN_CX + FIX16_TO_INT(fix_div(fix_mul_int(vx, DOOM_FOCAL), vz));
    int size = FIX16_TO_INT(fix_div(FIX16(DOOM_FOCAL * 3), vz));
    if (size < 1) size = 1;
    if (size > 30) size = 30;

    /* Center Y: enemy stands on floor (height ~3 map units → center at 1.5) */
    int eye_h = 5;
    int enemy_center_rel = 2 - eye_h;  /* center of enemy body relative to eye */
    int sy = SCREEN_CY - FIX16_TO_INT(fix_div(FIX16(enemy_center_rel * DOOM_FOCAL), vz));

    /* Hurt: shake offset */
    int ox = 0;
    if (e->state == DENEMY_HURT && (e->timer & 1))
        ox = (e->timer & 2) ? 2 : -2;

    /* Diamond wireframe sprite */
    int top  = sy - size;
    int bot  = sy + size;
    int left = sx - size + ox;
    int right = sx + size + ox;

    fb_line(sx + ox, top, right, sy);     /* top-right */
    fb_line(right, sy, sx + ox, bot);     /* bottom-right */
    fb_line(sx + ox, bot, left, sy);      /* bottom-left */
    fb_line(left, sy, sx + ox, top);      /* top-left */

    /* Attack: extra horizontal line (arms) */
    if (e->state == DENEMY_ATTACK) {
        int arm = size + size / 2;
        fb_line(sx - arm + ox, sy, sx + arm + ox, sy);
    }
}

/* ====== Weapon overlay ====== */

static void doom_weapon(uint8_t muzzle_flash)
{
    /* Crosshair */
    fb_line(62, 32, 66, 32);
    fb_line(64, 30, 64, 34);

    /* Wireframe pistol at bottom center */
    /* Barrel */
    fb_line(60, 40, 68, 40);
    fb_line(60, 40, 60, 50);
    fb_line(68, 40, 68, 50);
    /* Slide */
    fb_line(56, 50, 72, 50);
    fb_line(56, 50, 56, 54);
    fb_line(72, 50, 72, 54);
    fb_line(56, 54, 60, 54);
    fb_line(68, 54, 72, 54);
    /* Grip */
    fb_line(60, 54, 60, 63);
    fb_line(68, 54, 68, 63);
    fb_line(60, 63, 68, 63);

    /* Muzzle flash: radial lines from barrel tip */
    if (muzzle_flash > 0) {
        fb_line(64, 38, 64, 30);   /* up */
        fb_line(64, 38, 56, 34);   /* up-left */
        fb_line(64, 38, 72, 34);   /* up-right */
        fb_line(64, 38, 54, 38);   /* left */
        fb_line(64, 38, 74, 38);   /* right */
    }
}

/* ====== Main render function ====== */

void doom_render(doom_state_t *st)
{
    dlevel_t *lv = &st->level;
    dplayer_t *pl = &st->player;

    for (uint8_t i = 0; i < lv->num_lines; i++) {
        dlinedef_t *line = &lv->lines[i];

        /* Transform endpoints to view space */
        fix16_t v1x, v1z, v2x, v2z;
        world_to_view(pl, lv->verts[line->v1].x, lv->verts[line->v1].y,
                      &v1x, &v1z);
        world_to_view(pl, lv->verts[line->v2].x, lv->verts[line->v2].y,
                      &v2x, &v2z);

        if (line->flags & DLINE_SOLID) {
            /* Solid wall: draw full wireframe quad */
            dsector_t *sec = &lv->sectors[line->front_sec];
            draw_wall(v1x, v1z, v2x, v2z, sec->floor_h, sec->ceil_h);
        } else if (line->flags & DLINE_PORTAL) {
            /* Portal: draw height transitions */
            if (line->front_sec < lv->num_sectors &&
                line->back_sec < lv->num_sectors) {
                draw_portal(v1x, v1z, v2x, v2z,
                           &lv->sectors[line->front_sec],
                           &lv->sectors[line->back_sec]);
            }
        }
    }

    /* Draw enemies */
    for (uint8_t i = 0; i < st->num_enemies; i++) {
        draw_enemy(pl, &st->enemies[i]);
    }

    /* Weapon overlay (drawn last, on top of everything) */
    doom_weapon(st->muzzle_flash);

    /* Damage flash: border rectangle */
    if (st->damage_flash > 0) {
        fb_line(0, 0, 127, 0);
        fb_line(127, 0, 127, 63);
        fb_line(127, 63, 0, 63);
        fb_line(0, 63, 0, 0);
    }
}

/* ====== Minimap ====== */

void doom_minimap(doom_state_t *st)
{
    dlevel_t *lv = &st->level;

    /* Map fills entire framebuffer: 128x64 pixels.
     * Map is 64x64 units → 2 pixels per unit for X, 1 pixel per unit for Y.
     */
    int scale_x = 2;   /* pixels per map unit, X */
    /* Y: 64 units in 64 pixels = 1:1 */

    /* Draw all linedefs */
    for (uint8_t i = 0; i < lv->num_lines; i++) {
        dlinedef_t *line = &lv->lines[i];
        int x1 = lv->verts[line->v1].x * scale_x;
        int y1 = lv->verts[line->v1].y;
        int x2 = lv->verts[line->v2].x * scale_x;
        int y2 = lv->verts[line->v2].y;

        /* Only draw solid walls as full lines; portals as dotted */
        if (line->flags & DLINE_SOLID) {
            fb_line(x1, y1, x2, y2);
        } else {
            /* Draw portal as midpoint dot */
            int mx = (x1 + x2) / 2;
            int my = (y1 + y2) / 2;
            if (mx >= 0 && mx < 128 && my >= 0 && my < 64)
                fb_set_pixel(mx, my);
        }
    }

    /* Draw player position + direction */
    int px = FIX16_TO_INT(st->player.x) * scale_x;
    int py = FIX16_TO_INT(st->player.y);

    if (px >= 1 && px < 127 && py >= 1 && py < 63) {
        /* Player dot (3x3 cross) */
        fb_set_pixel(px, py);
        fb_set_pixel(px - 1, py);
        fb_set_pixel(px + 1, py);
        fb_set_pixel(px, py - 1);
        fb_set_pixel(px, py + 1);

        /* Direction line (4 pixels in facing direction) */
        fix16_t cs = fix_cos(st->player.angle);
        fix16_t sn = fix_sin(st->player.angle);
        int dx = FIX16_TO_INT(fix_mul_int(sn, 4 * scale_x));
        int dy = -FIX16_TO_INT(fix_mul_int(cs, 4));
        fb_line(px, py, px + dx, py + dy);
    }

    /* Draw enemies on minimap */
    for (uint8_t i = 0; i < st->num_enemies; i++) {
        denemy_t *e = &st->enemies[i];
        if (e->state == DENEMY_DEAD) continue;
        int ex = FIX16_TO_INT(e->x) * scale_x;
        int ey = FIX16_TO_INT(e->y);
        if (ex >= 1 && ex < 127 && ey >= 1 && ey < 63) {
            /* X mark for enemies */
            fb_set_pixel(ex - 1, ey - 1);
            fb_set_pixel(ex + 1, ey - 1);
            fb_set_pixel(ex, ey);
            fb_set_pixel(ex - 1, ey + 1);
            fb_set_pixel(ex + 1, ey + 1);
        }
    }

    /* Label */
    fb_text_puts(0, 0, "MAP");
}

} /* extern "C" */
