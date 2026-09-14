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
 *
 * The policy rows answered from a script rather than from a vtable a case
 * wrote. test_scenario_policy_lifecycle.c and test_scenario_policy_combat.c
 * pin the engine sites against a C policy; these pin the host's own rows —
 * the lookup, the arguments a script is handed, and what a script that says
 * nothing gets:
 *
 *   run_scenario_policy_lua_allow_base_win — a script takes the base sweep
 *       out of the round's endings and puts it back
 *   run_scenario_policy_lua_can_respawn    — a script holds a tank dead for a
 *       hundred ticks and then lets it up
 *   run_scenario_policy_lua_can_build      — a script refuses one kind of
 *       order by its word, and names the pill a place-pill order would put
 *       down, counted as Lua counts them
 *   run_scenario_policy_lua_can_capture    — a script keeps a base neutral
 *       under the tank that drove onto it, and hands it over on the next pass
 *   run_scenario_policy_lua_announce       — a script holds one join off the
 *       newswire and lets another through, at the same site
 *   run_scenario_policy_lua_can_die        — a script keeps a tank alive under
 *       a hundred shells and then lets it die
 *   run_scenario_policy_lua_nil_is_classic — every one of the six defined and
 *       returning nil: each is called, and each decision is the classic one
 *   run_scenario_policy_lua_op_in_policy   — an op issued from inside a policy
 *       is refused SCN_OP_IN_POLICY, and the same op applies outside one
 *   run_scenario_policy_lua_error_counts   — a policy that raises answers
 *       classic and counts toward the limit, which switches the scenario off
 *
 * Every case drives the real call site: the win sweep, the death wait, a
 * build request, a tank driving onto a base, a player joining and a shell.
 *
 * A script says what it was asked by appending a line to this case's own
 * record file, which is also what tells a decision the script made from one
 * the host took without calling it. The names are per-case on purpose: CTest
 * runs cases as separate processes in one directory.
 *
 * Reads the ServerSim, tank and lgm structs directly; the unittests profile
 * permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, its tanks, bases and pills */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "control_event.h"         /* CTRL_PLAYER_JOIN and its quiet byte */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"
#include "mines.h"
#include "pillbox.h"
#include "tank.h"
#include "everard_map.h"
#include "scenario_host.h"
#include "test_harness.h"

#define PLA_SELF  0   /* the slot the fixture seats and every case drives */
#define PLA_OTHER 1   /* the slot a case that needs a second tank seats */

/* Armour comfortably above the capture threshold — a held base. */
#define PLA_ARMOUR_HELD 50

/* Trees comfortably above any single build cost, so a refused order is the
   only thing that can leave the tank's stores where they were. */
#define PLA_TREES 20

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A sidecar sits beside the map: X.map is accompanied by X.scenario.lua. The
   map file itself is never written — the host reads the sidecar beside a map
   path and nothing else, and the sims here are built from the built-in map. */
static void plaSidecarFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SIDECAR_SUFFIX);
}

/* The script, with the note() every case writes its record through in front
   of it. */
static bool plaPut(const char *mapPath, const char *record, const char *body) {
    char  side[512];
    FILE *f;

    remove(record);
    plaSidecarFor(mapPath, side, sizeof(side));
    f = fopen(side, "wb");
    if (f == NULL) {
        return false;
    }
    fprintf(f,
            "local function note(s)\n"
            "  local f = io.open(\"%s\", \"a\")\n"
            "  if f then f:write(s) f:close() end\n"
            "end\n"
            "%s", record, body);
    fclose(f);
    return true;
}

static void plaDrop(const char *mapPath, const char *record) {
    char side[512];
    plaSidecarFor(mapPath, side, sizeof(side));
    remove(side);
    remove(record);
}

/* The record so far, or "" when no policy has written one. */
static void plaRead(const char *record, char *out, size_t outLen) {
    FILE  *f = fopen(record, "rb");
    size_t n;

    out[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(out, 1, outLen - 1, f);
    fclose(f);
    out[n] = '\0';
}

/* A round with the scenario attached and one player in slot 0. The host is
   attached before the start, because the start is what boots the round's VM
   and applies the table. */
static ServerSim *plaSim(const char *mapPath, ScenarioHost **host) {
    BYTE       emap[6000] = E_MAP;
    char       err[512];
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    *host = NULL;
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    err[0] = '\0';
    *host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (*host == NULL) {
        fprintf(stderr, "the sidecar was refused: %s\n", err);
        serverSimDestroy(sim);
        return NULL;
    }
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, PLA_SELF, "Tester", false);
    return sim;
}

