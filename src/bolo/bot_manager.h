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
 *  Zeros the bot array, eager-inits the worldsim trig
 *  tables, and creates the bot worker pool. Call once at
 *  startup.
 *
 *  `threads` is the total number of concurrent brain-tick
 *  runners including the producer (main) thread; the pool
 *  is sized to threads-1 since the producer runs one job
 *  inline. Pass 0 to auto-size from logical CPU cores.
 *  threads > logical cores is a fatal config error and
 *  returns false. threads > MAX_TANKS clamps silently.
 *
 *RETURNS:
 *  true on success. false if `threads` exceeds the logical
 *  core count, or if pool creation fails.
 *********************************************************/
bool botManagerInit(int threads);

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
 *NAME:          botManagerSetPreThinkHook
 *PURPOSE:
 *  Register a callback invoked just before each bot's
 *  brain.think runs. Called with the bot's playerNum.
 *  Called with -1 immediately after each think completes.
 *  Pass NULL to clear. Used by BrainTest to track which
 *  bot is currently thinking (for overlay registration).
 *********************************************************/
void botManagerSetPreThinkHook(void (*hook)(int playerNum));

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
 *NAME:          botManagerComputePerBotTargetMs
 *PURPOSE:
 *  Returns the per-bot brain-tick budget for the next
 *  tick, in milliseconds, given the currently active bot
 *  count. Folds in an EWMA of recent serial-stage cost so
 *  the budget tightens when the rest of the tick gets
 *  busier and loosens when it doesn't.
 *
 *  Producer-thread only — reads the EWMA file-static
 *  without synchronisation. Returns the full tick target
 *  if the EWMA hasn't been seeded yet or no bots are
 *  active.
 *
 *ARGUMENTS:
 *  activeBots - Number of bots that will dispatch this tick
 *********************************************************/
double botManagerComputePerBotTargetMs(int activeBots);

/*********************************************************
 *NAME:          botManagerRecordSerialMs
 *PURPOSE:
 *  Feed the EWMA with the serial-stage cost of the tick
 *  that just completed (the parts of botManagerTick that
 *  are not the parallel brain-think dispatch). The next
 *  call to botManagerComputePerBotTargetMs uses the
 *  updated EWMA.
 *
 *  Producer-thread only — writes the EWMA file-static
 *  without synchronisation. The first call seeds the
 *  EWMA directly to avoid an init bias toward zero.
 *
 *ARGUMENTS:
 *  ms - Wall-clock milliseconds spent in the tick's
 *       serial stages (snapshot/sync + input send).
 *********************************************************/
void botManagerRecordSerialMs(double ms);

/* Per-bot info populated by botManagerGetBotInfo. POD; no
 * allocations or ownership. brainName is fixed-size: the
 * bot's registered display name copied via SDL_strlcpy. */
typedef struct {
    bool     isBot;
    bool     hasBrain;          /* aiFull with a live brain */
    char     brainName[64];     /* brain identity: basename of the brain
                                 * script path, with .lua stripped and
                                 * "init" replaced by the parent directory
                                 * name (e.g. "NewAutopilot" for
                                 * brains/NewAutopilot/init.lua) */
    double   lastThinkMs;       /* most recent brain tick */
    double   targetMs;          /* target the next tick will use */
    uint32_t overrunCount;      /* cumulative since session start */
} BotInfo;

/* Bot pool snapshot populated by botManagerGetPoolStats. POD;
 * no allocations or ownership. */
typedef struct {
    int      workerCount;       /* botWorkerPoolGetSize() */
    int      activeBots;        /* currently-active bot count */
    double   ewmaSerialMs;      /* serial-stage EWMA */
    double   currentTargetMs;   /* per-bot budget for next tick */
    double   lastBrainPhaseMs;  /* wall-clock of last brain dispatch */
    double   ewmaBrainPhaseMs;  /* EWMA of brain dispatch wall-clock */
    double   lastSerialMs;      /* last serial-stage cost (ms) */
    uint32_t totalOverruns;     /* sum of overrunCount across bots */
} BotPoolStats;

/*********************************************************
 *NAME:          botManagerHasAnyBot
 *PURPOSE:
 *  Returns whether any bot slot is currently active. Used
 *  by the server `info` command to decide whether to print
 *  the bot pool summary block. Producer-thread only — caller
 *  must hold the server mutex; reads bots[] without
 *  synchronisation.
 *********************************************************/
bool botManagerHasAnyBot(void);

/*********************************************************
 *NAME:          botManagerGetBotInfo
 *PURPOSE:
 *  Fills `out` with telemetry for the given bot slot.
 *  Returns false (and zeros `out`) when the slot is not an
 *  active bot.
 *
 *  Producer-thread only — caller must hold the server mutex
 *  so no brain tick is in flight; the accessor reads the
 *  bot's lastThinkMs / overrunCount and walks bots[] without
 *  synchronisation.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *  out       - Output struct to fill
 *********************************************************/
bool botManagerGetBotInfo(BYTE playerNum, BotInfo *out);

/*********************************************************
 *NAME:          botManagerGetPoolStats
 *PURPOSE:
 *  Fills `out` with pool-wide telemetry: worker count,
 *  active bots, serial-stage EWMA, the per-bot target the
 *  next tick will use, the most recent brain-dispatch
 *  wall-clock, and the sum of per-bot overrun counters.
 *
 *  Producer-thread only — caller must hold the server mutex;
 *  reads file-static EWMAs and walks bots[] without
 *  synchronisation.
 *
 *ARGUMENTS:
 *  out - Output struct to fill (must not be NULL)
 *********************************************************/
void botManagerGetPoolStats(BotPoolStats *out);

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
