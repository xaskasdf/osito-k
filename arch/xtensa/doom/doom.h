/*
 * OsitoK - DOOM wireframe 2.5D engine
 *
 * Procedurally generated levels rendered as wireframe walls.
 * BSP room partition + wall-segment projection (like id DOOM).
 *
 * Data structures designed for minimal DRAM (~1.4KB static).
 */
#ifndef OSITO_DOOM_H
#define OSITO_DOOM_H

#include "math/fixedpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Limits ====== */

#define DOOM_MAX_VERTS    128
#define DOOM_MAX_LINES    160
#define DOOM_MAX_SECTORS   16
#define DOOM_MAX_ENEMIES    8

/* Enemy states */
#define DENEMY_IDLE    0
#define DENEMY_CHASE   1
#define DENEMY_ATTACK  2
#define DENEMY_HURT    3
#define DENEMY_DEAD    4

/* ====== Data structures ====== */

/* Vertex: world coordinates in fix16 map units */
typedef struct {
    int16_t x, y;             /* integer map coords (enough for ~+-32K) */
} dvertex_t;                  /* 4 bytes */

/* Linedef flags */
#define DLINE_SOLID    0x01   /* blocks movement */
#define DLINE_PORTAL   0x02   /* connects two sectors (walkable) */

/* Line definition: wall segment between two vertices */
typedef struct {
    uint8_t v1, v2;           /* vertex indices */
    uint8_t flags;            /* DLINE_SOLID | DLINE_PORTAL */
    uint8_t front_sec;        /* front sector index */
    uint8_t back_sec;         /* back sector index (0xFF if solid wall) */
} dlinedef_t;                 /* 5 bytes */

/* Sector: floor/ceiling heights */
typedef struct {
    int8_t floor_h;           /* floor height (map units) */
    int8_t ceil_h;            /* ceiling height (map units) */
} dsector_t;                  /* 2 bytes */

/* Enemy */
typedef struct {
    fix16_t x, y;             /* world position (fix16) */
    uint8_t state;            /* DENEMY_IDLE..DEAD */
    uint8_t hp;               /* hit points (3 = normal) */
    uint8_t timer;            /* state timer (frames) */
    uint8_t sector;           /* which sector they're in */
} denemy_t;                   /* 12 bytes */

/* Player state */
typedef struct {
    fix16_t x, y;             /* world position (fix16) */
    angle_t angle;            /* facing direction (0-255) */
    uint8_t _pad;
} dplayer_t;                  /* 10 bytes */

/* Level data */
typedef struct {
    dvertex_t  verts[DOOM_MAX_VERTS];
    dlinedef_t lines[DOOM_MAX_LINES];
    dsector_t  sectors[DOOM_MAX_SECTORS];
    uint8_t    num_verts;
    uint8_t    num_lines;
    uint8_t    num_sectors;
    int16_t    spawn_x, spawn_y;  /* spawn point */
    angle_t    spawn_angle;
} dlevel_t;

/* Full game state (~1.5KB) */
typedef struct {
    dplayer_t  player;
    dlevel_t   level;
    denemy_t   enemies[DOOM_MAX_ENEMIES];
    uint8_t    num_enemies;
    uint8_t    player_hp;     /* 0-10, starts at 10 */
    uint8_t    kill_count;
    uint8_t    muzzle_flash;  /* countdown frames for flash effect */
    uint8_t    damage_flash;  /* countdown frames for damage effect */
    uint8_t    game_over;     /* 1 = dead */
    uint32_t   rng_seed;
    uint32_t   frame_count;
    uint8_t    show_map;      /* 1 = show minimap overlay */
} doom_state_t;

/* ====== API ====== */

/* Entry point — called from shell "doom" command */
void game_doom(void);

/* Generate a procedural level into state */
void doom_generate(doom_state_t *st, uint32_t seed);

/* Render 2.5D wireframe view to framebuffer */
void doom_render(doom_state_t *st);

/* Render top-down minimap to framebuffer */
void doom_minimap(doom_state_t *st);

/* Move player with collision detection */
void doom_move(doom_state_t *st, fix16_t dx, fix16_t dy);

/* Update enemy AI (call once per frame) */
void doom_update_enemies(doom_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_DOOM_H */