static void plaEnd(ServerSim *sim, ScenarioHost *host, const char *mapPath,
                   const char *record) {
    scenarioHostDetach(host);
    serverSimDestroy(sim);
    plaDrop(mapPath, record);
}

/* Every base to one owner, held — the board the sweep is a win on. */
static void plaSweepBases(ServerSim *sim, BYTE owner) {
    BYTE n = basesGetNumBases(&sim->sim.bs);
    BYTE i;

    for (i = 0; i < n; i++) {
        (*sim->sim.bs).item[i].owner  = owner;
        (*sim->sim.bs).item[i].armour = PLA_ARMOUR_HELD;
    }
}

/* ── Driving the sites ────────────────────────────────────────────────── */

/* The same approach test_scenario_policy_combat.c drives its capture case
   with: the capture blocks sit in the ashore arm of the movement step, so a
   tank put on the square by hand reaches neither. */

/* Find a map tile of the wanted terrain carrying no base, pill or mine and
   not under the slot-0 tank. */
static bool plaFindTile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[PLA_SELF]);
    BYTE ty = tankGetMY(&sim->sim.tanks[PLA_SELF]);
    int  x, y;

    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) {
                continue;
            }
            if (mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y) == want &&
                !basesExistPos(&sim->sim.bs, (BYTE)x, (BYTE)y) &&
                !pillsExistPos(&sim->sim.pb, (BYTE)x, (BYTE)y) &&
                !minesExistPos(&sim->sim.mns, &sim->sim.mp, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

static WORLD plaSquareMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

#define PLA_APPROACH_SQUARES 1
#define PLA_DRIVE_TICKS      600

static bool plaHasApproach(BYTE x, BYTE y) {
    return x > (BYTE)(PLA_APPROACH_SQUARES + 1) && x < 253 && y > 0 && y < 255;
}

/* Road through the target square, the square the drive starts from and one
   beyond, with a row either side. Terrain only: whatever stands on the square
   keeps its owner and its armour. */
static void plaPaveRun(ServerSim *sim, BYTE tx, BYTE ty) {
    int dx, dy;

    for (dy = -1; dy <= 1; dy++) {
        for (dx = -(PLA_APPROACH_SQUARES + 1); dx <= 2; dx++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(tx + dx),
                      (BYTE)(ty + dy), ROAD, FALSE, FALSE);
        }
    }
}

/* Drive the slot-0 tank onto (tx,ty) under its own movement, from a square
   west of it, ashore and facing east. Returns whether it arrived. */
