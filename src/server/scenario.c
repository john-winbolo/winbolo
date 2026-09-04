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
 *Name:          Scenario
 *Filename:      scenario.c
 *Purpose:
 *  Server-side scripted map scenarios — see scenario.h for the model.
 *  One Lua VM per ServerSim, created when the hosted map has a
 *  "<map>.scenario.lua" sidecar. The whole thing runs on the server;
 *  clients see only ordinary wire traffic (owner changes, bot joins,
 *  server messages).
 *
 *  Script surface:
 *    scenario = { name = "...", max_players = 6,    -- optional metadata
 *                 game = "tournament" }             -- forced rules ("open"/
 *                                                   -- "tournament"/"strict"),
 *                                                   -- beats the lobby setting
 *    function on_setup(game) end                    -- once per round, BEFORE any
 *                                                   -- client sees the world:
 *                                                   -- tanks placed, no snapshot
 *                                                   -- yet, mutations are silent
 *    function on_start(game) end                    -- once per round, first tick
 *    function on_tick(game, tick) end               -- every game tick
 *    function on_choose_start(game, p) end          -- EVERY tank placement
 *                                                   -- (spawn + respawn): return
 *                                                   -- a start 1..num_starts to
 *                                                   -- force it, nil for engine
 *                                                   -- pick. Fires before
 *                                                   -- on_start at round start.
 *
 *  `game` API (n is 1-based for pills/bases to match engine numbering;
 *  players are 0-based slots to match everything on the wire):
 *    game.tick()                       -> current game tick
 *    game.max_tanks()                  -> MAX_TANKS
 *    game.map_tile(x, y)               -> terrain byte at (x, y)
 *    game.num_pills() / game.pill(n)   -> {x,y,owner,armour,speed,in_tank}
 *    game.num_bases() / game.base(n)   -> {x,y,owner,armour,shells,mines}
 *    game.num_starts() / game.start(n) -> {x,y,dir}
 *    game.tank(p)                      -> nil, or {mx,my,wx,wy,armour,
 *                                         shells,mines,trees,dir,boat,
 *                                         dead,name,bot}
 *    game.set_pill_owner(n, p|nil)     -- nil/-1 = neutral
 *    game.set_base_owner(n, p|nil)     -- NOTE: a non-neutral -> other
 *                                         non-neutral change DRAINS the
 *                                         base's stock (engine rule)
 *    game.set_base_stock(n, armour, shells, mines)
 *                                      -- set stock outright; each
 *                                         value clamped to the engine
 *                                         max (90), so pass anything
 *                                         big for "full"
 *    game.spawn_bot([name][, brain][, team][, mode])
 *                                      -> playerNum | nil, err; mode
 *                                         "open"/"tournament"/"strict"
 *                                         sets the STARTING LOADOUT
 *                                         (default: the sim's rules)
 *    game.remove_bot(p)                -- free a bot slot (wave cleanup)
 *    game.give_pill(p, n)              -- load pill n into p's tank
 *    game.set_team(p, team)            -- alliance by team id
 *    game.message(text)                -- broadcast, e.g. "Round 5!"
 *    game.end_round([text])            -- programmatic WIN condition
 *********************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "tank.h"
#include "players.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"     /* serverSimSetTeam(Batch) — roster edits */
#include "../common/wb_log.h"
#include "scenario.h"

/* Give up calling into a script after this many consecutive errors —
 * a broken on_tick must not spam the log at 50 Hz forever. */
#define SCENARIO_MAX_ERRORS 20

typedef struct ScenarioState {
    lua_State *L;
    ServerSim *sim;
    /* One lua_State, potentially reached from TWO threads: the server
     * timer thread (on_setup / on_tick / on_choose_start during sim
     * work) and the host GUI thread (lobby publishes → on_lobby /
     * query hooks in single-player, where the lobby writes directly
     * instead of going over the wire). Lua is not re-entrant across
     * threads — the 12:24 Ready-click crash was two threads inside one
     * VM. Every entry point below serializes on this recursion-aware
     * lock (same-thread re-entry, e.g. a hook's roster edit publishing
     * back into scenarioLobbyChanged, just bumps the depth). */
    SDL_Mutex   *luaLock;
    SDL_ThreadID luaOwner;
    int          luaDepth;
    gameType   game;
    bool       hiddenMines;
    int        maxPlayers;              /* scenario.max_players; 0 = none */
    int        errorCount;              /* consecutive hook failures */
    bool       disabled;
    int        tickRef;                 /* registry ref to on_tick (or NOREF) */
    int        gameRef;                 /* registry ref to the game table */
    BYTE       teamOf[MAX_TANKS];       /* game.set_team state */
    BYTE       spawnTeamHint[MAX_TANKS];/* team+1 a spawn_bot call intends
                                         * for a slot, staged BEFORE the
                                         * engine creates the tank (the join
                                         * path wipes the lobby slot, so
                                         * lobbyPlayers can't carry it);
                                         * l_lobby_slot reports it so
                                         * on_choose_start sees the side of
                                         * a tank being born. 0 = none. */
    char       winMessage[256];         /* game.end_round text */
    char       name[64];                /* scenario.name (logs) */
    char       description[256];        /* scenario.description — one or two
                                         * lobby lines under the map info */
    char       defaultBrain[256];       /* scenario.default_brain fallback */
    char       path[FILENAME_MAX];      /* sidecar path, for per-round reboots */
    bool       started;                 /* on_start fired for this round */
    bool       setupDone;               /* on_setup fired for this round */
    bool       inChooseStart;           /* re-entrancy guard: on_choose_start
                                         * must not spawn tanks (which would
                                         * recurse into the hook) */
    bool       inLobbyHook;             /* re-entrancy guard: on_lobby edits
                                         * the roster, roster edits publish,
                                         * publishes fire on_lobby */
    int        forcedGame;              /* scenario.game metadata as a gameType;
                                         * -1 = script doesn't force one */
} ScenarioState;

static ScenarioState *scState(const ServerSim *sim) {
    return sim ? (ScenarioState *)sim->scenario : NULL;
}

/* Recursion-aware VM lock (see luaLock above). SDL mutexes are not
 * guaranteed recursive, so ownership is tracked by thread id. */
static void scLuaLock(ScenarioState *st) {
    SDL_ThreadID me = SDL_GetCurrentThreadID();
    if (st->luaOwner == me) {
        st->luaDepth++;
        return;
    }
    SDL_LockMutex(st->luaLock);
    st->luaOwner = me;
    st->luaDepth = 1;
}

static void scLuaUnlock(ScenarioState *st) {
    if (--st->luaDepth > 0) return;
    st->luaOwner = 0;
    SDL_UnlockMutex(st->luaLock);
}

/* Every C closure carries the ScenarioState as a lightuserdata upvalue. */
static ScenarioState *scUp(lua_State *L) {
    return (ScenarioState *)lua_touserdata(L, lua_upvalueindex(1));
}

/* ------------------------------------------------------------------ */
/* Reads                                                               */
/* ------------------------------------------------------------------ */

static int l_tick(lua_State *L) {
    ScenarioState *st = scUp(L);
    lua_pushinteger(L, (lua_Integer)st->sim->tick);
    return 1;
}

static int l_max_tanks(lua_State *L) {
    lua_pushinteger(L, MAX_TANKS);
    return 1;
}

static int l_map_tile(lua_State *L) {
    ScenarioState *st = scUp(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    if (x < 0 || x > 255 || y < 0 || y > 255) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, mapGetPos(&serverSimGetGameSim(st->sim)->mp,
                                 (BYTE)x, (BYTE)y));
    return 1;
}

static int l_num_pills(lua_State *L) {
    ScenarioState *st = scUp(L);
    lua_pushinteger(L, pillsGetNumPills(&serverSimGetGameSim(st->sim)->pb));
    return 1;
}

static int l_pill(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    if (n < 1 || n > pillsGetNumPills(&gs->pb)) {
        lua_pushnil(L);
        return 1;
    }
    {
        pillbox *pi = &(*gs->pb).item[n - 1];
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, pi->x);       lua_setfield(L, -2, "x");
        lua_pushinteger(L, pi->y);       lua_setfield(L, -2, "y");
        lua_pushinteger(L, pi->owner);   lua_setfield(L, -2, "owner");
        lua_pushinteger(L, pi->armour);  lua_setfield(L, -2, "armour");
        lua_pushinteger(L, pi->speed);   lua_setfield(L, -2, "speed");
        lua_pushboolean(L, pi->inTank);  lua_setfield(L, -2, "in_tank");
    }
    return 1;
}

