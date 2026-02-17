/*
 * OsitoK - Elite ship models (wireframe data)
 *
 * Vertex/edge data from BBC Micro Elite (bbcelite.com).
 * Coordinates scaled from original range (~+-160) to fix16 +-2.0
 * using SHIP_SCALE(v) = v/80.0
 */
#ifndef OSITO_SHIPS_H
#define OSITO_SHIPS_H

#include "gfx/wire3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scale original Elite coordinates to fix16: divide by 80 */
#define SHIP_SCALE(v)  FIX16_C((v) / 80.0)

/* Ship models */
extern const wire_model_t ship_cobra;      /* 28v, 38e — player ship */
extern const wire_model_t ship_sidewinder; /* 10v, 15e — common enemy */
extern const wire_model_t ship_viper;      /* 15v, 20e — police */
extern const wire_model_t ship_coriolis;   /* 16v, 28e — space station */

/* Ship list for iteration */
#define SHIP_COUNT 4
extern const wire_model_t *ship_list[SHIP_COUNT];
extern const char *ship_names[SHIP_COUNT];

/* Shell commands */
void cmd_ship(const char *args);
void cmd_shipspin(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_SHIPS_H */
