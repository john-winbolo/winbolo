/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          View Policy
 *Filename:      view_policy.h
 *Author:        John Morrison
 *Purpose:
 *  Operator-chosen visibility rules for pillboxes, bases
 *  and allied tanks. Set at server startup or from the
 *  lobby, and broadcast to remote clients via the
 *  lobby-settings control event so the lobby UI and the
 *  server browser can reflect them.
 *********************************************************/

#ifndef VIEW_POLICY_H
#define VIEW_POLICY_H

/* Values are ordered so a zero-initialized server (and a client that
 * decodes a lobby-settings payload that predates the fields) defaults
 * to ALWAYS — today's pill behaviour. */
typedef enum {
    viewPolicyAlways = 0,   /* zero = wire default = today's pill behaviour */
    viewPolicyKey    = 1,
    viewPolicyDecay  = 2,
    viewPolicyOff    = 3
} ViewPolicy;

typedef enum {
    viewCategoryPill = 0,
    viewCategoryBase = 1,
    viewCategoryAlly = 2,
    VIEW_CATEGORY_COUNT
} ViewCategory;

/* Which block of squares the map overview keeps live around the player's
 * own tank. Expanded is everything the classic 15x15 view could scroll
 * to; Classic narrows it to the window that view is actually showing.
 * Zero is today's behaviour, so a zero-initialized server and a client
 * decoding a payload without the field both land on Expanded. */
typedef enum {
    overviewWindowExpanded = 0,
    overviewWindowClassic  = 1,
    OVERVIEW_WINDOW_COUNT
} OverviewWindow;

/* Whether anything stops the player seeing inside that block. A selector
 * rather than a bool so another rule can join it without a second
 * setting. Zero is today's behaviour. */
typedef enum {
    lineOfSightOff               = 0,
    lineOfSightBuildingsAndTrees = 1,
    LINE_OF_SIGHT_COUNT
} LineOfSightMode;

#define VIEW_DECAY_DEFAULT_SECS 30
#define VIEW_DECAY_MIN_SECS 5
#define VIEW_DECAY_MAX_SECS 600
#define VIEW_DECAY_NEAR_TILES 9       /* Chebyshev distance in map squares */
#define VIEW_DECAY_FADE_SECS 5        /* overview fade window before expiry */

#endif /* VIEW_POLICY_H */