static bool plaDriveOnto(ServerSim *sim, BYTE tx, BYTE ty) {
    tank *t = &sim->sim.tanks[PLA_SELF];
    int   ticks;

    plaPaveRun(sim, tx, ty);
    tankSetWorld(&sim->sim, t,
                 plaSquareMiddle((BYTE)(tx - PLA_APPROACH_SQUARES)),
                 plaSquareMiddle(ty), (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);

    for (ticks = 0; ticks < PLA_DRIVE_TICKS; ticks++) {
        if (tankGetMX(t) == tx && tankGetMY(t) == ty) {
            return true;
        }
        tankUpdate(&sim->sim, t, TACCEL, FALSE, FALSE);
    }
    return tankGetMX(t) == tx && tankGetMY(t) == ty;
}

/* The first live base with room to be driven onto, handed to nobody. */
static int plaNeutralBase(ServerSim *sim) {
    BYTE n = basesGetNumBases(&sim->sim.bs);
    BYTE i;

    for (i = 0; i < n; i++) {
        if ((*sim->sim.bs).active[i] != FALSE &&
            plaHasApproach((*sim->sim.bs).item[i].x,
                           (*sim->sim.bs).item[i].y)) {
            (*sim->sim.bs).item[i].owner = NEUTRAL;
            return (int)i;
        }
    }
    return -1;
}

/* The first live pill on the map, by index, or -1 for a map with none. */
static int plaFirstPill(ServerSim *sim) {
    BYTE n = pillsGetNumPills(&sim->sim.pb);
    BYTE i;

    for (i = 0; i < n; i++) {
        if ((*sim->sim.pb).active[i] != FALSE &&
            (*sim->sim.pb).item[i].inTank == FALSE) {
            return (int)i;
        }
    }
    return -1;
}

/* One shell from slot 0 landing on the slot-1 tank. */
static void plaShoot(ServerSim *sim) {
    WORLD tx, ty;
    tankGetWorld(&sim->sim.tanks[PLA_OTHER], &tx, &ty);
    tankIsTankHit(&sim->sim, &sim->sim.tanks[PLA_OTHER], tx, ty, (TURNTYPE)0,
                  PLA_SELF);
}

/* A build order asked as a question: no dispatch, no spending, no line. */
static bool plaAsk(ServerSim *sim, BYTE action, BYTE x, BYTE y) {
    return lgmRequestIsValid(&sim->sim, &sim->sim.lgmen[PLA_SELF],
                             &sim->sim.tanks[PLA_SELF], x, y, action);
}

/* Kill the slot-0 tank and stand it on the last tick of its death wait,
   which is where the respawn question is put. */
static void plaHoldAtDeathWait(ServerSim *sim) {
    tank *t = &sim->sim.tanks[PLA_SELF];

    tankSetArmour(t, 0);
    tankSetDestroyed(t, TRUE);
    tankSetDeathWait(t, 1);
}

/* ── What a client was told ───────────────────────────────────────────── */

/* A join reaches every client as a CTRL_PLAYER_JOIN carrying the quiet byte
   the announce policy answered with. Catching it at the bus is catching it
   where a client would. */
typedef struct {
    bool seen[MAX_TANKS];
    BYTE quiet[MAX_TANKS];
} PlaJoins;

/* Kept per slot rather than as the last one seen: a join publishes one event
   for the player who arrived, and a case reads the slot it asked about
   whatever else the server said in between. */
static void plaJoinCb(void *ctx, const ControlEvent *evt) {
    PlaJoins *c = (PlaJoins *)ctx;
    BYTE      slot;

    if (evt->type != CTRL_PLAYER_JOIN) {
        return;
    }
    slot = evt->u.playerJoin.playerNum;
    if (slot >= MAX_TANKS) {
        return;
    }
    c->seen[slot]  = true;
    c->quiet[slot] = evt->u.playerJoin.quiet;
}

/* Registration replays the server's current state to the new subscriber, so
   the record is cleared afterwards and counts only what happens next. */
static void plaWatchJoins(ServerSim *sim, PlaJoins *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, plaJoinCb, c);
    memset(c, 0, sizeof(*c));
}

/* A line a switched-off scenario sends the players. */
typedef struct {
    int  count;
    char last[256];
} PlaText;

static void plaTextCb(void *ctx, const ControlEvent *evt) {
    PlaText *c = (PlaText *)ctx;

    if (evt->type != CTRL_SERVER_TEXT) {
        return;
    }
    c->count++;
    snprintf(c->last, sizeof(c->last), "%s", evt->u.serverText.text);
}

static void plaWatchText(ServerSim *sim, PlaText *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, plaTextCb, c);
    memset(c, 0, sizeof(*c));
}

/* ================================================================
 * 1. A script takes the base sweep out of the round's endings.
 *
 * The answer is read off the round's own clock, so one sim shows both
 * directions at the site the sweep is decided at.
 * ================================================================ */
int run_scenario_policy_lua_allow_base_win(void) {
    static const char *const kMap    = "scnpol_lua_basewin.map";
    static const char *const kRecord = "scnpol_lua_basewin.record";
    static const char *const kBody =
        "scenario = { name = \"No Sweep\", api = 1 }\n"
        "function allow_base_win()\n"
        "  note(\"base_win\\n\")\n"
        "  return game.tick() >= 5\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[256];
    int           i;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);

    plaSweepBases(sim, PLA_SELF);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == FALSE,
                  "a swept board ended the round under a script that says "
                  "the sweep may not end it");
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "base_win") != NULL,
                  "the sweep was refused without the script being asked, so "
                  "something other than the policy answered");

    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "the same board must be a win once the script allows the "
                  "sweep");

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 2. A script holds a dead tank down.
 *
 * Asked on the last tick of the wait, so a no leaves the tank where it is and
 * the question is put again next tick.
 * ================================================================ */
