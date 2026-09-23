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

/*
 * The five comms ops:
 *
 *   run_scenario_comms_msg_all      — a line to the whole game
 *   run_scenario_comms_msg_team     — a line to one team, and who sees it
 *   run_scenario_comms_msg_player   — a line to one player, and who sees it
 *   run_scenario_comms_sound        — a sound at a square and a sound nowhere
 *   run_scenario_comms_log          — a line to the console and nowhere else
 *   run_scenario_comms_say          — a seat's own chat line, and the bot
 *       inbox it lands in that a server line never reaches
 *   run_scenario_comms_arm_records  — the three lines survive a recording
 *
 * plus the two cases that make the destination safe rather than testing an
 * arm. Neither is a scenario op; both are here because a rule and its proof
 * belong in one file:
 *
 *   run_scenario_comms_decoder_dest_player  — the body decoder rebuilds the
 *       event for every remote client, and a rebuilt one that left destPlayer
 *       at zero would be addressed to slot 0 alone.
 *   run_scenario_comms_apply_non_zero_slot  — the in-process filter reads
 *       0xFF as everyone, so a client that is not slot 0 still hears a
 *       broadcast.
 *
 * Who hears a line is observed at two layers: the published ControlEvent,
 * caught by a subscriber, and the in-process apply, driven at a ClientSim
 * whose slot and team the case chooses. A sound is observed off the snapshot
 * builder, which is where soundPickOffer decides what a recipient hears.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* events[] / eventCount, playerConnected[] */
#include "server_sim_scenario.h"
#include "control_event.h"
#include "transport_control_codec.h" /* transportControlCodecBodyDecoder */
#include "client_sim.h"            /* ClientSim — the in-process recipient */
#include "client_sim_internal.h"   /* cs->lobbySlots — the recipient's seat */
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "client_enums.h"          /* sndEffects — the ids the sound arm takes */
#include "messages.h"              /* MessageState::queueCount, messageSetNewswire */
#include "game_sim.h"              /* GameSim.tanks — parking the listener */
#include "tank.h"                  /* tankSetWorld */
#include "sounddist.h"             /* SDIST_NONE — the cull the far sound is past */
#include "input_packet.h"          /* EVENT_SOUND, SOUND_TIER_NEAR, SOUND_DIR_CENTRE */
#include "log.h"                   /* log_ServerText and the stream opcodes */
#include "allience.h"              /* allienceAdd — the bot and the talker are allies */
#include "players.h"                /* playersSetPlayer — the names a chat line formats with */
#include "threads.h"                /* the mutex serverSimApplyCommand runs under */
#include "replay_harness.h"
#include "test_harness.h"

/* The two seats the text cases use: one talker the sim starts with, and one
 * more player so a line can be aimed at somebody who is not slot 0. */
#define CC_SLOT_TALKER 0
#define CC_SLOT_TARGET 1
/* A seat inside the roster with nobody in it. */
#define CC_SLOT_EMPTY  9

/* Teams the team case separates. Both are inside 1..MAX_TANKS-1. */
#define CC_TEAM_IN  3
#define CC_TEAM_OUT 4

/* The listener square the sound case measures from, and the gaps either side
 * of SDIST_NONE (40). Both squares stay well inside the map. */
#define CC_LISTENER_MX 100
#define CC_LISTENER_MY 100
#define CC_GAP_HEARD   30
#define CC_GAP_SILENT  70

/* ── The published event ──────────────────────────────────────────────── */

typedef struct {
    int          textCount;   /* CTRL_SERVER_TEXT published since the reset */
    ControlEvent last;        /* the most recent one, whole */
} CcCapture;

static void ccCaptureCb(void *ctx, const ControlEvent *evt) {
    CcCapture *c = (CcCapture *)ctx;
    if (evt->type != CTRL_SERVER_TEXT) return;
    c->textCount++;
    c->last = *evt;
}

/* Registration replays the current server state to the new subscriber, so the
 * capture is cleared afterwards and counts only what happens next. */
static void ccSubscribe(ServerSim *sim, CcCapture *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, ccCaptureCb, c);
    memset(c, 0, sizeof(*c));
}

/* ── The in-process recipient ─────────────────────────────────────────── */

/* A client seated in `slot` on `team`, newswire on so a server line lands in
 * its message ring. Not in a lobby, which is the branch that sends the line to
 * the newswire rather than to lobby chat. */
static ClientSim *ccClient(BYTE slot, BYTE team) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    cs->lobbySlots[slot].connected  = true;
    cs->lobbySlots[slot].teamNumber = team;
    messageSetNewswire(clientSimGetMessages(cs), true);
    return cs;
}

/* A line's arrival shows up as the ring growing by its width, not by one, so
 * the cases below test "grew" against "held". */