static int l_num_bases(lua_State *L) {
    ScenarioState *st = scUp(L);
    lua_pushinteger(L, basesGetNumBases(&serverSimGetGameSim(st->sim)->bs));
    return 1;
}

static int l_num_starts(lua_State *L) {
    ScenarioState *st = scUp(L);
    lua_pushinteger(L, startsGetNumStarts(&serverSimGetGameSim(st->sim)->ss));
    return 1;
}

/* game.start(n) -> {x, y, dir} — the map's start list, so
 * on_choose_start can pick by position instead of memorised indices. */
static int l_start(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    start item;
    if (n < 1 || n > (int)startsGetNumStarts(&gs->ss)) {
        lua_pushnil(L);
        return 1;
    }
    startsGetStartStruct(&gs->ss, &item, (BYTE)n);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, item.x);   lua_setfield(L, -2, "x");
    lua_pushinteger(L, item.y);   lua_setfield(L, -2, "y");
    lua_pushinteger(L, item.dir); lua_setfield(L, -2, "dir");
    return 1;
}

static int l_base(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    if (n < 1 || n > basesGetNumBases(&gs->bs)) {
        lua_pushnil(L);
        return 1;
    }
    {
        base *b = &(*gs->bs).item[n - 1];
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, b->x);       lua_setfield(L, -2, "x");
        lua_pushinteger(L, b->y);       lua_setfield(L, -2, "y");
        lua_pushinteger(L, b->owner);   lua_setfield(L, -2, "owner");
        lua_pushinteger(L, b->armour);  lua_setfield(L, -2, "armour");
        lua_pushinteger(L, b->shells);  lua_setfield(L, -2, "shells");
        lua_pushinteger(L, b->mines);   lua_setfield(L, -2, "mines");
    }
    return 1;
}

