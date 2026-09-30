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
 * The questions a script needs to switch friendly fire off, answered from a
 * script, and the hit event beside them:
 *
 *   run_scenario_can_hit_tank        — a shell the script lets pass a tank
 *       leaves it untouched and unmoved, is asked about it once however long
 *       the two overlap, and hits the tank behind it
 *   run_scenario_can_hit_pill        — the same for a pillbox: no armour lost,
 *       no anger, and the tank behind it is hit
 *   run_scenario_pill_shell_names_pill — a shell a pillbox fires carries that
 *       pillbox's number into can_hit and damage_scale, beside the NEUTRAL
 *       attacker every pillbox shell has always had
 *   run_scenario_pill_damage_scale   — a shell priced at nothing leaves a
 *       pillbox's armour and still angers it, a blast priced at half takes
 *       half, and each is asked with its own cause
 *   run_scenario_blast_names_tank    — a dying tank's blast names that tank to
 *       can_die for a builder and a pillbox, and a death it is allowed still
 *       credits nobody
 *   run_scenario_mine_names_layer    — a mine names its layer to can_die for a
 *       builder, and a death it is allowed still credits nobody
 *   run_scenario_on_tank_hit         — a shell and a mine each raise the event
 *       with the armour the tank actually lost
 *
 * Every case drives the engine's own site: the shell list stepped the way the
 * tick steps it, the pillbox update, the explosion update and the mine
 * explosion. A script says what it was asked by printing a marked line, read
 * off the server console, as test_scenario_policy_lua.c does.
 *
 * Reads the ServerSim, tank, lgm, pill and shell structs directly; the
 * unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, playerConnected, events[] */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "input_packet.h"          /* EVENT_LGM_LOST, EVENT_PILL_KILLED */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"
#include "mines.h"
#include "minesexp.h"
#include "pillbox.h"
#include "shells.h"
#include "tank.h"
#include "tankexp.h"
#include "everard_map.h"
#include "scenario_host.h"
#include "test_harness.h"

#define SFF_SELF 0   /* the seat that fires, and whose builder is caught */
#define SFF_NEAR 1   /* the tank in the way */
#define SFF_FAR  2   /* the tank behind it */

/* How many squares of lane a case needs: the shooter, the tank or pillbox in
   the way three squares on, and the tank behind it three more. */
#define SFF_LANE_LEN 8

/* The squares along the lane each piece stands on. */
#define SFF_AT_SHOOTER 0
#define SFF_AT_NEAR    3
#define SFF_AT_FAR     6

/* A full-range tank shell, in map squares, which reaches past SFF_AT_FAR. */
#define SFF_SHELL_LEN 7

/* Enough game ticks for any shell to run its whole life and be gone. */
#define SFF_SHELL_TICKS 80

/* Enough explosion updates for the throttle to let one through. */
#define SFF_BLAST_UPDATES 32

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. */
static void sffScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* A script says what it was asked by printing a marked line. */
#define SFF_NOTE_MARK "note:"

static char   sffNote[16384];
static size_t sffNoteLen;

static void sffReset(void) {
    sffNote[0] = '\0';
    sffNoteLen = 0;
}

/* One console line, kept if the script wrote it, ended with a newline so the
   record reads one note to a line. */
static void sffNoteLine(const char *msg) {
    size_t mark = strlen(SFF_NOTE_MARK);
    size_t room;
    size_t n;

    if (msg == NULL || strncmp(msg, SFF_NOTE_MARK, mark) != 0) {
        return;
    }
    n = strlen(msg + mark);
    while (n > 0 && msg[mark + n - 1] == ' ') {
        n--;
    }
    room = sizeof(sffNote) - 1 - sffNoteLen;
    if (room == 0) {
        return;
    }
    if (n > room - 1) {
        n = room - 1;
    }
    memcpy(sffNote + sffNoteLen, msg + mark, n);
    sffNoteLen += n;
    sffNote[sffNoteLen++] = '\n';
    sffNote[sffNoteLen]   = '\0';
}

