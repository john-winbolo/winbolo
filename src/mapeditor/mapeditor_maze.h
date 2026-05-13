/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_maze.h
 * Purpose:
 *   Shared maze generation algorithm used by both the
 *   Generate dialog (full map) and the Maze drawing tool
 *   (interactive preview).
 *********************************************************/

#ifndef MAPEDITOR_MAZE_H
#define MAPEDITOR_MAZE_H

#include <stdint.h>
#include <stdbool.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maze algorithm types */
#define MAZE_ALGO_LABYRINTH 0  /* Single-solution (recursive backtracker) */
#define MAZE_ALGO_OPEN      1  /* Multi-path (labyrinth + wall removal) */
#define MAZE_ALGO_COUNT     2

/* Maze configuration */
typedef struct {
    int algo;           /* MAZE_ALGO_* */
    int wallThick;      /* 1 or 2 */
    int corridorWidth;  /* 1 or 2 */
    int entries;        /* 1-8, number of openings on perimeter */
    bool placeBases;
    bool placePills;
    int cityRooms;      /* 0-5, open rooms inside maze */
    BYTE wallTerrain;   /* terrain for walls (default BUILDING) */
    BYTE corridorTerrain; /* terrain for corridors (default ROAD) */
    uint32_t seed;
} MazeConfig;

/* Return a config with sensible defaults. */
MazeConfig mazeDefaultConfig(void);

/* Generate maze terrain into a buffer.
 * region: x1,y1 to x2,y2 (inclusive).
 * outTerrain: 2D array [256][256] — caller provides, function writes
 *             BUILDING/ROAD/HALFBUILDING. Only tiles within the region
 *             are written. Tiles outside are untouched. */
void mazeGenerate(int x1, int y1, int x2, int y2,
                  BYTE outTerrain[256][256],
                  const MazeConfig *cfg);

/* Lightweight version for preview — terrain only, no objects.
 * Writes into flat arrays (like the preview system).
 * Returns number of tiles written. */
int mazeGeneratePreview(int x1, int y1, int x2, int y2,
                        int *outX, int *outY, BYTE *outTerrain,
                        int maxTiles, const MazeConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_MAZE_H */