int run_scenario_policy_lua_can_respawn(void) {
    static const char *const kMap    = "scnpol_lua_respawn.map";
    static const char *const kRecord = "scnpol_lua_respawn.record";
    static const char *const kBody =
        "scenario = { name = \"Tickets\", api = 1 }\n"
        "asks = 0\n"
        "function can_respawn(p)\n"
        "  asks = asks + 1\n"
        "  note(\"respawn \" .. p .. \"\\n\")\n"
        "  return asks > 100\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    char          rec[4096];
    int           i;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    UT_ASSERT(gs->tanks[PLA_SELF] != NULL);

    plaHoldAtDeathWait(sim);
    for (i = 0; i < 100; i++) {
        tankUpdate(gs, &gs->tanks[PLA_SELF], TNONE, FALSE, FALSE);
    }
    UT_ASSERT_MSG(tankGetDeathWait(&gs->tanks[PLA_SELF]) == 1,
                  "the wait must be held at one, is %d",
                  tankGetDeathWait(&gs->tanks[PLA_SELF]));
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[PLA_SELF]),
                  "the tank must still be dead after a hundred held ticks");

    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "respawn 0\n") != NULL,
                  "the question named a player the script did not write down: "
                  "%s", rec);

    /* The hundred-and-first ask is the one the script answers yes to. */
    tankUpdate(gs, &gs->tanks[PLA_SELF], TNONE, FALSE, FALSE);
    UT_ASSERT_MSG(tankGetDeathWait(&gs->tanks[PLA_SELF]) == 0,
                  "the wait must run out the tick the script changes its "
                  "answer, is %d", tankGetDeathWait(&gs->tanks[PLA_SELF]));
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PLA_SELF]),
                  "the tank must be back");

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 3. A script refuses one kind of build order.
 *
 * The order reaches the script as the word the player asked for, and a
 * place-pill order names the pill it would put down as Lua counts pills — or
 * nothing at all where the tank carries none.
 * ================================================================ */
int run_scenario_policy_lua_can_build(void) {
    static const char *const kMap    = "scnpol_lua_build.map";
    static const char *const kRecord = "scnpol_lua_build.record";
    static const char *const kBody =
        "scenario = { name = \"Protected\", api = 1 }\n"
        "function can_build(p, action, x, y, n)\n"
        "  note(\"build \" .. p .. \" \" .. action .. \" \" .. x .. \" \"\n"
        "       .. y .. \" \" .. tostring(n) .. \"\\n\")\n"
        "  return action ~= \"road\"\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          rec[4096];
    char          want[128];
    BYTE          gx = 0, gy = 0, fx = 0, fy = 0;
    int           pillIdx;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[PLA_SELF];
    UT_ASSERT_MSG(plaFindTile(sim, GRASS, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    tankSetTrees(&sim->sim, t, PLA_TREES);

    UT_ASSERT_MSG(!plaAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "a road on grass with wood must be refused by a script that "
                  "turns roads down");
    snprintf(want, sizeof(want), "build 0 road %u %u nil\n",
             (unsigned)gx, (unsigned)gy);
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, want) != NULL,
                  "the script was asked \"%s\", expected \"%s\"", rec, want);

    /* And an order of another kind goes ahead, so the refusal above is the
       word the script turned down rather than the site answering no to
       everything. */
    UT_ASSERT_MSG(plaFindTile(sim, FOREST, &fx, &fy),
                  "setup: the map must carry a forest tile");
    UT_ASSERT_MSG(plaAsk(sim, LGM_TREE_REQUEST, fx, fy),
                  "a harvest on forest must stay valid under a script that "
                  "only turns roads down");

    /* The pill a place-pill order would put down. The carry list counts from
       one, the policy surface from zero, and Lua from one again — so what the
       script sees is the number it would use anywhere else. A tank carrying
       none names no pill at all, which must stay recognisable rather than
       becoming an index. */
    remove(kRecord);
    plaAsk(sim, LGM_PILL_REQUEST, gx, gy);
    snprintf(want, sizeof(want), "build 0 pill %u %u nil\n",
             (unsigned)gx, (unsigned)gy);
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, want) != NULL,
                  "a tank carrying nothing was written down as \"%s\", "
                  "expected \"%s\"", rec, want);

    pillIdx = plaFirstPill(sim);
    UT_ASSERT_MSG(pillIdx >= 0, "setup: the map must carry a pillbox");
    tankPutPill(&sim->sim, t, (BYTE)(pillIdx + 1));
    remove(kRecord);
    plaAsk(sim, LGM_PILL_REQUEST, gx, gy);
    snprintf(want, sizeof(want), "build 0 pill %u %u %d\n",
             (unsigned)gx, (unsigned)gy, pillIdx + 1);
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, want) != NULL,
                  "the carried pill reached the script as \"%s\", expected "
                  "\"%s\"", rec, want);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 4. A script locks a base against the tank driving over it.
 *
 * Two rounds rather than one script changing its mind: the drive crosses a
 * paved run that may hold objectives of its own, so which ask is which is not
 * the test's to assume. Each round answers one way for every square.
 * ================================================================ */