static int ccLines(ClientSim *cs) {
    return clientSimGetMessages(cs)->queueCount;
}

/* ── Op helpers ───────────────────────────────────────────────────────── */

/* A text field with no terminator in it at all — what every text arm refuses
 * with SCN_OP_TOO_BIG rather than reading past the end of. */
static void ccFillUnterminated(char *text, size_t cap) {
    memset(text, 'x', cap);
}

static ScnOpResult ccMsgAll(ServerSim *sim, const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_ALL;
    SDL_strlcpy(op.u.msgAll.text, text, sizeof(op.u.msgAll.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult ccMsgTeam(ServerSim *sim, BYTE team, const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_TEAM;
    op.u.msgTeam.team = team;
    SDL_strlcpy(op.u.msgTeam.text, text, sizeof(op.u.msgTeam.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult ccMsgPlayer(ServerSim *sim, BYTE slot, const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_PLAYER;
    op.u.msgPlayer.slot = slot;
    SDL_strlcpy(op.u.msgPlayer.text, text, sizeof(op.u.msgPlayer.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult ccSound(ServerSim *sim, BYTE sound, BYTE x, BYTE y) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SOUND;
    op.u.sound.sound = sound;
    op.u.sound.x = x;
    op.u.sound.y = y;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult ccLog(ServerSim *sim, const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOG;
    SDL_strlcpy(op.u.log.text, text, sizeof(op.u.log.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ================================================================
 * 1. A line to the whole game.
 * ================================================================ */
int run_scenario_comms_msg_all(void) {
    ServerSim *sim = ut_make_running_sim("Talker");
    CcCapture cap;
    ScenarioOp op;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ccSubscribe(sim, &cap);

    UT_ASSERT_MSG(ccMsgAll(sim, "everyone hears this") == SCN_OP_OK,
                  "a broadcast with a terminated line must be accepted");
    UT_ASSERT_MSG(cap.textCount == 1,
                  "a broadcast published %d server lines, expected 1",
                  cap.textCount);
    UT_ASSERT_MSG(strcmp(cap.last.u.serverText.text, "everyone hears this") == 0,
                  "the published line reads \"%s\"", cap.last.u.serverText.text);
    UT_ASSERT_MSG(cap.last.u.serverText.destTeam == 0,
                  "a broadcast must carry destTeam 0, got %u",
                  (unsigned)cap.last.u.serverText.destTeam);
    UT_ASSERT_MSG(cap.last.u.serverText.destPlayer == 0xFF,
                  "a broadcast must carry destPlayer 0xFF (everyone), got %u — "
                  "0 would deliver to slot 0 alone",
                  (unsigned)cap.last.u.serverText.destPlayer);

    /* Refusal: a field with no terminator. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_ALL;
    ccFillUnterminated(op.u.msgAll.text, sizeof(op.u.msgAll.text));
    cap.textCount = 0;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated line must be refused SCN_OP_TOO_BIG");
    UT_ASSERT_MSG(cap.textCount == 0,
                  "a refused broadcast published %d line(s) anyway",
                  cap.textCount);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. A line to one team: the event carries the team, a member sees it and
 *    somebody on another team does not.
 * ================================================================ */
int run_scenario_comms_msg_team(void) {
    ServerSim *sim = ut_make_running_sim("Talker");
    ClientSim *member = NULL;
    ClientSim *outsider = NULL;
    CcCapture cap;
    ScenarioOp op;
    int before;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ccSubscribe(sim, &cap);

    UT_ASSERT_MSG(ccMsgTeam(sim, CC_TEAM_IN, "team only") == SCN_OP_OK,
                  "a line to team %d must be accepted", CC_TEAM_IN);
    UT_ASSERT_MSG(cap.textCount == 1,
                  "a team line published %d server lines, expected 1",
                  cap.textCount);
    UT_ASSERT_MSG(cap.last.u.serverText.destTeam == CC_TEAM_IN,
                  "the published line carries destTeam %u, expected %d",
                  (unsigned)cap.last.u.serverText.destTeam, CC_TEAM_IN);
    UT_ASSERT_MSG(cap.last.u.serverText.destPlayer == 0xFF,
                  "a team line names no single slot, so destPlayer must be "
                  "0xFF, got %u", (unsigned)cap.last.u.serverText.destPlayer);

    /* The in-process filter, at a member and at somebody who is not one. */
    member = ccClient(CC_SLOT_TARGET, CC_TEAM_IN);
    outsider = ccClient(CC_SLOT_TARGET, CC_TEAM_OUT);
    UT_ASSERT(member != NULL && outsider != NULL);

    before = ccLines(member);
    clientSimApplyControl(member, &cap.last);
    UT_ASSERT_MSG(ccLines(member) > before,
                  "a team %d player did not receive their own team's line "
                  "(newswire %d -> %d)", CC_TEAM_IN, before, ccLines(member));

    before = ccLines(outsider);
    clientSimApplyControl(outsider, &cap.last);
    UT_ASSERT_MSG(ccLines(outsider) == before,
                  "a team %d player received team %d's line (newswire %d -> %d)",
                  CC_TEAM_OUT, CC_TEAM_IN, before, ccLines(outsider));

    /* Refusal: team 0 is "everyone" to both delivery filters, and
       SCN_OP_MSG_ALL is the op for that. */
    cap.textCount = 0;
    UT_ASSERT_MSG(ccMsgTeam(sim, 0, "no team") == SCN_OP_RANGE,
                  "team 0 must be refused SCN_OP_RANGE");
    UT_ASSERT_MSG(cap.textCount == 0, "a refused team line was published");

    /* Refusal: past the last team. teams[] is keyed 1..MAX_TANKS-1. */
    UT_ASSERT_MSG(ccMsgTeam(sim, MAX_TANKS, "no team") == SCN_OP_RANGE,
                  "team %d must be refused SCN_OP_RANGE", MAX_TANKS);
    UT_ASSERT_MSG(ccMsgTeam(sim, (BYTE)(MAX_TANKS - 1), "last team") == SCN_OP_OK,
                  "team %d is the last real team and must be accepted",
                  MAX_TANKS - 1);

    /* Refusal: a field with no terminator. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_TEAM;
    op.u.msgTeam.team = CC_TEAM_IN;
    ccFillUnterminated(op.u.msgTeam.text, sizeof(op.u.msgTeam.text));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated team line must be refused SCN_OP_TOO_BIG");

    clientSimDestroy(member);
    clientSimDestroy(outsider);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A line to one player: the event names the slot, that player sees it and
 *    another player does not.
 * ================================================================ */
int run_scenario_comms_msg_player(void) {
    ServerSim *sim = ut_make_running_sim("Talker");
    ClientSim *addressed = NULL;
    ClientSim *bystander = NULL;
    CcCapture cap;
    ScenarioOp op;
    int before;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, CC_SLOT_TARGET, "Target", false);
    ccSubscribe(sim, &cap);

    UT_ASSERT_MSG(ccMsgPlayer(sim, CC_SLOT_TARGET, "for you") == SCN_OP_OK,
                  "a line to a connected slot must be accepted");
    UT_ASSERT_MSG(cap.textCount == 1,
                  "a player line published %d server lines, expected 1",
                  cap.textCount);
    UT_ASSERT_MSG(cap.last.u.serverText.destPlayer == CC_SLOT_TARGET,
                  "the published line carries destPlayer %u, expected %d",
                  (unsigned)cap.last.u.serverText.destPlayer, CC_SLOT_TARGET);
    UT_ASSERT_MSG(cap.last.u.serverText.destTeam == 0,
                  "a player line holds no team, so destTeam must be 0, got %u",
                  (unsigned)cap.last.u.serverText.destTeam);

    /* The design's own line for this subphase: one test recipient sees it and
       another does not. Both are on the same team, so the slot is the only
       thing separating them. */
    addressed = ccClient(CC_SLOT_TARGET, CC_TEAM_IN);
    bystander = ccClient(CC_SLOT_EMPTY, CC_TEAM_IN);
    UT_ASSERT(addressed != NULL && bystander != NULL);

    before = ccLines(addressed);
    clientSimApplyControl(addressed, &cap.last);
    UT_ASSERT_MSG(ccLines(addressed) > before,
                  "the addressed slot %d did not receive its own line "
                  "(newswire %d -> %d)", CC_SLOT_TARGET, before,
                  ccLines(addressed));

    before = ccLines(bystander);
    clientSimApplyControl(bystander, &cap.last);
    UT_ASSERT_MSG(ccLines(bystander) == before,
                  "slot %d received a line addressed to slot %d "
                  "(newswire %d -> %d)", CC_SLOT_EMPTY, CC_SLOT_TARGET, before,
                  ccLines(bystander));

    /* Refusal: a seat inside the roster with nobody in it. */
    cap.textCount = 0;
    UT_ASSERT_MSG(ccMsgPlayer(sim, CC_SLOT_EMPTY, "nobody") ==
                      SCN_OP_NO_SUCH_PLAYER,
                  "an empty slot must be refused SCN_OP_NO_SUCH_PLAYER");
    UT_ASSERT_MSG(cap.textCount == 0, "a refused player line was published");

    /* Refusal: a slot past the roster. */
    UT_ASSERT_MSG(ccMsgPlayer(sim, MAX_TANKS, "nobody") ==
                      SCN_OP_NO_SUCH_PLAYER,
                  "slot %d is past the roster and must be refused "
                  "SCN_OP_NO_SUCH_PLAYER", MAX_TANKS);

    /* Refusal: a field with no terminator, at a slot that does exist — the
       slot is checked first, so a bad slot would answer for it instead. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_PLAYER;
    op.u.msgPlayer.slot = CC_SLOT_TARGET;
    ccFillUnterminated(op.u.msgPlayer.text, sizeof(op.u.msgPlayer.text));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated player line must be refused SCN_OP_TOO_BIG");

    clientSimDestroy(addressed);
    clientSimDestroy(bystander);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. Sound: a square is culled by distance as it always was, and the square
 *    that is nowhere reaches a listener anywhere on the map.
 * ================================================================ */

/* One snapshot build for `slot`, returning the emitted events and the count.
 * Everything else the build fills is scratch. */
static int ccBuild(ServerSim *sim, BYTE slot, GameEvent *evOut) {
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];

    memset(&hdr, 0, sizeof(hdr));
    serverSimBuildSnapshot(sim, slot, &hdr, tk, MAX_TANKS, sh,
                           MAX_SNAPSHOT_SHELLS, te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES, po, MAX_SNAPSHOT_PILLS,
                           evOut, MAX_SNAPSHOT_EVENTS, false);
    return (int)hdr.reliableEventCount;
}

static const GameEvent *ccFindSound(const GameEvent *ev, int n,
                                    uint8_t soundId) {
    int i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == EVENT_SOUND && ev[i].data[0] == soundId) {
            return &ev[i];
        }
    }
    return NULL;
}

int run_scenario_comms_sound(void) {
    ServerSim *sim = ut_make_running_sim("Listener");
    GameSim *gs;
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    const GameEvent *hit;
    int n;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->tanks[CC_SLOT_TALKER] != NULL);

    /* Park the listener on a known square. The cull measures from wx >> 8, so
       the square centre puts it exactly on CC_LISTENER_MX/MY. */
    {
        WORLD wx = (WORLD)(((int)CC_LISTENER_MX << M_W_SHIFT_SIZE) +
                           MAP_SQUARE_MIDDLE);
        WORLD wy = (WORLD)(((int)CC_LISTENER_MY << M_W_SHIFT_SIZE) +
                           MAP_SQUARE_MIDDLE);
        tankSetWorld(gs, &gs->tanks[CC_SLOT_TALKER], wx, wy, 0, false);
    }

    /* A sound inside SDIST_NONE, a sound past it, and one with no square at
       all. Three different ids, so the dedup cannot be what dropped any of
       them. */
    sim->eventCount = 0;
    sim->mapEventCount = 0;
    UT_ASSERT_MSG(ccSound(sim, (BYTE)bigExplosionNear,
                          (BYTE)(CC_LISTENER_MX + CC_GAP_HEARD),
                          CC_LISTENER_MY) == SCN_OP_OK,
                  "a sound on a real square must be accepted");
    UT_ASSERT_MSG(ccSound(sim, (BYTE)farmingTreeNear,
                          (BYTE)(CC_LISTENER_MX + CC_GAP_SILENT),
                          CC_LISTENER_MY) == SCN_OP_OK,
                  "a sound on a far square must be accepted — distance is the "
                  "delivery's business, not the arm's");
    UT_ASSERT_MSG(ccSound(sim, (BYTE)manDyingNear, 0xFF, 0xFF) == SCN_OP_OK,
                  "0xFF, 0xFF is the square that is nowhere and must be "
                  "accepted");

    n = ccBuild(sim, CC_SLOT_TALKER, ev);

    UT_ASSERT_MSG(ccFindSound(ev, n, (uint8_t)bigExplosionNear) != NULL,
                  "a sound %d squares away (inside SDIST_NONE %d) was not "
                  "delivered — %d event(s) came back",
                  CC_GAP_HEARD, SDIST_NONE, n);
    UT_ASSERT_MSG(ccFindSound(ev, n, (uint8_t)farmingTreeNear) == NULL,
                  "a sound %d squares away (past SDIST_NONE %d) was delivered "
                  "anyway — %d event(s) came back",
                  CC_GAP_SILENT, SDIST_NONE, n);

    hit = ccFindSound(ev, n, (uint8_t)manDyingNear);
    UT_ASSERT_MSG(hit != NULL,
                  "a sound published at 0xFF, 0xFF did not reach a listener at "
                  "%d,%d — %d event(s) came back",
                  CC_LISTENER_MX, CC_LISTENER_MY, n);
    UT_ASSERT_MSG(hit->data[1] == SOUND_TIER_NEAR,
                  "the everywhere sound arrived at tier %u, expected near (%d)",
                  (unsigned)hit->data[1], SOUND_TIER_NEAR);
    UT_ASSERT_MSG(hit->data[2] == SOUND_DIR_CENTRE,
                  "the everywhere sound arrived bearing %u, expected centre (%d)",
                  (unsigned)hit->data[2], SOUND_DIR_CENTRE);

    /* Refusal: an id that is not a sound. */
    UT_ASSERT_MSG(ccSound(sim, 200, CC_LISTENER_MX, CC_LISTENER_MY) ==
                      SCN_OP_RANGE,
                  "sound id 200 is past the last sndEffects value and must be "
                  "refused SCN_OP_RANGE");

    /* Refusal: the three that reach one player only, named by a byte this op
       has no field for. */
    UT_ASSERT_MSG(ccSound(sim, (BYTE)bubbles, CC_LISTENER_MX,
                          CC_LISTENER_MY) == SCN_OP_RANGE,
                  "bubbles reaches one player only and must be refused "
                  "SCN_OP_RANGE");
    UT_ASSERT_MSG(ccSound(sim, (BYTE)tankSinkNear, CC_LISTENER_MX,
                          CC_LISTENER_MY) == SCN_OP_RANGE,
                  "tankSinkNear reaches one player only and must be refused "
                  "SCN_OP_RANGE");
    UT_ASSERT_MSG(ccSound(sim, (BYTE)tankSinkFar, CC_LISTENER_MX,
                          CC_LISTENER_MY) == SCN_OP_RANGE,
                  "tankSinkFar reaches one player only and must be refused "
                  "SCN_OP_RANGE");

    /* Refusal: a square off the map. The sentinel is the one exemption. */
    UT_ASSERT_MSG(ccSound(sim, (BYTE)bigExplosionNear, 0, 0) ==
                      SCN_OP_BAD_SQUARE,
                  "0,0 is off the map and must be refused SCN_OP_BAD_SQUARE");
    serverSimScenarioDrainFill(sim);   /* a new frame's message allowance */
    UT_ASSERT_MSG(ccSound(sim, (BYTE)bigExplosionNear, 250, 250) ==
                      SCN_OP_BAD_SQUARE,
                  "250,250 is off the map and must be refused "
                  "SCN_OP_BAD_SQUARE");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. A console line goes to the console and nowhere else.
 * ================================================================ */
int run_scenario_comms_log(void) {
    ServerSim *sim = ut_make_running_sim("Talker");
    CcCapture cap;
    ScenarioOp op;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ccSubscribe(sim, &cap);
    sim->eventCount = 0;

    /* The console itself is the server's own callback, which a unit sim does
       not install, so what this case pins is the answer and the silence
       either side of it: no control event, no game event. */
    UT_ASSERT_MSG(ccLog(sim, "a note for the operator") == SCN_OP_OK,
                  "a console line with a terminated field must be accepted");
    UT_ASSERT_MSG(cap.textCount == 0,
                  "a console line published %d control event(s); it must "
                  "publish none", cap.textCount);
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 0,
                  "a console line staged %u game event(s); it must stage none",
                  (unsigned)serverSimGetEventCount(sim));

    /* Refusal: a field with no terminator. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOG;
    ccFillUnterminated(op.u.log.text, sizeof(op.u.log.text));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated console line must be refused "
                  "SCN_OP_TOO_BIG");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. The three lines survive a recording, destination bytes included.
 * ================================================================ */

#define CC_MAX_HITS 4

typedef struct {
    int     count;
    uint8_t payload[CC_MAX_HITS][32];
    int     payloadLen[CC_MAX_HITS];
} CcLogHits;

static int ccReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills, bases
 * and starts, the map runs up to the deep-sea terminator, then MAX_TANKS
 * player blocks. Plaintext, not length-framed. */
static bool ccSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = ccReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = ccReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = ccReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = ccReadByte(buf, len, p);
        y    = ccReadByte(buf, len, p + 1);
        sx   = ccReadByte(buf, len, p + 2);
        ex   = ccReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = ccReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every event of type `want`, in the
 * order they were written. Returns false if the stream did not end on a clean
 * LOG_QUIT. */
static bool ccFindLogged(const char *path, uint8_t want, CcLogHits *hits) {
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
        int code = ccReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (ccReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!ccSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = ccReadByte(buf, len, pos);
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
                if (ev == want && hits->count < CC_MAX_HITS) {
                    int slot = hits->count;
                    int copy = plen;
                    if (copy > (int)sizeof(hits->payload[0])) {
                        copy = (int)sizeof(hits->payload[0]);
                    }
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

/* One recorded line: destTeam, destPlayer, then a pascal string. Returns 0
 * when the record is the one asked for, 1 when it is not — the assertion macro
 * returns its own verdict, so the caller passes it straight on. */
static int ccCheckRecord(const CcLogHits *hits, int index, BYTE destTeam,
                         BYTE destPlayer, const char *text, const char *what) {
    const uint8_t *p = hits->payload[index];
    int textLen = (int)strlen(text);

    UT_ASSERT_MSG(hits->payloadLen[index] == 3 + textLen,
                  "%s recorded %d payload byte(s), expected %d",
                  what, hits->payloadLen[index], 3 + textLen);
    UT_ASSERT_MSG(p[0] == destTeam,
                  "%s recorded destTeam %u, expected %u",
                  what, (unsigned)p[0], (unsigned)destTeam);
    UT_ASSERT_MSG(p[1] == destPlayer,
                  "%s recorded destPlayer %u, expected %u",
                  what, (unsigned)p[1], (unsigned)destPlayer);
    UT_ASSERT_MSG(p[2] == (uint8_t)textLen,
                  "%s recorded a %u-byte line, expected %d",
                  what, (unsigned)p[2], textLen);
    UT_ASSERT_MSG(memcmp(p + 3, text, (size_t)textLen) == 0,
                  "%s recorded a line that is not the one published", what);
    return 0;
}

int run_scenario_comms_arm_records(void) {
    ReplayHarness h;
    ServerSim *sim;
    CcLogHits hits;
    int bad;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnCommsArms", "Talker"),
                  "could not start recording");
    sim = h.sim;

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);

    UT_ASSERT(ccMsgAll(sim, "all") == SCN_OP_OK);
    UT_ASSERT(ccMsgTeam(sim, CC_TEAM_IN, "team") == SCN_OP_OK);
    UT_ASSERT(ccMsgPlayer(sim, CC_SLOT_TALKER, "player") == SCN_OP_OK);

    /* Two ticks so the changes reach the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    UT_ASSERT_MSG(ccFindLogged(h.path, (uint8_t)log_ServerText, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count == 3,
                  "the recording holds %d log_ServerText record(s), expected 3",
                  hits.count);

    bad = ccCheckRecord(&hits, 0, 0, 0xFF, "all", "the broadcast");
    if (bad == 0) {
        bad = ccCheckRecord(&hits, 1, CC_TEAM_IN, 0xFF, "team",
                            "the team line");
    }
    if (bad == 0) {
        bad = ccCheckRecord(&hits, 2, 0, CC_SLOT_TALKER, "player",
                            "the player line");
    }

    replayHarnessStop(&h);
    return bad;
}

/* ================================================================
 * 7. The body decoder rebuilds the variant for every remote client, so it is
 *    a producer of the event like the sim-side publishers are. One that left
 *    destPlayer at zero would address every server line to slot 0.
 * ================================================================ */
int run_scenario_comms_decoder_dest_player(void) {
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SERVER_TEXT);
    ControlEvent out;
    uint8_t body[2 + 5];

    UT_ASSERT_MSG(dec != NULL, "no body decoder for CTRL_SERVER_TEXT");

    body[0] = 0xFE;   /* fromPlayer: server-originated English */
    body[1] = 0xFF;   /* the chat body's broadcast byte */
    memcpy(body + 2, "hello", 5);

    memset(&out, 0x5A, sizeof(out));
    UT_ASSERT_MSG(dec(body, sizeof(body), &out),
                  "the body decoder refused a well-formed server-text body");
    UT_ASSERT_MSG(out.type == CTRL_SERVER_TEXT,
                  "the decoded event is type %d", (int)out.type);
    UT_ASSERT_MSG(out.u.serverText.destPlayer == 0xFF,
                  "a decoded server line must be addressed to its recipient "
                  "(destPlayer 0xFF), got %u — the server already filtered by "
                  "destination before the encode, so any other value drops the "
                  "line at every client whose slot is not that number",
                  (unsigned)out.u.serverText.destPlayer);
    UT_ASSERT_MSG(out.u.serverText.destTeam == 0,
                  "a decoded server line must hold no team, got %u",
                  (unsigned)out.u.serverText.destTeam);
    UT_ASSERT_MSG(strcmp(out.u.serverText.text, "hello") == 0,
                  "the decoded line reads \"%s\"", out.u.serverText.text);
    return 0;
}

/* ================================================================
 * 8. The in-process filter reads 0xFF as everyone. A client that is not slot
 *    0 hears a broadcast, and does not hear a line addressed to slot 0.
 * ================================================================ */
int run_scenario_comms_apply_non_zero_slot(void) {
    ClientSim *cs = ccClient(3, CC_TEAM_IN);
    ControlEvent evt;
    int before;

    UT_ASSERT(cs != NULL);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, "everyone hears this",
                sizeof(evt.u.serverText.text));
    evt.u.serverText.destPlayer = 0xFF;

    before = ccLines(cs);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(ccLines(cs) > before,
                  "a broadcast (destPlayer 0xFF) did not reach the client in "
                  "slot 3 (newswire %d -> %d)", before, ccLines(cs));

    /* The same line addressed to slot 0 stops here. */
    evt.u.serverText.destPlayer = 0;
    before = ccLines(cs);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(ccLines(cs) == before,
                  "a line addressed to slot 0 reached the client in slot 3 "
                  "(newswire %d -> %d)", before, ccLines(cs));

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 9. game.say: a chat line said by a seat.
 *
 *    The four ops above are the server talking, and a server line never
 *    enters a brain's inbox — a brain reads chat. say is a seat talking, so
 *    the line goes down the CMD_CHAT path and lands in every allied brain's
 *    inbox, which is what a scenario needs to hand a bot an order.
 *
 *    Proven at the inbox itself: messageInboxCount + messageInboxPeek is
 *    what brainDataMakeInfo reads to build BrainInfo.messages, so an entry
 *    there is exactly what the brain sees as info.messages.
 * ================================================================ */

/* The bot this case talks to: a ClientSim in `slot` on `team`, registered as
 * a subscriber so a published CTRL_CHAT reaches it, with the two seats named
 * in its own player table because the chat arm formats a sender name. */
static ClientSim *ccBot(ServerSim *sim, BYTE slot, BYTE team,
                        SubscriberHandle *outHandle) {
    ClientSim *cs = clientSimAlloc();
    GameSim   *cgs;

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    clientSimSetIsBot(cs, true);
    cs->lobbySlots[slot].connected  = true;
    cs->lobbySlots[slot].teamNumber = team;

    cgs = clientSimGetGameSim(cs);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, CC_SLOT_TALKER,
                     (char *)"Talker", "??", 0, 0, 0, 0, 0, FALSE, 0, NULL,
                     TRUE);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, slot, (char *)"Bot1", "??",
                     0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
    *outHandle = serverSimRegisterClientSubscriber(sim, cs);
    if (*outHandle == SUBSCRIBER_HANDLE_INVALID) {
        clientSimDestroy(cs);
        return NULL;
    }

    /* The talker and this bot are allies in the bot's own player table, set
     * AFTER the sync replay, which rewrites that table.
     *
     * A hosted bot's inbox takes a BROADCAST only from an ally (client_sim.c
     * clientSimChatReachesInbox), and the round's alliances are what it reads
     * — a lobby team becomes an alliance at game start, and this fixture
     * seats its players straight into a running sim. */
    allienceAdd(&cgs->plyrs->item[CC_SLOT_TALKER].allie, slot);
    allienceAdd(&cgs->plyrs->item[slot].allie, CC_SLOT_TALKER);
    return cs;
}

static ScnOpResult ccSayTo(ServerSim *sim, BYTE slot, BYTE mode, BYTE target,
                           const char *text) {
    ScenarioOp  op;
    ScnOpResult r;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_SAY;
    op.u.msgSay.slot   = slot;
    op.u.msgSay.mode   = mode;
    op.u.msgSay.target = target;
    SDL_strlcpy(op.u.msgSay.text, text, sizeof(op.u.msgSay.text));
    /* The arm runs the dispatcher's CMD_CHAT arm, which every caller reaches
       holding the sim mutex. */
    threadsWaitForMutex();
    r = serverSimApplyScenarioOp(sim, &op, NULL);
    threadsReleaseMutex();
    return r;
}

static ScnOpResult ccSay(ServerSim *sim, BYTE slot, const char *text) {
    return ccSayTo(sim, slot, SCN_SAY_TEAM, 0, text);
}

int run_scenario_comms_say(void) {
    ServerSim       *sim;
    ClientSim       *bot = NULL;
    SubscriberHandle hBot = SUBSCRIBER_HANDLE_INVALID;
    ScenarioOp       op;
    char             pbuf[BRAIN_INBOX_MSG_LEN];
    char             body[BRAIN_INBOX_MSG_LEN];
    size_t           plen;
    BYTE             from;

    UT_ASSERT_MSG(threadsCreate(TRUE), "threadsCreate failed");
    sim = ut_make_running_sim("Talker");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, CC_SLOT_TARGET, "Bot1", false);

    /* Both seats on one team: team chat is what say writes, so the two have
       to share a team for the line to be addressed at all. */
    sim->lobbyPlayers[CC_SLOT_TALKER].teamNumber = CC_TEAM_IN;
    sim->lobbyPlayers[CC_SLOT_TARGET].teamNumber = CC_TEAM_IN;

    bot = ccBot(sim, CC_SLOT_TARGET, CC_TEAM_IN, &hBot);
    UT_ASSERT_MSG(bot != NULL, "the bot ClientSim would not register");

    /* A server line first, to show the difference the op exists for: it is
       published, and the brain's inbox does not grow by it. */
    UT_ASSERT_MSG(ccMsgTeam(sim, CC_TEAM_IN, "server line") == SCN_OP_OK,
                  "a line to team %d must be accepted", CC_TEAM_IN);
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(bot)) == 0,
                  "a server line put %d entries in the brain inbox; a brain "
                  "reads chat, and game.message is not chat",
                  messageInboxCount(clientSimGetMessages(bot)));

    /* And now the seat's own line. */
    UT_ASSERT_MSG(ccSay(sim, CC_SLOT_TALKER, "status") == SCN_OP_OK,
                  "say from a seated player on a team must be accepted");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(bot)) == 1,
                  "the brain inbox holds %d entries after one say, expected 1",
                  messageInboxCount(clientSimGetMessages(bot)));

    from = messageInboxPeek(clientSimGetMessages(bot), 0, pbuf);
    UT_ASSERT_MSG(from == CC_SLOT_TALKER,
                  "the inbox entry is from slot %u, expected %d — the sender "
                  "is the seat say named, not the server",
                  (unsigned)from, CC_SLOT_TALKER);
    plen = (size_t)(unsigned char)pbuf[0];
    if (plen >= sizeof(body)) plen = sizeof(body) - 1;
    memcpy(body, pbuf + 1, plen);
    body[plen] = '\0';
    UT_ASSERT_MSG(strcmp(body, "status") == 0,
                  "the inbox entry reads \"%s\", expected \"status\"", body);

    /* The other two destinations a player has. Everyone: no team filter to
       pass, which is what a round with no lobby behind it needs. */
    UT_ASSERT_MSG(ccSayTo(sim, CC_SLOT_TALKER, SCN_SAY_ALL, 0, "all of you")
                      == SCN_OP_OK,
                  "a broadcast say must be accepted");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(bot)) == 2,
                  "a broadcast say left %d inbox entries, expected 2",
                  messageInboxCount(clientSimGetMessages(bot)));

    /* And one seat by name. */
    UT_ASSERT_MSG(ccSayTo(sim, CC_SLOT_TALKER, SCN_SAY_PLAYER, CC_SLOT_TARGET,
                          "just you") == SCN_OP_OK,
                  "a say to one seat must be accepted");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(bot)) == 3,
                  "a say to the bot's own seat left %d inbox entries, "
                  "expected 3",
                  messageInboxCount(clientSimGetMessages(bot)));

    /* Refusal: a seat with nobody in it, as the sender and as the target. */
    UT_ASSERT_MSG(ccSay(sim, CC_SLOT_EMPTY, "nobody") == SCN_OP_NO_SUCH_PLAYER,
                  "say from an empty seat must be refused "
                  "SCN_OP_NO_SUCH_PLAYER");
    UT_ASSERT_MSG(ccSayTo(sim, CC_SLOT_TALKER, SCN_SAY_PLAYER, CC_SLOT_EMPTY,
                          "nobody") == SCN_OP_NO_SUCH_PLAYER,
                  "say to an empty seat must be refused "
                  "SCN_OP_NO_SUCH_PLAYER");

    /* Refusal: a seat on no team has nobody to say it to. game.message is
       the call for a line the whole game hears. */
    sim->lobbyPlayers[CC_SLOT_TALKER].teamNumber = 0;
    UT_ASSERT_MSG(ccSay(sim, CC_SLOT_TALKER, "teamless") == SCN_OP_RANGE,
                  "say from a seat on no team must be refused SCN_OP_RANGE");
    sim->lobbyPlayers[CC_SLOT_TALKER].teamNumber = CC_TEAM_IN;

    /* Refusal: an empty line, which every receiver would drop. */
    UT_ASSERT_MSG(ccSay(sim, CC_SLOT_TALKER, "") == SCN_OP_BAD_CALL,
                  "an empty line must be refused SCN_OP_BAD_CALL");

    /* Refusal: a field with no terminator. */
    serverSimScenarioDrainFill(sim);   /* a new frame's message allowance */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_SAY;
    op.u.msgSay.slot = CC_SLOT_TALKER;
    ccFillUnterminated(op.u.msgSay.text, sizeof(op.u.msgSay.text));
    threadsWaitForMutex();
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated line must be refused SCN_OP_TOO_BIG");
    threadsReleaseMutex();

    /* Nothing refused above reached the inbox: the three accepted lines are
       all that is in it. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(bot)) == 3,
                  "the refused lines put %d entries in the inbox, expected "
                  "the 3 the accepted says left",
                  messageInboxCount(clientSimGetMessages(bot)));

    serverSimUnregisterSubscriber(sim, hBot);
    clientSimDestroy(bot);
    serverSimDestroy(sim);
    threadsDestroy();
    return 0;
}
