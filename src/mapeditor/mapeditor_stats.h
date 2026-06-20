/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_stats.h
 * Purpose:
 *   Map statistics computation for the map editor.
 *   Provides terrain distribution, object counts, and
 *   on-demand spatial analysis metrics.
 *********************************************************/

#ifndef MAPEDITOR_STATS_H
#define MAPEDITOR_STATS_H

#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Per-terrain-type counts and percentages (mine-stripped) */
    int   terrainCount[256];
    float terrainPct[256];

    /* Totals */
    int   totalPlayable;    /* 215*215 = 46225 */
    int   landTiles;
    int   waterTiles;
    float landPct;

    /* Mines */
    int   mineCount;
    float mineDensity;      /* mines per land tile */

    /* Object counts */
    int   numBases;
    int   numPills;
    int   numStarts;

    /* Spatial metrics (expensive, on-demand) */
    float baseSpacing;      /* average pairwise Euclidean distance */
    float pillSpacing;
    float largestLandPct;   /* largest connected landmass as % of total land */

    /* Symmetry scores (0.0–1.0) */
    float symH;             /* horizontal mirror */
    float symV;             /* vertical mirror */
    float sym4;             /* 4-corner */
    float symR180;          /* 180° rotation */
    float symBest;          /* max of the above */
    const char *symLabel;   /* name of best match */

    bool  spatialValid;     /* true when expensive metrics have been computed */
} MapStats;

/* Compute basic stats (terrain distribution, object counts).
 * Fast (~0ms), safe to call every frame. */
void mapStatsComputeBasic(MapStats *stats, map mp, bases bs, pillboxes pb, starts ss);

/* Compute expensive spatial metrics (spacing, largest land, symmetry).
 * ~5–20ms, call on-demand only. */
void mapStatsComputeSpatial(MapStats *stats, map mp, bases bs, pillboxes pb);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_STATS_H */
