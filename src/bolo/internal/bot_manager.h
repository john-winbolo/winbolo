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
#include "client_enums.h"  /* aiType, gameType */
#include "brain_pathfinder.h"
#include "brain_overlay.h"
#include "server_sim.h"    /* BotInfo, BotPoolStats, BRAIN_GOAL_MAX_CANDIDATES, SubscriberHandle */
#include "transport.h"     /* Transport */
#include "client_snapshot.h"
#include "input_packet.h"  /* SnapshotHeader, TankSnapshot, ... MAX_SNAPSHOT_* */
#include "control_event.h"
#include "client_command.h"  /* ClientCommand — per-bot pending command queue */
#include "../gui/sdl3/luabrainshandler.h"  /* LuaBrainInstance */

/* Forward declarations */
struct ServerSim;
struct lua_State;

/* Per-bot context. Owns the bot's ClientSim, passive transport, brain
 * instance, and the abort-flag plumbing the count hook and C bindings
 * (cpf_/wsim_/NA) read to cut think short on budget overrun. */
typedef struct {
    /* Back-pointer to the owning ServerSim. Set by botManagerAddBot so
     * Lua bindings that only get a lua_State (via lua_getextraspace)
     * can route back to the per-sim BotManager without a global. */
    struct ServerSim *sim;
    ClientSim      *cs;
    Transport       transport;
    LuaBrainInstance brain;
    /* Filesystem path to the brain script for this bot. Captured at
     * botManagerAddBot time so botManagerGetBotInfo can surface the
     * brain identity without confusing it with the player display
     * name (multiple bots commonly share one brain script). */
    char            brainPath[256];
    BYTE            playerNum;
    bool            active;
    aiType          ai;
    /* Wall-clock duration of this bot's most recent brain.think call,
     * in milliseconds. Updated every botManagerTick. Surfaced via
     * botManagerGetLastThinkMs so HUDs / perf graphs can read it. */
    double          lastThinkMs;
    /* Number of ticks this bot's think exceeded targetMs * 1.5. Counts
     * every overrun; see lastOverrunWarnTick for the rate-limited log. */
    Uint32          overrunCount;
    /* Last tick at which a budget-overrun warning was logged for this
     * bot. Limits the warning to at most one per ~50 ticks. */
    Uint32          lastOverrunWarnTick;
    /* Set by the count hook when SDL_GetPerformanceCounter() passes
     * thinkDeadlineCounter; polled by C bindings (cpf/wsim/NA) at
     * their outer-loop checkpoints so they can return a partial result
     * cheaply instead of running to completion past the budget. The
     * hook also raises luaL_error("tick_budget_exceeded") in the same
     * step, so once C returns to Lua the longjmp unwinds to the
     * brainCoreCallThink pcall within ~1000 instructions. Reset to 0
     * each tick before dispatch.  */
    SDL_AtomicInt   abort_flag;
    /* Absolute SDL_GetPerformanceCounter() value at which this bot's
     * think budget elapses. Set per-tick by runBotThinkJob from
     * t0 + sim->botMgr.lastTargetMs * SDL_GetPerformanceFrequency() / 1000
     * so the count hook does one cheap compare against `now`. */
    Uint64          thinkDeadlineCounter;
    /* One-tick edge signal set by the producer when the previous tick
     * was aborted by the budget hook. Surfaced to the brain via
     * brain.wasKilled; reset to false immediately after being passed,
     * so a brain only sees it for the single tick that follows an
     * abort. */
    bool            wasKilled;
    /* Consecutive crashes (brain.think Lua errors) since the last
     * successful tick. Reset to 0 on any successful tick. Once it
     * reaches BOT_CRASH_KICK_THRESHOLD the producer kicks the bot
     * with a server-text broadcast naming it. See runBotThinkJobImpl
     * and the kick loop in botManagerTick. */
    Uint32          consecutiveCrashes;
    /* The bot's lua_State stores BotContext * via lua_getextraspace(L).
     * Set up exactly once during botManagerAddBot after the brain
     * instance is created; the count hook and every cpf_/wsim_/NA
     * binding cast lua_getextraspace(L) back to BotContext * to read
     * abort_flag and thinkDeadlineCounter without per-binding plumbing. */
    SubscriberHandle controlSub;
} BotContext;

/* Per-bot scratch carried across the three within-tick stages
 * (snapshot/sync, brain tick, input dispatch). One instance per bot
 * slot lives in sim->botMgr.jobs[] so the worker thread sees only its
 * own indexed entry — packets and the needRemove flag stay thread-local
 * to that bot. */
