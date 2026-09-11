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
 * The two flow ops:
 *
 *   run_scenario_flow_end_round            — the round ends on the script's
 *       terms: the state, the reason and the line the lobby is given
 *   run_scenario_flow_end_round_resolve    — the resolve keeps that line and
 *       credits nobody, on a board the base sweep would otherwise win
 *   run_scenario_flow_end_round_refusals   — a lobby has no round to end, and
 *       a line with no terminator is refused rather than read past
 *   run_scenario_flow_set_game_time        — an absolute and a relative
 *       change, on the sim and at a subscriber
 *   run_scenario_flow_set_game_time_refusals — the four lengths the arm will
 *       not write, and the state it will not write in
 *   run_scenario_flow_arm_records          — the length change is in the
 *       recording, and ending the round adds nothing of its own
 *
 * Crediting is observed through the WIN-event spy in test_stubs.c
 * (wbnStubWinEventCalls / wbnStubWinEventMask), which counts every
 * winbolonetAddEvent WIN — so it catches either crediting function. The
 * resolve case stacks the board so that both of them would have something to
 * credit, which is what makes a count of zero mean the arm called neither
 * rather than meaning there was nobody to credit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, returnToLobby*, pendingWinMessage,
                                    * gameLength */
#include "server_sim_scenario.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / serverSimSetTeam */
#include "control_event.h"
#include "game_sim.h"              /* GameSim.bs — the board the sweep reads */
#include "bases.h"                 /* basesGetNumBases */
#include "everard_map.h"           /* E_MAP */
#include "log.h"                   /* log_GameTimeSet, logWriteTick, the stream
                                    * opcodes */
#include "replay_harness.h"
#include "test_harness.h"

/* WIN-event spy from test_stubs.c. */
extern int      wbnStubWinEventCalls;
extern uint16_t wbnStubWinEventMask;

/* The two seats the resolve case seats, on opposing teams: the sweeper holds
 * every base, so the base arm would credit him, and the holdout is on a team
 * other than the one the op names as the winner, so the surrender arm would
 * credit him. Either arm taken by mistake shows up as a win event. */
#define FA_SWEEP_SLOT  0
#define FA_SWEEP_NAME  "Scripter"
#define FA_SWEEP_TEAM  1
#define FA_OTHER_SLOT  1
#define FA_OTHER_NAME  "Holdout"
#define FA_OTHER_TEAM  2

/* Armour comfortably above MIN_ARMOUR_CAPTURE — a held base. */
#define FA_ARMOUR_HELD 50

/* The line the script leaves in the returning lobby. Nothing the base sweep
 * or the vote arms write reads like this, so finding it after a resolve says
 * the resolve left it alone. */
#define FA_WIN_LINE "*** The scenario called the round. ***"

/* Five minutes at 50 ticks a second, and a minute on top of it. */
#define FA_ABS_TICKS 15000
#define FA_REL_DELTA 3000

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A running, lobby-enabled round with no players yet: each case seats what it
 * needs. The lobby flag matters — serverSimResolveGameOver is a no-op without
 * a lobby to return a line to. */
static ServerSim *faRunningSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetQuitOnWin(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* A lobby that has not started: the state both arms refuse in. */
static ServerSim *faLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, FA_SWEEP_NAME, false);
    return sim;
}

/* Hand every base to one owner so serverSimWinningOwner names him and the
 * base arm of the resolve has a winner to credit. item[] is 0-based. */
static void faSweepBases(ServerSim *sim, BYTE owner) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE n, i;
    if (gs == NULL) return;
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        (*gs->bs).item[i].owner  = owner;
        (*gs->bs).item[i].armour = FA_ARMOUR_HELD;
    }
}

/* Both spy globals live for the whole binary run, so a case clears them right
 * before the resolve it measures. */
static void faResetWinSpy(void) {
    wbnStubWinEventCalls = 0;
    wbnStubWinEventMask  = 0;
}

/* ── The published settings event ─────────────────────────────────────── */

typedef struct {
    int     count;        /* CTRL_LOBBY_SETTINGS published since the reset */
    int32_t lastLimit;    /* lobbyTimeLimit of the most recent one */
} FaCapture;

