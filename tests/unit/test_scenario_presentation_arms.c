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
 */

/*
 * The four presentation ops: the panel, the scoreboard row, the
 * centre-screen announcement and the map marker.
 *
 *   run_scn_arm_panel_publishes_and_records
 *       a list through the funnel: the event carries the target unpacked
 *       into its destination pair, and the record carries the same bytes.
 *       A list at SCN_PANEL_MAX goes through the same path, which is what
 *       the record buffer is sized for.
 *   run_scn_arm_panel_refusals
 *       a bad panel id, a bad target, a list past the cap and a list the
 *       shared parser refuses, each with its own code.
 *   run_scn_arm_panel_one_update_per_tick
 *       the coalescing key is the pair (panel, target): a second update
 *       for the same pair in one tick is refused, two for different
 *       targets in one tick are not, and the next tick admits another.
 *   run_scn_arm_panel_replayed_to_joiner
 *       a ClientSim registering after the op is given the stored list,
 *       and is not given one addressed to somebody else.
 *   run_scn_arm_panel_snapshot_bounded
 *       the delayed spectator ring's control snapshot carries each panel's
 *       everyone-addressed list and none of the targeted ones, so a
 *       scenario writing a panel per player cannot cost the keyframe the
 *       bytes that make it overflow.
 *   run_scn_arm_score_announce_marker
 *       the other three arms: one success each, the fields the event
 *       carries and the record it writes, and every refusal.
 *   run_scn_arm_markers_scores_replayed_to_joiner
 *       a ClientSim registering after the markers and scores were set is
 *       given the markers up and every score row; a marker held to another
 *       team is filtered on the way in, and a cleared one is not sent.
 *   run_scn_arm_markers_scores_snapshot
 *       the ring's control snapshot carries the everyone-addressed marker
 *       and both score rows, and not the marker held to a team.
 *   run_scn_arm_markers_scores_reset
 *       after the presentation reset a joiner is given none of them.
 *   run_scn_markers_scores_loopback_late_join
 *       a second client joining a running round over the real transport
 *       ends up with the markers and scores the first one was sent live.
 *
 * What a published event carries is observed at a subscriber. What a
 * recording carries is observed by walking the .wbv's event stream, which
 * is what a replay reads.
 *
 * Drives serverSimApplyScenarioOp and reads the ServerSim struct directly;
 * the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* the panel store, playerConnected[], sim->tick */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"
#include "control_event.h"
#include "scenario_panel.h"
#include "client_sim.h"            /* ClientSim — the joiner the replay reaches */
#include "client_sim_internal.h"   /* cs->lobbySlots — the joiner's seat */
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "log.h"                   /* log_Scn* and the stream opcodes */
#include "log_internal.h"          /* serverSimSerializeControlSnapshot,
                                    * LOG_CONTROL_SNAPSHOT_MAX */
#include "everard_map.h"
#include "replay_harness.h"
#include "loopback_harness.h"
#include "test_harness.h"

/* The seat the harness fills, and a second one the fixtures address. */
#define PA_SLOT_HOST  0
#define PA_SLOT_OTHER 3
/* A seat inside the roster with nobody in it. */
#define PA_SLOT_EMPTY 9
/* Teams are 1-based, so this names a real team. */
#define PA_TEAM       5
/* A square well inside the playable band, and one on the border, which is
   outside it whatever the map holds. */
#define PA_SQUARE_X   100
#define PA_SQUARE_Y   100
#define PA_EDGE_X     0
#define PA_EDGE_Y     0

/* A target byte naming one player slot. The slot is 0-based. */
#define PA_TARGET_PLAYER(slot) ((BYTE)(0x80 | (slot)))

/* ── What a publish looks like ─────────────────────────────────────── */

typedef struct {
    int          panelCount;
    int          scoreCount;
    int          announceCount;
    int          markerCount;
    ControlEvent lastPanel;
    ControlEvent lastScore;
    ControlEvent lastAnnounce;
    ControlEvent lastMarker;
} PaCapture;

static void paCaptureCb(void *ctx, const ControlEvent *evt) {
    PaCapture *c = (PaCapture *)ctx;
    switch (evt->type) {
        case CTRL_SCN_PANEL:    c->panelCount++;    c->lastPanel = *evt;    break;
        case CTRL_SCN_SCORE:    c->scoreCount++;    c->lastScore = *evt;    break;
        case CTRL_SCN_ANNOUNCE: c->announceCount++; c->lastAnnounce = *evt; break;
        case CTRL_SCN_MARKER:   c->markerCount++;   c->lastMarker = *evt;   break;
        default: break;
    }
}

/* Registration replays the current state to the new subscriber, so the
   capture is cleared afterwards and counts only what happens next. */
static void paSubscribe(ServerSim *sim, PaCapture *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, paCaptureCb, c);
    memset(c, 0, sizeof(*c));
}

/* ── Sims ─────────────────────────────────────────────────────────── */

static ServerSim *paMakeLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    return sim;
}

/* ── Lists ────────────────────────────────────────────────────────── */

/* One sprite primitive: opcode, x, y, tile. Four bytes, and the parser
   range-checks none of them, so this is the shortest list worth sending. */
static uint16_t paSpriteList(uint8_t *out, uint8_t tile) {
    out[0] = SCN_PANEL_OP_SPRITE;
    out[1] = 10;
    out[2] = 11;
    out[3] = tile;
    return 4;
}

/* A list of text primitives filled to exactly SCN_PANEL_MAX bytes: as many
   48-byte lines as fit, then one shorter line landing on the cap. A text
   primitive is opcode, x, y, colour, size, align, len, then len bytes. */
static uint16_t paFullList(uint8_t *out) {
    uint16_t off = 0;
    while (off < SCN_PANEL_MAX) {
        uint16_t room = (uint16_t)(SCN_PANEL_MAX - off);
        uint8_t  textLen = SCN_PANEL_TEXT_MAX;
        if (room < 7) {
            /* No room for another primitive's own bytes. The cap divides
               evenly enough today that this is unreachable; if it ever is
               not, the caller's check that the list came out at the cap is
               what says so. */
            break;
        }
        if (room < 7 + (uint16_t)textLen) {
            textLen = (uint8_t)(room - 7);
        }
        out[off++] = SCN_PANEL_OP_TEXT;
        out[off++] = 4;                        /* x */
        out[off++] = 4;                        /* y */
        out[off++] = SCN_PANEL_COLOUR_WHITE;
        out[off++] = SCN_PANEL_SIZE_SMALL;
        out[off++] = SCN_PANEL_ALIGN_LEFT;
        out[off++] = textLen;
        memset(out + off, 'A', textLen);
        off = (uint16_t)(off + textLen);
    }
    return off;
}

/* ── Ops ──────────────────────────────────────────────────────────── */