int run_scenario_policy_lua_can_capture(void) {
    static const char *const kLockedMap  = "scnpol_lua_capture_no.map";
    static const char *const kOpenMap    = "scnpol_lua_capture_yes.map";
    static const char *const kRecord     = "scnpol_lua_capture.record";
    static const char *const kLockedBody =
        "scenario = { name = \"Locked\", api = 1 }\n"
        "function can_capture(kind, n, p)\n"
        "  note(\"capture \" .. kind .. \" \" .. n .. \" \" .. p .. \"\\n\")\n"
        "  return false\n"
        "end\n";
    static const char *const kOpenBody =
        "scenario = { name = \"Open\", api = 1 }\n"
        "function can_capture(kind, n, p)\n"
        "  note(\"capture \" .. kind .. \" \" .. n .. \" \" .. p .. \"\\n\")\n"
        "  return true\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    char          want[128];
    int           baseIdx;
    BYTE          bx, by;

    UT_ASSERT(plaPut(kLockedMap, kRecord, kLockedBody));
    sim = plaSim(kLockedMap, &h);
    UT_ASSERT(sim != NULL);

    baseIdx = plaNeutralBase(sim);
    UT_ASSERT_MSG(baseIdx >= 0,
                  "setup: the map must carry a base clear of its edges");
    bx = (*sim->sim.bs).item[baseIdx].x;
    by = (*sim->sim.bs).item[baseIdx].y;

    UT_ASSERT_MSG(plaDriveOnto(sim, bx, by),
                  "the tank never drove onto base square (%u,%u), so the "
                  "capture block was never reached",
                  (unsigned)bx, (unsigned)by);
    UT_ASSERT_MSG((*sim->sim.bs).item[baseIdx].owner == NEUTRAL,
                  "a locked base changed hands to %u",
                  (unsigned)(*sim->sim.bs).item[baseIdx].owner);
    UT_ASSERT_MSG(!tankIsDestroyed(&sim->sim.tanks[PLA_SELF]),
                  "a refused capture must not harm the tank");

    snprintf(want, sizeof(want), "capture base %d 0\n", baseIdx + 1);
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, want) != NULL,
                  "the script was asked \"%s\", expected \"%s\"", rec, want);

    plaEnd(sim, h, kLockedMap, kRecord);

    /* The same base, the same drive, a script that allows it. */
    UT_ASSERT(plaPut(kOpenMap, kRecord, kOpenBody));
    sim = plaSim(kOpenMap, &h);
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(plaNeutralBase(sim) == baseIdx,
                  "the second round picked a different base");
    UT_ASSERT_MSG(plaDriveOnto(sim, bx, by),
                  "the tank never drove onto base square (%u,%u) in the "
                  "second round", (unsigned)bx, (unsigned)by);
    UT_ASSERT_MSG((*sim->sim.bs).item[baseIdx].owner == PLA_SELF,
                  "the base must change hands under a script that allows it, "
                  "owner is %u",
                  (unsigned)(*sim->sim.bs).item[baseIdx].owner);

    plaEnd(sim, h, kOpenMap, kRecord);
    return 0;
}