static void faCaptureCb(void *ctx, const ControlEvent *evt) {
    FaCapture *c = (FaCapture *)ctx;
    if (evt->type != CTRL_LOBBY_SETTINGS) return;
    c->count++;
    c->lastLimit = evt->u.lobbySettings.lobbyTimeLimit;
}

/* Registration replays the current server state to the new subscriber, so the
 * capture is cleared afterwards and counts only what happens next. */
static void faSubscribe(ServerSim *sim, FaCapture *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, faCaptureCb, c);
    memset(c, 0, sizeof(*c));
}

/* ── Op helpers ───────────────────────────────────────────────────────── */

static ScnOpResult faEndRound(ServerSim *sim, const char *text,
                              BYTE winnerTeam) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_END_ROUND;
    op.u.endRound.winnerTeam = winnerTeam;
    SDL_strlcpy(op.u.endRound.text, text, sizeof(op.u.endRound.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult faSetGameTime(ServerSim *sim, int32_t ticks, bool relative) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SET_GAME_TIME;
    op.u.setGameTime.ticks    = ticks;
    op.u.setGameTime.relative = relative;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ================================================================
 * 1. A script ends the running round.
 * ================================================================ */
int run_scenario_flow_end_round(void) {
    ServerSim *sim = faRunningSim();

    UT_ASSERT_MSG(sim != NULL, "faRunningSim returned NULL");
    serverSimAddPlayer(sim, FA_SWEEP_SLOT, FA_SWEEP_NAME, false);
    serverSimSetTeam(sim, FA_SWEEP_SLOT, FA_SWEEP_TEAM);

    UT_ASSERT_MSG(faEndRound(sim, FA_WIN_LINE, FA_SWEEP_TEAM) == SCN_OP_OK,
                  "a scripted end to a running round must be accepted");

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the round must be in game-over, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SCENARIO,
                  "the round must end as a scenario end (%d), got %u",
                  RETURN_REASON_SCENARIO, (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(strcmp(sim->pendingWinMessage, FA_WIN_LINE) == 0,
                  "the returning lobby must be given the op's line, got \"%s\"",
                  sim->pendingWinMessage);
    /* Written where the other reasons write theirs even though nothing reads
       it under this one, so the op's payload is not silently dropped. */
    UT_ASSERT_MSG(sim->returnToLobbyTeamId == FA_SWEEP_TEAM,
                  "the op's winning team must be recorded as %d, got %u",
                  FA_SWEEP_TEAM, (unsigned)sim->returnToLobbyTeamId);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. The resolve keeps the script's line and credits nobody.
 *
 * The board is a completed sweep and the op names a winning team, so the base
 * arm and the surrender arm both have somebody to credit. The case resolves
 * the same board as a base win first: that leg proves the board really does
 * credit, which is what makes a count of zero in the second leg mean the arm
 * called neither crediting function rather than meaning there was nobody to
 * credit. Resolving does not move the state, so the op still meets a running
 * round.
 * ================================================================ */
int run_scenario_flow_end_round_resolve(void) {
    ServerSim *sim = faRunningSim();

    UT_ASSERT_MSG(sim != NULL, "faRunningSim returned NULL");
    serverSimAddPlayer(sim, FA_SWEEP_SLOT, FA_SWEEP_NAME, false);
    serverSimSetTeam(sim, FA_SWEEP_SLOT, FA_SWEEP_TEAM);
    serverSimAddPlayer(sim, FA_OTHER_SLOT, FA_OTHER_NAME, false);
    serverSimSetTeam(sim, FA_OTHER_SLOT, FA_OTHER_TEAM);
    faSweepBases(sim, FA_SWEEP_SLOT);

    /* The control leg: this board, under the reason the sweep arms, both
       speaks and credits. */
    sim->returnToLobbyReason = RETURN_REASON_BASE_WIN;
    faResetWinSpy();
    serverSimResolveGameOver(sim);
    UT_ASSERT_MSG(wbnStubWinEventCalls > 0,
                  "setup: this board must be one a base win credits, got %d "
                  "win event(s) — the second leg below would prove nothing",
                  wbnStubWinEventCalls);
    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, FA_SWEEP_NAME) != NULL,
                  "setup: a base win must name the sweeper, got \"%s\"",
                  sim->pendingWinMessage);

    UT_ASSERT_MSG(faEndRound(sim, FA_WIN_LINE, FA_OTHER_TEAM) == SCN_OP_OK,
                  "a scripted end to a running round must be accepted");

    faResetWinSpy();
    serverSimResolveGameOver(sim);

    UT_ASSERT_MSG(strcmp(sim->pendingWinMessage, FA_WIN_LINE) == 0,
                  "the resolve must leave the op's line exactly as it was, "
                  "got \"%s\" — the base sweep's message has replaced it",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(wbnStubWinEventCalls == 0,
                  "a scripted end must credit nobody with a WinBolo.net win, "
                  "got %d win event(s), mask 0x%04x — the resolve reached a "
                  "crediting arm", wbnStubWinEventCalls,
                  (unsigned)wbnStubWinEventMask);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. What ending a round is refused for.
 * ================================================================ */
int run_scenario_flow_end_round_refusals(void) {
    ServerSim *lobby = faLobbySim();
    ServerSim *sim;
    ScenarioOp op;

    UT_ASSERT_MSG(lobby != NULL, "faLobbySim returned NULL");
    UT_ASSERT_MSG(faEndRound(lobby, FA_WIN_LINE, 0) == SCN_OP_WRONG_STATE,
                  "a lobby has no round to end");
    UT_ASSERT_MSG(lobby->returnToLobbyReason == RETURN_REASON_NONE,
                  "a refused end must leave the reason alone, got %u",
                  (unsigned)lobby->returnToLobbyReason);
    UT_ASSERT_MSG(serverSimGetState(lobby) == serverStateLobby,
                  "a refused end must leave the lobby in the lobby, state %d",
                  (int)serverSimGetState(lobby));
    serverSimDestroy(lobby);

    sim = faRunningSim();
    UT_ASSERT_MSG(sim != NULL, "faRunningSim returned NULL");
    serverSimAddPlayer(sim, FA_SWEEP_SLOT, FA_SWEEP_NAME, false);

    /* A line with no terminator anywhere in the field. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_END_ROUND;
    memset(op.u.endRound.text, 'x', sizeof(op.u.endRound.text));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "a line with no terminator must be refused rather than read "
                  "past the end of");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "a refused end must leave the round running, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->pendingWinMessage[0] == '\0',
                  "a refused end must write no lobby line, got \"%s\"",
                  sim->pendingWinMessage);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A script sets the round's game time, absolutely and by a delta.
 * ================================================================ */
int run_scenario_flow_set_game_time(void) {
    ServerSim *sim = faRunningSim();
    FaCapture cap;

    UT_ASSERT_MSG(sim != NULL, "faRunningSim returned NULL");
    serverSimAddPlayer(sim, FA_SWEEP_SLOT, FA_SWEEP_NAME, false);
    faSubscribe(sim, &cap);

    /* Absolute: the length becomes the number the op carries. */
    UT_ASSERT_MSG(faSetGameTime(sim, FA_ABS_TICKS, false) == SCN_OP_OK,
                  "an absolute length inside the range must be accepted");
    UT_ASSERT_MSG(sim->gameLength == FA_ABS_TICKS,
                  "the round's length is %d, expected %d",
                  (int)sim->gameLength, FA_ABS_TICKS);
    UT_ASSERT_MSG(cap.count == 1,
                  "an absolute set published %d settings event(s), expected 1",
                  cap.count);
    UT_ASSERT_MSG(cap.lastLimit == FA_ABS_TICKS,
                  "the settings event carries lobbyTimeLimit %d, expected %d",
                  (int)cap.lastLimit, FA_ABS_TICKS);

    /* Relative: the delta lands on the length that is there now. */
    UT_ASSERT_MSG(faSetGameTime(sim, FA_REL_DELTA, true) == SCN_OP_OK,
                  "a delta that keeps the length inside the range must be "
                  "accepted");
    UT_ASSERT_MSG(sim->gameLength == FA_ABS_TICKS + FA_REL_DELTA,
                  "the round's length is %d, expected %d",
                  (int)sim->gameLength, FA_ABS_TICKS + FA_REL_DELTA);
    UT_ASSERT_MSG(cap.count == 2,
                  "a relative set published %d settings event(s) in total, "
                  "expected 2", cap.count);
    UT_ASSERT_MSG(cap.lastLimit == FA_ABS_TICKS + FA_REL_DELTA,
                  "the settings event carries lobbyTimeLimit %d, expected %d",
                  (int)cap.lastLimit, FA_ABS_TICKS + FA_REL_DELTA);

    /* A negative delta shortens it. */
    UT_ASSERT_MSG(faSetGameTime(sim, -FA_REL_DELTA, true) == SCN_OP_OK,
                  "a delta that shortens the round must be accepted");
    UT_ASSERT_MSG(sim->gameLength == FA_ABS_TICKS,
                  "the round's length is %d, expected %d",
                  (int)sim->gameLength, FA_ABS_TICKS);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. The lengths the arm will not write.
 * ================================================================ */
int run_scenario_flow_set_game_time_refusals(void) {
    ServerSim *lobby = faLobbySim();
    ServerSim *sim;

    UT_ASSERT_MSG(lobby != NULL, "faLobbySim returned NULL");
    UT_ASSERT_MSG(faSetGameTime(lobby, FA_ABS_TICKS, false) == SCN_OP_WRONG_STATE,
                  "a lobby has no running round to time");
    serverSimDestroy(lobby);

    sim = faRunningSim();
    UT_ASSERT_MSG(sim != NULL, "faRunningSim returned NULL");
    serverSimAddPlayer(sim, FA_SWEEP_SLOT, FA_SWEEP_NAME, false);
    UT_ASSERT_MSG(sim->gameLength == UNLIMITED_GAME_TIME,
                  "setup: the fixture round must start unlimited, got %d",
                  (int)sim->gameLength);

    /* A delta on a round with no limit: -1 is not a length to add to. */
    UT_ASSERT_MSG(faSetGameTime(sim, FA_REL_DELTA, true) == SCN_OP_RANGE,
                  "a delta on an unlimited round must be refused rather than "
                  "turning it into a round of %d ticks", FA_REL_DELTA - 1);
    UT_ASSERT_MSG(sim->gameLength == UNLIMITED_GAME_TIME,
                  "the refused delta wrote a length of %d",
                  (int)sim->gameLength);

    /* Zero: the countdown runs only above zero, so zero is an endless round
       rather than one with no time left. */
    UT_ASSERT_MSG(faSetGameTime(sim, 0, false) == SCN_OP_RANGE,
                  "a length of zero must be refused — it disables the "
                  "countdown instead of ending the round");
    /* Below zero. */
    UT_ASSERT_MSG(faSetGameTime(sim, -1, false) == SCN_OP_RANGE,
                  "a negative length must be refused");
    UT_ASSERT_MSG(sim->gameLength == UNLIMITED_GAME_TIME,
                  "a refused length wrote %d", (int)sim->gameLength);

    /* Past the end of the field, reached by a delta rather than by a number
       the op could not have carried. */
    UT_ASSERT_MSG(faSetGameTime(sim, INT32_MAX, false) == SCN_OP_OK,
                  "the longest length the field holds must be accepted");
    UT_ASSERT_MSG(faSetGameTime(sim, 1, true) == SCN_OP_RANGE,
                  "a delta past the end of the field must be refused rather "
                  "than wrapping into a short round");
    UT_ASSERT_MSG(sim->gameLength == INT32_MAX,
                  "the refused delta wrote a length of %d",
                  (int)sim->gameLength);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. The records the two arms leave.
 *
 * The stream: everything the settle ticks wrote, then whatever the two ops
 * queue. No tick runs between them, so the flush below frames the ops' own
 * writes and nothing else — which is why "the length change is the last
 * record in the file" states that ending the round added none of its own.
 * ================================================================ */

#define FA_MAX_HITS 64

typedef struct {
    int     count;
    uint8_t code[FA_MAX_HITS];
    uint8_t payload[FA_MAX_HITS][16];
    int     payloadLen[FA_MAX_HITS];
} FaLogHits;

static int faReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills, bases
 * and starts, the map runs up to the deep-sea terminator, then MAX_TANKS
 * player blocks. Plaintext, not length-framed. */
static bool faSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = faReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = faReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = faReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = faReadByte(buf, len, p);
        y    = faReadByte(buf, len, p + 1);
        sx   = faReadByte(buf, len, p + 2);
        ex   = faReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = faReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every record in it, in order.
 * Returns false if the stream did not end on a clean LOG_QUIT. */
static bool faCollectLogged(const char *path, FaLogHits *hits) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   pos;
    bool     ok = false;

    memset(hits, 0, sizeof(*hits));
    if (!extractLogDat(path, &buf, &len)) return false;
    /* Header: WBOLOMOV(8) + version(1) + mapname pstr + game(8) + addr(4) +
       port(2) + time(4) + WBN key(32). */
    if (len < 10 || memcmp(buf, "WBOLOMOV", 8) != 0 || buf[8] != LOG_VERSION) {
        free(buf);
        return false;
    }
    pos = 8 + 1;
    pos += 1 + buf[pos];
    pos += 8 + 4 + 2 + 4 + 32;

    while (pos < len) {
        int code = faReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (faReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!faSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = faReadByte(buf, len, pos);
                pos += 1;
                if (n < 0) break;
            } else {
                if (pos + 2 > len) break;
                /* The writer stores data[1]=low, data[2]=high and the reader
                   rebuilds the count as (lo << 8) | hi. */
                n = (buf[pos + 1] << 8) | buf[pos];
                pos += 2;
            }
            for (i = 0; i < n; i++) {
                int ev, plen;
                size_t payloadStart;
                if (pos + 3 > len) { n = -1; break; }
                ev   = buf[pos];
                plen = (buf[pos + 1] << 8) | buf[pos + 2];
                payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) { n = -1; break; }
                if (hits->count < FA_MAX_HITS) {
                    int slot = hits->count;
                    int copy = plen;
                    if (copy > (int)sizeof(hits->payload[0])) {
                        copy = (int)sizeof(hits->payload[0]);
                    }
                    hits->code[slot] = (uint8_t)ev;
                    hits->payloadLen[slot] = plen;
                    memcpy(hits->payload[slot], buf + payloadStart,
                           (size_t)copy);
                    hits->count++;
                }
                pos = payloadStart + (size_t)plen;
            }
            if (n < 0) break;
        } else {
            break;
        }
    }

    free(buf);
    return ok;
}

int run_scenario_flow_arm_records(void) {
    ReplayHarness h;
    ServerSim *sim;
    FaLogHits hits;
    int last, i;
    int timeRecords = 0;
    int32_t recorded;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnFlowArms", FA_SWEEP_NAME),
                  "could not start recording");
    sim = h.sim;

    /* Let the round settle after the opening snapshot. Everything these ticks
       write is flushed by the ticks themselves. */
    replayHarnessTick(&h, 4);

    UT_ASSERT(faSetGameTime(sim, FA_ABS_TICKS, false) == SCN_OP_OK);
    UT_ASSERT(faEndRound(sim, FA_WIN_LINE, FA_SWEEP_TEAM) == SCN_OP_OK);

    /* The game-over tick returns before the recorder's flush, so the test
       drains the two ops' writes itself. Without this an arm that did write a
       record would leave it in the buffer and the assertions below would pass
       for the wrong reason. */
    logWriteTick();

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(faCollectLogged(h.path, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count > 0, "the recording holds no records at all");
    UT_ASSERT_MSG(hits.count < FA_MAX_HITS,
                  "the recording holds at least %d records, more than the "
                  "walker keeps", hits.count);

    for (i = 0; i < hits.count; i++) {
        if (hits.code[i] == (uint8_t)log_GameTimeSet) timeRecords++;
    }
    UT_ASSERT_MSG(timeRecords == 1,
                  "the recording holds %d log_GameTimeSet record(s), "
                  "expected 1", timeRecords);

    last = hits.count - 1;
    UT_ASSERT_MSG(hits.code[last] == (uint8_t)log_GameTimeSet,
                  "record %d of %d is opcode %u, not the length change — "
                  "ending the round wrote a record of its own",
                  last, hits.count, (unsigned)hits.code[last]);
    UT_ASSERT_MSG(hits.payloadLen[last] == 4,
                  "the length change recorded %d payload byte(s), expected 4",
                  hits.payloadLen[last]);
    recorded = (int32_t)(((uint32_t)hits.payload[last][0] << 24) |
                         ((uint32_t)hits.payload[last][1] << 16) |
                         ((uint32_t)hits.payload[last][2] << 8)  |
                         (uint32_t)hits.payload[last][3]);
    UT_ASSERT_MSG(recorded == FA_ABS_TICKS,
                  "the length change recorded %d ticks, expected %d",
                  (int)recorded, FA_ABS_TICKS);

    replayHarnessStop(&h);
    return 0;
}
