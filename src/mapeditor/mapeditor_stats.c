/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_stats.c
 * Purpose:
 *   Map statistics computation — terrain distribution,
 *   object counts, and on-demand spatial analysis.
 *   Algorithms ported from tools/map_analyze.c.
 *********************************************************/

#include "mapeditor_stats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Playable area bounds (same as map_analyze.c) */
#define PLAY_MIN  21
#define PLAY_MAX  235
#define PLAY_SIZE (PLAY_MAX - PLAY_MIN + 1)
#define PLAY_TILES (PLAY_SIZE * PLAY_SIZE)

static BYTE stripMine(BYTE t) {
    return (t >= MINE_START && t <= MINE_END) ? (BYTE)(t - MINE_SUBTRACT) : t;
}

static bool isWater(BYTE t) {
    t = stripMine(t);
    return t == DEEP_SEA || t == RIVER;
}

/* -------------------------------------------------------
 * Basic stats — fast, call every frame
 * ------------------------------------------------------- */

void mapStatsComputeBasic(MapStats *stats, map mp, bases bs, pillboxes pb, starts ss) {
    memset(stats->terrainCount, 0, sizeof(stats->terrainCount));
    memset(stats->terrainPct, 0, sizeof(stats->terrainPct));
    stats->totalPlayable = PLAY_TILES;
    stats->landTiles = 0;
    stats->waterTiles = 0;
    stats->mineCount = 0;

    for (int x = PLAY_MIN; x <= PLAY_MAX; x++) {
        for (int y = PLAY_MIN; y <= PLAY_MAX; y++) {
            BYTE raw = mp->mapItem[x][y];
            BYTE base = stripMine(raw);
            stats->terrainCount[base]++;
            if (raw >= MINE_START && raw <= MINE_END) stats->mineCount++;
            if (isWater(raw))
                stats->waterTiles++;
            else
                stats->landTiles++;
        }
    }

    /* Percentages */
    for (int i = 0; i < 256; i++) {
        stats->terrainPct[i] = (float)stats->terrainCount[i] / (float)PLAY_TILES;
    }
    stats->landPct = (float)stats->landTiles / (float)PLAY_TILES;

    /* Mine density */
    stats->mineDensity = (stats->landTiles > 0)
        ? (float)stats->mineCount / (float)stats->landTiles
        : 0.0f;

    /* Object counts */
    stats->numBases  = bs->numBases;
    stats->numPills  = pb->numPills;
    stats->numStarts = ss->numStarts;

    /* Mark spatial metrics as stale (caller decides when to recompute) */
    stats->spatialValid = false;
}

/* -------------------------------------------------------
 * Spatial stats — expensive, on-demand
 * ------------------------------------------------------- */

static float dist(int x1, int y1, int x2, int y2) {
    float dx = (float)(x1 - x2);
    float dy = (float)(y1 - y2);
    return sqrtf(dx * dx + dy * dy);
}

static float avgSpacing(int *xs, int *ys, int n) {
    if (n < 2) return 0.0f;
    float total = 0.0f;
    int pairs = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            total += dist(xs[i], ys[i], xs[j], ys[j]);
            pairs++;
        }
    }
    return total / (float)pairs;
}

static float largestLand(map mp, int totalLand) {
    if (totalLand == 0) return 0.0f;

    /* Heap-allocate visited array (64KB) and BFS queue (~370KB) */
    bool (*visited)[256] = (bool (*)[256])calloc(256, 256 * sizeof(bool));
    int *qx = (int *)malloc(PLAY_TILES * sizeof(int));
    int *qy = (int *)malloc(PLAY_TILES * sizeof(int));
    if (!visited || !qx || !qy) {
        free(visited);
        free(qx);
        free(qy);
        return 0.0f;
    }

    int largest = 0;

    for (int x = PLAY_MIN; x <= PLAY_MAX; x++) {
        for (int y = PLAY_MIN; y <= PLAY_MAX; y++) {
            if (visited[x][y] || isWater(mp->mapItem[x][y])) continue;

            int head = 0, tail = 0, size = 0;
            qx[tail] = x; qy[tail] = y; tail++;
            visited[x][y] = true;

            while (head < tail) {
                int cx = qx[head], cy = qy[head]; head++; size++;
                static const int dx[] = {0, 0, -1, 1};
                static const int dy[] = {-1, 1, 0, 0};
                for (int d = 0; d < 4; d++) {
                    int nx = cx + dx[d], ny = cy + dy[d];
                    if (nx < PLAY_MIN || nx > PLAY_MAX ||
                        ny < PLAY_MIN || ny > PLAY_MAX) continue;
                    if (visited[nx][ny] || isWater(mp->mapItem[nx][ny])) continue;
                    visited[nx][ny] = true;
                    qx[tail] = nx; qy[tail] = ny; tail++;
                }
            }
            if (size > largest) largest = size;
        }
    }

    free(visited);
    free(qx);
    free(qy);

    return (float)largest / (float)totalLand;
}