static int l_tank(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    GameSim *gs = serverSimGetGameSim(sim);
    int p = (int)luaL_checkinteger(L, 1);          /* 0-based player slot */
    if (p < 0 || p >= MAX_TANKS || !serverSimIsPlayerConnected(sim, (BYTE)p)
        || gs->tanks[p] == NULL) {
        lua_pushnil(L);
        return 1;
    }
    {
        tank *t = &gs->tanks[p];
        WORLD wx, wy;
        BYTE shells, mines, armour, trees;
        char name[64];
        bool dead;
        tankGetWorld(t, &wx, &wy);
        tankGetStats(t, &shells, &mines, &armour, &trees);
        dead = (tankGetDeathWait(t) > 0) || (armour > TANK_FULL_ARMOUR);
        name[0] = '\0';
        playersGetPlayerName(&gs->plyrs, (BYTE)p, name, sizeof(name), FALSE);
        lua_createtable(L, 0, 13);
        lua_pushinteger(L, wx >> TANK_SHIFT_MAPSIZE); lua_setfield(L, -2, "mx");
        lua_pushinteger(L, wy >> TANK_SHIFT_MAPSIZE); lua_setfield(L, -2, "my");
        lua_pushinteger(L, wx);            lua_setfield(L, -2, "wx");
        lua_pushinteger(L, wy);            lua_setfield(L, -2, "wy");
        lua_pushinteger(L, armour);        lua_setfield(L, -2, "armour");
        lua_pushinteger(L, shells);        lua_setfield(L, -2, "shells");
        lua_pushinteger(L, mines);         lua_setfield(L, -2, "mines");
        lua_pushinteger(L, trees);         lua_setfield(L, -2, "trees");
        lua_pushinteger(L, tankGet256Dir(t)); lua_setfield(L, -2, "dir");
        lua_pushboolean(L, tankIsOnBoat(t));  lua_setfield(L, -2, "boat");
        lua_pushboolean(L, dead);          lua_setfield(L, -2, "dead");
        lua_pushstring(L, name);           lua_setfield(L, -2, "name");
        lua_pushboolean(L, serverSimIsBot(sim, (BYTE)p));
        lua_setfield(L, -2, "bot");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Writes                                                              */
/* ------------------------------------------------------------------ */

static BYTE ownerArg(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return NEUTRAL;
    {
        int o = (int)luaL_checkinteger(L, idx);
        if (o < 0 || o >= MAX_TANKS) return NEUTRAL;
        return (BYTE)o;
    }
}

static int l_set_pill_owner(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    BYTE owner = ownerArg(L, 2);
    if (n < 1 || n > pillsGetNumPills(&gs->pb)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    pillsSetPillOwner(gs, &gs->pb, (BYTE)n, owner, FALSE);
    lua_pushboolean(L, TRUE);
    return 1;
}

static int l_set_base_owner(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    BYTE owner = ownerArg(L, 2);
    if (n < 1 || n > basesGetNumBases(&gs->bs)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    basesSetBaseOwner(gs, (BYTE)n, owner, FALSE);
    lua_pushboolean(L, TRUE);
    return 1;
}

/* game.set_base_stock(n, armour, shells, mines): set base n's stock
 * outright. Each value is CLAMPED to the engine maximum rather than
 * refused, so a script can just ask for "full" with a big number.
 *
 * This exists because set_base_owner DRAINS a base: basesSetBaseOwner
 * zeroes armour/shells/mines whenever it moves a base from one
 * non-neutral owner to another, which is exactly what a scenario that
 * re-deals its bases every round (or every wave) does. Returns true, or
 * false for an unknown base.
 *
 * Nothing else to publish: base stock rides the periodic full base/pill
 * sync in the snapshot builder, which reads the bases structure live -
 * the same way the engine's own refuel timer's writes reach clients. */
static int l_set_base_stock(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int n = (int)luaL_checkinteger(L, 1);          /* 1-based */
    lua_Integer armour = luaL_checkinteger(L, 2);
    lua_Integer shells = luaL_checkinteger(L, 3);
    lua_Integer mines  = luaL_checkinteger(L, 4);
    if (armour < 0) armour = 0;
    if (shells < 0) shells = 0;
    if (mines  < 0) mines  = 0;
    if (armour > BASE_FULL_ARMOUR) armour = BASE_FULL_ARMOUR;
    if (shells > BASE_FULL_SHELLS) shells = BASE_FULL_SHELLS;
    if (mines  > BASE_FULL_MINES)  mines  = BASE_FULL_MINES;
    if (n < 1 || n > basesGetNumBases(&gs->bs)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    lua_pushboolean(L, basesSetStock(gs, (BYTE)n, (BYTE)armour,
                                     (BYTE)shells, (BYTE)mines));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Spawning / teams                                                    */
/* ------------------------------------------------------------------ */

/* Optional spawn-mode string -> gameType. The mode decides the bot's
 * STARTING LOADOUT only (open = full tank; tournament = farm first),
 * independent of the rules the humans play under — a survival map can
 * run tournament for humans while its wave bots arrive armed. */
static gameType scGameTypeArg(lua_State *L, int idx, gameType fallback) {
    const char *mode = luaL_optstring(L, idx, NULL);
    if (mode == NULL || mode[0] == '\0') return fallback;
    if (strcmp(mode, "open") == 0)       return gameOpen;
    if (strcmp(mode, "tournament") == 0) return gameTournament;
    if (strcmp(mode, "strict") == 0)     return gameStrictTournament;
    return (gameType)luaL_error(L,
        "bad spawn mode '%s' (want 'open', 'tournament' or 'strict')", mode);
}

static int l_spawn_bot(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    const char *name  = luaL_optstring(L, 1, NULL);
    const char *brain = luaL_optstring(L, 2, NULL);
    int hasTeam = !lua_isnoneornil(L, 3);
    int team = hasTeam ? (int)luaL_checkinteger(L, 3) : 0;
    gameType spawnGame = scGameTypeArg(L, 4,
        gameTypeGet(&serverSimGetGameSim(sim)->game));
    char botName[32];
    BYTE slot;
    int s;

    /* Fill from the TOP down: humans join through serverSimFindFreeSlot,
     * which scans 0..maxPlayers-1, so keeping scenario bots in the high
     * slots stops a wave from squatting on the slots humans are allowed
     * to take (e.g. max_players=6 + 10 wave bots = exactly 16). */
    for (s = MAX_TANKS - 1; s >= 0; s--) {
        if (!serverSimIsPlayerConnected(sim, (BYTE)s)) break;
    }
    if (s < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free player slot");
        return 2;
    }
    slot = (BYTE)s;
    /* Brain fallback chain: explicit argument > scenario.default_brain
     * (the script ships with the map and knows what it wants) > the
     * sim's operator-configured bot brain. */
    if (brain == NULL || brain[0] == '\0') {
        brain = st->defaultBrain;
    }
    if (brain == NULL || brain[0] == '\0') {
        brain = serverSimGetBotBrainPath(sim);
    }
    if (brain == NULL || brain[0] == '\0') {
        lua_pushnil(L);
        lua_pushstring(L, "no brain path configured");
        return 2;
    }
    if (name != NULL && name[0] != '\0') {
        SDL_strlcpy(botName, name, sizeof(botName));
    } else {
        snprintf(botName, sizeof(botName), "Bot %d", (int)slot);
    }

    /* Arm the slot's spawn-loadout override BEFORE the tank exists so
     * tankCreate loads it with the requested mode's items — and every
     * respawn after that gets the same loadout (cleared when the slot's
     * player is removed). This is how a tournament survival map fields
     * fully-armed open-mode wave bots that come back armed. */
    serverSimGetGameSim(sim)->spawnLoadout[slot] = (BYTE)spawnGame;

    /* Stage the intended team where the join path can't wipe it:
     * on_choose_start fires DURING tank creation (inside
     * serverSimCreateBot), and the create path resets the lobby slot —
     * so a team-keyed placement rule reads this hint via
     * game.lobby_slot until the real team lands below. */
    if (hasTeam && team >= 0 && team < MAX_TANKS) {
        st->spawnTeamHint[slot] = (BYTE)(team + 1);
    }

    if (!serverSimCreateBot(sim, slot, brain, botName,
                            (aiType)serverSimGetBotAiType(sim),
                            spawnGame, st->hiddenMines)) {
        serverSimGetGameSim(sim)->spawnLoadout[slot] = 0;
        st->spawnTeamHint[slot] = 0;
        lua_pushnil(L);
        lua_pushstring(L, "serverSimCreateBot failed");
        return 2;
    }
    st->spawnTeamHint[slot] = 0;
    if (hasTeam) {
        st->teamOf[slot] = (BYTE)team;
        serverSimSetBotTeams(sim, st->teamOf, MAX_TANKS);
        /* Restore the lobby-team mirror the create path just reset. */
        if (team >= 0 && team < MAX_TANKS) {
            serverSimSetTeamBatch(sim, slot, (BYTE)team);
        }
    }
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "scenario: spawned bot '%s' in slot %d (brain=%s team=%s)",
        botName, (int)slot, brain, hasTeam ? "set" : "-");
    lua_pushinteger(L, slot);
    return 1;
}

static int l_remove_bot(lua_State *L) {
    ScenarioState *st = scUp(L);
    int p = (int)luaL_checkinteger(L, 1);
    /* BOTS ONLY — the invariant "all human players are represented"
     * holds against the in-game API too, not just the lobby one. */
    if (p < 0 || p >= MAX_TANKS || !serverSimIsBot(st->sim, (BYTE)p)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    /* Script-driven removal: the leave event goes out flagged silent so
     * clients don't newswire "has quit" for wave churn — ONLY this path;
     * every other removal (host kick, crash kick, disconnect) announces
     * exactly as before. */
    st->sim->scenarioSilentRemove = TRUE;
    serverSimRemoveBot(st->sim, (BYTE)p);
    st->sim->scenarioSilentRemove = FALSE;
    /* Break the slot out of its team so a future occupant doesn't
     * inherit stale alliance intent from our side. (Engine alliances
     * for the departed player are cleaned by the remove itself.) */
    st->teamOf[p] = (BYTE)(100 + p);   /* non-colliding default (see scBoot) */
    lua_pushboolean(L, TRUE);
    return 1;
}

static int l_set_team(lua_State *L) {
    ScenarioState *st = scUp(L);
    int p = (int)luaL_checkinteger(L, 1);
    int team = (int)luaL_checkinteger(L, 2);
    if (p < 0 || p >= MAX_TANKS) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    st->teamOf[p] = (BYTE)team;
    serverSimSetBotTeams(st->sim, st->teamOf, MAX_TANKS);
    /* Mirror into the lobby roster too, so game.lobby_slot(p).team and
     * team-keyed hooks (on_choose_start placing enemies by TEAM) see
     * the same side for mid-game spawns as for lobby-seeded bots. */
    if (team >= 0 && team < MAX_TANKS) {
        serverSimSetTeamBatch(st->sim, (BYTE)p, (BYTE)team);
    }
    lua_pushboolean(L, TRUE);
    return 1;
}

/* game.enemy_team_size() -> the CURRENT size of lobby Team 2 — the
 * enemy side's roster as the host arranged it (seeded bots plus any
 * additions/removals). Scripts read it once at round start and use it
 * as their wave size. */
static int l_enemy_team_size(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    int n = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].teamNumber == 2) {
            n++;
        }
    }
    lua_pushinteger(L, n);
    return 1;
}

/* game.lobby_slot(p) -> nil, or {connected, bot, team, name} — the
 * LOBBY view of a slot (game.tank covers the in-game view). This is
 * what lets query hooks like show_add_team_button(game) reason about
 * the roster ("max 10 humans in any team arrangement", ...). */
static int l_lobby_slot(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    int p = (int)luaL_checkinteger(L, 1);
    if (p < 0 || p >= MAX_TANKS || !sim->playerConnected[p]) {
        lua_pushnil(L);
        return 1;
    }
    {
        char name[64];
        name[0] = '\0';
        playersGetPlayerName(&serverSimGetGameSim(sim)->plyrs, (BYTE)p,
                             name, sizeof(name), FALSE);
        int teamOut = sim->lobbyPlayers[p].teamNumber;
        if (st->spawnTeamHint[p] != 0) {
            teamOut = st->spawnTeamHint[p] - 1;   /* mid-spawn intent */
        }
        lua_createtable(L, 0, 4);
        lua_pushboolean(L, TRUE);          lua_setfield(L, -2, "connected");
        lua_pushboolean(L, serverSimIsBot(sim, (BYTE)p));
        lua_setfield(L, -2, "bot");
        lua_pushinteger(L, teamOut);       lua_setfield(L, -2, "team");
        lua_pushstring(L, name);           lua_setfield(L, -2, "name");
    }
    return 1;
}

/* game.set_tile(x, y, terrain): server-authoritative terrain write via
 * mapSetPos, so the change rides the normal EVENT_MAP_CHANGE delta to
 * every client (humans AND bot ClientSims). Terrain is the engine
 * code (BUILDING 0..MINE_GRASS 15). Best used from a RUNNING tick
 * (on_start/on_tick) — the per-tick map-event buffer fans it out;
 * mutations made in on_setup ride the baseline snapshot instead. */
static int l_set_tile(lua_State *L) {
    ScenarioState *st = scUp(L);
    GameSim *gs = serverSimGetGameSim(st->sim);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int t = (int)luaL_checkinteger(L, 3);
    /* Terrain 0..15 are the normal codes; 0xFF is DEEP_SEA (global.h), which
     * scenarios need too — e.g. restoring open sea under a dead pill the map
     * loader put a ROAD pedestal beneath (tests/water_pills.scenario.lua). */
    if (x < 0 || x > 255 || y < 0 || y > 255 || t < 0 || (t > 15 && t != 0xFF)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, (BYTE)t, TRUE, FALSE);
    lua_pushboolean(L, TRUE);
    return 1;
}

/* game.give_pill(p, n): load pill n (1-based, matching game.pill) into
 * player p's tank as if it had been driven over — the survival-map seam
 * for "each enemy tank spawns carrying pillboxes". Returns true, or
 * nil + reason. */
static int l_give_pill(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    GameSim *gs = serverSimGetGameSim(sim);
    int p = (int)luaL_checkinteger(L, 1);          /* 0-based player slot */
    int n = (int)luaL_checkinteger(L, 2);          /* 1-based pill number */

    if (p < 0 || p >= MAX_TANKS || !serverSimIsPlayerConnected(sim, (BYTE)p)
        || gs->tanks[p] == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "no such player");
        return 2;
    }
    if (n < 1 || n > (int)pillsGetNumPills(&gs->pb)) {
        lua_pushnil(L);
        lua_pushstring(L, "no such pill");
        return 2;
    }
    if (!tankGivePill(gs, &gs->tanks[p], (BYTE)n)) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "scenario: give_pill(%d, %d) refused (inTank=%d armour=%u)",
            p, n, (int)(*gs->pb).item[n - 1].inTank,
            (unsigned)tankGetArmour(&gs->tanks[p]));
        lua_pushnil(L);
        lua_pushstring(L, "pill already carried or tank dead");
        return 2;
    }
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "scenario: pill %d loaded into player %d's tank", n, p);
    lua_pushboolean(L, TRUE);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lobby roster edits — the scenario has FULL authority over the team
 * lists, under two engine-enforced invariants: bots + players never
 * exceed MAX_TANKS (adds are refused at the cap), and human players
 * are always represented (they can be MOVED between teams but never
 * removed). Lobby state only.                                         */
