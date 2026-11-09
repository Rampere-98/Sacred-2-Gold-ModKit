/*
 * Map memory API shared with other mods (exploration). Implemented in mod_map.c.
 */
#ifndef MOD_MAP_H
#define MOD_MAP_H
#include "modkit.h"

#define MAP_TILE 42.5f   /* world units per tile (1 m) */

/* Hero tile data of the current tick; 0 until the vertical axis is known. */
int map_hero(int *layer, float *east, float *north);
/* Reads a remembered tile; returns 1 if anything is known about it. */
int map_tile(int layer, int tx, int ty, int *visits, int *blocked, int *scan);
/* The hero could not get onto this tile: count one more block. */
void map_block(int layer, int tx, int ty);
/* Tile index of a ground coordinate (floor division by MAP_TILE). */
int map_tile_of(float v);
/* Screen point of a ground position on the radar; 0 if the radar is off or it is out of range. */
int map_radar_xy(const DrawApi *d, float east, float north, float *x, float *y);
#endif
