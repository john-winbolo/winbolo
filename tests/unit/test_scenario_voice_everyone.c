/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Voice to everyone, the scenario op behind game.set_voice_everyone, and
 * serverSimVoiceSidesAllow, the rule the server's voice forward asks for
 * every talker and recipient:
 *
 *   run_scenario_voice_everyone_default  — with no script, a running round
 *       sends voice to allies only and the lobby sends it to everyone
 *   run_scenario_voice_everyone_on       — the op turns it on, two seats on
 *       different teams hear each other, and off puts it back
 *   run_scenario_voice_everyone_resets   — the return to the lobby, a new
 *       round's start and a scenario detach each turn it off
 *   run_scenario_voice_everyone_voice_off — a server with voice off refuses
 *       the op's on, takes its off, and the getter answers false
 *
 * And how the players are told (CTRL_VOICE_EVERYONE):
 *
 *   run_scenario_voice_everyone_publish  — one event per change of value, from
 *       the op and from each of the three clears, and none for a repeat
 *   run_scenario_voice_everyone_join     — a client joining while it is on is
 *       given it in its sync, holds it and is told; while off, nothing is sent
 *   run_scenario_voice_everyone_client   — the client prints its line only
 *       when the rule a running round plays by changes: not in the lobby, as
 *       the round starts when the flag is already on, and silently on the way
 *       out of the round
 *   run_scenario_voice_everyone_mic_bits — a snapshot shows a non-ally's
 *       microphone state while voice goes to everyone, and hides it otherwise
 *
 * The round is two seats on team 1 and one on team 2, so 0 and 1 are allies
 * and 2 is nobody's.
 */

#include <stdbool.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers[].teamNumber */
#include "server_sim_scenario.h"   /* serverSimApplyScenarioOp */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "scenario_defs.h"         /* ScenarioOp, SCN_OP_SET_VOICE_EVERYONE */
#include "control_event.h"         /* CTRL_VOICE_EVERYONE */
#include "client_sim.h"
#include "client_sim_internal.h"   /* cs->messages */
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "messages.h"              /* MessageState's queue */
#include "players.h"               /* playersSetClientFlags */
#include "player_flags.h"          /* PLAYER_FLAG_HAS_MIC */
#include "input_packet.h"          /* SnapshotHeader, TANK_SNAPSHOT_PLAYER_MASK */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* A lobby with three seats, 0 and 1 on team 1 and 2 on team 2. */
static ServerSim *veLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Zero", false);
    serverSimAddPlayer(sim, 1, "One", false);
    serverSimAddPlayer(sim, 2, "Two", false);
    sim->lobbyPlayers[0].teamNumber = 1;
    sim->lobbyPlayers[1].teamNumber = 1;
    sim->lobbyPlayers[2].teamNumber = 2;
    return sim;
}

/* The same three, in a running round. */
static ServerSim *veRunningSim(void) {
    ServerSim *sim = veLobbySim();
    if (sim == NULL) return NULL;
    serverSimStartGame(sim);
    return sim;
}

static ScnOpResult veSet(ServerSim *sim, bool on) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_SET_VOICE_EVERYONE;
    op.u.setVoiceEveryone.on = on;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* The round's sides as the fixture means them, checked before a case leans
 * on them. */
static int veSidesAreSet(ServerSim *sim) {
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is not running (state %d)",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(serverSimIsAllied(sim, 0, 1),
                  "seats 0 and 1 share team 1 but are not allies");
    UT_ASSERT_MSG(!serverSimIsAllied(sim, 0, 2),
                  "seats 0 and 2 are on different teams but are allies");
    return 0;
}

