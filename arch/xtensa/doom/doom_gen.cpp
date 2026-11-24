/*
 * OsitoK - DOOM procedural level generator
 *
 * Grid-based room layout: 4x4 grid of rooms, each 16x16 map units.
 * Connectivity guaranteed via snake path through all rooms,
 * plus random extra connections for variety.
 *
 * Produces 16 rooms with ~64 vertices and ~80-100 linedefs.
 */

#include "doom/doom.h"
#include "kernel/task.h"

extern "C" {

/* ====== Grid parameters ====== */

#define GRID_COLS  4
#define GRID_ROWS  4
#define CELL_SIZE  16   /* map units per cell (16*4 = 64) */

/* ====== PRNG ====== */

static uint32_t doom_rand(doom_state_t *st)
{
    st->rng_seed ^= st->rng_seed << 13;
    st->rng_seed ^= st->rng_seed >> 17;
    st->rng_seed ^= st->rng_seed << 5;
    return st->rng_seed;
}

static int16_t doom_rand_range(doom_state_t *st, int16_t lo, int16_t hi)
{
    if (lo >= hi) return lo;
    return lo + (int16_t)(doom_rand(st) % (uint32_t)(hi - lo + 1));
}

/* ====== Vertex/line/sector helpers ====== */

static uint8_t add_vert(dlevel_t *lv, int16_t x, int16_t y)
{
    /* Dedup: reuse vertex if within 1 unit */
    for (uint8_t i = 0; i < lv->num_verts; i++) {
        int16_t dx = lv->verts[i].x - x;
        int16_t dy = lv->verts[i].y - y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
            return i;
    }
    if (lv->num_verts >= DOOM_MAX_VERTS) return 0;
    uint8_t idx = lv->num_verts++;
    lv->verts[idx].x = x;
    lv->verts[idx].y = y;
    return idx;
}

static void add_line(dlevel_t *lv, uint8_t v1, uint8_t v2, uint8_t flags,
                     uint8_t front_sec, uint8_t back_sec)
{
    if (lv->num_lines >= DOOM_MAX_LINES || v1 == v2) return;
    dlinedef_t *l = &lv->lines[lv->num_lines++];
    l->v1 = v1;
    l->v2 = v2;
    l->flags = flags;
    l->front_sec = front_sec;
    l->back_sec = back_sec;
}

static uint8_t add_sector(dlevel_t *lv, int8_t floor_h, int8_t ceil_h)
{
    if (lv->num_sectors >= DOOM_MAX_SECTORS) return 0;
    uint8_t idx = lv->num_sectors++;
    lv->sectors[idx].floor_h = floor_h;
    lv->sectors[idx].ceil_h = ceil_h;
    return idx;
}

/* ====== Room creation ====== */

/*
 * Create a rectangular room. No inset — walls exactly at cell boundaries
 * so adjacent rooms share vertices (via add_vert dedup).
 */
static uint8_t make_room(doom_state_t *st, dlevel_t *lv,
                         int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int8_t floor_h = (int8_t)doom_rand_range(st, 0, 2);
    int8_t ceil_h  = (int8_t)doom_rand_range(st, 8, 12);
    uint8_t sec = add_sector(lv, floor_h, ceil_h);

    uint8_t v_tl = add_vert(lv, x0, y0);
    uint8_t v_tr = add_vert(lv, x1, y0);
    uint8_t v_br = add_vert(lv, x1, y1);
    uint8_t v_bl = add_vert(lv, x0, y1);

    add_line(lv, v_tl, v_tr, DLINE_SOLID, sec, 0xFF);  /* top */
    add_line(lv, v_tr, v_br, DLINE_SOLID, sec, 0xFF);  /* right */
    add_line(lv, v_br, v_bl, DLINE_SOLID, sec, 0xFF);  /* bottom */
    add_line(lv, v_bl, v_tl, DLINE_SOLID, sec, 0xFF);  /* left */

    return sec;
}

/* ====== Room connection ====== */

/*
 * Open a wall between two rooms by finding their shared wall segment
 * and converting it to a portal. Since rooms share exact boundary vertices
 * (via add_vert dedup), matching walls have the same vertex indices.
 */
static void open_wall(dlevel_t *lv, uint8_t sec_a, uint8_t sec_b)
{
    for (uint8_t i = 0; i < lv->num_lines; i++) {
        dlinedef_t *la = &lv->lines[i];
        if (la->front_sec != sec_a || !(la->flags & DLINE_SOLID))
            continue;

        for (uint8_t j = 0; j < lv->num_lines; j++) {
            dlinedef_t *lb = &lv->lines[j];
            if (lb->front_sec != sec_b || !(lb->flags & DLINE_SOLID))
                continue;

            /* Check if walls share same vertices (either order) */
            if ((la->v1 == lb->v1 && la->v2 == lb->v2) ||
                (la->v1 == lb->v2 && la->v2 == lb->v1)) {
                /* Convert both to portals */
                la->flags = DLINE_PORTAL;
                la->back_sec = sec_b;
                lb->flags = DLINE_PORTAL;
                lb->back_sec = sec_a;
                return;
            }
        }
    }
}

/* ====== Public API ====== */

void doom_generate(doom_state_t *st, uint32_t seed)
{
    dlevel_t *lv = &st->level;
    lv->num_verts = 0;
    lv->num_lines = 0;
    lv->num_sectors = 0;

    st->rng_seed = seed ? seed : 42;

    /* Create 4x4 grid of rooms */
    uint8_t grid[GRID_COLS][GRID_ROWS];

    for (int gy = 0; gy < GRID_ROWS; gy++) {
        for (int gx = 0; gx < GRID_COLS; gx++) {
            int16_t x0 = (int16_t)(gx * CELL_SIZE);
            int16_t y0 = (int16_t)(gy * CELL_SIZE);
            int16_t x1 = x0 + CELL_SIZE;
            int16_t y1 = y0 + CELL_SIZE;
            grid[gx][gy] = make_room(st, lv, x0, y0, x1, y1);
        }
    }

    /*
     * Guaranteed connectivity: snake path through all rooms.
     * Row 0: left→right, Row 1: right→left, etc.
     */
    for (int gy = 0; gy < GRID_ROWS; gy++) {
        /* Horizontal connections within this row */
        for (int gx = 0; gx < GRID_COLS - 1; gx++) {
            open_wall(lv, grid[gx][gy], grid[gx + 1][gy]);
        }
        /* Vertical connection to next row (snake turn) */
        if (gy < GRID_ROWS - 1) {
            int turn_x = (gy & 1) ? 0 : (GRID_COLS - 1);
            open_wall(lv, grid[turn_x][gy], grid[turn_x][gy + 1]);
        }
    }

    /* Random extra connections for loops (makes navigation more interesting) */
    for (int gy = 0; gy < GRID_ROWS; gy++) {
        for (int gx = 0; gx < GRID_COLS; gx++) {
            /* Try to open extra horizontal connection */
            if (gx < GRID_COLS - 1 && (doom_rand(st) % 3) == 0) {
                open_wall(lv, grid[gx][gy], grid[gx + 1][gy]);
            }
            /* Try to open extra vertical connection */
            if (gy < GRID_ROWS - 1 && (doom_rand(st) % 3) == 0) {
                open_wall(lv, grid[gx][gy], grid[gx][gy + 1]);
            }
        }
    }

    /* Spawn at center of room (1,1) — not a corner room */
    lv->spawn_x = 1 * CELL_SIZE + CELL_SIZE / 2;
    lv->spawn_y = 1 * CELL_SIZE + CELL_SIZE / 2;
    lv->spawn_angle = 0;

    /* Place enemies in rooms away from spawn */
    st->num_enemies = 0;
    for (int gy = 0; gy < GRID_ROWS; gy++) {
        for (int gx = 0; gx < GRID_COLS; gx++) {
            if (st->num_enemies >= DOOM_MAX_ENEMIES) break;

            /* Skip spawn room (1,1) and its 4 neighbors */
            int dx = gx - 1;
            int dy = gy - 1;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx + dy <= 1) continue;

            /* 50% chance to place an enemy */
            if (doom_rand(st) & 1) continue;

            denemy_t *e = &st->enemies[st->num_enemies++];
            int16_t cx = gx * CELL_SIZE + CELL_SIZE / 2;
            int16_t cy = gy * CELL_SIZE + CELL_SIZE / 2;
            e->x = FIX16(cx + doom_rand_range(st, -3, 3));
            e->y = FIX16(cy + doom_rand_range(st, -3, 3));
            e->state = DENEMY_IDLE;
            e->hp = 3;
            e->timer = 0;
            e->sector = grid[gx][gy];
        }
    }
}

} /* extern "C" */