static void (*sffConsolePrev)(void *ctx, char *msg) = NULL;

static void sffConsoleCb(void *ctx, char *msg) {
    if (sffConsolePrev != NULL) {
        sffConsolePrev(ctx, msg);
    }
    sffNoteLine(msg);
}

/* How many times a whole line appears in the record. */
static int sffLines(const char *line) {
    const char *at = sffNote;
    size_t      n  = strlen(line);
    int         count = 0;

    while ((at = strstr(at, line)) != NULL) {
        if (at == sffNote || at[-1] == '\n') {
            count++;
        }
        at += n;
    }
    return count;
}

static bool sffPut(const char *mapPath, const char *body) {
    char  path[512];
    FILE *f;

    sffReset();
    sffScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fprintf(f,
            "local function note(s)\n"
            "  print(\"" SFF_NOTE_MARK "\" .. tostring(s))\n"
            "end\n"
            "%s", body);
    fclose(f);
    return true;
}

/* A round with the script attached and slot 0 seated; the case seats the
   rest. */
static ServerSim *sffSim(const char *mapPath, ScenarioHost **host) {
    BYTE       emap[6000] = E_MAP;
    char       err[512];
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    *host = NULL;
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    sffConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = sffConsoleCb;
    err[0] = '\0';
    *host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (*host == NULL) {
        fprintf(stderr, "the script was refused: %s\n", err);
        sim->sim.callbacks.consoleMessage = sffConsolePrev;
        sffConsolePrev = NULL;
        serverSimDestroy(sim);
        return NULL;
    }
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, SFF_SELF, "Shooter", false);
    return sim;
}

static void sffEnd(ServerSim *sim, ScenarioHost *host, const char *mapPath) {
    char path[512];

    sim->sim.callbacks.consoleMessage = sffConsolePrev;
    sffConsolePrev = NULL;
    scenarioHostDetach(host);
    serverSimDestroy(sim);
    sffScriptFor(mapPath, path, sizeof(path));
    remove(path);
    sffReset();
}

static WORLD sffMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A run of squares, SFF_LANE_LEN long and a row either side, with nothing on
   it and room around it — no pillbox, base or mine within two squares — laid
   to road so a shell crosses known ground. Returns false for a map with no
   such place. */
