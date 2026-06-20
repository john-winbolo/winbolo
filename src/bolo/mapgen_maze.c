/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapgen_maze.c
 * Purpose:
 *   Maze generation algorithm. Produces BUILDING walls,
 *   ROAD corridors, and HALFBUILDING perimeter borders.
 *   Used by both the random-map generator and the Maze
 *   drawing tool in the map editor.
 *********************************************************/

#include "mapgen_maze.h"
#include "mapgen.h"
#include <string.h>
#include <stdlib.h>

/* Maximum cell grid dimensions (256/2 = 128 is the theoretical max) */
#define MAZE_MAX_CELLS 128

MazeConfig mazeDefaultConfig(void) {
    MazeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.algo = MAZE_ALGO_LABYRINTH;
    cfg.wallThick = 1;
    cfg.corridorWidth = 1;
    cfg.entries = 2;
    cfg.placeBases = false;
    cfg.placePills = false;
    cfg.cityRooms = 0;
    cfg.wallTerrain = BUILDING;
    cfg.corridorTerrain = ROAD;
    cfg.seed = 1;
    return cfg;
}

/* -------------------------------------------------------
 * Core maze generation into a 256x256 terrain buffer
 * ------------------------------------------------------- */

/* Cell visited flags */
static bool visited[MAZE_MAX_CELLS][MAZE_MAX_CELLS];

/* Explicit stack for iterative backtracker (avoids stack overflow on large grids) */
typedef struct {
    int col, row;
} CellPos;

static CellPos cellStack[MAZE_MAX_CELLS * MAZE_MAX_CELLS];

/* Carve corridor tiles for a cell at grid position (col, row).
 * The cell's top-left corner in map coords is:
 *   mapX = x1 + wallThick + col * cellSize
 *   mapY = y1 + wallThick + row * cellSize
 * Corridor area is corridorWidth x corridorWidth tiles. */
static void carveCell(BYTE terrain[256][256],
                      int x1, int y1, int wallThick, int corridorWidth,
                      int cellSize, int col, int row, BYTE corrT) {
    int ox = x1 + wallThick + col * cellSize;
    int oy = y1 + wallThick + row * cellSize;
    for (int dx = 0; dx < corridorWidth; dx++) {
        for (int dy = 0; dy < corridorWidth; dy++) {
            int tx = ox + dx;
            int ty = oy + dy;
            if (tx >= 0 && tx < 256 && ty >= 0 && ty < 256) {
                terrain[tx][ty] = corrT;
            }
        }
    }
}

/* Carve the wall between two adjacent cells.
 * Direction: 0=up, 1=right, 2=down, 3=left */
static void carveWall(BYTE terrain[256][256],
                      int x1, int y1, int wallThick, int corridorWidth,
                      int cellSize, int col, int row, int dir, BYTE corrT) {
    int ox = x1 + wallThick + col * cellSize;
    int oy = y1 + wallThick + row * cellSize;

    /* The wall segment to carve depends on the direction.
     * We need to carve wallThick tiles in the direction of travel,
     * spanning corridorWidth tiles in the perpendicular. */
    int sx, sy, w, h;
    switch (dir) {
    case 0: /* up: carve from (ox, oy - wallThick) size (corridorWidth x wallThick) */
        sx = ox; sy = oy - wallThick;
        w = corridorWidth; h = wallThick;
        break;
    case 1: /* right: carve from (ox + corridorWidth, oy) size (wallThick x corridorWidth) */
        sx = ox + corridorWidth; sy = oy;
        w = wallThick; h = corridorWidth;
        break;
    case 2: /* down: carve from (ox, oy + corridorWidth) size (corridorWidth x wallThick) */
        sx = ox; sy = oy + corridorWidth;
        w = corridorWidth; h = wallThick;
        break;
    case 3: /* left: carve from (ox - wallThick, oy) size (wallThick x corridorWidth) */
        sx = ox - wallThick; sy = oy;
        w = wallThick; h = corridorWidth;
        break;
    default: return;
    }

    for (int dx = 0; dx < w; dx++) {
        for (int dy = 0; dy < h; dy++) {
            int tx = sx + dx;
            int ty = sy + dy;
            if (tx >= 0 && tx < 256 && ty >= 0 && ty < 256) {
                terrain[tx][ty] = corrT;
            }
        }
    }
}