typedef struct {
    BotContext *bot;
    ServerSim  *sim;
    SnapshotHeader      hdr;
    TankSnapshot        tanks[MAX_TANKS];
    ShellSnapshot       shells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bases[MAX_SNAPSHOT_BASES];
    PillSnapshot        pills[MAX_SNAPSHOT_PILLS];
    GameEvent           events[MAX_SNAPSHOT_EVENTS];
    InputPacket         pkt1, pkt2;
    bool                needRemove;  /* worker → producer: brain tick failed */
    bool                hasInput;    /* worker → producer: pkt1/pkt2 valid */
    /* worker → producer: brain.think was aborted by the count hook this
     * tick (tick_budget_exceeded). The bot stays alive; the producer
     * surfaces the kill to the next tick via bot->wasKilled. */
    bool                wasKilled;
    /* Deferred ClientCommand queue: worker threads CAN'T call
     * clientSimSubmitCommand directly because it acquires threadsMutex,
     * which the producer (timer thread) holds for the duration of
     * botManagerTick — that would deadlock the worker against the
     * producer's wait-for-workers in botWorkerPoolRun.  Instead the
     * worker pushes commands here from inside the brain-think stage,
     * and the producer drains the queue in Stage 3 (serial, already
     * under the mutex) via serverSimApplyCommand.  4 slots covers
     * /info state + /info extra in a single tick plus a couple of
     * future-proofing extras. */
#define BOT_PENDING_CMD_MAX 4
    ClientCommand       pendingCmds[BOT_PENDING_CMD_MAX];
    int                 pendingCmdCount;
} BotJobCtx;

typedef struct BotManager {
    struct ServerSim *sim;  /* back-pointer; callbacks read it */

    BotContext   bots[MAX_TANKS];
    int          numBots;

    /* Total concurrent brain-tick runners including the producer thread.
     * Saved by botManagerInit after validation; read by the budget formula
     * to scale per-bot time when more bots than runners are active. */
    int          threadsConfig;

    /* Pending thread-count request from the BrainTest panel. -1 = no
     * pending change. Applied at the top of botManagerTick (between ticks),
     * never mid-dispatch. Single-producer (main thread) so a plain int
     * is fine — no atomics needed. */
    int          pendingThreads;

    /* Default debug mode for newly-created bots. Hosts override via
     * botManagerSetDefaultDebugMode (BrainTest sets true at startup;
     * release game leaves false). */
    bool         defaultDebugMode;

    /* Runtime debug mode tracker. Flipped by botManagerToggleAllBrainDebugMode
     * so the toggle alternates correctly across calls. */
    bool         brainDebugMode;

    /* EWMA of the serial-stage cost (ms) of recent ticks. Seeded by the
     * first call to botManagerRecordSerialMs to avoid biasing toward zero. */
    double       serialMsEwma;
    /* Last serial-stage cost (ms). Companion to serialMsEwma so the
     * server `info` summary can show last + EWMA together. */
    double       lastSerialMs;

    /* Wall-clock cost of the most recent dispatched brain-think stage, in
     * milliseconds. Set per-tick at the end of botManagerTick; reserved
     * for future refinements (e.g. computing the EWMA from the full
     * serverInstanceTick instead of approximating it via kReservedSimMs). */
    double       lastBrainPhaseMs;
    /* EWMA of the brain-think stage cost (ms). Display-only — the budget
     * formula reads serialMsEwma, not this. */
    double       brainPhaseMsEwma;

    /* Per-bot brain-tick budget computed for the current tick. Set per-tick
     * before dispatch so the input-send overrun check and any future server
     * info command can read the same number the brains were given. */
    double       lastTargetMs;

    BotJobCtx    jobs[MAX_TANKS];
    int          jobIndices[MAX_TANKS];

    void       (*preThinkHook)(int playerNum);
} BotManager;

/*********************************************************
 *NAME:          botManagerInit
 *PURPOSE:
 *  Eager-inits the worldsim trig tables and creates the bot
 *  worker pool. Call once at process startup.
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
 *NAME:          botManagerInitInSim
 *PURPOSE:
 *  Initialise a per-sim BotManager embedded in a ServerSim.
 *  Zeros the BotManager, plants the sim back-pointer, and
 *  seeds threadsConfig. threads <= 0 means "use the worker
 *  pool's current size + 1" (falling back to 1 if the pool
 *  isn't created yet); otherwise clamps to logical cores
 *  and MAX_TANKS.
 *********************************************************/