/* ------------------------------------------------------------------ */

/* game.lobby_add_bot(team[, name]) -> slot | nil, err. Adds a real
 * lobby bot (pool-named when no name given) in the highest free slot
 * so the low seats stay open for humans. */
static int l_lobby_add_bot(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    int team = (int)luaL_checkinteger(L, 1);
    const char *name = luaL_optstring(L, 2, NULL);
    const char *brain;
    char botName[64];
    int slot;

    if (sim->state != serverStateLobby) {
        lua_pushnil(L); lua_pushstring(L, "not in lobby"); return 2;
    }
    if (team < 1 || team >= MAX_TANKS) {
        lua_pushnil(L); lua_pushstring(L, "bad team"); return 2;
    }
    for (slot = MAX_TANKS - 1; slot >= 0; slot--) {
        if (!serverSimIsPlayerConnected(sim, (BYTE)slot)) break;
    }
    if (slot < 0) {
        lua_pushnil(L); lua_pushstring(L, "lobby full"); return 2;
    }
    brain = serverSimGetBotBrainPath(sim);
    if (brain == NULL || brain[0] == '\0') brain = st->defaultBrain;
    if (brain == NULL || brain[0] == '\0') {
        lua_pushnil(L); lua_pushstring(L, "no brain configured"); return 2;
    }
    if (name != NULL && name[0] != '\0') {
        SDL_strlcpy(botName, name, sizeof(botName));
    } else {
        snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
    }
    if (!serverSimCreateBot(sim, (BYTE)slot, brain, botName,
                            (aiType)serverSimGetBotAiType(sim),
                            gameTypeGet(&serverSimGetGameSim(sim)->game),
                            st->hiddenMines)) {
        lua_pushnil(L); lua_pushstring(L, "add failed"); return 2;
    }
    serverSimSetTeam(sim, (BYTE)slot, (BYTE)team);
    serverSimPublishLobbySlot(sim, (BYTE)slot);
    lua_pushinteger(L, slot);
    return 1;
}