static bool sffLane(ServerSim *sim, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;

    for (y = 30; y < 220; y++) {
        for (x = 30; x < 200; x++) {
            bool clear = true;
            for (dy = -2; dy <= 2 && clear; dy++) {
                for (dx = -2; dx <= SFF_LANE_LEN + 2 && clear; dx++) {
                    BYTE sx = (BYTE)(x + dx);
                    BYTE sy = (BYTE)(y + dy);
                    if (pillsExistPos(&sim->sim.pb, sx, sy) ||
                        basesExistPos(&sim->sim.bs, sx, sy) ||
                        minesExistPos(&sim->sim.mns, &sim->sim.mp, sx, sy)) {
                        clear = false;
                    }
                }
            }
            if (!clear) {
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= SFF_LANE_LEN; dx++) {
                    mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(x + dx),
                              (BYTE)(y + dy), ROAD, FALSE, FALSE);
                }
            }
            *ox = (BYTE)x;
            *oy = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A tank at rest, ashore, on the middle of a square and facing east. */
static void sffPlace(ServerSim *sim, BYTE slot, BYTE mx, BYTE my) {
    tank *t = &sim->sim.tanks[slot];

    tankSetWorld(&sim->sim, t, sffMiddle(mx), sffMiddle(my),
                 (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

/* The first live pillbox standing on the map, by index, or -1. */
static int sffFirstPill(ServerSim *sim) {
    BYTE n = pillsGetNumPills(&sim->sim.pb);
    BYTE i;

    for (i = 0; i < n; i++) {
        if ((*sim->sim.pb).active[i] != FALSE &&
            (*sim->sim.pb).item[i].inTank == FALSE &&
            (*sim->sim.pb).item[i].armour > 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Stand pillbox idx on a square, alive at the armour given. */
static void sffMovePill(ServerSim *sim, int idx, BYTE mx, BYTE my,
                        BYTE armour) {
    (*sim->sim.pb).item[idx].x      = mx;
    (*sim->sim.pb).item[idx].y      = my;
    (*sim->sim.pb).item[idx].armour = armour;
}

/* The tanks and builders of the seats in play, compacted the way the tick
   compacts them. */
static BYTE sffArrays(ServerSim *sim, tank *tanks, lgm **lgms) {
    BYTE n = 0;
    BYTE c;

    for (c = 0; c < MAX_TANKS; c++) {
        if (sim->playerConnected[c] && sim->sim.tanks[c] != NULL) {
            tanks[n] = sim->sim.tanks[c];
            lgms[n]  = &sim->sim.lgmen[c];
            n++;
        }
    }
    return n;
}

/* Step every shell in flight, one game tick at a time, until none is left. */
static void sffFlyShells(ServerSim *sim) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;

    for (t = 0; t < SFF_SHELL_TICKS && sim->sim.shs != NULL; t++) {
        BYTE n = sffArrays(sim, tanks, lgms);
        shellsUpdate(&sim->sim, tanks, n, lgms, &sim->sim.ss);
    }
}

/* Run the explosion update until every fireball has gone off. */
static void sffBlast(ServerSim *sim) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;

    for (t = 0; t < SFF_BLAST_UPDATES && sim->sim.tankExplosions != NULL;
         t++) {
        BYTE n = sffArrays(sim, tanks, lgms);
        tkExplosionUpdate(&sim->sim, lgms, n, tanks, &sim->sim.ss);
    }
}

/* One tank shell from the shooter's square, east along the lane. */
static void sffFire(ServerSim *sim, BYTE lx, BYTE ly) {
    shellsAddItem(&sim->sim, &sim->sim.shs,
                  sffMiddle((BYTE)(lx + SFF_AT_SHOOTER)), sffMiddle(ly),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)SFF_SHELL_LEN, SFF_SELF,
                  NEUTRAL, DMG_NO_PILL, FALSE);
}

/* Stand the slot-0 builder on a square, out of the tank and alive. */
static void sffBuilderOut(ServerSim *sim, BYTE mx, BYTE my) {
    lgm *l = &sim->sim.lgmen[SFF_SELF];

    (*l)->inTank = FALSE;
    (*l)->isDead = FALSE;
    (*l)->x      = sffMiddle(mx);
    (*l)->y      = sffMiddle(my);
}

/* The first event of this type in the tick's buffer, or NULL. */
static const GameEvent *sffEvent(ServerSim *sim, uint8_t type) {
    int i;

    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == type) {
            return &sim->events[i];
        }
    }
    return NULL;
}

/* ================================================================
 * 1. A shell let past a tank.
 * ================================================================ */
int run_scenario_can_hit_tank(void) {
    static const char *const kMap = "scnff_hit_tank.map";
    static const char *const kBody =
        "scenario = { name = \"Friendly\", api = 1 }\n"
        "function can_hit(attacker, kind, n, pill)\n"
        "  note(\"hit \" .. attacker .. \" \" .. kind .. \" \" .. n .. \" \"\n"
        "       .. tostring(pill))\n"
        "  if kind == \"tank\" and n == 1 then return false end\n"
        "  return nil\n"
        "end\n"
        "function on_tank_hit(v, a, c, n, pill, s)\n"
        "  note(\"tank_hit \" .. v .. \" \" .. a)\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    BYTE          lx = 0, ly = 0;
    BYTE          nearBefore, farBefore;

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Friend", false);
    serverSimAddPlayer(sim, SFF_FAR, "Enemy", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL && gs->tanks[SFF_FAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");

    sffPlace(sim, SFF_SELF, (BYTE)(lx + SFF_AT_SHOOTER), ly);
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_AT_NEAR), ly);
    sffPlace(sim, SFF_FAR, (BYTE)(lx + SFF_AT_FAR), ly);
    nearBefore = tankGetArmour(&gs->tanks[SFF_NEAR]);
    farBefore  = tankGetArmour(&gs->tanks[SFF_FAR]);

    sffReset();
    sffFire(sim, lx, ly);
    sffFlyShells(sim);

    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[SFF_NEAR]) == nearBefore,
                  "the tank the script let the shell pass lost armour, %u "
                  "to %u", (unsigned)nearBefore,
                  (unsigned)tankGetArmour(&gs->tanks[SFF_NEAR]));
    UT_ASSERT_MSG(gs->tanks[SFF_NEAR]->bumpX == 0 &&
                  gs->tanks[SFF_NEAR]->bumpY == 0,
                  "the tank the shell passed was knocked back");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[SFF_FAR]) < farBefore,
                  "the shell went through the first tank and did not hit "
                  "the one behind it; armour is still %u",
                  (unsigned)farBefore);
    UT_ASSERT_MSG(sffLines("hit 0 tank 1 nil\n") == 1,
                  "the passed tank should be asked about once however many "
                  "ticks the shell sits in it; the record was:\n%s", sffNote);
    UT_ASSERT_MSG(sffLines("hit 0 tank 2 nil\n") == 1,
                  "the tank behind was not asked about once; the record "
                  "was:\n%s", sffNote);

    /* A tick drains the hit events. The one behind was hit; the one passed
       was not, so it raises nothing. */
    serverSimTick(sim);
    UT_ASSERT_MSG(sffLines("tank_hit 2 0\n") == 1,
                  "the tank behind raised no hit; the record was:\n%s",
                  sffNote);
    UT_ASSERT_MSG(strstr(sffNote, "tank_hit 1 0\n") == NULL,
                  "the tank the shell passed raised a hit; the record "
                  "was:\n%s", sffNote);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 2. A shell let past a pillbox.
 * ================================================================ */