void botManagerInitInSim(BotManager *bm, struct ServerSim *sim, int threads);

/*********************************************************
 *NAME:          botManagerSetDefaultDebugMode
 *PURPOSE:
 *  Set the BRAIN_DEBUG_MODE value bots inherit at creation.
 *  BrainTest calls this with true at startup so its bots
 *  load with viz-supporting code active. The release game
 *  doesn't call it — default stays false, bots load from
 *  stripped opt/ source.
 *
 *  Affects bots created AFTER this call. Live bots are
 *  unchanged; use botManagerToggleAllBrainDebugMode to flip
 *  the runtime global on existing brains.
 *********************************************************/
void botManagerSetDefaultDebugMode(struct ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          botManagerRequestThreads
 *PURPOSE:
 *  Request a live resize of the worker pool. `total_runners`
 *  is the total number of concurrent brain-tick runners
 *  INCLUDING the producer (main thread). 1 = serial (no
 *  workers); N>1 = pool of size N-1 plus producer.
 *
 *  Doesn't touch the pool immediately — stashes the value as
 *  pending. The next botManagerTick() call applies it before
 *  dispatching, guaranteeing the resize happens between ticks
 *  and never mid-dispatch. Safe to call from the main thread
 *  any time (e.g. from a panel slider during render).
 *********************************************************/
void botManagerRequestThreads(struct ServerSim *sim, int total_runners);

/*********************************************************
 *NAME:          botManagerGetThreads
 *PURPOSE:
 *  Returns the current total runner count (workers + producer).
 *  1 means serial dispatch. Reflects the live state, not any
 *  pending request.
 *********************************************************/
int  botManagerGetThreads(const struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerGetPendingThreads
 *PURPOSE:
 *  Returns the pending thread-count request, or -1 if no
 *  resize is pending. Lets the panel UI render an "applying"
 *  annotation between the slider change and the next tick
 *  applying it. Cleared to -1 by the apply step (whether the
 *  pool create succeeded or fell back to serial).
 *********************************************************/
int  botManagerGetPendingThreads(const struct ServerSim *sim);

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

/* Swap the brain script on an already-added bot, identified by its
 * catalogue index. brainIdx == 0xFF resolves to the CLI-configured
 * default brain via serverSimGetBrainPathForIdx. Returns false when
 * the slot is empty / out-of-range or the index does not resolve to
 * a path. */
bool botManagerSetBrainIdx(struct ServerSim *sim, BYTE playerNum,
                           uint8_t brainIdx);

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
void botManagerSetPreThinkHook(struct ServerSim *sim,
                               void (*hook)(int playerNum));

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
 *NAME:          botManagerSetTeams
 *PURPOSE:
 *  Apply mutual alliance bits between every pair of slots
 *  in [0, numPlayers) that share a team in teamOf[]. Writes
 *  to both the server sim's plyrs and every active bot's
 *  cs.sim.plyrs, so each bot's local view of friend/foe
 *  matches the server. Headless-only helper for harnesses
 *  (BrainTest, bg_game) that bypass the lobby/alliance
 *  packet flow. Idempotent.
 *
 *ARGUMENTS:
 *  sim        - The ServerSim
 *  teamOf     - Array of length numPlayers; teamOf[i] is the
 *               team id for slot i (any small int; only
 *               equality matters)
 *  numPlayers - Length of teamOf (clamped to MAX_TANKS)
 *********************************************************/
void botManagerSetTeams(struct ServerSim *sim,
                        const BYTE *teamOf, BYTE numPlayers);

/*********************************************************
 *NAME:          botManagerDeliverInternalMessage
 *PURPOSE:
 *  Fans an internal (messagedest=0) brain message from
 *  fromPlayer's bot into every allied bot's MessageState
 *  inbox. Used by brain_data.c so the GoalHunter
 *  coordination slate (/info state, /info extra) and any
 *  future bot-to-bot signalling can ride the same brain API
 *  as real chat without touching the chat wire or any
 *  human's newswire. Non-bot allies are skipped because
 *  there is no inbox to write into and human visibility is
 *  the whole thing we are avoiding here. No-op for slots
 *  that aren't active bots in this sim.
 *
 *ARGUMENTS:
 *  sim        - The ServerSim hosting the bot manager
 *  fromPlayer - Slot of the bot that produced the message
 *  msg        - C string (no length prefix); will be wrapped
 *               into the inbox Pascal-string format. May be
 *               truncated to PACKET_MAX_CHAT_MESSAGE bytes.
 *********************************************************/
void botManagerDeliverInternalMessage(struct ServerSim *sim,
                                      BYTE fromPlayer,
                                      const char *msg);

/* Bot-comms debug logger. Appends to "botmsg_debug.log" in the CWD, but ONLY
 * when bot debug mode is on (set via -braindebug / SetDefaultDebugMode) — a
 * no-op otherwise. Used to audit the internal message bus on a dedicated server
 * where SDL_Log output isn't visible. */
void botMsgDebugLog(const char *fmt, ...);

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
BYTE botManagerGetNumBots(const struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerIsBot
 *PURPOSE:
 *  Returns whether the given player slot is an active bot.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to check
 *********************************************************/
bool botManagerIsBot(const struct ServerSim *sim, BYTE playerNum);

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
BrainPathfinder *botManagerGetBrainPathfinder(const struct ServerSim *sim,
                                              BYTE playerNum);

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
OverlayCmdBuffer *botManagerGetOverlayCmds(const struct ServerSim *sim,
                                           BYTE playerNum);

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
double botManagerGetLastThinkMs(const struct ServerSim *sim, BYTE playerNum);

/*********************************************************
 *NAME:          botManagerComputePerBotTargetMs
 *PURPOSE:
 *  Returns the per-bot brain-tick budget for the next
 *  tick, in milliseconds, given the currently active bot
 *  count. Folds in an EWMA of recent serial-stage cost so
 *  the budget tightens when the rest of the tick gets
 *  busier and loosens when it doesn't.
 *
 *  Producer-thread only — reads the EWMA without
 *  synchronisation. Returns the full tick target if the
 *  EWMA hasn't been seeded yet or no bots are active.
 *
 *ARGUMENTS:
 *  activeBots - Number of bots that will dispatch this tick
 *********************************************************/
double botManagerComputePerBotTargetMs(const struct ServerSim *sim,
                                       int activeBots);

/*********************************************************
 *NAME:          botManagerSetSlowMoDebug / botManagerGetSlowMoDebug
 *PURPOSE:
 *  BrainTest "slow-motion" debug mode. When enabled, every
 *  brain.think() is handed an oversized per-tick budget, so it
 *  runs its full capacity tier and is never truncated by the
 *  budget hook, and the consecutive-crash kick is suppressed so
 *  a crashing / over-budget bot stays in the game for
 *  inspection instead of being removed.
 *
 *  Default OFF. The real game / WinBoloDS never enables it, so
 *  production timing and behaviour are unchanged — this only
 *  affects a host that explicitly turns it on (BrainTest). The
 *  matching wall-clock slowdown is paced by the host's tick
 *  scheduler, not here.
 *
 *  Process-global (a debug toggle, not per-sim game state).
 *********************************************************/
void botManagerSetSlowMoDebug(int on);
int  botManagerGetSlowMoDebug(void);

/*********************************************************
 *NAME:          botManagerFlushBrainLogs
 *PURPOSE:
 *  Invokes each active bot brain's _G.__brain_flush_logs()
 *  Lua hook, which drains the batched print2 log to disk
 *  immediately. Intended for the host to call when the sim
 *  is paused so the on-screen tick's log is readable.
 *
 *  Producer-thread only, and only safe to call between
 *  ticks (no brain.think() in flight) — it enters each
 *  bot's lua_State directly.
 *
 *ARGUMENTS:
 *  sim - The ServerSim whose bots to flush
 *********************************************************/
void botManagerFlushBrainLogs(struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerRecordSerialMs
 *PURPOSE:
 *  Feed the EWMA with the serial-stage cost of the tick
 *  that just completed (the parts of botManagerTick that
 *  are not the parallel brain-think dispatch). The next
 *  call to botManagerComputePerBotTargetMs uses the
 *  updated EWMA.
 *
 *  Producer-thread only — writes the EWMA without
 *  synchronisation. The first call seeds the EWMA
 *  directly to avoid an init bias toward zero.
 *
 *ARGUMENTS:
 *  ms - Wall-clock milliseconds spent in the tick's
 *       serial stages (snapshot/sync + input send).
 *********************************************************/
void botManagerRecordSerialMs(struct ServerSim *sim, double ms);

/*********************************************************
 *NAME:          botManagerHasAnyBot
 *PURPOSE:
 *  Returns whether any bot slot is currently active. Used
 *  by the server `info` command to decide whether to print
 *  the bot pool summary block. Producer-thread only — caller
 *  must hold the server mutex; reads sim->botMgr.bots[] without
 *  synchronisation.
 *********************************************************/
bool botManagerHasAnyBot(const struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerGetBotInfo
 *PURPOSE:
 *  Fills `out` with telemetry for the given bot slot.
 *  Returns false (and zeros `out`) when the slot is not an
 *  active bot.
 *
 *  Producer-thread only — caller must hold the server mutex
 *  so no brain tick is in flight; the accessor reads the
 *  bot's lastThinkMs / overrunCount and walks sim->botMgr.bots[]
 *  without synchronisation.
 *
 *ARGUMENTS:
 *  playerNum - Player slot to query
 *  out       - Output struct to fill
 *********************************************************/
bool botManagerGetBotInfo(const struct ServerSim *sim, BYTE playerNum,
                          BotInfo *out);

/*********************************************************
 *NAME:          botManagerGetPoolStats
 *PURPOSE:
 *  Fills `out` with pool-wide telemetry: worker count,
 *  active bots, serial-stage EWMA, the per-bot target the
 *  next tick will use, the most recent brain-dispatch
 *  wall-clock, and the sum of per-bot overrun counters.
 *
 *  Producer-thread only — caller must hold the server mutex;
 *  reads sim->botMgr EWMAs and walks sim->botMgr.bots[] without
 *  synchronisation.
 *
 *ARGUMENTS:
 *  out - Output struct to fill (must not be NULL)
 *********************************************************/
void botManagerGetPoolStats(const struct ServerSim *sim, BotPoolStats *out);

/*********************************************************
 *NAME:          botManagerGetActiveBotCount
 *PURPOSE:
 *  Returns the count of currently-active bot slots.
 *
 *  Worker-thread safe. Reads only bots[i].active, which the
 *  producer thread mutates between ticks (add/remove run
 *  producer-side), so worker-thread reads during brain.think
 *  see a stable count. Worst case on a 1→2 transition is
 *  the new bot's first tick observes the new count one tick
 *  early — fine for safety guards.
 *
 *  The heavier botManagerGetPoolStats remains producer-only;
 *  use this when only the active-bot count is needed.
 *********************************************************/
int botManagerGetActiveBotCount(const struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerActiveBotCountForLua
 *PURPOSE:
 *  Active-bot count for the sim that owns the bot whose
 *  lua_State this is. Used by brain C bindings that have
 *  only an L (e.g. GoalHunter's pillcontrib overlay) and
 *  need to gate on multi-bot vs single-bot mode.
 *
 *  Returns 0 when L has no associated bot (e.g. the
 *  singleton GUI brain) so callers using a `> 1` multi-bot
 *  check still let the binding run.
 *********************************************************/
int botManagerActiveBotCountForLua(struct lua_State *L);

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
char *botManagerEvalLuaString(struct ServerSim *sim, BYTE playerNum,
                              const char *src);

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
bool botManagerExecLua(struct ServerSim *sim, BYTE playerNum,
                       const char *src);

/*********************************************************
 *NAME:          botManagerToggleAllBrainDebugMode
 *PURPOSE:
 *  Flip the BRAIN_DEBUG_MODE Lua global on every active bot.
 *  Used by BrainTest's debug-toggle hotkey so the user can
 *  feel production perf without reloading the bot. Returns
 *  the new value (true = debug now on).
 *
 *  Note: only affects un-stripped Lua source. For brains
 *  loaded from stripped opt/, the lua_strip pass already
 *  removed `if BRAIN_DEBUG_MODE then ... end` blocks at
 *  build time, so toggling has no runtime effect there.
 *********************************************************/
bool botManagerToggleAllBrainDebugMode(struct ServerSim *sim);

/*********************************************************
 *NAME:          botManagerShouldAbort
 *PURPOSE:
 *  Returns true if the bot whose lua_State this is has had
 *  its tick-budget exceeded for the current tick. Inner C
 *  loops invoked from cpf_/wsim_/NA bindings poll this at
 *  their natural checkpoint and break out with a partial
 *  result; the count hook then raises tick_budget_exceeded
 *  on the Lua side once control returns to the VM.
 *
 *  Works because botManagerAddBot stashes BotContext * in
 *  lua_getextraspace(L). Returns false if the lua_State has
 *  no associated bot (e.g. the singleton GUI brain) so loops
 *  outside the worker pool stay unbounded.
 *********************************************************/
bool botManagerShouldAbort(struct lua_State *L);

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
bool botManagerGetGoalInfo(const struct ServerSim *sim, BYTE playerNum,
                           BrainGoalInfo *out);

#endif /* BOT_MANAGER_H */