/* ================================================================
 * 5. A script holds one join off the newswire.
 *
 * Both answers at one site, told apart by the subject the script is handed.
 * ================================================================ */
int run_scenario_policy_lua_announce(void) {
    static const char *const kMap    = "scnpol_lua_announce.map";
    static const char *const kRecord = "scnpol_lua_announce.record";
    static const char *const kBody =
        "scenario = { name = \"Quiet\", api = 1 }\n"
        "function announce(kind, subject, actor)\n"
        "  note(\"announce \" .. kind .. \" \" .. subject .. \" \"\n"
        "       .. actor .. \"\\n\")\n"
        "  return not (kind == \"joined\" and subject == 2)\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    PlaJoins      joins;
    char          rec[4096];

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);

    plaWatchJoins(sim, &joins);

    serverSimAddPlayer(sim, PLA_OTHER, "Loud", false);
    UT_ASSERT_MSG(joins.seen[PLA_OTHER],
                  "the join of slot %d never reached the bus", PLA_OTHER);
    UT_ASSERT_MSG(joins.quiet[PLA_OTHER] == 0,
                  "a join the script allows carried quiet %u, expected 0",
                  (unsigned)joins.quiet[PLA_OTHER]);

    serverSimAddPlayer(sim, 2, "Hushed", false);
    UT_ASSERT_MSG(joins.seen[2], "the join of slot 2 never reached the bus");
    UT_ASSERT_MSG(joins.quiet[2] == 1,
                  "a join the script holds back carried quiet %u, expected 1",
                  (unsigned)joins.quiet[2]);

    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "announce joined 2 2\n") != NULL,
                  "the script was asked \"%s\", expected a joined line for "
                  "slot 2", rec);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 6. A script keeps a tank alive under a hundred shells.
 *
 * A refused death leaves the tank at zero armour and alive; the cause reaches
 * the script as the word a death cause takes.
 * ================================================================ */
int run_scenario_policy_lua_can_die(void) {
    static const char *const kMap    = "scnpol_lua_die.map";
    static const char *const kRecord = "scnpol_lua_die.record";
    static const char *const kBody =
        "scenario = { name = \"Shellproof\", api = 1 }\n"
        "function can_die(kind, n, killer, cause)\n"
        "  note(\"die \" .. kind .. \" \" .. n .. \" \" .. killer .. \" \"\n"
        "       .. tostring(cause) .. \"\\n\")\n"
        "  return cause ~= \"shell\"\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    int           i;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, PLA_OTHER, "Target", false);
    UT_ASSERT(sim->sim.tanks[PLA_OTHER] != NULL);

    for (i = 0; i < 100; i++) {
        plaShoot(sim);
    }
    UT_ASSERT_MSG(!tankIsDestroyed(&sim->sim.tanks[PLA_OTHER]),
                  "a hundred shells destroyed a tank the script will not let "
                  "die");
    UT_ASSERT_MSG(tankGetArmour(&sim->sim.tanks[PLA_OTHER]) == 0,
                  "a refused death leaves the tank at zero armour, is %u",
                  (unsigned)tankGetArmour(&sim->sim.tanks[PLA_OTHER]));

    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "die tank 1 0 shell\n") != NULL,
                  "the script was asked \"%s\", expected a tank death by "
                  "shell for slot 1 credited to slot 0", rec);

    /* A blow the script does not turn down, at the same site: the cause is
       what it reads, so a mine kills the tank a shell could not. */
    remove(kRecord);
    tankMineDamage(&sim->sim, &sim->sim.tanks[PLA_OTHER],
                   tankGetMX(&sim->sim.tanks[PLA_OTHER]),
                   tankGetMY(&sim->sim.tanks[PLA_OTHER]), PLA_SELF);
    UT_ASSERT_MSG(tankIsDestroyed(&sim->sim.tanks[PLA_OTHER]),
                  "a mine must kill the tank the script only protects from "
                  "shells");
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "die tank 1 0 mine\n") != NULL,
                  "the mine was put to the script as \"%s\", expected a "
                  "cause of mine", rec);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 7. Every policy defined and answering nil is the classic rule.
 *
 * The record is what tells this apart from a script the host never called:
 * each of the six is asked, and each decision is the one a round with no
 * scenario takes.
 * ================================================================ */