/* game.lobby_remove_bot(p) -> true | nil, err. BOTS ONLY — humans are
 * always represented and cannot be removed by the script. */
static int l_lobby_remove_bot(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    int p = (int)luaL_checkinteger(L, 1);
    if (sim->state != serverStateLobby) {
        lua_pushnil(L); lua_pushstring(L, "not in lobby"); return 2;
    }
    if (p < 0 || p >= MAX_TANKS || !serverSimIsBot(sim, (BYTE)p)) {
        lua_pushnil(L); lua_pushstring(L, "not a bot"); return 2;
    }
    serverSimRemoveBot(sim, (BYTE)p);
    lua_pushboolean(L, TRUE);
    return 1;
}

/* game.lobby_set_team(p, team) -> true | nil, err. Works on humans and
 * bots alike — rearranging is allowed, removing humans is not. */
static int l_lobby_set_team(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    int p = (int)luaL_checkinteger(L, 1);
    int team = (int)luaL_checkinteger(L, 2);
    if (sim->state != serverStateLobby) {
        lua_pushnil(L); lua_pushstring(L, "not in lobby"); return 2;
    }
    if (p < 0 || p >= MAX_TANKS || !serverSimIsPlayerConnected(sim, (BYTE)p)
        || team < 1 || team >= MAX_TANKS) {
        lua_pushnil(L); lua_pushstring(L, "bad slot or team"); return 2;
    }
    serverSimSetTeam(sim, (BYTE)p, (BYTE)team);
    serverSimPublishLobbySlot(sim, (BYTE)p);
    lua_pushboolean(L, TRUE);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Messages / round end                                                */
/* ------------------------------------------------------------------ */

static int l_message(lua_State *L) {
    ScenarioState *st = scUp(L);
    const char *msg = luaL_checkstring(L, 1);
    serverSimScenarioPublish(st->sim, msg);
    return 0;
}

static int l_end_round(lua_State *L) {
    ScenarioState *st = scUp(L);
    const char *msg = luaL_optstring(L, 1, "*** Scenario complete. ***");
    SDL_strlcpy(st->winMessage, msg, sizeof(st->winMessage));
    serverSimScenarioPublish(st->sim, msg);
    serverSimScenarioEndRound(st->sim);
    return 0;
}

/* ------------------------------------------------------------------ */
/* VM setup / hooks                                                    */
/* ------------------------------------------------------------------ */

static void scRegister(lua_State *L, ScenarioState *st,
                       const char *name, lua_CFunction fn) {
    lua_pushlightuserdata(L, st);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

static void scBuildGameTable(lua_State *L, ScenarioState *st) {
    lua_newtable(L);
    scRegister(L, st, "tick",           l_tick);
    scRegister(L, st, "max_tanks",      l_max_tanks);
    scRegister(L, st, "map_tile",       l_map_tile);
    scRegister(L, st, "num_pills",      l_num_pills);
    scRegister(L, st, "pill",           l_pill);
    scRegister(L, st, "num_bases",      l_num_bases);
    scRegister(L, st, "base",           l_base);
    scRegister(L, st, "num_starts",     l_num_starts);
    scRegister(L, st, "start",          l_start);
    scRegister(L, st, "tank",           l_tank);
    scRegister(L, st, "set_pill_owner", l_set_pill_owner);
    scRegister(L, st, "set_base_owner", l_set_base_owner);
    scRegister(L, st, "set_base_stock", l_set_base_stock);
    scRegister(L, st, "set_tile",       l_set_tile);
    scRegister(L, st, "spawn_bot",      l_spawn_bot);
    scRegister(L, st, "remove_bot",     l_remove_bot);
    scRegister(L, st, "give_pill",      l_give_pill);
    scRegister(L, st, "enemy_team_size", l_enemy_team_size);
    scRegister(L, st, "lobby_slot",     l_lobby_slot);
    scRegister(L, st, "lobby_add_bot",  l_lobby_add_bot);
    scRegister(L, st, "lobby_remove_bot", l_lobby_remove_bot);
    scRegister(L, st, "lobby_set_team", l_lobby_set_team);
    scRegister(L, st, "set_team",       l_set_team);
    scRegister(L, st, "message",        l_message);
    scRegister(L, st, "end_round",      l_end_round);
    /* NEUTRAL constant so scripts don't hardcode 0xFF */
    lua_pushinteger(L, NEUTRAL);
    lua_setfield(L, -2, "NEUTRAL");
    /* keep a registry ref AND expose as global `game` */
    lua_pushvalue(L, -1);
    st->gameRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setglobal(L, "game");
}

/* Push the hook function named `fn` — either scenario.<fn> or global
 * <fn>. Returns TRUE with the function on the stack. */
static bool scPushHook(lua_State *L, const char *fn) {
    lua_getglobal(L, "scenario");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, fn);
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) return TRUE;
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    lua_getglobal(L, fn);
    if (lua_isfunction(L, -1)) return TRUE;
    lua_pop(L, 1);
    return FALSE;
}

