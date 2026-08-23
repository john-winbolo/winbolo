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
 *    scenario = { name = "...", max_players = 6 }   -- optional metadata
 *    function on_start(game) end                    -- once per round, first tick
 *    function on_tick(game, tick) end               -- every game tick
 *
 *  `game` API (n is 1-based for pills/bases to match engine numbering;
 *  players are 0-based slots to match everything on the wire):
 *    game.tick()                       -> current game tick
 *    game.max_tanks()                  -> MAX_TANKS
 *    game.map_tile(x, y)               -> terrain byte at (x, y)
 *    game.num_pills() / game.pill(n)   -> {x,y,owner,armour,speed,in_tank}
 *    game.num_bases() / game.base(n)   -> {x,y,owner,armour,shells,mines}
 *    game.tank(p)                      -> nil, or {mx,my,wx,wy,armour,
 *                                         shells,mines,trees,dir,boat,
 *                                         dead,name,bot}
 *    game.set_pill_owner(n, p|nil)     -- nil/-1 = neutral
 *    game.set_base_owner(n, p|nil)
 *    game.spawn_bot([name][, brain][, team]) -> playerNum | nil, err
 *    game.remove_bot(p)                -- free a bot slot (wave cleanup)
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
#include "../common/wb_log.h"
#include "scenario.h"

/* Give up calling into a script after this many consecutive errors —
 * a broken on_tick must not spam the log at 50 Hz forever. */
#define SCENARIO_MAX_ERRORS 20

typedef struct ScenarioState {
    lua_State *L;
    ServerSim *sim;
    gameType   game;
    bool       hiddenMines;
    int        maxPlayers;              /* scenario.max_players; 0 = none */
    int        errorCount;              /* consecutive hook failures */
    bool       disabled;
    int        tickRef;                 /* registry ref to on_tick (or NOREF) */
    int        gameRef;                 /* registry ref to the game table */
    BYTE       teamOf[MAX_TANKS];       /* game.set_team state */
    char       winMessage[256];         /* game.end_round text */
    char       name[64];                /* scenario.name (logs) */
    char       defaultBrain[256];       /* scenario.default_brain fallback */
    char       path[FILENAME_MAX];      /* sidecar path, for per-round reboots */
    bool       started;                 /* on_start fired for this round */
} ScenarioState;

static ScenarioState *scState(const ServerSim *sim) {
    return sim ? (ScenarioState *)sim->scenario : NULL;
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
        playersGetPlayerName(&gs->plyrs, (BYTE)p, name, FALSE);
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

/* ------------------------------------------------------------------ */
/* Spawning / teams                                                    */
/* ------------------------------------------------------------------ */

static int l_spawn_bot(lua_State *L) {
    ScenarioState *st = scUp(L);
    ServerSim *sim = st->sim;
    const char *name  = luaL_optstring(L, 1, NULL);
    const char *brain = luaL_optstring(L, 2, NULL);
    int hasTeam = !lua_isnoneornil(L, 3);
    int team = hasTeam ? (int)luaL_checkinteger(L, 3) : 0;
    char botName[32];
    BYTE slot;

    for (slot = 0; slot < MAX_TANKS; slot++) {
        if (!serverSimIsPlayerConnected(sim, slot)) break;
    }
    if (slot >= MAX_TANKS) {
        lua_pushnil(L);
        lua_pushstring(L, "no free player slot");
        return 2;
    }
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

    if (!serverSimCreateBot(sim, slot, brain, botName,
                            (aiType)serverSimGetBotAiType(sim),
                            st->game, st->hiddenMines)) {
        lua_pushnil(L);
        lua_pushstring(L, "serverSimCreateBot failed");
        return 2;
    }
    if (hasTeam) {
        st->teamOf[slot] = (BYTE)team;
        serverSimSetBotTeams(sim, st->teamOf, MAX_TANKS);
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
    if (p < 0 || p >= MAX_TANKS) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    serverSimRemoveBot(st->sim, (BYTE)p);
    /* Break the slot out of its team so a future occupant doesn't
     * inherit stale alliance intent from our side. (Engine alliances
     * for the departed player are cleaned by the remove itself.) */
    st->teamOf[p] = (BYTE)p;
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
    scRegister(L, st, "tank",           l_tank);
    scRegister(L, st, "set_pill_owner", l_set_pill_owner);
    scRegister(L, st, "set_base_owner", l_set_base_owner);
    scRegister(L, st, "spawn_bot",      l_spawn_bot);
    scRegister(L, st, "remove_bot",     l_remove_bot);
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
static bool scBoot(ScenarioState *st) {
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
    st->maxPlayers = 0;
    st->winMessage[0] = '\0';
    for (i = 0; i < MAX_TANKS; i++) st->teamOf[i] = i;

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
        lua_getfield(L, -1, "default_brain");
        if (lua_isstring(L, -1)) {
            SDL_strlcpy(st->defaultBrain, lua_tostring(L, -1),
                        sizeof(st->defaultBrain));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

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

    if (!scBoot(st)) {
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
        free(st);
    }
}

void scenarioTick(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    if (st == NULL || st->disabled || st->L == NULL) return;

    /* First tick of a freshly booted VM = the round is genuinely
     * running (this is only ever called in serverStateRunning), so this
     * is where on_start belongs — the lobby wait is over and the reset
     * world + restored tanks exist. Re-armed by every scBoot, so a
     * lobby server's round 2+ gets its own on_start. */
    if (!st->started) {
        st->started = TRUE;
        if (scPushHook(st->L, "on_start")) {
            lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
            if (lua_pcall(st->L, 1, 0, 0) != 0) {
                scReportError(st, "on_start");
            }
        }
        if (st->disabled || st->L == NULL) return;
    }

    if (st->tickRef == LUA_NOREF) return;
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->tickRef);
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->gameRef);
    lua_pushinteger(st->L, (lua_Integer)sim->tick);
    if (lua_pcall(st->L, 2, 0, 0) != 0) {
        scReportError(st, "on_tick");
    } else {
        st->errorCount = 0;
    }
}

void scenarioShutdown(ServerSim *sim) {
    ScenarioState *st = scState(sim);
    if (st == NULL) return;
    if (st->L != NULL) lua_close(st->L);
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

const char *scenarioGetWinMessage(const ServerSim *sim) {
    ScenarioState *st = scState(sim);
    return st ? st->winMessage : "";
}