int run_scenario_policy_lua_nil_is_classic(void) {
    static const char *const kMap    = "scnpol_lua_nil.map";
    static const char *const kRecord = "scnpol_lua_nil.record";
    static const char *const kBody =
        "scenario = { name = \"Silent\", api = 1 }\n"
        "function allow_base_win() note(\"base_win\\n\") return nil end\n"
        "function can_respawn(p) note(\"respawn\\n\") return nil end\n"
        "function can_build(p, a, x, y, n) note(\"build\\n\") return nil end\n"
        "function can_capture(k, n, p) note(\"capture\\n\") return nil end\n"
        "function announce(k, s, a) note(\"announce\\n\") return nil end\n"
        "function can_die(k, n, killer, c) note(\"die\\n\") return nil end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    PlaJoins      joins;
    char          rec[8192];
    BYTE          gx = 0, gy = 0;
    int           baseIdx;
    int           i;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);

    /* The sweep still ends the round. */
    plaSweepBases(sim, PLA_SELF);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "a swept board must still be a win when the script answers "
                  "nil");

    /* A road on grass is still a valid order. */
    UT_ASSERT_MSG(plaFindTile(sim, GRASS, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    tankSetTrees(&sim->sim, &sim->sim.tanks[PLA_SELF], PLA_TREES);
    UT_ASSERT_MSG(plaAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "a road on grass must stay valid when the script answers "
                  "nil");

    /* A join still reaches the newswire. */
    plaWatchJoins(sim, &joins);
    serverSimAddPlayer(sim, PLA_OTHER, "Target", false);
    UT_ASSERT_MSG(joins.seen[PLA_OTHER] && joins.quiet[PLA_OTHER] == 0,
                  "a join carried quiet %u when the script answers nil, "
                  "expected 0", (unsigned)joins.quiet[PLA_OTHER]);

    /* A shell still kills. */
    for (i = 0; i < 512 && !tankIsDestroyed(&sim->sim.tanks[PLA_OTHER]); i++) {
        plaShoot(sim);
    }
    UT_ASSERT_MSG(tankIsDestroyed(&sim->sim.tanks[PLA_OTHER]),
                  "shells must still kill when the script answers nil");

    /* A dead tank still comes back. */
    plaHoldAtDeathWait(sim);
    tankUpdate(&sim->sim, &sim->sim.tanks[PLA_SELF], TNONE, FALSE, FALSE);
    UT_ASSERT_MSG(!tankIsDestroyed(&sim->sim.tanks[PLA_SELF]),
                  "the tank must come back when the script answers nil");

    /* And a base still changes hands. */
    baseIdx = plaNeutralBase(sim);
    UT_ASSERT_MSG(baseIdx >= 0,
                  "setup: the map must carry a base clear of its edges");
    UT_ASSERT_MSG(plaDriveOnto(sim, (*sim->sim.bs).item[baseIdx].x,
                               (*sim->sim.bs).item[baseIdx].y),
                  "the tank never drove onto the base square");
    UT_ASSERT_MSG((*sim->sim.bs).item[baseIdx].owner == PLA_SELF,
                  "the base must change hands when the script answers nil, "
                  "owner is %u",
                  (unsigned)(*sim->sim.bs).item[baseIdx].owner);

    /* Every one of them was asked: nil is the script answering, not the host
       failing to find the function. */
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "base_win") != NULL, "allow_base_win was never "
                                                   "asked: %s", rec);
    UT_ASSERT_MSG(strstr(rec, "respawn") != NULL, "can_respawn was never "
                                                  "asked: %s", rec);
    UT_ASSERT_MSG(strstr(rec, "build") != NULL, "can_build was never asked: "
                                                "%s", rec);
    UT_ASSERT_MSG(strstr(rec, "capture") != NULL, "can_capture was never "
                                                  "asked: %s", rec);
    UT_ASSERT_MSG(strstr(rec, "announce") != NULL, "announce was never asked: "
                                                   "%s", rec);
    UT_ASSERT_MSG(strstr(rec, "die") != NULL, "can_die was never asked: %s",
                  rec);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 8. An op issued from inside a policy is refused.
 *
 * The funnel refuses every op while a policy call is on the stack, and what
 * the script reads back is the refusal's own name. The same op outside a
 * policy is what says the refusal was the policy and not the op.
 * ================================================================ */