static void scReportError(ScenarioState *st, const char *what) {
    const char *err = lua_tostring(st->L, -1);
    WB_LOG_ERROR(WB_LOG_CAT_SERVER, "scenario %s error: %s",
                 what, err ? err : "(no message)");
    lua_pop(st->L, 1);
    st->errorCount++;
    if (st->errorCount >= SCENARIO_MAX_ERRORS) {
        st->disabled = TRUE;
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "scenario disabled after %d consecutive errors", st->errorCount);
        serverSimScenarioPublish(st->sim,
            "*** Scenario script disabled (script errors — see server log) ***");
    }
}

/* Boot (or re-boot) the VM from st->path: fresh lua_State, run the
 * chunk, capture metadata, cache on_tick. Resets all per-round script
 * state by construction; on_start fires lazily from the first
 * scenarioTick of the (re)booted VM — i.e. once the round is actually
 * running with the world in place — never here, because at boot time
 * neither lobbyEnabled nor the round's tanks are reliably set yet.
 * Returns FALSE (with L=NULL) when the chunk fails to load. */
static bool scBootLocked(ScenarioState *st);

static bool scBoot(ScenarioState *st) {
    bool ok;
    scLuaLock(st);
    ok = scBootLocked(st);
    scLuaUnlock(st);
    return ok;
}

static bool scBootLocked(ScenarioState *st) {
    lua_State *L;
    BYTE i;

    if (st->L != NULL) {
        lua_close(st->L);
        st->L = NULL;
    }
    st->tickRef = LUA_NOREF;
    st->gameRef = LUA_NOREF;
    st->errorCount = 0;
    st->disabled = FALSE;
    st->started = FALSE;
    st->setupDone = FALSE;
    st->inChooseStart = FALSE;
    st->inLobbyHook = FALSE;
    memset(st->spawnTeamHint, 0, sizeof(st->spawnTeamHint));
    st->maxPlayers = 0;
    st->description[0] = '\0';
    st->forcedGame = -1;
    st->winMessage[0] = '\0';
    /* teamOf defaults must NEVER collide with a real team id (1..15):
     * botManagerSetTeams allies any two slots holding EQUAL values, so
     * a default of the slot index meant "slot 2" matched "team 2" and
     * silently allied whoever sat in seat 2 with the whole enemy team
     * (the all-pills-went-green bug). 100+i is unique per slot and
     * outside the team-id range, so untouched slots pair with nobody. */
    for (i = 0; i < MAX_TANKS; i++) st->teamOf[i] = (BYTE)(100 + i);

    L = luaL_newstate();
    if (L == NULL) return FALSE;
    st->L = L;
    /* The sidecar lives next to the operator's own map files and runs
     * with server privileges by design ("the server will execute all
     * this") — full stdlib is intentional. */
    luaL_openlibs(L);
    scBuildGameTable(L, st);

    if (luaL_loadfile(L, st->path) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER, "scenario load '%s' failed: %s",
                     st->path,
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_close(L);
        st->L = NULL;
        return FALSE;
    }

    /* Metadata */
    lua_getglobal(L, "scenario");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "max_players");
        if (lua_isnumber(L, -1)) {
            st->maxPlayers = (int)lua_tointeger(L, -1);
            if (st->maxPlayers < 1) st->maxPlayers = 0;
            if (st->maxPlayers > MAX_TANKS) st->maxPlayers = MAX_TANKS;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "name");
        if (lua_isstring(L, -1)) {
            SDL_strlcpy(st->name, lua_tostring(L, -1), sizeof(st->name));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "description");
        if (lua_isstring(L, -1)) {
            SDL_strlcpy(st->description, lua_tostring(L, -1),
                        sizeof(st->description));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "default_brain");
        if (lua_isstring(L, -1)) {
            SDL_strlcpy(st->defaultBrain, lua_tostring(L, -1),
                        sizeof(st->defaultBrain));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "game");
        if (lua_isstring(L, -1)) {
            const char *g = lua_tostring(L, -1);
            if (strcmp(g, "open") == 0)            st->forcedGame = gameOpen;
            else if (strcmp(g, "tournament") == 0) st->forcedGame = gameTournament;
            else if (strcmp(g, "strict") == 0)     st->forcedGame = gameStrictTournament;
            else WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "scenario.game '%s' unknown (want open/tournament/strict) "
                "— ignored", g);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    /* The map's declared rules beat the lobby's game-type setting.
     * Applied at every boot: at sim create for lobby-less runs, and at
     * every round start via scenarioReset — AFTER the world reset and
     * BEFORE tank creation, so the round's tanks spawn under the
     * scripted rules whatever the lobby was set to. */
    if (st->forcedGame != -1) {
        gameTypeSet(&serverSimGetGameSim(st->sim)->game,
                    (gameType)st->forcedGame);
        st->game = (gameType)st->forcedGame;
    }

    /* Cache on_tick */
    if (scPushHook(L, "on_tick")) {
        st->tickRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "scenario booted: '%s' (%s) max_players=%d on_tick=%s",
        st->name[0] ? st->name : "(unnamed)", st->path, st->maxPlayers,
        st->tickRef != LUA_NOREF ? "yes" : "no");
    return TRUE;
}