static float measureSym(map mp, int mode) {
    int match = 0, tested = 0;
    for (int x = PLAY_MIN; x <= PLAY_MAX; x++) {
        for (int y = PLAY_MIN; y <= PLAY_MAX; y++) {
            BYTE a = stripMine(mp->mapItem[x][y]);
            if (a == DEEP_SEA) continue; /* skip water background */
            tested++;
            int mx, my;
            switch (mode) {
            case 0: mx = PLAY_MIN + (PLAY_MAX - x); my = y; break;                         /* H mirror */
            case 1: mx = x; my = PLAY_MIN + (PLAY_MAX - y); break;                         /* V mirror */
            case 2: mx = PLAY_MIN + (PLAY_MAX - x); my = PLAY_MIN + (PLAY_MAX - y); break; /* 4-corner */
            case 3: mx = PLAY_MIN + (PLAY_MAX - x); my = PLAY_MIN + (PLAY_MAX - y); break; /* 180° rot */
            default: mx = x; my = y;
            }
            BYTE b = stripMine(mp->mapItem[mx][my]);
            if (mode == 2) {
                /* 4-corner: all 4 quadrants must match */
                BYTE c = stripMine(mp->mapItem[PLAY_MIN + (PLAY_MAX - x)][y]);
                BYTE d = stripMine(mp->mapItem[x][PLAY_MIN + (PLAY_MAX - y)]);
                if (a == b && a == c && a == d) match++;
            } else {
                if (a == b) match++;
            }
        }
    }
    return tested > 0 ? (float)match / (float)tested : 0.0f;
}

void mapStatsComputeSpatial(MapStats *stats, map mp, bases bs, pillboxes pb) {
    /* Pill spacing */
    {
        int xs[MAX_PILLS], ys[MAX_PILLS];
        for (int i = 0; i < pb->numPills && i < MAX_PILLS; i++) {
            xs[i] = pb->item[i].x;
            ys[i] = pb->item[i].y;
        }
        stats->pillSpacing = avgSpacing(xs, ys, pb->numPills);
    }
    {
        int xs[MAX_BASES], ys[MAX_BASES];
        for (int i = 0; i < bs->numBases && i < MAX_BASES; i++) {
            xs[i] = bs->item[i].x;
            ys[i] = bs->item[i].y;
        }
        stats->baseSpacing = avgSpacing(xs, ys, bs->numBases);
    }

    /* Largest connected landmass */
    stats->largestLandPct = largestLand(mp, stats->landTiles);

    /* Symmetry */
    stats->symH    = measureSym(mp, 0);
    stats->symV    = measureSym(mp, 1);
    stats->sym4    = measureSym(mp, 2);
    stats->symR180 = measureSym(mp, 3);

    /* Best symmetry */
    stats->symBest = stats->symH;
    stats->symLabel = "mirror-H";
    if (stats->symV > stats->symBest) {
        stats->symBest = stats->symV;
        stats->symLabel = "mirror-V";
    }
    if (stats->sym4 > stats->symBest) {
        stats->symBest = stats->sym4;
        stats->symLabel = "4-corner";
    }
    if (stats->symR180 > stats->symBest) {
        stats->symBest = stats->symR180;
        stats->symLabel = "rotate-180";
    }
    if (stats->symBest < 0.5f) {
        stats->symLabel = "none";
    }

    stats->spatialValid = true;
}