int run_scenario_policy_lua_op_in_policy(void) {
    static const char *const kMap    = "scnpol_lua_inpolicy.map";
    static const char *const kRecord = "scnpol_lua_inpolicy.record";
    static const char *const kBody =
        "scenario = { name = \"Writes Back\", api = 1 }\n"
        "function can_respawn(p)\n"
        "  local ok, why = game.message(\"from inside a policy\")\n"
        "  note(\"policy \" .. tostring(ok) .. \" \" .. tostring(why)\n"
        "       .. \"\\n\")\n"
        "  return true\n"
        "end\n"
        "function on_tick(t)\n"
        "  local ok = game.message(\"from a tick\")\n"
        "  note(\"tick \" .. tostring(ok) .. \"\\n\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);

    plaHoldAtDeathWait(sim);
    tankUpdate(&sim->sim, &sim->sim.tanks[PLA_SELF], TNONE, FALSE, FALSE);

    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "policy nil SCN_OP_IN_POLICY\n") != NULL,
                  "an op issued from inside a policy answered \"%s\", "
                  "expected nil and SCN_OP_IN_POLICY", rec);

    /* The same op from a tick, where no policy is on the stack. */
    remove(kRecord);
    serverSimTick(sim);
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "tick true\n") != NULL,
                  "the same op outside a policy answered \"%s\", expected "
                  "true", rec);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}

/* ================================================================
 * 9. A policy that raises answers classic, and counts.
 *
 * The call that raised takes the classic answer, and SCN_ERROR_LIMIT raises
 * in a row switch the scenario off for the round with one line to the
 * players — the same rule a hook that keeps raising meets.
 * ================================================================ */
int run_scenario_policy_lua_error_counts(void) {
    static const char *const kMap    = "scnpol_lua_raises.map";
    static const char *const kRecord = "scnpol_lua_raises.record";
    static const char *const kBody =
        "scenario = { name = \"Boom\", api = 1 }\n"
        "function allow_base_win()\n"
        "  note(\"base_win\\n\")\n"
        "  error(\"boom\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    PlaText       text;
    char          rec[4096];
    int           i;

    UT_ASSERT(plaPut(kMap, kRecord, kBody));
    sim = plaSim(kMap, &h);
    UT_ASSERT(sim != NULL);

    plaSweepBases(sim, PLA_SELF);
    plaWatchText(sim, &text);

    /* The classic answer, from a call that raised. That call is error one. */
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "a policy that raised must leave the classic sweep, which "
                  "a swept board is a win under");
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(strstr(rec, "base_win") != NULL,
                  "the script was never asked, so nothing raised");

    /* Errors two to nineteen: one short of the limit says nothing to the
       players. */
    for (i = 2; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                      "error %d of %d answered anything but the classic "
                      "sweep", i, SCN_ERROR_LIMIT);
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 0,
                  "%d errors in a row sent %d lines to the game; the limit "
                  "is %d", SCN_ERROR_LIMIT - 1, text.count, SCN_ERROR_LIMIT);

    /* The one that reaches it switches the scenario off and says so once. */
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "the error at the limit answered anything but the classic "
                  "sweep");
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1,
                  "the %dth error sent %d lines to the game, expected one",
                  SCN_ERROR_LIMIT, text.count);
    UT_ASSERT_MSG(strstr(text.last, "Boom") != NULL,
                  "the line does not name the scenario: %s", text.last);

    /* A switched-off round runs none of the script and still answers
       classic. */
    remove(kRecord);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "the switched-off round must leave the classic sweep");
    plaRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(rec[0] == '\0',
                  "the switched-off round ran the script again: %s", rec);

    plaEnd(sim, h, kMap, kRecord);
    return 0;
}