bool scenarioLoad(ServerSim *sim, const char *mapFileName,
                  gameType game, bool hiddenMines) {
    char path[FILENAME_MAX];
    size_t len;
    FILE *probe;
    ScenarioState *st;

    if (sim == NULL || mapFileName == NULL) return FALSE;
    sim->scenario = NULL;

    /* Sidecar path: strip a trailing ".map", append ".scenario.lua". */
    SDL_strlcpy(path, mapFileName, sizeof(path));
    len = strlen(path);
    if (len >= 4 && strcmp(path + len - 4, ".map") == 0) {
        path[len - 4] = '\0';
    }
    SDL_strlcat(path, ".scenario.lua", sizeof(path));

    probe = fopen(path, "rb");
    if (probe == NULL) return FALSE;   /* plain map — not an error */
    fclose(probe);

    st = (ScenarioState *)calloc(1, sizeof(ScenarioState));
    if (st == NULL) return FALSE;
    st->sim = sim;
    st->game = game;
    st->hiddenMines = hiddenMines;
    st->tickRef = LUA_NOREF;
    st->gameRef = LUA_NOREF;
    SDL_strlcpy(st->path, path, sizeof(st->path));
    st->luaLock = SDL_CreateMutex();
    if (st->luaLock == NULL) {
        free(st);
        return FALSE;
    }

    if (!scBoot(st)) {
        SDL_DestroyMutex(st->luaLock);
        free(st);
        return FALSE;
    }
    sim->scenario = st;
    return TRUE;
}

void scenarioReset(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    if (st == NULL) return;
    if (!scBoot(st)) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "scenario reset failed — scenario disabled for this round");
        sim->scenario = NULL;
        SDL_DestroyMutex(st->luaLock);
        free(st);
    }
}

/* Defined below with scenarioLobbyChanged; used by scenarioSetup too. */
static void scSnapshotHumans(ServerSim *sim, bool out[MAX_TANKS]);
static void scAuditRoster(ScenarioState *st,
                          const bool humanBefore[MAX_TANKS],
                          const char *what);

bool scenarioSetup(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    bool humanBefore[MAX_TANKS];
    if (st == NULL) return FALSE;
    scLuaLock(st);
    if (st->disabled || st->L == NULL || st->setupDone) {
        scLuaUnlock(st);
        return FALSE;
    }
    st->setupDone = TRUE;
    if (!scPushHook(st->L, "on_setup")) {
        scLuaUnlock(st);
        return FALSE;
    }
    scSnapshotHumans(sim, humanBefore);
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    if (lua_pcall(st->L, 1, 0, 0) != 0) {
        scReportError(st, "on_setup");
    }
    scAuditRoster(st, humanBefore, "on_setup");
    scLuaUnlock(st);
    return TRUE;
}

void scenarioTick(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    if (st == NULL) return;
    scLuaLock(st);
    if (st->disabled || st->L == NULL) {
        scLuaUnlock(st);
        return;
    }

    /* First tick of a freshly booted VM = the round is genuinely
     * running (this is only ever called in serverStateRunning), so this
     * is where on_start belongs — the lobby wait is over and the reset
     * world + restored tanks exist. Re-armed by every scBoot, so a
     * lobby server's round 2+ gets its own on_start. Lobby-less sims
     * (BrainTest, -noLobby) never pass through serverSimStartGame, so
     * on_setup fires here too, right before on_start — there is no
     * pre-snapshot moment to hide it in on that path, and no remote
     * players to spam either. */
    if (!st->started) {
        scenarioSetup(sim);
        st->started = TRUE;
        if (scPushHook(st->L, "on_start")) {
            lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
            if (lua_pcall(st->L, 1, 0, 0) != 0) {
                scReportError(st, "on_start");
            }
        }
        if (st->disabled || st->L == NULL) {
            scLuaUnlock(st);
            return;
        }
    }

    if (st->tickRef == LUA_NOREF) {
        scLuaUnlock(st);
        return;
    }
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->tickRef);
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    lua_pushinteger(st->L, (lua_Integer)sim->tick);
    if (lua_pcall(st->L, 2, 0, 0) != 0) {
        scReportError(st, "on_tick");
    } else {
        st->errorCount = 0;
    }
    scLuaUnlock(st);
}

/* Snapshot which slots hold HUMANS, for the post-hook audit below. */
static void scSnapshotHumans(ServerSim *sim, bool out[MAX_TANKS]) {
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        out[i] = sim->playerConnected[i] && !serverSimIsBot(sim, i);
    }
}

/* The C-side check on the scenario's roster work. The API guards make
 * violations unreachable by construction (removal calls refuse humans;
 * adds are refused with no free slot, so the total can never exceed
 * MAX_TANKS), but the scenario has FULL roster authority, so the
 * engine audits after every hook that could have edited it:
 *   - every human present before the hook must still be present. A
 *     violation cannot be repaired (the connection is gone), so it
 *     disables the scenario on the spot and is logged loudly.
 *   - every connected slot must sit on a valid team (1..15); strays
 *     are clamped back to team 1.
 *   - the connected total is re-counted against MAX_TANKS (structural,
 *     but the audit states it rather than assuming it). */
