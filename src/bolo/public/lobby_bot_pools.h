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
 *Name:          Lobby Bot Naming Pools
 *Filename:      lobby_bot_pools.h
 *Purpose:
 *  Hardcoded thematic name pools for AI players, used by the
 *  Layout A lobby. Each team picks one pool; bots added to
 *  that team draw names from it (auto-pick on add, dice-reroll
 *  via the AiConfig sub-panel).
 *
 *  Server-side: doesn't know pool contents. The server tracks
 *  the per-team pool index and the per-bot name (assigned by
 *  whichever client added the bot). This module exists so the
 *  client can render the dropdown labels and pick names.
 *
 *  Pool index 0 is "classic" (AI references); other indices
 *  match the order in s_pools[]. Adding a pool: append to
 *  the array. Old clients seeing an unknown index pick a
 *  random known pool for *display* — bot names themselves
 *  travel as strings, so functionality isn't affected.
 *********************************************************/

#ifndef LOBBY_BOT_POOLS_H
#define LOBBY_BOT_POOLS_H

#include "global.h"

/* Number of available pools. Must match the s_pools[] array
 * length in lobby_bot_pools.c. */
int  lobbyBotPoolCount(void);

/* Display label for the pool dropdown (e.g. "Famous Painters").
 * Returns "Unknown" for out-of-range indices. */
const char *lobbyBotPoolLabel(int poolIdx);

/* Name count in a pool (for fallback overflow naming). */
int  lobbyBotPoolNameCount(int poolIdx);

/* Read a single name from a pool. Returns NULL for out-of-range. */
const char *lobbyBotPoolName(int poolIdx, int nameIdx);

/* Pick the next free name from the pool, skipping any name in
 * the `used` array (caller-supplied list of currently-used names
 * across the lobby; case-sensitive compare). Pool exhaustion
 * falls back to "<label> N" overflow into outBuf.
 *
 *   poolIdx   — pool to draw from (0..lobbyBotPoolCount()-1)
 *   used[]    — null-terminated array of strings to avoid
 *   usedCount — entries in used[]
 *   outBuf    — caller buffer for the chosen name
 *   outBufLen — outBuf capacity (MUST be ≥ 32 for safety)
 *
 * Always writes a null-terminated name into outBuf. Returns
 * pointer to outBuf for convenience. */
char *lobbyBotPoolPick(int poolIdx,
                       const char **used, int usedCount,
                       char *outBuf, int outBufLen);

#endif /* LOBBY_BOT_POOLS_H */