/* Direction offsets: up, right, down, left */
static const int dirDC[] = { 0, 1, 0, -1 };
static const int dirDR[] = { -1, 0, 1, 0 };

/* Opposite direction */
static int oppositeDir(int d) { return (d + 2) & 3; }

/* Count carved neighbors of a cell */
static int countCarvedNeighbors(int cols, int rows, int col, int row) {
    int count = 0;
    for (int d = 0; d < 4; d++) {
        int nc = col + dirDC[d];
        int nr = row + dirDR[d];
        if (nc >= 0 && nc < cols && nr >= 0 && nr < rows && visited[nc][nr]) {
            count++;
        }
    }
    return count;
}

void mazeGenerate(int x1, int y1, int x2, int y2,
                  BYTE outTerrain[256][256],
                  const MazeConfig *cfg) {
    uint32_t rng = cfg->seed;
    if (rng == 0) rng = 1;

    int wallThick = cfg->wallThick;
    int corridorWidth = cfg->corridorWidth;
    BYTE wallT = cfg->wallTerrain;
    BYTE corrT = cfg->corridorTerrain;
    if (wallThick < 1) wallThick = 1;
    if (wallThick > 2) wallThick = 2;
    if (corridorWidth < 1) corridorWidth = 1;
    if (corridorWidth > 2) corridorWidth = 2;

    int regionW = x2 - x1 + 1;
    int regionH = y2 - y1 + 1;
    int cellSize = corridorWidth + wallThick;
    int cols = (regionW - wallThick) / cellSize;
    int rows = (regionH - wallThick) / cellSize;

    /* If region is too small for even a 2x2 grid, fill with corridor terrain */
    if (cols < 2 || rows < 2) {
        for (int x = x1; x <= x2; x++) {
            for (int y = y1; y <= y2; y++) {
                if (x >= 0 && x < 256 && y >= 0 && y < 256)
                    outTerrain[x][y] = corrT;
            }
        }
        return;
    }
    if (cols > MAZE_MAX_CELLS) cols = MAZE_MAX_CELLS;
    if (rows > MAZE_MAX_CELLS) rows = MAZE_MAX_CELLS;

    /* Step 1: Fill region with wall terrain */
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            if (x >= 0 && x < 256 && y >= 0 && y < 256)
                outTerrain[x][y] = wallT;
        }
    }

    /* Step 2: Initialize visited array */
    memset(visited, 0, sizeof(visited));

    /* Step 3: Recursive backtracker (iterative with explicit stack) */
    int startCol = (int)(mapGenXorshift32(&rng) % (uint32_t)cols);
    int startRow = (int)(mapGenXorshift32(&rng) % (uint32_t)rows);

    int stackTop = 0;
    cellStack[stackTop].col = startCol;
    cellStack[stackTop].row = startRow;
    stackTop++;
    visited[startCol][startRow] = true;
    carveCell(outTerrain, x1, y1, wallThick, corridorWidth, cellSize, startCol, startRow, corrT);

    while (stackTop > 0) {
        int curCol = cellStack[stackTop - 1].col;
        int curRow = cellStack[stackTop - 1].row;

        /* Find unvisited neighbors */
        int neighbors[4];
        int numNeighbors = 0;
        for (int d = 0; d < 4; d++) {
            int nc = curCol + dirDC[d];
            int nr = curRow + dirDR[d];
            if (nc >= 0 && nc < cols && nr >= 0 && nr < rows && !visited[nc][nr]) {
                neighbors[numNeighbors++] = d;
            }
        }

        if (numNeighbors > 0) {
            /* Pick a random unvisited neighbor */
            int pick = (int)(mapGenXorshift32(&rng) % (uint32_t)numNeighbors);
            int dir = neighbors[pick];
            int nc = curCol + dirDC[dir];
            int nr = curRow + dirDR[dir];

            /* Carve wall between current and neighbor */
            carveWall(outTerrain, x1, y1, wallThick, corridorWidth, cellSize,
                      curCol, curRow, dir, corrT);
            /* Carve the neighbor cell */
            visited[nc][nr] = true;
            carveCell(outTerrain, x1, y1, wallThick, corridorWidth, cellSize, nc, nr, corrT);

            /* Push neighbor */
            cellStack[stackTop].col = nc;
            cellStack[stackTop].row = nr;
            stackTop++;
        } else {
            /* Backtrack */
            stackTop--;
        }
    }

    /* Step 5: Punch entry/exit openings evenly spaced around the perimeter */
    int numEntries = cfg->entries;
    if (numEntries < 1) numEntries = 1;
    if (numEntries > 8) numEntries = 8;

    int perim = 2 * (regionW + regionH) - 4;
    if (perim < 1) perim = 1;

    for (int e = 0; e < numEntries; e++) {
        int pos = (perim * e) / numEntries + (int)(mapGenXorshift32(&rng) % (uint32_t)(perim / (numEntries > 0 ? numEntries : 1) / 2 + 1));
        pos = pos % perim;

        int ex, ey;
        if (pos < regionW) {
            ex = x1 + pos; ey = y1;
        } else if (pos < regionW + regionH - 1) {
            ex = x2; ey = y1 + (pos - regionW + 1);
        } else if (pos < 2 * regionW + regionH - 2) {
            ex = x2 - (pos - regionW - regionH + 2); ey = y2;
        } else {
            ex = x1; ey = y2 - (pos - 2 * regionW - regionH + 3);
        }

        /* Carve a corridorWidth-wide opening at this border position */
        for (int d = 0; d < corridorWidth; d++) {
            int tx, ty;
            /* For top/bottom edges, offset horizontally; for left/right, vertically */
            if (ey == y1 || ey == y2) {
                tx = ex + d; ty = ey;
            } else {
                tx = ex; ty = ey + d;
            }
            if (tx >= 0 && tx < 256 && ty >= 0 && ty < 256) {
                outTerrain[tx][ty] = corrT;
            }
            /* Also carve the wall tile just inside the border */
            if (ey == y1 && ty + 1 < 256) {
                for (int w = 0; w < wallThick && ty + 1 + w <= y2; w++)
                    if (tx >= 0 && tx < 256) outTerrain[tx][ty + 1 + w] = corrT;
            } else if (ey == y2 && ty - 1 >= 0) {
                for (int w = 0; w < wallThick && ty - 1 - w >= y1; w++)
                    if (tx >= 0 && tx < 256) outTerrain[tx][ty - 1 - w] = corrT;
            } else if (ex == x1 && tx + 1 < 256) {
                for (int w = 0; w < wallThick && tx + 1 + w <= x2; w++)
                    if (ty >= 0 && ty < 256) outTerrain[tx + 1 + w][ty] = corrT;
            } else if (ex == x2 && tx - 1 >= 0) {
                for (int w = 0; w < wallThick && tx - 1 - w >= x1; w++)
                    if (ty >= 0 && ty < 256) outTerrain[tx - 1 - w][ty] = corrT;
            }
        }
    }

    /* Open maze: remove some internal walls */
    if (cfg->algo == MAZE_ALGO_OPEN) {
        /* Iterate over internal wall segments between cells and remove ~25% */
        for (int c = 0; c < cols; c++) {
            for (int r = 0; r < rows; r++) {
                /* Check wall to the right */
                if (c + 1 < cols) {
                    uint32_t chance = mapGenXorshift32(&rng) % 100;
                    if (chance < 25) {
                        carveWall(outTerrain, x1, y1, wallThick, corridorWidth,
                                  cellSize, c, r, 1, corrT);
                    }
                }
                /* Check wall below */
                if (r + 1 < rows) {
                    uint32_t chance = mapGenXorshift32(&rng) % 100;
                    if (chance < 25) {
                        carveWall(outTerrain, x1, y1, wallThick, corridorWidth,
                                  cellSize, c, r, 2, corrT);
                    }
                }
            }
        }

        /* Clean up isolated single-tile wall pillars:
         * wall tiles with no wall cardinal neighbor → corridor */
        for (int x = x1 + 1; x < x2; x++) {
            for (int y = y1 + 1; y < y2; y++) {
                if (x < 0 || x >= 256 || y < 0 || y >= 256) continue;
                if (outTerrain[x][y] != wallT) continue;
                bool hasNeighbor = false;
                if (x > 0   && outTerrain[x-1][y] == wallT) hasNeighbor = true;
                if (x < 255 && outTerrain[x+1][y] == wallT) hasNeighbor = true;
                if (y > 0   && outTerrain[x][y-1] == wallT) hasNeighbor = true;
                if (y < 255 && outTerrain[x][y+1] == wallT) hasNeighbor = true;
                if (!hasNeighbor) outTerrain[x][y] = corrT;
            }
        }
    }

    /* City rooms: carve open areas at dead-end cells */
    int cityRooms = cfg->cityRooms;
    if (cityRooms > 5) cityRooms = 5;
    for (int room = 0; room < cityRooms; room++) {
        /* Find a dead-end cell (only one carved neighbor) */
        int bestCol = -1, bestRow = -1;
        int attempts = 0;
        while (attempts < cols * rows) {
            int tc = (int)(mapGenXorshift32(&rng) % (uint32_t)cols);
            int tr = (int)(mapGenXorshift32(&rng) % (uint32_t)rows);
            if (countCarvedNeighbors(cols, rows, tc, tr) == 1) {
                bestCol = tc;
                bestRow = tr;
                break;
            }
            attempts++;
        }
        if (bestCol < 0) continue;

        /* Carve a 3x3 to 5x5 clearing centered on the cell */
        int roomSize = 3 + (int)(mapGenXorshift32(&rng) % 3); /* 3, 4, or 5 */
        int cx = x1 + wallThick + bestCol * cellSize + corridorWidth / 2;
        int cy = y1 + wallThick + bestRow * cellSize + corridorWidth / 2;
        int half = roomSize / 2;
        for (int dx = -half; dx <= half; dx++) {
            for (int dy = -half; dy <= half; dy++) {
                int tx = cx + dx;
                int ty = cy + dy;
                if (tx > x1 && tx < x2 && ty > y1 && ty < y2 &&
                    tx >= 0 && tx < 256 && ty >= 0 && ty < 256) {
                    outTerrain[tx][ty] = corrT;
                }
            }
        }
    }
}