static void scAuditRoster(ScenarioState *st,
                          const bool humanBefore[MAX_TANKS],
                          const char *what) {
    ServerSim *sim = st->sim;
    int connected = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (humanBefore[i] && !sim->playerConnected[i]) {
            WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "scenario %s removed HUMAN slot %u — scenario disabled",
                what, (unsigned)i);
            serverSimScenarioPublish(sim,
                "*** Scenario script disabled (it removed a human "
                "player) ***");
            st->disabled = TRUE;
        }
        if (sim->playerConnected[i]) {
            connected++;
            BYTE t = sim->lobbyPlayers[i].teamNumber;
            if (t == 0 || t >= MAX_TANKS) {
                WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                    "scenario %s left slot %u on invalid team %u — "
                    "clamped to team 1", what, (unsigned)i, (unsigned)t);
                serverSimSetTeam(sim, i, 1);
            }
        }
    }
    if (connected > MAX_TANKS) {
        /* Cannot happen (16 slots), stated for the audit's sake. */
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "scenario %s roster overflow: %d connected", what, connected);
    }
}

void scenarioLobbyChanged(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    bool humanBefore[MAX_TANKS];
    if (st == NULL) return;
    scLuaLock(st);
    if (st->disabled || st->L == NULL || st->inLobbyHook ||
        sim->state != serverStateLobby ||
        !scPushHook(st->L, "on_lobby")) {
        scLuaUnlock(st);
        return;
    }
    /* Guarded: the hook edits the roster with game.lobby_*, roster
     * edits publish, and publishes land back here. The audit runs
     * while the guard is still up so its own repairs don't recurse. */
    st->inLobbyHook = TRUE;
    scSnapshotHumans(sim, humanBefore);
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    if (lua_pcall(st->L, 1, 0, 0) != 0) {
        scReportError(st, "on_lobby");
    }
    scAuditRoster(st, humanBefore, "on_lobby");
    st->inLobbyHook = FALSE;
    scLuaUnlock(st);
}

bool scenarioChooseStart(ServerSim *sim, BYTE playerNum, BYTE *startIdx) {
    ScenarioState *st = scState(sim);
    bool ok = FALSE;
    if (st == NULL) return FALSE;
    scLuaLock(st);
    if (st->disabled || st->L == NULL || st->inChooseStart ||
        !scPushHook(st->L, "on_choose_start")) {
        scLuaUnlock(st);
        return FALSE;
    }

    /* Guarded: the hook runs mid-tank-placement, so a script that
     * spawns bots (or otherwise re-enters placement) from inside it
     * would recurse — those nested picks fall through to the engine. */
    st->inChooseStart = TRUE;
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    lua_pushinteger(st->L, playerNum);
    if (lua_pcall(st->L, 2, 1, 0) != 0) {
        scReportError(st, "on_choose_start");
    } else {
        if (lua_isnumber(st->L, -1)) {
            lua_Integer n = lua_tointeger(st->L, -1);
            GameSim *gs = serverSimGetGameSim(sim);
            if (n >= 1 && n <= (lua_Integer)startsGetNumStarts(&gs->ss)) {
                *startIdx = (BYTE)(n - 1);
                ok = TRUE;
            } else {
                WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                    "scenario: on_choose_start(%d) returned start %d "
                    "(map has 1..%d) — falling back to the engine pick",
                    (int)playerNum, (int)n,
                    (int)startsGetNumStarts(&gs->ss));
            }
        }
        lua_pop(st->L, 1);
    }
    st->inChooseStart = FALSE;
    scLuaUnlock(st);
    return ok;
}

void scenarioShutdown(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    if (st == NULL) return;
    scLuaLock(st);
    if (st->L != NULL) lua_close(st->L);
    st->L = NULL;
    scLuaUnlock(st);
    SDL_DestroyMutex(st->luaLock);
    free(st);
    sim->scenario = NULL;
}

bool scenarioIsActive(const ServerSim *sim) {
    return scState(sim) != NULL;
}

int scenarioGetMaxPlayers(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    return st ? st->maxPlayers : 0;
}

const char *scenarioGetDescription(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    return st ? st->description : "";
}

const char *scenarioGetDefaultBrain(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    return st ? st->defaultBrain : "";
}

/* Call an optional script-defined QUERY hook `fn(game)` returning one
 * value. The scenario's lobby-behavior surface is methods, not
 * constants — the engine asks the script each time it needs the
 * answer. Missing hook / error / wrong type -> the caller's default. */
static bool scQueryHook(const ServerSim *sim, const char *fn,
                        int *outInt, bool *outBool) {
    ScenarioState *st = scState(sim);
    bool ok = FALSE;
    if (st == NULL) return FALSE;
    scLuaLock(st);
    if (st->disabled || st->L == NULL || !scPushHook(st->L, fn)) {
        scLuaUnlock(st);
        return FALSE;
    }
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    if (lua_pcall(st->L, 1, 1, 0) != 0) {
        scReportError(st, fn);
        scLuaUnlock(st);
        return FALSE;
    }
    if (outInt != NULL && lua_isnumber(st->L, -1)) {
        *outInt = (int)lua_tointeger(st->L, -1);
        ok = TRUE;
    }
    if (outBool != NULL && lua_isboolean(st->L, -1)) {
        *outBool = lua_toboolean(st->L, -1) ? TRUE : FALSE;
        ok = TRUE;
    }
    lua_pop(st->L, 1);
    scLuaUnlock(st);
    return ok;
}

int scenarioGetEnemyBots(const ServerSim *sim) {
    int n = 0;
    if (scQueryHook(sim, "enemy_bots", &n, NULL)) {
        if (n < 0) n = 0;
        if (n > MAX_TANKS) n = MAX_TANKS;
        return n;
    }
    return 0;
}

bool scenarioGetAllowExtraTeams(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    bool b = FALSE;
    if (st == NULL) return TRUE;   /* plain maps keep their Add Team */
    if (scQueryHook(sim, "show_add_team_button", NULL, &b)) return b;
    return FALSE;                  /* scenarios default to two sides */
}

bool scenarioGetAllowBaseWin(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    bool b = TRUE;
    if (st == NULL) return TRUE;   /* plain maps keep the engine sweep */
    if (scQueryHook(sim, "allow_base_win", NULL, &b)) return b;
    return TRUE;                   /* scenarios keep it unless they opt out */
}

const char *scenarioGetWinMessage(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    return st ? st->winMessage : "";
}