int run_scenario_can_hit_pill(void) {
    static const char *const kMap = "scnff_hit_pill.map";
    static const char *const kBody =
        "scenario = { name = \"Friendly\", api = 1 }\n"
        "function can_hit(attacker, kind, n, pill)\n"
        "  note(\"hit \" .. attacker .. \" \" .. kind .. \" \" .. n .. \" \"\n"
        "       .. tostring(pill))\n"
        "  if kind == \"pill\" then return false end\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    BYTE          lx = 0, ly = 0;
    BYTE          farBefore, speedBefore, coolBefore;
    int           idx;
    char          want[64];

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_FAR, "Enemy", false);
    UT_ASSERT(gs->tanks[SFF_FAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    idx = sffFirstPill(sim);
    UT_ASSERT_MSG(idx >= 0, "setup: the map must carry a pillbox");

    sffPlace(sim, SFF_SELF, (BYTE)(lx + SFF_AT_SHOOTER), ly);
    sffMovePill(sim, idx, (BYTE)(lx + SFF_AT_NEAR), ly, 10);
    sffPlace(sim, SFF_FAR, (BYTE)(lx + SFF_AT_FAR), ly);
    farBefore   = tankGetArmour(&gs->tanks[SFF_FAR]);
    speedBefore = (*gs->pb).item[idx].speed;
    coolBefore  = (*gs->pb).item[idx].coolDown;

    sffReset();
    sffFire(sim, lx, ly);
    sffFlyShells(sim);

    UT_ASSERT_MSG((*gs->pb).item[idx].armour == 10,
                  "the pillbox the script let the shell pass lost armour, "
                  "now %u", (unsigned)(*gs->pb).item[idx].armour);
    UT_ASSERT_MSG((*gs->pb).item[idx].speed == speedBefore &&
                  (*gs->pb).item[idx].coolDown == coolBefore,
                  "the pillbox the shell passed was angered");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[SFF_FAR]) < farBefore,
                  "the shell went through the pillbox and did not hit the "
                  "tank behind it");
    snprintf(want, sizeof(want), "hit 0 pill %d nil\n", idx + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 3. A pillbox's shell names its pillbox.
 * ================================================================ */
int run_scenario_pill_shell_names_pill(void) {
    static const char *const kMap = "scnff_pill_shell.map";
    static const char *const kBody =
        "scenario = { name = \"Friendly\", api = 1 }\n"
        "function can_hit(attacker, kind, n, pill)\n"
        "  note(\"hit \" .. attacker .. \" \" .. kind .. \" \" .. n .. \" \"\n"
        "       .. tostring(pill))\n"
        "  return nil\n"
        "end\n"
        "function damage_scale(attacker, victim, cause, pill)\n"
        "  note(\"scale \" .. attacker .. \" \" .. victim .. \" \" .. cause\n"
        "       .. \" \" .. tostring(pill))\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    BYTE          lx = 0, ly = 0;
    BYTE          nearBefore;
    int           idx;
    char          want[64];

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Target", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    idx = sffFirstPill(sim);
    UT_ASSERT_MSG(idx >= 0, "setup: the map must carry a pillbox");

    /* The pillbox belongs to slot 0, so the one tank it may fire at is the
       target on the lane, and it is loaded and has seen the target. */
    sffMovePill(sim, idx, (BYTE)(lx + SFF_AT_SHOOTER), ly,
                (BYTE)gs->rules.pill_max_armour);
    (*gs->pb).item[idx].owner    = SFF_SELF;
    (*gs->pb).item[idx].reload   = (*gs->pb).item[idx].speed;
    (*gs->pb).item[idx].justSeen = TRUE;
    sffPlace(sim, SFF_SELF, (BYTE)(lx + SFF_AT_FAR), (BYTE)(ly + 1));
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_AT_NEAR), ly);
    nearBefore = tankGetArmour(&gs->tanks[SFF_NEAR]);

    pillsUpdate(gs, gs->tanks, sim->playerConnected, MAX_TANKS);
    UT_ASSERT_MSG(gs->shs != NULL, "setup: the pillbox did not fire");
    UT_ASSERT_MSG(gs->shs->owner == NEUTRAL,
                  "a pillbox's shell must stay owned by nobody, is %u",
                  (unsigned)gs->shs->owner);
    UT_ASSERT_MSG(gs->shs->pill == (BYTE)idx,
                  "the shell carries pill %u, expected %d",
                  (unsigned)gs->shs->pill, idx);

    sffReset();
    sffFlyShells(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[SFF_NEAR]) < nearBefore,
                  "setup: the pillbox's shell did not hit the target");

    snprintf(want, sizeof(want), "hit 255 tank 1 %d\n", idx + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);
    snprintf(want, sizeof(want), "scale 255 1 shell %d\n", idx + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 4. What a blow on a pillbox is worth.
 * ================================================================ */
int run_scenario_pill_damage_scale(void) {
    static const char *const kMap = "scnff_pill_scale.map";
    static const char *const kBody =
        "scenario = { name = \"Bunker\", api = 1 }\n"
        "function pill_damage_scale(attacker, n, cause, pill)\n"
        "  note(\"pscale \" .. attacker .. \" \" .. n .. \" \" .. cause\n"
        "       .. \" \" .. tostring(pill))\n"
        "  if cause == \"shell\" then return 0 end\n"
        "  return 50\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    BYTE          lx = 0, ly = 0, px, py;
    int           idx;
    int           other;
    int           blast;
    char          want[64];

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Doomed", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    idx = sffFirstPill(sim);
    UT_ASSERT_MSG(idx >= 0, "setup: the map must carry a pillbox");
    px = (BYTE)(lx + SFF_AT_NEAR);
    py = ly;
    sffMovePill(sim, idx, px, py, 10);
    (*gs->pb).item[idx].coolDown = 0;
    sffPlace(sim, SFF_SELF, (BYTE)(lx + SFF_AT_SHOOTER), ly);
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_AT_FAR), ly);

    /* A shell priced at nothing: the armour stays and the pillbox still turns
       angry, which is the cool-down it is put on. Fired by another pillbox,
       so the pill argument names that one. */
    other = (idx == 0) ? 1 : 0;
    sffReset();
    pillsDamagePos(gs, px, py, TRUE, TRUE, NEUTRAL, (BYTE)other);
    UT_ASSERT_MSG((*gs->pb).item[idx].armour == 10,
                  "a shell priced at nothing took armour, now %u",
                  (unsigned)(*gs->pb).item[idx].armour);
    UT_ASSERT_MSG((*gs->pb).item[idx].coolDown ==
                      (BYTE)gs->rules.pill_cooldown_ticks,
                  "a shell priced at nothing must still anger the pillbox");
    snprintf(want, sizeof(want), "pscale 255 %d shell %d\n", idx + 1,
             other + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    /* A dying tank's blast priced at half, gone off on the pillbox's square
       from the explosion the tank left. */
    blast = (gs->rules.tank_explosion_damage * 50 + 50) / 100;
    UT_ASSERT_MSG(blast > 0 && blast < gs->rules.tank_explosion_damage,
                  "setup: half a blast must be some of it and not all");
    sffReset();
    tkExplosionAddItem(gs, sffMiddle(px), sffMiddle(py), (TURNTYPE)0, 0,
                       TK_LARGE_EXPLOSION, SFF_NEAR);
    sffBlast(sim);
    UT_ASSERT_MSG((int)(*gs->pb).item[idx].armour == 10 - blast,
                  "a blast priced at half left armour %u, expected %d",
                  (unsigned)(*gs->pb).item[idx].armour, 10 - blast);
    snprintf(want, sizeof(want), "pscale 1 %d explosion nil\n", idx + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 5. A dying tank's blast names the tank.
 * ================================================================ */
int run_scenario_blast_names_tank(void) {
    static const char *const kMap = "scnff_blast.map";
    /* The first question about each kind is refused and every later one
       allowed, so one case sees both the question and the death. */
    static const char *const kBody =
        "scenario = { name = \"Blast\", api = 1 }\n"
        "local asked = {}\n"
        "function can_die(kind, n, killer, cause, pill)\n"
        "  note(\"die \" .. kind .. \" \" .. n .. \" \" .. killer .. \" \"\n"
        "       .. tostring(cause) .. \" \" .. tostring(pill))\n"
        "  asked[kind] = (asked[kind] or 0) + 1\n"
        "  return asked[kind] > 1\n"
        "end\n";
    ServerSim       *sim;
    ScenarioHost    *h;
    GameSim         *gs;
    lgm             *l;
    const GameEvent *ev;
    BYTE             lx = 0, ly = 0, bx, px;
    int              idx;
    char             want[64];

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Doomed", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    idx = sffFirstPill(sim);
    UT_ASSERT_MSG(idx >= 0, "setup: the map must carry a pillbox");
    l = &gs->lgmen[SFF_SELF];
    UT_ASSERT(*l != NULL);

    sffPlace(sim, SFF_SELF, lx, (BYTE)(ly - 1));
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_LANE_LEN - 1), (BYTE)(ly + 1));

    /* The builder, caught by the small blast a dying tank leaves. */
    bx = (BYTE)(lx + 2);
    sffBuilderOut(sim, bx, ly);
    sffReset();
    tkExplosionAddItem(gs, sffMiddle(bx), sffMiddle(ly), (TURNTYPE)0, 0,
                       TK_SMALL_EXPLOSION, SFF_NEAR);
    sffBlast(sim);
    UT_ASSERT_MSG((*l)->isDead == FALSE,
                  "a builder the script refused to let die was killed");
    UT_ASSERT_MSG(sffLines("die builder 0 1 explosion nil\n") == 1,
                  "the blast was not put as slot 1's; the record was:\n%s",
                  sffNote);

    /* Allowed this time. The question named slot 1; the loss credits
       nobody, as a blast always has. */
    sim->eventCount = 0;
    sffBuilderOut(sim, bx, ly);
    tkExplosionAddItem(gs, sffMiddle(bx), sffMiddle(ly), (TURNTYPE)0, 0,
                       TK_SMALL_EXPLOSION, SFF_NEAR);
    sffBlast(sim);
    UT_ASSERT_MSG((*l)->isDead == TRUE,
                  "the builder must die once the script allows it");
    ev = sffEvent(sim, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ev != NULL, "the builder's death raised no loss");
    UT_ASSERT_MSG(ev->data[1] == NEUTRAL,
                  "a blast's builder kill credited slot %u, expected nobody",
                  (unsigned)ev->data[1]);

    /* The pillbox, caught by the big blast. */
    px = (BYTE)(lx + 5);
    sffMovePill(sim, idx, px, ly, 1);
    sffReset();
    tkExplosionAddItem(gs, sffMiddle(px), sffMiddle(ly), (TURNTYPE)0, 0,
                       TK_LARGE_EXPLOSION, SFF_NEAR);
    sffBlast(sim);
    UT_ASSERT_MSG((*gs->pb).item[idx].armour == 1,
                  "a pillbox the script refused to let die was killed");
    snprintf(want, sizeof(want), "die pill %d 1 explosion nil\n", idx + 1);
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    sim->eventCount = 0;
    tkExplosionAddItem(gs, sffMiddle(px), sffMiddle(ly), (TURNTYPE)0, 0,
                       TK_LARGE_EXPLOSION, SFF_NEAR);
    sffBlast(sim);
    UT_ASSERT_MSG((*gs->pb).item[idx].armour == 0,
                  "the pillbox must die once the script allows it");
    ev = sffEvent(sim, EVENT_PILL_KILLED);
    UT_ASSERT_MSG(ev != NULL, "the pillbox's death raised no kill");
    UT_ASSERT_MSG(ev->data[1] == NEUTRAL,
                  "a blast's pillbox kill credited slot %u, expected nobody",
                  (unsigned)ev->data[1]);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 6. A mine names its layer.
 * ================================================================ */

/* A mine on the square, laid by slot `layer`, and set off. */
static void sffMineGoesOff(ServerSim *sim, BYTE mx, BYTE my, BYTE layer) {
    GameSim *gs = &sim->sim;
    tank     tanks[MAX_TANKS];
    lgm     *lgms[MAX_TANKS];
    BYTE     terrain = mapGetPos(&gs->mp, mx, my);
    BYTE     n;

    if (terrain < MINE_START || terrain > MINE_END) {
        mapSetPos(gs, &gs->mp, mx, my, (BYTE)(terrain + MINE_SUBTRACT), FALSE,
                  FALSE);
    }
    minesAddItem(&gs->mns, mx, my);
    minesSetOwner(&gs->mns, mx, my, layer);
    n = sffArrays(sim, tanks, lgms);
    minesExpCheckFill(gs, lgms, n, mx, my, tanks, &gs->ss);
}

int run_scenario_mine_names_layer(void) {
    static const char *const kMap = "scnff_mine.map";
    static const char *const kBody =
        "scenario = { name = \"Mines\", api = 1 }\n"
        "local asked = 0\n"
        "function can_die(kind, n, killer, cause, pill)\n"
        "  note(\"die \" .. kind .. \" \" .. n .. \" \" .. killer .. \" \"\n"
        "       .. tostring(cause) .. \" \" .. tostring(pill))\n"
        "  asked = asked + 1\n"
        "  return asked > 1\n"
        "end\n";
    ServerSim       *sim;
    ScenarioHost    *h;
    GameSim         *gs;
    lgm             *l;
    const GameEvent *ev;
    BYTE             lx = 0, ly = 0, mx;

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Layer", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    l = &gs->lgmen[SFF_SELF];
    UT_ASSERT(*l != NULL);

    sffPlace(sim, SFF_SELF, lx, (BYTE)(ly - 1));
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_LANE_LEN - 1), (BYTE)(ly + 1));
    mx = (BYTE)(lx + 3);
    sffBuilderOut(sim, mx, ly);

    sffReset();
    sffMineGoesOff(sim, mx, ly, SFF_NEAR);
    UT_ASSERT_MSG((*l)->isDead == FALSE,
                  "a builder the script refused to let die was killed");
    UT_ASSERT_MSG(sffLines("die builder 0 1 mine nil\n") == 1,
                  "the mine was not put as slot 1's; the record was:\n%s",
                  sffNote);

    /* Allowed this time, on the next square along, since the first mine
       left a crater. The question named the layer; the loss credits nobody,
       as a mine always has. */
    sim->eventCount = 0;
    mx = (BYTE)(mx + 1);
    sffBuilderOut(sim, mx, ly);
    sffMineGoesOff(sim, mx, ly, SFF_NEAR);
    UT_ASSERT_MSG((*l)->isDead == TRUE,
                  "the builder must die once the script allows it");
    ev = sffEvent(sim, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ev != NULL, "the builder's death raised no loss");
    UT_ASSERT_MSG(ev->data[1] == NEUTRAL,
                  "a mine's builder kill credited slot %u, expected nobody",
                  (unsigned)ev->data[1]);

    sffEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 7. The hit event carries what the armour lost.
 * ================================================================ */