int run_scenario_voice_everyone_default(void) {
    ServerSim *sim = veLobbySim();
    UT_ASSERT(sim != NULL);

    /* The lobby: everyone hears everyone, as it always has. */
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 2),
                  "the lobby kept seat 2's voice from seat 0");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 2, 0),
                  "the lobby kept seat 0's voice from seat 2");

    /* The round: allies only. */
    serverSimStartGame(sim);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 1),
                  "allies 0 and 1 did not hear each other");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 1, 0),
                  "allies 1 and 0 did not hear each other");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "seat 0 heard seat 2, who is not an ally");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 2, 1),
                  "seat 2 heard seat 1, who is not an ally");

    /* Out of range and no sim are no. */
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, MAX_TANKS, 0));
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, 0, MAX_TANKS));
    UT_ASSERT(!serverSimVoiceSidesAllow(NULL, 0, 1));
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(NULL));

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_on(void) {
    ServerSim  *sim = veRunningSim();
    ScnOpResult r;

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    r = veSet(sim, true);
    UT_ASSERT_MSG(r == SCN_OP_OK, "on answered %d", (int)r);
    UT_ASSERT(serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 2),
                  "with voice to everyone on, seat 0 did not hear seat 2");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 2, 1),
                  "with voice to everyone on, seat 2 did not hear seat 1");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 1),
                  "with voice to everyone on, allies stopped hearing each "
                  "other");
    /* The sides themselves are untouched: this is voice, not alliance. */
    UT_ASSERT(!serverSimIsAllied(sim, 0, 2));

    r = veSet(sim, false);
    UT_ASSERT_MSG(r == SCN_OP_OK, "off answered %d", (int)r);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "after off, seat 0 still heard seat 2");
    UT_ASSERT(serverSimVoiceSidesAllow(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_resets(void) {
    ServerSim *sim = veRunningSim();

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    /* The round ends and the server goes back to the lobby. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimReturnToLobby(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the return to the lobby");

    /* Set in the lobby, then a new round starts. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone set in the lobby reached the round");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "the new round sent seat 2's voice to seat 0");

    /* Set in the round, then the scenario is taken off the server. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the scenario's detach");

    /* Set in the round, then a new round starts straight from it. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the start of the next round");

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_voice_off(void) {
    ServerSim  *sim = veRunningSim();
    ScnOpResult r;

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    serverSimSetVoiceMode(sim, serverVoiceOff);
    r = veSet(sim, true);
    UT_ASSERT_MSG(r == SCN_OP_WRONG_STATE,
                  "on with server voice off answered %d, expected %d",
                  (int)r, (int)SCN_OP_WRONG_STATE);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, 0, 2));

    r = veSet(sim, false);
    UT_ASSERT_MSG(r == SCN_OP_OK, "off with server voice off answered %d",
                  (int)r);

    /* Turned on while voice was on, and then the server's voice goes off:
       the getter answers false, because nobody hears anybody. */
    serverSimSetVoiceMode(sim, serverVoiceOn);
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    UT_ASSERT(serverSimGetScenarioVoiceEveryone(sim));
    serverSimSetVoiceMode(sim, serverVoiceOff);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "the getter said voice goes to everyone on a server with "
                  "voice off");

    serverSimDestroy(sim);
    return 0;
}

/* ── How the players are told ───────────────────────────────────────── */

/* What a bus subscriber has been sent of CTRL_VOICE_EVERYONE. */
typedef struct {
    int  count;
    bool last;
} VeSeen;

static void veCount(void *ctx, const ControlEvent *evt) {
    VeSeen *seen = (VeSeen *)ctx;
    if (evt->type == CTRL_VOICE_EVERYONE) {
        seen->count++;
        seen->last = evt->u.voiceEveryone.on;
    }
}

/* Expect exactly `n` events so far, the last of them saying `value`. */
#define VE_EXPECT(seen, n, value, what)                                      \
    UT_ASSERT_MSG((seen).count == (n) && (seen).last == (value),              \
                  "%s: %d event(s), last %s; expected %d, last %s", (what),   \
                  (seen).count, (seen).last ? "on" : "off", (n),              \
                  (value) ? "on" : "off")

int run_scenario_voice_everyone_publish(void) {
    ServerSim       *sim = veRunningSim();
    VeSeen           seen;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    memset(&seen, 0, sizeof(seen));
    h = serverSimRegisterSubscriber(sim, veCount, &seen);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    VE_EXPECT(seen, 0, false, "a sync with the flag off");

    /* The op: one event for each change, none for a repeat. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    VE_EXPECT(seen, 1, true, "on");
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    VE_EXPECT(seen, 1, true, "on again");
    UT_ASSERT(veSet(sim, false) == SCN_OP_OK);
    VE_EXPECT(seen, 2, false, "off");
    UT_ASSERT(veSet(sim, false) == SCN_OP_OK);
    VE_EXPECT(seen, 2, false, "off again");

    /* The three clears, each while the flag is on. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimReturnToLobby(sim);
    VE_EXPECT(seen, 4, false, "the return to the lobby");

    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimStartGame(sim);
    VE_EXPECT(seen, 6, false, "a round start after a lobby call");

    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    VE_EXPECT(seen, 8, false, "a detach");

    /* And a clear while it is already off says nothing. */
    serverSimStartGame(sim);
    VE_EXPECT(seen, 8, false, "a round start with the flag off");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* The bottom lines queued on a client's newswire, ASCII only, as one string. */
static void veNewswire(ClientSim *cs, char *out, size_t cap) {
    MessageState *ms = clientSimGetMessages(cs);
    size_t        n  = 0;
    int           i;

    for (i = 0; i < ms->queueCount && n + 1 < cap; i++) {
        uint32_t cp = ms->queueBottom[(ms->queueHead + i) % MESSAGE_QUEUE_CAP];
        out[n++] = (cp < 0x80) ? (char)cp : '?';
    }
    out[n] = '\0';
}

/* How many lines the client has queued on its newswire. This binary stubs
 * langGetText to return "?" for every string (test_stubs.c), so a line told
 * is one "?" in the queue, and which of the two lines it was is read off
 * clientSimGetVoiceEveryone instead. Nothing else in these cases writes to
 * the newswire. */
static int veToldCount(ClientSim *cs) {
    static char text[MESSAGE_QUEUE_CAP + 1];
    const char *p;
    int         n = 0;

    veNewswire(cs, text, sizeof(text));
    for (p = strchr(text, '?'); p != NULL; p = strchr(p + 1, '?')) {
        n++;
    }
    return n;
}

int run_scenario_voice_everyone_join(void) {
    ServerSim       *sim = veRunningSim();
    VeSeen           seen;
    SubscriberHandle h;
    ClientSim       *cs;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);

    /* A subscriber joining now is given the flag in its sync. */
    memset(&seen, 0, sizeof(seen));
    h = serverSimRegisterSubscriber(sim, veCount, &seen);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    VE_EXPECT(seen, 1, true, "a sync with the flag on");
    serverSimUnregisterSubscriber(sim, h);

    /* And a client joining now holds it and is told, because it has no
       other way to know. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 2);
    h = serverSimRegisterClientSubscriber(sim, cs);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(clientSimGetVoiceEveryone(cs),
                  "a client joining mid-round does not know voice goes to "
                  "everyone");
    UT_ASSERT_MSG(veToldCount(cs) == 1,
                  "a client joining mid-round was told %d time(s), expected 1",
                  veToldCount(cs));
    serverSimUnregisterSubscriber(sim, h);
    clientSimDestroy(cs);

    serverSimDestroy(sim);
    return 0;
}

static void veApply(ClientSim *cs, ControlEventType type, bool on) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    if (type == CTRL_VOICE_EVERYONE) {
        evt.u.voiceEveryone.on = on;
    }
    clientSimApplyControl(cs, &evt);
}

int run_scenario_voice_everyone_client(void) {
    ClientSim *cs = clientSimAlloc();

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    veApply(cs, CTRL_GAME_PHASE_LOBBY, false);

    /* In the lobby: held, not told, since the lobby is all-talk. */
    veApply(cs, CTRL_VOICE_EVERYONE, true);
    UT_ASSERT(!clientSimGetVoiceEveryone(cs));
    UT_ASSERT_MSG(veToldCount(cs) == 0,
                  "the lobby was told about voice to everyone");

    /* The round starts with it on, as a script's on_setup leaves it. */
    veApply(cs, CTRL_GAME_PHASE_RUNNING, false);
    UT_ASSERT(clientSimGetVoiceEveryone(cs));
    UT_ASSERT_MSG(veToldCount(cs) == 1,
                  "the round start told %d line(s), expected 1",
                  veToldCount(cs));

    /* A repeat tells nothing; a change tells each time. */
    veApply(cs, CTRL_VOICE_EVERYONE, true);
    UT_ASSERT(veToldCount(cs) == 1);
    veApply(cs, CTRL_VOICE_EVERYONE, false);
    UT_ASSERT(!clientSimGetVoiceEveryone(cs));
    UT_ASSERT_MSG(veToldCount(cs) == 2,
                  "off in the round left %d line(s) told, expected 2",
                  veToldCount(cs));
    veApply(cs, CTRL_VOICE_EVERYONE, true);
    UT_ASSERT(clientSimGetVoiceEveryone(cs));
    UT_ASSERT(veToldCount(cs) == 3);

    /* The round ends: the value goes, and nobody is told "allies only" on
       the way into an all-talk lobby, by the end or by the clear after it. */
    veApply(cs, CTRL_GAME_PHASE_GAME_OVER, false);
    UT_ASSERT(!clientSimGetVoiceEveryone(cs));
    veApply(cs, CTRL_VOICE_EVERYONE, false);
    veApply(cs, CTRL_GAME_PHASE_LOBBY, false);
    UT_ASSERT(!clientSimGetVoiceEveryone(cs));
    UT_ASSERT_MSG(veToldCount(cs) == 3,
                  "leaving the round left %d line(s) told, expected 3",
                  veToldCount(cs));

    clientSimDestroy(cs);
    return 0;
}

/* Seat 2's microphone flags as seat 0's snapshot carries them. Returns 0 when
 * seat 2 is not in the snapshot at all. */
static int veMicFlagsSeenBy0(ServerSim *sim, uint8_t *out) {
    SnapshotHeader      hdr;
    TankSnapshot        tk[MAX_TANKS];
    ShellSnapshot       sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bo[MAX_SNAPSHOT_BASES];
    PillSnapshot        po[MAX_SNAPSHOT_PILLS];
    GameEvent           ev[MAX_SNAPSHOT_EVENTS];
    int                 i;

    memset(&hdr, 0, sizeof(hdr));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS,
                           sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS,
                           ev, MAX_SNAPSHOT_EVENTS, true);
    for (i = 0; i < (int)hdr.tankCount; i++) {
        if ((tk[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK) == 2) {
            *out = (uint8_t)(tk[i].clientFlags & PLAYER_VOICE_FLAG_MASK);
            return 1;
        }
    }
    return 0;
}

int run_scenario_voice_everyone_mic_bits(void) {
    ServerSim *sim = veRunningSim();
    uint8_t    mic = 0xFF;

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }
    playersSetClientFlags(&sim->sim.plyrs, 2, PLAYER_FLAG_HAS_MIC);

    /* Allies only: seat 0 cannot hear seat 2, so is not shown a microphone. */
    UT_ASSERT_MSG(veMicFlagsSeenBy0(sim, &mic),
                  "seat 2 is not in seat 0's snapshot");
    UT_ASSERT_MSG(mic == 0,
                  "seat 0 was shown seat 2's microphone (0x%02x) with voice to "
                  "allies only", (unsigned)mic);

    /* Voice to everyone: seat 0 hears seat 2, so is shown it. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    UT_ASSERT(veMicFlagsSeenBy0(sim, &mic));
    UT_ASSERT_MSG(mic == PLAYER_FLAG_HAS_MIC,
                  "seat 0 was not shown seat 2's microphone (0x%02x) with "
                  "voice to everyone", (unsigned)mic);

    /* And back. */
    UT_ASSERT(veSet(sim, false) == SCN_OP_OK);
    UT_ASSERT(veMicFlagsSeenBy0(sim, &mic));
    UT_ASSERT(mic == 0);

    serverSimDestroy(sim);
    return 0;
}