/* -------------------------------------------------------
 * Preview version: writes into flat arrays
 * ------------------------------------------------------- */
int mazeGeneratePreview(int x1, int y1, int x2, int y2,
                        int *outX, int *outY, BYTE *outTerrain,
                        int maxTiles, const MazeConfig *cfg) {
    /* Use a temporary 256x256 buffer on the heap to avoid huge stack alloc.
     * Only the region [x1..x2, y1..y2] is used. */
    static BYTE tempTerrain[256][256];

    /* Initialize region to a sentinel so we know what was written */
    for (int x = x1; x <= x2 && x < 256; x++) {
        for (int y = y1; y <= y2 && y < 256; y++) {
            if (x >= 0 && y >= 0)
                tempTerrain[x][y] = DEEP_SEA;
        }
    }

    mazeGenerate(x1, y1, x2, y2, tempTerrain, cfg);

    /* Copy non-DEEP_SEA tiles into flat arrays */
    int count = 0;
    for (int x = x1; x <= x2 && x < 256; x++) {
        for (int y = y1; y <= y2 && y < 256; y++) {
            if (x < 0 || y < 0) continue;
            if (count >= maxTiles) return count;
            BYTE t = tempTerrain[x][y];
            outX[count] = x;
            outY[count] = y;
            outTerrain[count] = t;
            count++;
        }
    }
    return count;
}