int run_scenario_on_tank_hit(void) {
    static const char *const kMap = "scnff_tank_hit.map";
    static const char *const kBody =
        "scenario = { name = \"Hits\", api = 1 }\n"
        "function damage_scale(attacker, victim, cause, pill)\n"
        "  return 50\n"
        "end\n"
        "function on_tank_hit(v, a, c, n, pill, s)\n"
        "  note(\"tank_hit \" .. v .. \" \" .. a .. \" \" .. c .. \" \" .. n\n"
        "       .. \" \" .. tostring(pill) .. \" \" .. tostring(s))\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    tank         *t;
    BYTE          lx = 0, ly = 0;
    BYTE          before, afterShell, afterMine;
    WORLD         tx, ty;
    char          want[64];

    UT_ASSERT(sffPut(kMap, kBody));
    sim = sffSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    serverSimAddPlayer(sim, SFF_NEAR, "Target", false);
    UT_ASSERT(gs->tanks[SFF_NEAR] != NULL);
    UT_ASSERT_MSG(sffLane(sim, &lx, &ly), "setup: the map has no clear lane");
    sffPlace(sim, SFF_SELF, lx, ly);
    sffPlace(sim, SFF_NEAR, (BYTE)(lx + SFF_AT_FAR), ly);
    t = &gs->tanks[SFF_NEAR];
    serverSimTick(sim);

    /* A shell at half price. */
    sffReset();
    before = tankGetArmour(t);
    tankGetWorld(t, &tx, &ty);
    tankIsTankHit(gs, t, tx, ty, (TURNTYPE)0, SFF_SELF, DMG_NO_PILL);
    afterShell = tankGetArmour(t);
    UT_ASSERT_MSG(afterShell < before, "setup: the shell took no armour");

    /* A mine at half price. */
    tankMineDamage(gs, t, tankGetMX(t), tankGetMY(t), SFF_SELF);
    afterMine = tankGetArmour(t);
    UT_ASSERT_MSG(afterMine < afterShell, "setup: the mine took no armour");

    serverSimTick(sim);
    snprintf(want, sizeof(want), "tank_hit 1 0 shell %u nil false\n",
             (unsigned)(before - afterShell));
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);
    snprintf(want, sizeof(want), "tank_hit 1 0 mine %u nil false\n",
             (unsigned)(afterShell - afterMine));
    UT_ASSERT_MSG(sffLines(want) == 1,
                  "expected '%.*s' once; the record was:\n%s",
                  (int)strlen(want) - 1, want, sffNote);

    sffEnd(sim, h, kMap);
    return 0;
}
