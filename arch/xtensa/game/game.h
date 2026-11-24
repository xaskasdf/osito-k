/*
 * OsitoK - Elite flight demo (game loop + HUD)
 *
 * First-person wireframe flight with starfield, HUD cockpit display,
 * and joystick controls. Renders Elite ship models from ships.h.
 */
#ifndef OSITO_GAME_H
#define OSITO_GAME_H

#include "osito.h"
#include "math/fixedpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STAR_COUNT    16
#define GAME_FOCAL    FIX16(64)
#define GAME_SHIP_Z   FIX16(4)
#define VIEW_Y_MIN    1
#define VIEW_Y_MAX    47
#define HUD_Y         48

typedef struct {
    angle_t   yaw;
    angle_t   pitch;
    uint8_t   speed;          /* 0-6 */
    uint8_t   ship_idx;       /* 0-3, index into ship_list[] */
    int8_t    star_x[STAR_COUNT];
    int8_t    star_y[STAR_COUNT];
    uint32_t  rng_seed;
    uint32_t  frame_count;
} game_state_t;

/* Entry point — called from shell "elite" command */
void game_elite(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_GAME_H */
