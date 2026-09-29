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
 * The can_ally policy and the game.allied read.
 *
 *   run_scenario_can_ally_request  — the request arm asks the policy with the
 *       requester and the seat asked; a no refuses the command and nobody is
 *       shown the request
 *   run_scenario_can_ally_accept   — the accept arm asks again with the
 *       requester first; a no leaves both seats unallied, whether the seat
 *       accepting is a person or a bot
 *   run_scenario_can_ally_script_not_asked — set_team and a scenario's own
 *       seating accept ally seats without asking
 *   run_scenario_can_ally_lua      — a script's can_ally is what the arms ask,
 *       handed the two seats, and nil is the ordinary yes
 *   run_scenario_allied_read       — game.allied for the same side, the
 *       other side, a seat and itself, an empty seat and a seat out of range,
 *       and after a leave splits two teammates
 *
 * The first three drive the command dispatch against a C policy, the way
 * test_scenario_policy_lifecycle.c drives the lobby's arms. The last two
 * attach a real script.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_ALLIANCE_*, CmdResult */
#include "control_event.h"         /* CTRL_ALLIANCE_REQUEST */
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers, sim->sim.plyrs */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* serverSimSetScenarioPolicy */
#include "game_sim.h"
#include "players.h"               /* playersIsAllie */
#include "everard_map.h"
#include "scenario_host.h"
#include "threads.h"
#include "test_harness.h"

/* ── The test policy ────────────────────────────────────────────────── */

/* What the policy answers and what it was last asked. */
typedef struct {
    bool answer;
    int  asks;
    BYTE player;
    BYTE other;
} CaCtx;

static bool caCanAlly(void *ctx, BYTE player, BYTE other) {
    CaCtx *c = (CaCtx *)ctx;
    c->asks++;
    c->player = player;
    c->other  = other;
    return c->answer;
}

static void caFillPolicy(ScenarioPolicy *pol, CaCtx *c, bool answer) {
    memset(pol, 0, sizeof(*pol));
    memset(c, 0, sizeof(*c));
    c->answer    = answer;
    pol->canAlly = caCanAlly;
    pol->ctx     = c;
}

/* ── Fixtures ───────────────────────────────────────────────────────── */

/* A running round with four people in it, none of them allied. */
static ServerSim *caRunningSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    BYTE i;

    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Zero", false);
    serverSimAddPlayer(sim, 1, "One", false);
    serverSimAddPlayer(sim, 2, "Two", false);
    serverSimAddPlayer(sim, 3, "Three", false);
    threadsWaitForMutex();
    for (i = 0; i < 4; i++) {
        serverSimSetTeamBatch(sim, i, (BYTE)(i + 1));
    }
    serverSimReapplyTeamAlliances(sim);
    threadsReleaseMutex();
    return sim;
}

static CmdResult caRequest(ServerSim *sim, BYTE from, BYTE to) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ALLIANCE_REQUEST;
    cmd.cmdSeq = 1;
    cmd.u.allianceRequest.toPlayer = to;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, from, &cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult caAccept(ServerSim *sim, BYTE accepter, BYTE newMember) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ALLIANCE_ACCEPT;
    cmd.cmdSeq = 1;
    cmd.u.allianceAccept.newMember = newMember;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, accepter, &cmd);
    threadsReleaseMutex();
    return r;
}

static bool caAllied(ServerSim *sim, BYTE a, BYTE b) {
    return playersIsAllie(&sim->sim.plyrs, a, b) == TRUE;
}

/* The alliance requests the server published. */
typedef struct {
    int  count;
    BYTE from;
    BYTE to;
} CaRequests;

static void caWatchRequests(void *ctx, const ControlEvent *evt) {
    CaRequests *r = (CaRequests *)ctx;
    if (evt->type == CTRL_ALLIANCE_REQUEST) {
        r->count++;
        r->from = evt->u.allianceRequest.fromPlayer;
        r->to   = evt->u.allianceRequest.toPlayer;
    }
}

/* ================================================================
 * 1. The request arm.
 *
 * A no refuses the command before the request goes out, so the seat asked
 * is never shown a request it could not accept. The requester is the first
 * seat the policy is handed.
 * ================================================================ */
