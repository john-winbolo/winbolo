/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *Name:          Bot Manager
 *Filename:      bot_manager.h
 *Author:        John Morrison
 *Purpose:
 *  Manages AI bot players. Each bot has its own ClientSim,
 *  passive transport, and LuaBrainInstance. Replaces
 *  server_brains.c.
 *********************************************************/

#ifndef BOT_MANAGER_H
#define BOT_MANAGER_H

#include "global.h"
#include "screen.h"  /* For aiType, gameType */
#include "brain_pathfinder.h"
#include "brain_overlay.h"

/* Forward declarations */
struct ServerSim;

/*********************************************************
 *NAME:          botManagerInit
 *PURPOSE:
 *  Zeros the bot array. Call once at startup.
 *********************************************************/
void botManagerInit(void);

/*********************************************************
 *NAME:          botManagerAddBot
 *PURPOSE:
 *  Creates a bot with its own ClientSim, passive transport,
 *  brain map, and LuaBrainInstance. Registers the player
 *  in the ServerSim.
 *
 *ARGUMENTS:
 *  sim         - The ServerSim to attach to
 *  playerNum   - Player slot (0..MAX_TANKS-1)
 *  brainPath   - Path to the brain .lua file/directory
 *  brainName   - Display name for the bot
 *  ai          - AI advantage level
 *  game        - Game type
 *  hiddenMines - Whether hidden mines are enabled
 *********************************************************/
bool botManagerAddBot(struct ServerSim *sim, BYTE playerNum,
                      const char *brainPath, const char *brainName,
                      aiType ai, gameType game, bool hiddenMines);

/*********************************************************
 *NAME:          botManagerTick
 *PURPOSE:
 *  Called once per game tick. For each active bot:
 *  gets snapshot, syncs ClientSim, runs brain, builds
 *  and sends 2 input packets.
 *
 *ARGUMENTS:
 *  sim - The ServerSim
 *  ai  - AI advantage level (for brain map updates)
 *********************************************************/
void botManagerTick(struct ServerSim *sim, aiType ai);

/*********************************************************
 *NAME:          botManagerOnGameStart
 *PURPOSE:
 *  Called when a new round starts (countdown→running).
 *  Reloads each bot's map from the server, recreates
 *  tanks, and resets brain state for the new round.
 *
 *ARGUMENTS:
 *  sim - The ServerSim
 *********************************************************/
void botManagerOnGameStart(struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerRemoveBot
 *PURPOSE:
 *  Destroys a bot's brain, transport, and ClientSim.
 *  Removes the player from the ServerSim.
 *
 *ARGUMENTS:
 *  sim       - The ServerSim
 *  playerNum - Player slot to remove
 *********************************************************/
void botManagerRemoveBot(struct ServerSim *sim, BYTE playerNum);

/*********************************************************
 *NAME:          botManagerDestroy
 *PURPOSE:
 *  Removes all active bots.
 *
 *ARGUMENTS:
 *  sim - The ServerSim
 *********************************************************/
void botManagerDestroy(struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerGetNumBots
 *PURPOSE:
 *  Returns the number of currently active bots.
 *********************************************************/
BYTE botManagerGetNumBots(void);

/*********************************************************
 *NAME:          botManagerIsBot
 *PURPOSE:
 *  Returns whether the given player slot is an active bot.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to check
 *********************************************************/
bool botManagerIsBot(BYTE playerNum);

/*********************************************************
 *NAME:          botManagerGetBrainPathfinder
 *PURPOSE:
 *  Returns the BrainPathfinder for the given bot slot,
 *  or NULL if the slot is not an active bot.
 *  Read-only access for debug overlays.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *********************************************************/
BrainPathfinder *botManagerGetBrainPathfinder(BYTE playerNum);

/*********************************************************
 *NAME:          botManagerGetOverlayCmds
 *PURPOSE:
 *  Returns the per-bot overlay command buffer the brain
 *  populates each tick via overlay_* Lua calls. Read-only
 *  access for renderers (BrainTest, recording capture).
 *  Returns NULL if the slot is not an active bot.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *********************************************************/
OverlayCmdBuffer *botManagerGetOverlayCmds(BYTE playerNum);

/*********************************************************
 *NAME:          botManagerGetLastThinkMs
 *PURPOSE:
 *  Wall-clock duration of the bot's most recent
 *  brain.think() call, in milliseconds. Updated every
 *  botManagerTick. Returns 0 if the slot is inactive.
 *  Used by debug HUDs and perf graphs.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *********************************************************/
double botManagerGetLastThinkMs(BYTE playerNum);

/*********************************************************
 *NAME:          botManagerEvalLuaString
 *PURPOSE:
 *  Compile + run a Lua chunk in the bot's state and return
 *  the resulting string (NULL if the chunk doesn't produce
 *  a string, the bot is inactive, or evaluation fails).
 *  Caller owns the returned buffer and must free() it.
 *
 *  Used by BrainTest's panel system to poll
 *  brain.get_pool_breakdown() etc. on demand.
 *
 *ARGUMENTS:
 *  playerNum - Player slot
 *  src       - Lua chunk; should `return <something>`
 *********************************************************/
char *botManagerEvalLuaString(BYTE playerNum, const char *src);

/*********************************************************
 *NAME:          botManagerExecLua
 *PURPOSE:
 *  Compile + run a string of Lua code in the given bot's
 *  Lua state. Used by hosts (e.g. BrainTest) to push UI
 *  toggle state into the brain's globals each frame.
 *  Errors are swallowed; returns false on any failure.
 *
 *ARGUMENTS:
 *  playerNum - Bot slot
 *  src       - Lua source string (NUL-terminated)
 *********************************************************/
bool botManagerExecLua(BYTE playerNum, const char *src);

/* ------------------------------------------------------------------ */
/* Goal info for debug viewer (BrainTest)                              */
/* ------------------------------------------------------------------ */

#define BRAIN_GOAL_MAX_CANDIDATES 32

typedef struct {
    char kind[32];          /* goal kind string (e.g. "attack_pill") */
    int  mx, my;            /* goal target map coords */
    int  target_id;         /* item index (base/pill number, -1 if none) */
    char substate[32];      /* attack substate (or empty) */

    /* Last evaluated candidate pool from cost competition */
    struct {
        char  desc[120];
        float cost;
        bool  winner;
        float phase_weight;
    } candidates[BRAIN_GOAL_MAX_CANDIDATES];
    int num_candidates;
} BrainGoalInfo;

/*********************************************************
 *NAME:          botManagerGetGoalInfo
 *PURPOSE:
 *  Reads the brain's current goal and last evaluated
 *  candidate pool from the Lua state. Returns false if
 *  the bot slot is not active or has no Lua state.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *  out       - Output struct to fill
 *********************************************************/
bool botManagerGetGoalInfo(BYTE playerNum, BrainGoalInfo *out);

#endif /* BOT_MANAGER_H */