static ScnOpResult paPanel(ServerSim *sim, BYTE target, BYTE panel,
                           const uint8_t *bytes, uint16_t len) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PANEL;
    op.u.panel.target = target;
    op.u.panel.panel  = panel;
    op.u.panel.len    = len;
    if (len > 0 && len <= SCN_PANEL_MAX) {
        memcpy(op.u.panel.bytes, bytes, len);
    }
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult paScore(ServerSim *sim, BYTE kind, BYTE target,
                           int32_t score, const char *label) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SCORE;
    op.u.score.kind   = kind;
    op.u.score.target = target;
    op.u.score.score  = score;
    SDL_strlcpy(op.u.score.label, label, sizeof(op.u.score.label));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult paAnnounce(ServerSim *sim, BYTE target, uint16_t ticks,
                              const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ANNOUNCE;
    op.u.announce.target = target;
    op.u.announce.ticks  = ticks;
    SDL_strlcpy(op.u.announce.text, text, sizeof(op.u.announce.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult paMarker(ServerSim *sim, BYTE target, BYTE id, BYTE kind,
                            BYTE x, BYTE y, BYTE slot, BYTE colour) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MARKER;
    op.u.marker.target = target;
    op.u.marker.id     = id;
    op.u.marker.kind   = kind;
    op.u.marker.x      = x;
    op.u.marker.y      = y;
    op.u.marker.slot   = slot;
    op.u.marker.colour = colour;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ── What a recording holds ───────────────────────────────────────── */

#define PA_MAX_HITS 6
/* The widest record here is a panel at the cap: three destination bytes,
   the two-byte length and the list. */
#define PA_MAX_PAYLOAD (5 + SCN_PANEL_MAX)

typedef struct {
    int     count;
    uint8_t payload[PA_MAX_HITS][PA_MAX_PAYLOAD];
    int     payloadLen[PA_MAX_HITS];
} PaLogHits;

static int paReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills,
   bases and starts, the map runs up to the deep-sea terminator, then
   MAX_TANKS player blocks. Plaintext, not length-framed. */
static bool paSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    for (i = 0; i < 3; i++) {
        if ((n = paReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = paReadByte(buf, len, p);
        y    = paReadByte(buf, len, p + 1);
        sx   = paReadByte(buf, len, p + 2);
        ex   = paReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = paReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every record of type `want`, in
   the order they were written. False if the stream did not end on a clean
   LOG_QUIT. */
static bool paFindLogged(const char *path, uint8_t want, PaLogHits *hits) {
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
        int code = paReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (paReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!paSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = paReadByte(buf, len, pos);
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
                if (ev == want && hits->count < PA_MAX_HITS) {
                    int slot = hits->count;
                    int copy = plen;
                    if (copy > PA_MAX_PAYLOAD) copy = PA_MAX_PAYLOAD;
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

/* ================================================================
 * 1. A list through the funnel: one event, one record, the target
 *    unpacked into the pair both of them carry.
 * ================================================================ */
int run_scn_arm_panel_publishes_and_records(void) {
    ReplayHarness h;
    ServerSim    *sim;
    PaCapture     cap;
    PaLogHits     hits;
    uint8_t       sprite[8];
    uint8_t       list[SCN_PANEL_MAX];
    uint16_t      listLen;
    uint16_t      fullLen;
    int           bad = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnPanelArm", "Panelist"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);
    paSubscribe(sim, &cap);

    /* A list held to one player. The target byte packs a 0-based slot, so
       what comes out is that same slot and not the byte it arrived in. */
    listLen = paSpriteList(sprite, 32);
    UT_ASSERT(paPanel(sim, PA_TARGET_PLAYER(PA_SLOT_OTHER), 0, sprite,
                      listLen) == SCN_OP_OK);
    UT_ASSERT_MSG(cap.panelCount == 1,
                  "the arm published %d CTRL_SCN_PANEL, expected 1",
                  cap.panelCount);
    UT_ASSERT(cap.lastPanel.u.scnPanel.panel == 0);
    UT_ASSERT(cap.lastPanel.u.scnPanel.len == listLen);
    UT_ASSERT(memcmp(cap.lastPanel.u.scnPanel.bytes, sprite, listLen) == 0);
    UT_ASSERT_MSG(cap.lastPanel.u.scnPanel.destTeam == 0,
                  "a player target set destTeam to %u",
                  (unsigned)cap.lastPanel.u.scnPanel.destTeam);
    UT_ASSERT_MSG(cap.lastPanel.u.scnPanel.destPlayer == PA_SLOT_OTHER,
                  "a target naming slot %d unpacked to slot %u",
                  PA_SLOT_OTHER,
                  (unsigned)cap.lastPanel.u.scnPanel.destPlayer);

    /* A list held to one team. Team numbers are 1-based and travel as
       themselves. */
    replayHarnessTick(&h, 2);
    UT_ASSERT(paPanel(sim, PA_TEAM, 0, sprite, listLen) == SCN_OP_OK);
    UT_ASSERT(cap.panelCount == 2);
    UT_ASSERT_MSG(cap.lastPanel.u.scnPanel.destTeam == PA_TEAM,
                  "a target naming team %d unpacked to team %u", PA_TEAM,
                  (unsigned)cap.lastPanel.u.scnPanel.destTeam);
    UT_ASSERT(cap.lastPanel.u.scnPanel.destPlayer == 0xFF);

    /* A list for everyone, at the cap: the longest record the format
       carries goes down the same path as the shortest. */
    replayHarnessTick(&h, 2);
    fullLen = paFullList(list);
    UT_ASSERT_MSG(fullLen == SCN_PANEL_MAX,
                  "the filled list is %u bytes, expected %u",
                  (unsigned)fullLen, (unsigned)SCN_PANEL_MAX);
    UT_ASSERT(paPanel(sim, 0, 0, list, fullLen) == SCN_OP_OK);
    UT_ASSERT(cap.panelCount == 3);
    UT_ASSERT(cap.lastPanel.u.scnPanel.destTeam == 0);
    UT_ASSERT(cap.lastPanel.u.scnPanel.destPlayer == 0xFF);
    UT_ASSERT(cap.lastPanel.u.scnPanel.len == SCN_PANEL_MAX);

    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    UT_ASSERT_MSG(paFindLogged(h.path, (uint8_t)log_ScnPanel, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count == 3,
                  "the recording holds %d log_ScnPanel record(s), expected 3",
                  hits.count);

    /* The player-held list: panel id, the destination pair, the length as a
       big-endian u16, then the bytes. */
    UT_ASSERT(hits.payloadLen[0] == 5 + (int)listLen);
    UT_ASSERT(hits.payload[0][0] == 0);
    UT_ASSERT(hits.payload[0][1] == 0);
    UT_ASSERT_MSG(hits.payload[0][2] == PA_SLOT_OTHER,
                  "the record names slot %u, expected %d",
                  (unsigned)hits.payload[0][2], PA_SLOT_OTHER);
    UT_ASSERT(((hits.payload[0][3] << 8) | hits.payload[0][4]) == listLen);
    UT_ASSERT_MSG(memcmp(hits.payload[0] + 5, sprite, listLen) == 0,
                  "the recorded list is not the one published");

    UT_ASSERT(hits.payload[1][1] == PA_TEAM);
    UT_ASSERT(hits.payload[1][2] == 0xFF);

    /* And the one at the cap, whole. */
    UT_ASSERT_MSG(hits.payloadLen[2] == 5 + SCN_PANEL_MAX,
                  "the full list recorded %d payload bytes, expected %d",
                  hits.payloadLen[2], 5 + SCN_PANEL_MAX);
    UT_ASSERT(hits.payload[2][0] == 0);
    UT_ASSERT(((hits.payload[2][3] << 8) | hits.payload[2][4]) ==
              SCN_PANEL_MAX);
    UT_ASSERT_MSG(memcmp(hits.payload[2] + 5, list, SCN_PANEL_MAX) == 0,
                  "the recorded list is not the one published");

    replayHarnessStop(&h);
    return bad;
}

/* ================================================================
 * 2. Every refusal the panel arm can give.
 * ================================================================ */
int run_scn_arm_panel_refusals(void) {
    ServerSim *sim = paMakeLobbySim();
    uint8_t    list[SCN_PANEL_MAX];
    uint16_t   listLen;

    UT_ASSERT(sim != NULL);
    listLen = paSpriteList(list, 32);

    /* Panel ids run 0..SCN_PANEL_IDS-1. */
    UT_ASSERT_MSG(paPanel(sim, 0, SCN_PANEL_IDS, list, listLen) ==
                  SCN_OP_RANGE, "a panel id past the last one was taken");

    /* Target bytes: MAX_TANKS..0x7F name neither everyone, a team nor a
       player, and 0x80 | slot only reaches slot MAX_TANKS-1. */
    UT_ASSERT_MSG(paPanel(sim, MAX_TANKS, 0, list, listLen) == SCN_OP_RANGE,
                  "a target past the last team was taken");
    UT_ASSERT_MSG(paPanel(sim, 0x7F, 0, list, listLen) == SCN_OP_RANGE,
                  "a target naming nothing was taken");
    UT_ASSERT_MSG(paPanel(sim, (BYTE)(0x80 | MAX_TANKS), 0, list, listLen) ==
                  SCN_OP_RANGE, "a target past the last slot was taken");

    /* A list longer than one control segment carries. */
    UT_ASSERT_MSG(paPanel(sim, 0, 0, list, SCN_PANEL_MAX + 1) ==
                  SCN_OP_TOO_BIG, "a list past the cap was taken");

    /* And a list the shared parser refuses: opcode 0 is not a primitive. */
    list[0] = 0;
    UT_ASSERT_MSG(paPanel(sim, 0, 0, list, listLen) == SCN_OP_RANGE,
                  "a malformed list was taken");

    /* An empty list is the clear, and is not a refusal. */
    UT_ASSERT(paPanel(sim, 0, 0, NULL, 0) == SCN_OP_OK);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. One update per panel per target per tick.
 * ================================================================ */
int run_scn_arm_panel_one_update_per_tick(void) {
    ServerSim *sim = paMakeLobbySim();
    uint8_t    list[SCN_PANEL_MAX];
    uint16_t   listLen;

    UT_ASSERT(sim != NULL);
    listLen = paSpriteList(list, 32);

    /* Two updates for the same panel and the same target in one tick: the
       second is refused. */
    UT_ASSERT(paPanel(sim, 0, 0, list, listLen) == SCN_OP_OK);
    UT_ASSERT_MSG(paPanel(sim, 0, 0, list, listLen) == SCN_OP_RATE,
                  "a second update for one pair in one tick was taken");

    /* The key is the pair and not the panel alone: a different target in the
       same tick is a different key and goes through. With one panel id the
       target is the whole of what separates two keys, which is the half that
       matters — a scenario giving each of sixteen players its own copy of the
       panel in one tick is ordinary. */
    UT_ASSERT_MSG(paPanel(sim, PA_TARGET_PLAYER(PA_SLOT_HOST), 0, list,
                          listLen) == SCN_OP_OK,
                  "a second target in one tick was refused");
    UT_ASSERT_MSG(paPanel(sim, PA_TEAM, 0, list, listLen) == SCN_OP_OK,
                  "a team target in one tick was refused");

    /* And each of those is itself one per tick. */
    UT_ASSERT(paPanel(sim, PA_TEAM, 0, list, listLen) == SCN_OP_RATE);

    /* The next tick admits another. */
    sim->tick++;
    UT_ASSERT_MSG(paPanel(sim, 0, 0, list, listLen) == SCN_OP_OK,
                  "the next tick refused an update");
    UT_ASSERT(paPanel(sim, PA_TEAM, 0, list, listLen) == SCN_OP_OK);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A subscriber registering after the op is given the stored list.
 * ================================================================ */
int run_scn_arm_panel_replayed_to_joiner(void) {
    ServerSim          *sim = paMakeLobbySim();
    ClientSim          *cs;
    SubscriberHandle    handle;
    const ScnPanelList *held;
    uint8_t             list[SCN_PANEL_MAX];
    uint8_t             other[SCN_PANEL_MAX];
    uint16_t            listLen;
    uint16_t            otherLen;

    UT_ASSERT(sim != NULL);
    listLen  = paSpriteList(list, 32);
    otherLen = paSpriteList(other, 99);

    /* The same panel addressed two ways: one list for everyone, and one held
       to a slot the joiner is not in. The two carry different sprites, so
       which of them the joiner ends up holding says whether the filter ran. */
    UT_ASSERT(paPanel(sim, 0, 0, list, listLen) == SCN_OP_OK);
    UT_ASSERT(paPanel(sim, PA_TARGET_PLAYER(PA_SLOT_OTHER), 0, other,
                      otherLen) == SCN_OP_OK);

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, PA_SLOT_HOST);
    cs->lobbySlots[PA_SLOT_HOST].connected  = true;
    cs->lobbySlots[PA_SLOT_HOST].teamNumber = PA_TEAM;

    /* The replay runs inside the registration, before it returns, so the
       list is on the client by the time this call has answered. */
    handle = serverSimRegisterClientSubscriber(sim, cs);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the client could not register");

    held = clientSimGetScnPanel(cs, 0);
    UT_ASSERT_MSG(held != NULL, "the joiner was not given panel 0's list");
    UT_ASSERT_MSG(held->count == 1,
                  "the joiner's panel 0 holds %u primitive(s), expected 1",
                  (unsigned)held->count);
    UT_ASSERT(held->items[0].op == SCN_PANEL_OP_SPRITE);

    /* The list held to another slot is filtered on the way in, exactly as a
       live publish is, so what the joiner holds is the everyone-addressed
       one and not the sprite the other slot's list carried. */
    UT_ASSERT_MSG(held->items[0].u.sprite.tile == 32,
                  "the joiner's panel holds tile %u — 99 means the list "
                  "addressed to slot %d reached it",
                  (unsigned)held->items[0].u.sprite.tile, PA_SLOT_OTHER);

    serverSimUnregisterSubscriber(sim, handle);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. The delayed spectator ring's control snapshot takes each panel's
 *    everyone-addressed list and none of the rest.
 *
 *    A spectator belongs to no team and holds no slot, so a targeted list
 *    reaches nobody down that path; serialising one would spend keyframe
 *    bytes on a record that is dropped at the far end, and enough of them
 *    push the snapshot past LOG_CONTROL_SNAPSHOT_MAX, at which point the
 *    whole keyframe goes without a word.
 * ================================================================ */

/* The tiles the lists are told apart by. The body carries no destination —
 * the recipient pair is a server-side filter and does not travel — so each
 * list names its own destination in the one byte the walk can read back. */
#define PA_TILE_ALL_0    100
#define PA_TILE_TEAM_0   101
#define PA_TILE_PLAYER_0 102

/* One CTRL_SCN_PANEL record the snapshot holds. */
typedef struct {
    uint8_t panel;
    uint8_t tile;
} PaSnapPanel;

#define PA_MAX_SNAP_PANELS 8

/* Walk the snapshot's [u16 type][u16 bodyLen][body] records and collect
 * every panel one, in order. Returns the count, or -1 if the framing does
 * not land exactly on the end of the buffer. The panel body is
 * [panel 1][len 2 BE][bytes], and each list here is one sprite primitive:
 * opcode, x, y, tile. */
static int paSnapshotPanels(const uint8_t *snap, int snapLen,
                            PaSnapPanel *out, int outCap) {
    int pos = 0;
    int found = 0;

    while (pos < snapLen) {
        uint16_t type;
        uint16_t bodyLen;
        const uint8_t *body;

        if (pos + 4 > snapLen) return -1;
        type    = (uint16_t)((snap[pos] << 8) | snap[pos + 1]);
        bodyLen = (uint16_t)((snap[pos + 2] << 8) | snap[pos + 3]);
        body    = snap + pos + 4;
        if (pos + 4 + (int)bodyLen > snapLen) return -1;
        if (type == (uint16_t)CTRL_SCN_PANEL) {
            if (bodyLen < 3 + 4) return -1;
            if (found < outCap) {
                out[found].panel = body[0];
                out[found].tile  = body[3 + 3];
            }
            found++;
        }
        pos += 4 + (int)bodyLen;
    }
    return found;
}

int run_scn_arm_panel_snapshot_bounded(void) {
    ServerSim  *sim = paMakeLobbySim();
    uint8_t    *snap;
    uint8_t     list[SCN_PANEL_MAX];
    uint16_t    listLen;
    PaSnapPanel held[PA_MAX_SNAP_PANELS];
    int         snapLen;
    int         count;

    UT_ASSERT(sim != NULL);

    /* Panel 0 addressed three ways at once, which is what a scenario giving
       each player its own copy looks like. */
    listLen = paSpriteList(list, PA_TILE_ALL_0);
    UT_ASSERT(paPanel(sim, 0, 0, list, listLen) == SCN_OP_OK);
    listLen = paSpriteList(list, PA_TILE_TEAM_0);
    UT_ASSERT(paPanel(sim, PA_TEAM, 0, list, listLen) == SCN_OP_OK);
    listLen = paSpriteList(list, PA_TILE_PLAYER_0);
    UT_ASSERT(paPanel(sim, PA_TARGET_PLAYER(PA_SLOT_OTHER), 0, list,
                      listLen) == SCN_OP_OK);

    snap = (uint8_t *)malloc(LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT(snap != NULL);
    snapLen = serverSimSerializeControlSnapshot(sim, snap,
                                                LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT_MSG(snapLen > 0,
                  "the control snapshot did not fit %d bytes, so the ring "
                  "drops the keyframe", LOG_CONTROL_SNAPSHOT_MAX);

    count = paSnapshotPanels(snap, snapLen, held, PA_MAX_SNAP_PANELS);
    UT_ASSERT_MSG(count >= 0, "the snapshot's record framing does not close");
    UT_ASSERT_MSG(count == 1,
                  "the snapshot holds %d panel record(s), expected 1 — the "
                  "everyone-addressed list and neither of the two held to a "
                  "team or a slot", count);
    UT_ASSERT_MSG(held[0].panel == 0 && held[0].tile == PA_TILE_ALL_0,
                  "the panel record is panel %u tile %u, so a targeted list "
                  "reached the ring's keyframe",
                  (unsigned)held[0].panel, (unsigned)held[0].tile);

    free(snap);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. The score, the announcement and the marker: what each publishes,
 *    what each records, and what each refuses.
 * ================================================================ */

/* The refusals, on a lobby sim of their own so a refused op writes nothing
 * into the recording the successes are checked against. */
static int paOtherRefusals(void) {
    ServerSim *sim = paMakeLobbySim();
    PaCapture  cap;
    ScenarioOp op;

    UT_ASSERT(sim != NULL);
    /* A second seat, so a slot that is not zero can be named and the base
       the arms read a slot at is visible. PA_SLOT_EMPTY stays empty. */
    serverSimAddPlayer(sim, PA_SLOT_OTHER, "Other", false);
    paSubscribe(sim, &cap);

    /* ── score ── */
    UT_ASSERT_MSG(paScore(sim, 2, PA_SLOT_HOST, 1, "x") == SCN_OP_RANGE,
                  "a score kind that is neither player nor team was taken");
    /* Player kind: a 0-based slot with somebody in it. */
    UT_ASSERT_MSG(paScore(sim, SCN_SCORE_KIND_PLAYER, PA_SLOT_EMPTY, 1, "x") ==
                  SCN_OP_NO_SUCH_PLAYER, "a score against an empty seat was taken");
    /* And the row the arm writes is the one the slot names, not its
       neighbour: slot PA_SLOT_OTHER is read as itself. */
    UT_ASSERT(paScore(sim, SCN_SCORE_KIND_PLAYER, PA_SLOT_OTHER, 9, "Row") ==
              SCN_OP_OK);
    UT_ASSERT_MSG(cap.lastScore.u.scnScore.target == PA_SLOT_OTHER,
                  "the row names slot %u, expected %d",
                  (unsigned)cap.lastScore.u.scnScore.target, PA_SLOT_OTHER);
    UT_ASSERT_MSG(sim->scenarioPlayerScores[PA_SLOT_OTHER].score == 9,
                  "slot %d's row did not take the score", PA_SLOT_OTHER);
    UT_ASSERT_MSG(!sim->scenarioPlayerScores[PA_SLOT_OTHER + 1].valid,
                  "the row after slot %d was written too", PA_SLOT_OTHER);
    UT_ASSERT_MSG(paScore(sim, SCN_SCORE_KIND_PLAYER, MAX_TANKS, 1, "x") ==
                  SCN_OP_NO_SUCH_PLAYER, "a score past the last slot was taken");
    /* Team kind: 1..MAX_TANKS-1. */
    UT_ASSERT_MSG(paScore(sim, SCN_SCORE_KIND_TEAM, 0, 1, "x") == SCN_OP_RANGE,
                  "a score against team 0 was taken");
    UT_ASSERT_MSG(paScore(sim, SCN_SCORE_KIND_TEAM, MAX_TANKS, 1, "x") ==
                  SCN_OP_RANGE, "a score past the last team was taken");
    /* A label with no terminator inside its sixteen bytes. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SCORE;
    op.u.score.kind   = SCN_SCORE_KIND_PLAYER;
    op.u.score.target = PA_SLOT_HOST;
    memset(op.u.score.label, 'L', sizeof(op.u.score.label));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated label was taken");

    /* ── announce ── */
    UT_ASSERT_MSG(paAnnounce(sim, (BYTE)(0x80 | MAX_TANKS), 10, "hi") ==
                  SCN_OP_RANGE, "an announcement to nobody was taken");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ANNOUNCE;
    op.u.announce.ticks = 10;
    memset(op.u.announce.text, 'x', sizeof(op.u.announce.text));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "an unterminated announcement was taken");
    UT_ASSERT_MSG(paAnnounce(sim, 0, 0, "hi") == SCN_OP_RANGE,
                  "a line that is up for no time was taken");
    /* An empty line is the clear, and its ticks are not read. */
    UT_ASSERT_MSG(paAnnounce(sim, 0, 0, "") == SCN_OP_OK,
                  "the clear was refused");

    /* ── marker ── */
    UT_ASSERT_MSG(paMarker(sim, (BYTE)(0x80 | MAX_TANKS), 0,
                           SCN_MARKER_KIND_CLEAR, 0, 0, 0, 0) == SCN_OP_RANGE,
                  "a marker for nobody was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, SCN_MARKERS_MAX, SCN_MARKER_KIND_CLEAR,
                           0, 0, 0, 0) == SCN_OP_RANGE,
                  "a marker id past the last one was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, 0, 3, PA_SQUARE_X, PA_SQUARE_Y, 0,
                           SCN_PANEL_COLOUR_RED) == SCN_OP_RANGE,
                  "a marker kind outside the three was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, 0, SCN_MARKER_KIND_SQUARE, PA_SQUARE_X,
                           PA_SQUARE_Y, 0, SCN_PANEL_COLOURS) == SCN_OP_RANGE,
                  "a colour past the palette was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, 0, SCN_MARKER_KIND_SQUARE, PA_EDGE_X,
                           PA_EDGE_Y, 0, SCN_PANEL_COLOUR_RED) ==
                  SCN_OP_BAD_SQUARE, "a marker off the map was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, 0, SCN_MARKER_KIND_FOLLOW, 0, 0,
                           PA_SLOT_EMPTY, SCN_PANEL_COLOUR_RED) ==
                  SCN_OP_NO_SUCH_PLAYER,
                  "a marker following an empty seat was taken");
    UT_ASSERT_MSG(paMarker(sim, 0, 0, SCN_MARKER_KIND_FOLLOW, 0, 0, MAX_TANKS,
                           SCN_PANEL_COLOUR_RED) == SCN_OP_NO_SUCH_PLAYER,
                  "a marker following a slot past the last one was taken");
    /* And one that does name a filled seat rides that slot, read at the same
       base the refusal above read it at. */
    UT_ASSERT(paMarker(sim, 0, 1, SCN_MARKER_KIND_FOLLOW, 0, 0, PA_SLOT_OTHER,
                       SCN_PANEL_COLOUR_RED) == SCN_OP_OK);
    UT_ASSERT_MSG(cap.lastMarker.u.scnMarker.slot == PA_SLOT_OTHER,
                  "the marker follows slot %u, expected %d",
                  (unsigned)cap.lastMarker.u.scnMarker.slot, PA_SLOT_OTHER);
    /* The clear reads none of the fields that place a marker, so values it
       would refuse under another kind do not reach it. */
    UT_ASSERT_MSG(paMarker(sim, 0, 0, SCN_MARKER_KIND_CLEAR, PA_EDGE_X,
                           PA_EDGE_Y, MAX_TANKS, SCN_PANEL_COLOURS) ==
                  SCN_OP_OK, "the clear read a field it does not use");

    serverSimDestroy(sim);
    return 0;
}

int run_scn_arm_score_announce_marker(void) {
    ReplayHarness h;
    ServerSim    *sim;
    PaCapture     cap;
    PaLogHits     hits;
    int           bad;

    bad = paOtherRefusals();
    if (bad != 0) return bad;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnPresArms", "Panelist"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);
    paSubscribe(sim, &cap);

    /* ── score ── a team row. Broadcast: no destination pair. */
    UT_ASSERT(paScore(sim, SCN_SCORE_KIND_TEAM, PA_TEAM, -2, "Waves") ==
              SCN_OP_OK);
    UT_ASSERT(cap.scoreCount == 1);
    UT_ASSERT(cap.lastScore.u.scnScore.kind == SCN_SCORE_KIND_TEAM);
    UT_ASSERT_MSG(cap.lastScore.u.scnScore.target == PA_TEAM,
                  "the row names team %u, expected %d",
                  (unsigned)cap.lastScore.u.scnScore.target, PA_TEAM);
    UT_ASSERT(cap.lastScore.u.scnScore.score == -2);
    UT_ASSERT(strcmp(cap.lastScore.u.scnScore.label, "Waves") == 0);
    /* And it is on the sim, under the team number it was set for. */
    UT_ASSERT(sim->scenarioTeamScores[PA_TEAM].valid);
    UT_ASSERT(sim->scenarioTeamScores[PA_TEAM].score == -2);
    UT_ASSERT(strcmp(sim->scenarioTeamScores[PA_TEAM].label, "Waves") == 0);

    /* A player row, against the 0-based slot the harness filled. */
    UT_ASSERT(paScore(sim, SCN_SCORE_KIND_PLAYER, PA_SLOT_HOST, 41, "Kills") ==
              SCN_OP_OK);
    UT_ASSERT(cap.scoreCount == 2);
    UT_ASSERT_MSG(cap.lastScore.u.scnScore.target == PA_SLOT_HOST,
                  "the row names slot %u, expected %d",
                  (unsigned)cap.lastScore.u.scnScore.target, PA_SLOT_HOST);
    UT_ASSERT(sim->scenarioPlayerScores[PA_SLOT_HOST].valid);
    UT_ASSERT(sim->scenarioPlayerScores[PA_SLOT_HOST].score == 41);

    /* ── announce ── held to one team. */
    UT_ASSERT(paAnnounce(sim, PA_TEAM, 300, "Go!") == SCN_OP_OK);
    UT_ASSERT(cap.announceCount == 1);
    UT_ASSERT(strcmp(cap.lastAnnounce.u.scnAnnounce.text, "Go!") == 0);
    UT_ASSERT(cap.lastAnnounce.u.scnAnnounce.ticks == 300);
    UT_ASSERT_MSG(cap.lastAnnounce.u.scnAnnounce.destTeam == PA_TEAM,
                  "the line went to team %u, expected %d",
                  (unsigned)cap.lastAnnounce.u.scnAnnounce.destTeam, PA_TEAM);
    UT_ASSERT(cap.lastAnnounce.u.scnAnnounce.destPlayer == 0xFF);

    /* ── marker ── a square, for everyone. */
    UT_ASSERT(paMarker(sim, 0, 7, SCN_MARKER_KIND_SQUARE, PA_SQUARE_X,
                       PA_SQUARE_Y, 0, SCN_PANEL_COLOUR_RED) == SCN_OP_OK);
    UT_ASSERT(cap.markerCount == 1);
    UT_ASSERT(cap.lastMarker.u.scnMarker.id == 7);
    UT_ASSERT(cap.lastMarker.u.scnMarker.kind == SCN_MARKER_KIND_SQUARE);
    UT_ASSERT(cap.lastMarker.u.scnMarker.x == PA_SQUARE_X);
    UT_ASSERT(cap.lastMarker.u.scnMarker.y == PA_SQUARE_Y);
    UT_ASSERT(cap.lastMarker.u.scnMarker.colour == SCN_PANEL_COLOUR_RED);
    UT_ASSERT(cap.lastMarker.u.scnMarker.destTeam == 0);
    UT_ASSERT(cap.lastMarker.u.scnMarker.destPlayer == 0xFF);

    /* One following the 0-based slot the harness filled, held to a different
       slot: the slot a marker rides and the slot it is sent to are two
       fields, and each is read at its own base. */
    UT_ASSERT(paMarker(sim, PA_TARGET_PLAYER(PA_SLOT_OTHER), 8,
                       SCN_MARKER_KIND_FOLLOW, 0, 0, PA_SLOT_HOST,
                       SCN_PANEL_COLOUR_GREEN) == SCN_OP_OK);
    UT_ASSERT(cap.markerCount == 2);
    UT_ASSERT_MSG(cap.lastMarker.u.scnMarker.slot == PA_SLOT_HOST,
                  "the marker follows slot %u, expected %d",
                  (unsigned)cap.lastMarker.u.scnMarker.slot, PA_SLOT_HOST);
    UT_ASSERT_MSG(cap.lastMarker.u.scnMarker.destPlayer == PA_SLOT_OTHER,
                  "the marker went to slot %u, expected %d",
                  (unsigned)cap.lastMarker.u.scnMarker.destPlayer,
                  PA_SLOT_OTHER);

    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* ── the records ── */
    UT_ASSERT_MSG(paFindLogged(h.path, (uint8_t)log_ScnScore, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count == 2,
                  "the recording holds %d log_ScnScore record(s), expected 2",
                  hits.count);
    /* kind, target, the score as a big-endian int32, then a pascal label. */
    UT_ASSERT(hits.payloadLen[0] == 7 + 5);
    UT_ASSERT(hits.payload[0][0] == SCN_SCORE_KIND_TEAM);
    UT_ASSERT(hits.payload[0][1] == PA_TEAM);
    UT_ASSERT(hits.payload[0][2] == 0xFF && hits.payload[0][3] == 0xFF &&
              hits.payload[0][4] == 0xFF && hits.payload[0][5] == 0xFE);
    UT_ASSERT(hits.payload[0][6] == 5);
    UT_ASSERT(memcmp(hits.payload[0] + 7, "Waves", 5) == 0);
    UT_ASSERT(hits.payload[1][1] == PA_SLOT_HOST);
    UT_ASSERT(hits.payload[1][5] == 41);

    UT_ASSERT(paFindLogged(h.path, (uint8_t)log_ScnAnnounce, &hits));
    UT_ASSERT_MSG(hits.count == 1,
                  "the recording holds %d log_ScnAnnounce record(s), expected 1",
                  hits.count);
    UT_ASSERT(hits.payloadLen[0] == 5 + 3);
    UT_ASSERT(hits.payload[0][0] == PA_TEAM);
    UT_ASSERT(hits.payload[0][1] == 0xFF);
    UT_ASSERT(((hits.payload[0][2] << 8) | hits.payload[0][3]) == 300);
    UT_ASSERT(hits.payload[0][4] == 3);
    UT_ASSERT(memcmp(hits.payload[0] + 5, "Go!", 3) == 0);

    UT_ASSERT(paFindLogged(h.path, (uint8_t)log_ScnMarker, &hits));
    UT_ASSERT_MSG(hits.count == 2,
                  "the recording holds %d log_ScnMarker record(s), expected 2",
                  hits.count);
    UT_ASSERT(hits.payloadLen[0] == 5 + 4);
    UT_ASSERT(hits.payload[0][0] == 7);
    UT_ASSERT(hits.payload[0][1] == SCN_MARKER_KIND_SQUARE);
    UT_ASSERT(hits.payload[0][2] == 0);
    UT_ASSERT(hits.payload[0][3] == 0xFF);
    UT_ASSERT(hits.payload[0][4] == 4);
    UT_ASSERT(hits.payload[0][5] == PA_SQUARE_X);
    UT_ASSERT(hits.payload[0][6] == PA_SQUARE_Y);
    UT_ASSERT(hits.payload[0][8] == SCN_PANEL_COLOUR_RED);
    UT_ASSERT(hits.payload[1][0] == 8);
    UT_ASSERT_MSG(hits.payload[1][3] == PA_SLOT_OTHER,
                  "the recorded marker went to slot %u, expected %d",
                  (unsigned)hits.payload[1][3], PA_SLOT_OTHER);
    UT_ASSERT_MSG(hits.payload[1][7] == PA_SLOT_HOST,
                  "the recorded marker follows slot %u, expected %d",
                  (unsigned)hits.payload[1][7], PA_SLOT_HOST);

    replayHarnessStop(&h);
    return 0;
}

/* ================================================================
 * 7. Markers and scores handed to a subscriber that arrives after them.
 * ================================================================ */

/* The team the host's seat is put on, which the team-held marker names. The
   other seat stays on PA_TEAM, so the one marker reaches one joiner and not
   the other. */
#define PA_JOIN_TEAM 2

/* The marker ids the fixture uses: one for everyone, one held to
   PA_JOIN_TEAM, and one placed and then cleared. */
#define PA_MARKER_ALL     3
#define PA_MARKER_TEAM    5
#define PA_MARKER_CLEARED 7

/* A lobby sim with two filled seats on two teams, and the markers and the
   two score rows the replay cases read. The teams are set on the server's
   lobby rows because the replay's CTRL_LOBBY_SLOT events carry them to the
   joiner, overwriting anything the joiner was given beforehand. */
static ServerSim *paMakeMarkerScoreSim(void) {
    ServerSim *sim = paMakeLobbySim();
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, PA_SLOT_OTHER, "Other", false);
    sim->lobbyPlayers[PA_SLOT_HOST].teamNumber  = PA_JOIN_TEAM;
    sim->lobbyPlayers[PA_SLOT_OTHER].teamNumber = PA_TEAM;

    if (paMarker(sim, 0, PA_MARKER_ALL, SCN_MARKER_KIND_SQUARE, PA_SQUARE_X,
                 PA_SQUARE_Y, 0, SCN_PANEL_COLOUR_RED) != SCN_OP_OK ||
        paMarker(sim, PA_JOIN_TEAM, PA_MARKER_TEAM, SCN_MARKER_KIND_FOLLOW,
                 0, 0, PA_SLOT_HOST, SCN_PANEL_COLOUR_GREEN) != SCN_OP_OK ||
        paMarker(sim, 0, PA_MARKER_CLEARED, SCN_MARKER_KIND_SQUARE,
                 PA_SQUARE_X, PA_SQUARE_Y, 0,
                 SCN_PANEL_COLOUR_RED) != SCN_OP_OK ||
        paMarker(sim, 0, PA_MARKER_CLEARED, SCN_MARKER_KIND_CLEAR,
                 0, 0, 0, 0) != SCN_OP_OK ||
        paScore(sim, SCN_SCORE_KIND_PLAYER, PA_SLOT_OTHER, 12,
                "Kills") != SCN_OP_OK ||
        paScore(sim, SCN_SCORE_KIND_TEAM, PA_JOIN_TEAM, 30,
                "Waves") != SCN_OP_OK) {
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

/* A ClientSim sitting in `slot`, registered on the sim, so the replay has
   run on it by the time this returns. */
static ClientSim *paJoin(ServerSim *sim, BYTE slot, SubscriberHandle *out) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    *out = serverSimRegisterClientSubscriber(sim, cs);
    if (*out == SUBSCRIBER_HANDLE_INVALID) {
        clientSimDestroy(cs);
        return NULL;
    }
    return cs;
}

/* Both score rows the fixture set, as the joiner holds them. */
static int paJoinerHasScores(const ClientSim *cs, const char *who) {
    const ClientScnScore *p = clientSimGetScnPlayerScore(cs, PA_SLOT_OTHER);
    const ClientScnScore *t = clientSimGetScnTeamScore(cs, PA_JOIN_TEAM);
    UT_ASSERT_MSG(p != NULL && p->valid,
                  "%s was not given slot %d's score row", who, PA_SLOT_OTHER);
    UT_ASSERT(p->score == 12);
    UT_ASSERT(strcmp(p->label, "Kills") == 0);
    UT_ASSERT_MSG(t != NULL && t->valid,
                  "%s was not given team %d's score row", who, PA_JOIN_TEAM);
    UT_ASSERT(t->score == 30);
    UT_ASSERT(strcmp(t->label, "Waves") == 0);
    return 0;
}

int run_scn_arm_markers_scores_replayed_to_joiner(void) {
    ServerSim             *sim = paMakeMarkerScoreSim();
    PaCapture              cap;
    SubscriberHandle       capHandle;
    ClientSim             *onTeam;
    ClientSim             *offTeam;
    SubscriberHandle       onHandle;
    SubscriberHandle       offHandle;
    const ClientScnMarker *m;

    UT_ASSERT_MSG(sim != NULL, "the fixture's ops were refused");

    /* What the replay sends, before any filter: the two markers still up
       and the two score rows. The cleared marker is not among them. */
    memset(&cap, 0, sizeof(cap));
    capHandle = serverSimRegisterSubscriber(sim, paCaptureCb, &cap);
    UT_ASSERT(capHandle != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(cap.markerCount == 2,
                  "the replay sent %d CTRL_SCN_MARKER, expected 2 — the "
                  "cleared marker is not replayed", cap.markerCount);
    UT_ASSERT_MSG(cap.scoreCount == 2,
                  "the replay sent %d CTRL_SCN_SCORE, expected 2",
                  cap.scoreCount);
    serverSimUnregisterSubscriber(sim, capHandle);

    /* A joiner on the team the follow marker is held to. */
    onTeam = paJoin(sim, PA_SLOT_HOST, &onHandle);
    UT_ASSERT_MSG(onTeam != NULL, "the first joiner could not register");

    m = clientSimGetScnMarker(onTeam, PA_MARKER_ALL);
    UT_ASSERT_MSG(m != NULL && m->active,
                  "the joiner was not given marker %d", PA_MARKER_ALL);
    UT_ASSERT(m->kind == SCN_MARKER_KIND_SQUARE);
    UT_ASSERT(m->x == PA_SQUARE_X && m->y == PA_SQUARE_Y);
    UT_ASSERT(m->colour == SCN_PANEL_COLOUR_RED);

    m = clientSimGetScnMarker(onTeam, PA_MARKER_TEAM);
    UT_ASSERT_MSG(m != NULL && m->active,
                  "a joiner on team %d was not given marker %d, which is "
                  "held to that team", PA_JOIN_TEAM, PA_MARKER_TEAM);
    UT_ASSERT(m->kind == SCN_MARKER_KIND_FOLLOW);
    UT_ASSERT_MSG(m->slot == PA_SLOT_HOST,
                  "the marker follows slot %u, expected %d",
                  (unsigned)m->slot, PA_SLOT_HOST);
    UT_ASSERT(m->colour == SCN_PANEL_COLOUR_GREEN);

    m = clientSimGetScnMarker(onTeam, PA_MARKER_CLEARED);
    UT_ASSERT_MSG(m != NULL && !m->active,
                  "the joiner holds marker %d, which was cleared",
                  PA_MARKER_CLEARED);
    if (paJoinerHasScores(onTeam, "the joiner on the marker's team") != 0) {
        return 1;
    }

    /* And one on another team: the everyone-addressed marker and the scores
       reach it, the team-held marker is filtered on the way in. */
    offTeam = paJoin(sim, PA_SLOT_OTHER, &offHandle);
    UT_ASSERT_MSG(offTeam != NULL, "the second joiner could not register");

    m = clientSimGetScnMarker(offTeam, PA_MARKER_ALL);
    UT_ASSERT_MSG(m != NULL && m->active,
                  "the second joiner was not given marker %d", PA_MARKER_ALL);
    m = clientSimGetScnMarker(offTeam, PA_MARKER_TEAM);
    UT_ASSERT_MSG(m != NULL && !m->active,
                  "a joiner on team %d holds marker %d, which is held to "
                  "team %d", PA_TEAM, PA_MARKER_TEAM, PA_JOIN_TEAM);
    m = clientSimGetScnMarker(offTeam, PA_MARKER_CLEARED);
    UT_ASSERT(m != NULL && !m->active);
    if (paJoinerHasScores(offTeam, "the joiner on another team") != 0) {
        return 1;
    }

    serverSimUnregisterSubscriber(sim, offHandle);
    serverSimUnregisterSubscriber(sim, onHandle);
    clientSimDestroy(offTeam);
    clientSimDestroy(onTeam);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 8. The ring's control snapshot takes the everyone-addressed markers and
 *    every score row, and leaves the markers held to a team or a slot.
 * ================================================================ */

#define PA_MAX_SNAP_MARKERS 8

/* Walk the snapshot's records and collect every marker id, in order, and
 * count the score records. The marker body is [id][kind][x][y][slot]
 * [colour]. Returns the marker count, or -1 if the framing does not land
 * exactly on the end of the buffer. */
static int paSnapshotMarkers(const uint8_t *snap, int snapLen,
                             uint8_t *ids, int idCap, int *scores) {
    int pos = 0;
    int found = 0;

    *scores = 0;
    while (pos < snapLen) {
        uint16_t type;
        uint16_t bodyLen;

        if (pos + 4 > snapLen) return -1;
        type    = (uint16_t)((snap[pos] << 8) | snap[pos + 1]);
        bodyLen = (uint16_t)((snap[pos + 2] << 8) | snap[pos + 3]);
        if (pos + 4 + (int)bodyLen > snapLen) return -1;
        if (type == (uint16_t)CTRL_SCN_MARKER) {
            if (bodyLen < 1) return -1;
            if (found < idCap) ids[found] = snap[pos + 4];
            found++;
        } else if (type == (uint16_t)CTRL_SCN_SCORE) {
            (*scores)++;
        }
        pos += 4 + (int)bodyLen;
    }
    return found;
}

int run_scn_arm_markers_scores_snapshot(void) {
    ServerSim *sim = paMakeMarkerScoreSim();
    uint8_t   *snap;
    uint8_t    ids[PA_MAX_SNAP_MARKERS];
    int        snapLen;
    int        count;
    int        scores;

    UT_ASSERT_MSG(sim != NULL, "the fixture's ops were refused");

    snap = (uint8_t *)malloc(LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT(snap != NULL);
    snapLen = serverSimSerializeControlSnapshot(sim, snap,
                                                LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT_MSG(snapLen > 0,
                  "the control snapshot did not fit %d bytes, so the ring "
                  "drops the keyframe", LOG_CONTROL_SNAPSHOT_MAX);

    count = paSnapshotMarkers(snap, snapLen, ids, PA_MAX_SNAP_MARKERS,
                              &scores);
    UT_ASSERT_MSG(count >= 0, "the snapshot's record framing does not close");
    UT_ASSERT_MSG(count == 1,
                  "the snapshot holds %d marker record(s), expected 1 — the "
                  "everyone-addressed one and not the one held to a team",
                  count);
    UT_ASSERT_MSG(ids[0] == PA_MARKER_ALL,
                  "the snapshot's marker is id %u, expected %d",
                  (unsigned)ids[0], PA_MARKER_ALL);
    UT_ASSERT_MSG(scores == 2,
                  "the snapshot holds %d score record(s), expected 2 — "
                  "scores are broadcast and go in either way", scores);

    free(snap);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 9. The presentation reset drops the markers and the scores, so a joiner
 *    after it is given none of them.
 * ================================================================ */
int run_scn_arm_markers_scores_reset(void) {
    ServerSim        *sim = paMakeMarkerScoreSim();
    PaCapture         cap;
    SubscriberHandle  handle;
    int               i;

    UT_ASSERT_MSG(sim != NULL, "the fixture's ops were refused");

    serverSimScenarioResetPresentation(sim);
    for (i = 0; i < SCN_MARKERS_MAX; i++) {
        UT_ASSERT_MSG(!sim->scenarioMarkers[i].valid,
                      "marker %d survived the reset", i);
    }

    memset(&cap, 0, sizeof(cap));
    handle = serverSimRegisterSubscriber(sim, paCaptureCb, &cap);
    UT_ASSERT(handle != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(cap.markerCount == 0,
                  "a joiner after the reset was sent %d CTRL_SCN_MARKER",
                  cap.markerCount);
    UT_ASSERT_MSG(cap.scoreCount == 0,
                  "a joiner after the reset was sent %d CTRL_SCN_SCORE",
                  cap.scoreCount);

    serverSimUnregisterSubscriber(sim, handle);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 10. Over the real transport: a second client joining a running round
 *     after the markers and scores were set ends up holding them.
 * ================================================================ */

#define PA_LB_CONNECT_MAX 2000   /* join + map download                   */
#define PA_LB_SETTLE_MAX   200   /* a few running ticks after the download */
#define PA_LB_SYNC_MAX     600   /* the replay crossing to the joiner      */

/* The team the loopback score row is set for. Any team number takes a
   score; it need not have anybody on it. */
#define PA_LB_TEAM 1

static bool paLbConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool paLbJoinerConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* True once the joiner holds the marker and both score rows. `user` is the
   slot the player row was set for. */
static bool paLbJoinerHasAll(LoopbackHarness *h, void *user) {
    BYTE slot = *(const BYTE *)user;
    const ClientScnMarker *m;
    const ClientScnScore  *p;
    const ClientScnScore  *t;
    if (h->cs2 == NULL) return false;
    m = clientSimGetScnMarker(h->cs2, PA_MARKER_ALL);
    p = clientSimGetScnPlayerScore(h->cs2, slot);
    t = clientSimGetScnTeamScore(h->cs2, PA_LB_TEAM);
    return m != NULL && m->active && p != NULL && p->valid &&
           t != NULL && t->valid;
}

int run_scn_markers_scores_loopback_late_join(void) {
    LoopbackHarness        h;
    const ClientScnMarker *m;
    const ClientScnScore  *p;
    const ClientScnScore  *t;
    BYTE                   firstSlot = MAX_TANKS;
    BYTE                   i;
    int                    at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "MarkFirst", false, NULL, 90212),
                  "loopback start failed");
    at = loopbackHarnessPumpUntil(&h, PA_LB_CONNECT_MAX, paLbConnected, NULL);
    UT_ASSERT_MSG(at > 0, "the first client never connected");
    loopbackHarnessPumpUntil(&h, PA_LB_SETTLE_MAX, NULL, NULL);

    /* The first client's seat, which the player score row is set against:
       a player row has to name a filled seat. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (h.sim->playerConnected[i]) {
            firstSlot = i;
            break;
        }
    }
    UT_ASSERT_MSG(firstSlot < MAX_TANKS, "the first client holds no seat");

    /* A marker for everyone, one placed and cleared, and a score row of each
       kind, all before anyone else is here to be told about them. */
    UT_ASSERT(paMarker(h.sim, 0, PA_MARKER_ALL, SCN_MARKER_KIND_SQUARE,
                       PA_SQUARE_X, PA_SQUARE_Y, 0,
                       SCN_PANEL_COLOUR_RED) == SCN_OP_OK);
    UT_ASSERT(paMarker(h.sim, 0, PA_MARKER_CLEARED, SCN_MARKER_KIND_SQUARE,
                       PA_SQUARE_X, PA_SQUARE_Y, 0,
                       SCN_PANEL_COLOUR_RED) == SCN_OP_OK);
    UT_ASSERT(paMarker(h.sim, 0, PA_MARKER_CLEARED, SCN_MARKER_KIND_CLEAR,
                       0, 0, 0, 0) == SCN_OP_OK);
    UT_ASSERT(paScore(h.sim, SCN_SCORE_KIND_PLAYER, firstSlot, 12,
                      "Kills") == SCN_OP_OK);
    UT_ASSERT(paScore(h.sim, SCN_SCORE_KIND_TEAM, PA_LB_TEAM, 30,
                      "Waves") == SCN_OP_OK);
    loopbackHarnessPumpUntil(&h, PA_LB_SETTLE_MAX, NULL, NULL);

    UT_ASSERT_MSG(loopbackHarnessAddClient(&h, "MarkJoiner"),
                  "the second client failed to connect");
    at = loopbackHarnessPumpUntil(&h, PA_LB_CONNECT_MAX, paLbJoinerConnected,
                                  NULL);
    UT_ASSERT_MSG(at > 0, "the joiner never connected");

    at = loopbackHarnessPumpUntil(&h, PA_LB_SYNC_MAX, paLbJoinerHasAll,
                                  &firstSlot);
    UT_ASSERT_MSG(at > 0,
                  "the joiner did not hold marker %d and both score rows "
                  "after %d pumps", PA_MARKER_ALL, PA_LB_SYNC_MAX);

    /* What it holds matches what the first client was sent live. */
    m = clientSimGetScnMarker(h.cs2, PA_MARKER_ALL);
    UT_ASSERT(m->kind == SCN_MARKER_KIND_SQUARE);
    UT_ASSERT(m->x == PA_SQUARE_X && m->y == PA_SQUARE_Y);
    UT_ASSERT(m->colour == SCN_PANEL_COLOUR_RED);
    m = clientSimGetScnMarker(h.cs2, PA_MARKER_CLEARED);
    UT_ASSERT_MSG(m != NULL && !m->active,
                  "the joiner holds marker %d, which was cleared",
                  PA_MARKER_CLEARED);
    p = clientSimGetScnPlayerScore(h.cs2, firstSlot);
    t = clientSimGetScnTeamScore(h.cs2, PA_LB_TEAM);
    UT_ASSERT(p->score == 12 && strcmp(p->label, "Kills") == 0);
    UT_ASSERT(t->score == 30 && strcmp(t->label, "Waves") == 0);

    m = clientSimGetScnMarker(h.cs, PA_MARKER_ALL);
    UT_ASSERT_MSG(m != NULL && m->active,
                  "the first client does not hold marker %d either, so the "
                  "two agreeing says nothing", PA_MARKER_ALL);
    p = clientSimGetScnPlayerScore(h.cs, firstSlot);
    UT_ASSERT(p != NULL && p->valid && p->score == 12);

    loopbackHarnessStop(&h);
    return 0;
}