int run_scenario_can_ally_request(void) {
    ServerSim *sim = caRunningSim();
    ScenarioPolicy pol;
    CaCtx c;
    CaRequests seen;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    memset(&seen, 0, sizeof(seen));
    h = serverSimRegisterSubscriber(sim, caWatchRequests, &seen);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(&seen, 0, sizeof(seen));

    caFillPolicy(&pol, &c, false);
    serverSimSetScenarioPolicy(sim, &pol);

    UT_ASSERT_MSG(caRequest(sim, 1, 2) == CMD_REJECT_BAD_STATE,
                  "a request the policy refuses must be rejected");
    UT_ASSERT_MSG(c.asks == 1, "the policy was asked %d times, expected 1",
                  c.asks);
    UT_ASSERT_MSG(c.player == 1 && c.other == 2,
                  "the policy was asked about %u and %u, expected the "
                  "requester 1 and the seat asked 2",
                  (unsigned)c.player, (unsigned)c.other);
    UT_ASSERT_MSG(seen.count == 0,
                  "a refused request still went out %d times", seen.count);

    c.answer = true;
    UT_ASSERT_MSG(caRequest(sim, 1, 2) == CMD_OK,
                  "a request the policy allows must go through");
    UT_ASSERT_MSG(seen.count == 1 && seen.from == 1 && seen.to == 2,
                  "the allowed request went out %d times, last %u to %u",
                  seen.count, (unsigned)seen.from, (unsigned)seen.to);

    /* With no policy the arm behaves as it always has. */
    serverSimSetScenarioPolicy(sim, NULL);
    UT_ASSERT(caRequest(sim, 3, 0) == CMD_OK);
    UT_ASSERT(seen.count == 2);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. The accept arm.
 *
 * Asked again, with the new member — the seat that asked — first. A no
 * leaves both seats unallied. A bot accepts through the same command a
 * person sends, so a bot seat accepting is refused the same way; the seat
 * is marked a bot here because the arm is where the answer is applied, and
 * nothing before it looks at who is sending.
 * ================================================================ */
int run_scenario_can_ally_accept(void) {
    ServerSim *sim = caRunningSim();
    ScenarioPolicy pol;
    CaCtx c;

    UT_ASSERT(sim != NULL);
    /* The server only takes an accept of a request the new member made, so
       both seats ask first, before there is a policy to refuse them. */
    UT_ASSERT(caRequest(sim, 1, 2) == CMD_OK);
    UT_ASSERT(caRequest(sim, 0, 3) == CMD_OK);
    caFillPolicy(&pol, &c, false);
    serverSimSetScenarioPolicy(sim, &pol);

    UT_ASSERT_MSG(caAccept(sim, 2, 1) == CMD_REJECT_BAD_STATE,
                  "an accept the policy refuses must be rejected");
    UT_ASSERT_MSG(c.asks == 1 && c.player == 1 && c.other == 2,
                  "the policy was asked %d times, last about %u and %u; "
                  "expected once, the new member 1 then the accepter 2",
                  c.asks, (unsigned)c.player, (unsigned)c.other);
    UT_ASSERT_MSG(!caAllied(sim, 1, 2) && !caAllied(sim, 2, 1),
                  "a refused accept still allied the two seats");

    /* A bot accepting. */
    sim->lobbyPlayers[3].isBot = TRUE;
    UT_ASSERT_MSG(caAccept(sim, 3, 0) == CMD_REJECT_BAD_STATE,
                  "a bot's accept the policy refuses must be rejected");
    UT_ASSERT_MSG(c.asks == 2 && c.player == 0 && c.other == 3,
                  "the bot's accept was put to the policy as %u and %u",
                  (unsigned)c.player, (unsigned)c.other);
    UT_ASSERT_MSG(!caAllied(sim, 0, 3), "a refused bot accept allied");

    c.answer = true;
    UT_ASSERT(caAccept(sim, 2, 1) == CMD_OK);
    UT_ASSERT_MSG(caAllied(sim, 1, 2) && caAllied(sim, 2, 1),
                  "an accept the policy allows must ally the two seats");
    UT_ASSERT(caAccept(sim, 3, 0) == CMD_OK);
    UT_ASSERT_MSG(caAllied(sim, 0, 3), "an allowed bot accept must ally");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. The script's own moves are not asked.
 *
 * set_team rebuilds the alliances from the teams, and a scenario seating a
 * bot allies it quietly. Both are the script deciding, so neither is put to
 * a policy that would refuse every alliance.
 * ================================================================ */
int run_scenario_can_ally_script_not_asked(void) {
    ServerSim *sim = caRunningSim();
    ScenarioPolicy pol;
    CaCtx c;

    UT_ASSERT(sim != NULL);
    caFillPolicy(&pol, &c, false);
    serverSimSetScenarioPolicy(sim, &pol);

    threadsWaitForMutex();
    serverSimSetTeam(sim, 1, 1);
    threadsReleaseMutex();
    UT_ASSERT_MSG(caAllied(sim, 0, 1),
                  "set_team onto seat 0's team must ally the two seats");

    threadsWaitForMutex();
    serverSimAcceptAllianceQuiet(sim, 2, 3);
    threadsReleaseMutex();
    UT_ASSERT_MSG(caAllied(sim, 2, 3), "the quiet accept must ally");

    UT_ASSERT_MSG(c.asks == 0,
                  "the policy was asked %d times about the script's own "
                  "moves, expected never", c.asks);

    serverSimDestroy(sim);
    return 0;
}

/* ── Script fixtures ────────────────────────────────────────────────── */

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. The
   map file itself is never written; the sims are built from the built-in
   map. Each case names its own map, because ctest runs the cases as
   concurrent processes in one working directory. */
static void caScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* A script says what it saw by printing a marked line. A scenario state has
   no io, and its print goes to the server console, which the watcher below
   catches. The mark tells a script's line from the host's own. */
#define CA_NOTE_MARK "note:"

static char   caNote[4096];
static size_t caNoteLen;

static void caNoteReset(void) {
    caNote[0] = '\0';
    caNoteLen = 0;
}

/* One console line, kept if the script wrote it, and ended with a newline.
   print turns a trailing newline into a space, so trailing spaces are
   dropped first. */
static void caNoteLine(const char *msg) {
    size_t mark = strlen(CA_NOTE_MARK);
    size_t room;
    size_t n;

    if (msg == NULL || strncmp(msg, CA_NOTE_MARK, mark) != 0) {
        return;
    }
    n = strlen(msg + mark);
    while (n > 0 && msg[mark + n - 1] == ' ') {
        n--;
    }
    room = sizeof(caNote) - 1 - caNoteLen;
    if (room == 0) {
        return;
    }
    if (n > room - 1) {
        n = room - 1;
    }
    memcpy(caNote + caNoteLen, msg + mark, n);
    caNoteLen += n;
    caNote[caNoteLen++] = '\n';
    caNote[caNoteLen]   = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void (*caConsolePrev)(void *ctx, char *msg) = NULL;

static void caConsoleCb(void *ctx, char *msg) {
    if (caConsolePrev != NULL) {
        caConsolePrev(ctx, msg);
    }
    caNoteLine(msg);
}

/* The script, with the note() it writes its record through in front of
   it. */
static bool caPut(const char *mapPath, const char *body) {
    char  path[512];
    FILE *f;

    caNoteReset();
    caScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fprintf(f,
            "local function note(s)\n"
            "  print(\"" CA_NOTE_MARK "\" .. tostring(s))\n"
            "end\n"
            "%s", body);
    fclose(f);
    return true;
}

/* A running round with the script attached and seats 0 to 3 on teams 1, 1,
   2 and 2. The host is attached before the start, which boots the round's
   VM, and the console is watched before the attach. */
static ServerSim *caScriptSim(const char *mapPath, ScenarioHost **host) {
    BYTE       emap[6000] = E_MAP;
    char       err[512];
    const BYTE teams[4] = { 1, 1, 2, 2 };
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    BYTE       i;

    *host = NULL;
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    caConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = caConsoleCb;
    err[0] = '\0';
    *host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (*host == NULL) {
        fprintf(stderr, "the script was refused: %s\n", err);
        sim->sim.callbacks.consoleMessage = caConsolePrev;
        caConsolePrev = NULL;
        serverSimDestroy(sim);
        return NULL;
    }
    serverSimStartGame(sim);
    for (i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Seat%u", (unsigned)i);
        serverSimAddPlayer(sim, i, name, false);
    }
    threadsWaitForMutex();
    for (i = 0; i < 4; i++) {
        serverSimSetTeamBatch(sim, i, teams[i]);
    }
    serverSimReapplyTeamAlliances(sim);
    threadsReleaseMutex();
    return sim;
}

static void caScriptEnd(ServerSim *sim, ScenarioHost *host,
                        const char *mapPath) {
    char path[512];

    sim->sim.callbacks.consoleMessage = caConsolePrev;
    caConsolePrev = NULL;
    scenarioHostDetach(host);
    serverSimDestroy(sim);
    caScriptFor(mapPath, path, sizeof(path));
    remove(path);
    caNoteReset();
}

/* ================================================================
 * 4. A script's can_ally.
 *
 * The host asks the script with the two seats as Lua sees them, which are
 * the same numbers: seats are not converted. The script refuses any
 * alliance that crosses teams and answers nil, the ordinary yes, for the
 * rest, and notes what it was asked.
 * ================================================================ */
int run_scenario_can_ally_lua(void) {
    static const char *const kMap = "scn_can_ally_lua.map";
    static const char *const kBody =
        "scenario = { name = \"Two Sides\", api = 1 }\n"
        "function can_ally(p, q)\n"
        "  note(p .. \">\" .. q)\n"
        "  local a, b = game.lobby_slot(p), game.lobby_slot(q)\n"
        "  if a and b and a.team ~= b.team then return false end\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;

    UT_ASSERT(caPut(kMap, kBody));
    sim = caScriptSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    caNoteReset();

    /* Across the teams: refused at the request and at the accept. */
    UT_ASSERT_MSG(caRequest(sim, 0, 2) == CMD_REJECT_BAD_STATE,
                  "a request across teams must be refused by the script");
    /* The server only takes an accept of a request it recorded. Record one
       from 0 to 2, as if it had been made before the script said no, so the
       accept reaches the script. */
    sim->allianceAskedBy[2] |= (uint16_t)(1u << 0);
    UT_ASSERT_MSG(caAccept(sim, 2, 0) == CMD_REJECT_BAD_STATE,
                  "an accept across teams must be refused by the script");
    UT_ASSERT(!caAllied(sim, 0, 2));

    /* A leave splits seat 1 from seat 0; they share a team, so the script
       answers nil and seat 0 may take seat 1 back. */
    threadsWaitForMutex();
    serverSimLeaveAlliance(sim, 1);
    threadsReleaseMutex();
    UT_ASSERT(!caAllied(sim, 0, 1));
    UT_ASSERT_MSG(caRequest(sim, 1, 0) == CMD_OK,
                  "a nil answer must let the request through");
    UT_ASSERT_MSG(caAccept(sim, 0, 1) == CMD_OK,
                  "a nil answer must let the accept through");
    UT_ASSERT(caAllied(sim, 0, 1));

    UT_ASSERT_MSG(strcmp(caNote, "0>2\n0>2\n1>0\n1>0\n") == 0,
                  "the script was asked \"%s\", expected the requester "
                  "first at both the request and the accept", caNote);

    caScriptEnd(sim, h, kMap);
    return 0;
}

/* ================================================================
 * 5. game.allied.
 *
 * The sim's alliance table as the game rules read it. The script reads a
 * row of pairs each tick once every seat is on its team, and notes the row
 * when it differs from the last one. A leave takes seat 1 off seat 0's side
 * while their lobby team stays the same, which is where the read and
 * lobby_slot differ.
 *
 * The row is: 0 and 1 (the same side), 1 and 0, 0 and 2 (the other side),
 * 3 and itself, 0 and an empty seat, an empty seat and itself, a seat past
 * the roster, a negative seat, then seat 0's and seat 1's lobby teams.
 * ================================================================ */
int run_scenario_allied_read(void) {
    static const char *const kMap = "scn_allied_read.map";
    static const char *const kBody =
        "scenario = { name = \"Allied\", api = 1 }\n"
        "local last = nil\n"
        "local function a(x, y) return tostring(game.allied(x, y)) end\n"
        "function on_tick(tick)\n"
        "  local s3 = game.lobby_slot(3)\n"
        "  if s3 == nil or s3.team ~= 2 then return end\n"
        "  local row = a(0, 1) .. \",\" .. a(1, 0) .. \",\" .. a(0, 2)\n"
        "    .. \",\" .. a(3, 3) .. \",\" .. a(0, 9) .. \",\" .. a(9, 9)\n"
        "    .. \",\" .. a(0, 16) .. \",\" .. a(-1, 0)\n"
        "    .. \",\" .. game.lobby_slot(0).team\n"
        "    .. \",\" .. game.lobby_slot(1).team\n"
        "  if row ~= last then last = row note(row) end\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;

    UT_ASSERT(caPut(kMap, kBody));
    sim = caScriptSim(kMap, &h);
    UT_ASSERT(sim != NULL);
    caNoteReset();

    serverSimTick(sim);
    UT_ASSERT_MSG(strcmp(caNote,
                         "true,true,false,true,nil,nil,nil,nil,1,1\n") == 0,
                  "game.allied read \"%s\" with seats 0 and 1 on team 1 and "
                  "seats 2 and 3 on team 2", caNote);

    caNoteReset();
    threadsWaitForMutex();
    serverSimLeaveAlliance(sim, 1);
    threadsReleaseMutex();
    serverSimTick(sim);
    UT_ASSERT_MSG(strcmp(caNote,
                         "false,false,false,true,nil,nil,nil,nil,1,1\n") == 0,
                  "after seat 1 left the alliance game.allied read \"%s\"; "
                  "expected 0 and 1 not allied on the same lobby team",
                  caNote);

    caScriptEnd(sim, h, kMap);
    return 0;
}
