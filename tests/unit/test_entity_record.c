/*
 * What an entity add or removal reaches (test_entity_record.c).
 *
 * The six entity ops publish a CTRL_ENTITY_CHANGE, and a control event never
 * reaches the .wbv — the stream carries logitem records only. So each of them
 * also writes a log_EntityChange beside the publish, carrying the same kind,
 * the same 0-based index and the same map record the event does, and the log
 * viewer reads it back. Without that a replay would go on showing an item the
 * round had taken off the map and would never show one it had added.
 *
 * What the cases pin:
 *   - round_trip_per_kind: the bytes the writer puts in the file for each of
 *     the six arms, read out of the raw stream by hand rather than through the
 *     writer's own helper, and checked against the record the op named.
 *   - viewer_across_a_snapshot: a snapshot restates the count and every
 *     record but carries no live flags, so the viewer keeps its own — a pill
 *     taken off the map before a snapshot is still off it after, and a base
 *     added before one raises the viewer's count.
 *   - removed_and_restored_pill_replays: the design's own done-when. A round
 *     that takes a pill off the map and puts it back replays to the same
 *     world the sim ended on, snapshot in the middle and all.
 *   - brain_rect_skips_a_removed_pill: brain_data.c rebuilds a brain's pill
 *     and base lists each tick from the live items, so a removed one simply
 *     disappears from a brain's view. That is a test, not a change.
 *   - lobby_add_republishes_counts: the lobby learns the map's pill, base and
 *     start counts from the settings event, so an add at lobby time sends it
 *     again with the new count.
 *
 * A log_EntityChange only reaches a reader that was there when it was written.
 * A snapshot restates every record and every count but has nowhere to put the
 * live flags, so on its own it puts every item back on the map. That is what
 * log_EntityMasks fixes: three 16-bit masks written after every snapshot,
 * carrying what CTRL_ENTITY_SYNC carries to a live client, and suppressed when
 * every item is on the map so an ordinary round records not one extra byte.
 * Two more cases pin the holes it closes:
 *   - lobby_removal_replays: a pillbox taken off the map before the round's
 *     own segment opens is invisible to the log_EntityChange path, because the
 *     record was written before this recording had a first byte. The opening
 *     snapshot's masks are the only thing that can say so.
 *   - seek_lands_on_the_right_liveness: decoding from a mid-round snapshot
 *     alone, with every record before it unread, still reports the removed
 *     pillbox as off the map.
 *
 * Everard Island is the map behind ut_make_running_sim: 16 pillboxes, 11
 * bases and 16 starts. The pill and start lists are already at MAX_*, so
 * every add here is staged by removing an item first and landing back in the
 * slot that frees — which is what the sim's own add does with the lowest
 * removed slot.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, sim.pb / .bs / .ss */
#include "server_sim_scenario.h"
#include "game_sim.h"
#include "client_sim.h"            /* the brain object list, and ObjectInfo */
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "mines.h"
#include "control_event.h"
#include "log.h"
#include "replay_harness.h"
#include "test_harness.h"

/* ── Small helpers ───────────────────────────────────────────────── */

/* An empty land square a pillbox or a base could stand on: inside the minable
   area, nothing already on it, and ground the builder would take. */
static bool erFindLand(ServerSim *sim, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            BYTE t = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);
            if (t != GRASS && t != ROAD && t != SWAMP && t != RUBBLE &&
                t != CRATER && t != FOREST) {
                continue;
            }
            if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) continue;
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) continue;
            if (minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y)) continue;
            *ox = (BYTE)x;
            *oy = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A square a start can name: open water with no mine on it. */
static bool erFindDeepSea(ServerSim *sim, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == DEEP_SEA &&
                mapIsMine(&gs->mp, (BYTE)x, (BYTE)y) == FALSE &&
                startsExistPos(&gs->ss, (BYTE)x, (BYTE)y) == FALSE) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

static void erRemoveOp(ScenarioOp *op, ScenarioOpType type, BYTE index) {
    memset(op, 0, sizeof(*op));
    op->type = type;
    if (type == SCN_OP_ENTITY_REMOVE_PILL) {
        op->u.entityRemovePill.pill = index;
    } else if (type == SCN_OP_ENTITY_REMOVE_BASE) {
        op->u.entityRemoveBase.base = index;
    } else {
        op->u.entityRemoveStart.start = index;
    }
}

/* ── Reading the records back out of the file ────────────────────── */

/* One log_EntityChange as it sits in the stream: the three header bytes and
 * the pascal blob after them, taken off the raw bytes rather than handed back
 * by the writer's own serializer.
 *
 * snapshotsBefore is how many snapshots the walk had passed when it reached
 * this record. A case that means to straddle a snapshot boundary compares it
 * against the file's total, or against another record's, rather than against
 * a fixed number of snapshots: a recording carries as many as the round
 * happened to write, and the property each case is about is where they sit
 * relative to the records, not how many there are. */
#define ER_MAX_RECORDS 16
typedef struct {
    uint8_t kind;
    uint8_t index;
    uint8_t added;
    uint8_t recLen;
    uint8_t rec[8];
    int     snapshotsBefore;
} ErRecord;

static int erReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills, bases
 * and starts, the map runs up to the deep-sea terminator, then MAX_TANKS
 * player blocks. Plaintext, not length-framed. */
static bool erSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = erReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = erReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = erReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    for (;;) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = erReadByte(buf, len, p);
        y    = erReadByte(buf, len, p + 1);
        sx   = erReadByte(buf, len, p + 2);
        ex   = erReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = erReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every log_EntityChange in the
 * order it was written, and count the snapshots on the way past. The payload
 * is split here by hand — kind, index, added, then a length byte and that many
 * record bytes — so what the case compares against comes off the file rather
 * than through the code that put it there. Returns false if the stream did not
 * end on a clean LOG_QUIT. snapshots may be NULL. */
static bool erCollect(const char *path, ErRecord *out, int max, int *count,
                      int *snapshots) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   pos;
    bool     ok = false;
    int      snapSeen = 0;

    *count = 0;
    if (snapshots != NULL) {
        *snapshots = 0;
    }
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
        int code = erReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (erReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!erSkipSnapshot(buf, len, &pos)) break;
            snapSeen++;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = erReadByte(buf, len, pos);
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
                if (ev == (int)log_EntityChange && plen >= 4 && *count < max) {
                    const uint8_t *p = buf + payloadStart;
                    ErRecord *r = &out[*count];
                    int blobLen = p[3];
                    memset(r, 0, sizeof(*r));
                    r->kind  = p[0];
                    r->index = p[1];
                    r->added = p[2];
                    r->snapshotsBefore = snapSeen;
                    if (blobLen > (int)sizeof(r->rec)) {
                        blobLen = (int)sizeof(r->rec);
                    }
                    if (4 + blobLen <= plen) {
                        r->recLen = (uint8_t)blobLen;
                        memcpy(r->rec, p + 4, (size_t)blobLen);
                    }
                    (*count)++;
                }
                pos = payloadStart + (size_t)plen;
            }
            if (n < 0) break;
        } else {
            break;
        }
    }

    if (snapshots != NULL) {
        *snapshots = snapSeen;
    }
    free(buf);
    return ok;
}

/* ── The records themselves ──────────────────────────────────────── */

/* Every arm writes its change into the recording: the kind, the index the
 * list chose, whether the item is now on the map, and the item's map record
 * — six bytes for a pillbox or a base, three for a start. */
int run_entity_record_round_trip_per_kind(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;
    ErRecord recs[ER_MAX_RECORDS];
    int n = 0;
    pillbox oldPill;
    base oldBase;
    start oldStart;
    BYTE lx = 0, ly = 0, bx = 0, by = 0, sx = 0, sy = 0;
    BYTE addedPill, addedBase, addedStart;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "entityRecordKinds", "Tester"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);

    /* The records the removals must carry, read straight off the lists. */
    oldPill  = sim->sim.pb->item[0];
    oldBase  = sim->sim.bs->item[0];
    oldStart = sim->sim.ss->item[0];
    /* A carried pillbox is refused, and a pillbox in a tank would make the
       record's last byte a 1 for a reason this case is not about. */
    UT_ASSERT_MSG(oldPill.inTank == FALSE,
                  "pill 1 starts in a tank on this map");

    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    replayHarnessTick(&h, 2);

    /* And three adds, each landing in the slot its removal freed. */
    UT_ASSERT_MSG(erFindLand(sim, &lx, &ly), "map has no free land for a pill");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_PILL;
    op.u.entityAddPill.x = lx;
    op.u.entityAddPill.y = ly;
    op.u.entityAddPill.owner = NEUTRAL;
    op.u.entityAddPill.armour = 9;
    op.u.entityAddPill.speed = 60;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    addedPill = out.index;

    UT_ASSERT_MSG(erFindLand(sim, &bx, &by), "map has no free land for a base");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_BASE;
    op.u.entityAddBase.x = bx;
    op.u.entityAddBase.y = by;
    op.u.entityAddBase.owner = NEUTRAL;
    op.u.entityAddBase.armour = 40;
    op.u.entityAddBase.shells = 30;
    op.u.entityAddBase.mines = 20;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    addedBase = out.index;

    UT_ASSERT_MSG(erFindDeepSea(sim, &sx, &sy), "map has no free open water");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_START;
    op.u.entityAddStart.x = sx;
    op.u.entityAddStart.y = sy;
    op.u.entityAddStart.dir = 7;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    addedStart = out.index;

    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    UT_ASSERT_MSG(erCollect(h.path, recs, ER_MAX_RECORDS, &n, NULL),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(n == 6, "the round wrote %d entity records, wanted 6", n);

    /* Pillbox removal: x, y, owner, armour, speed, inTank. */
    UT_ASSERT_MSG(recs[0].kind == ENTITY_KIND_PILL, "kind %u",
                  (unsigned)recs[0].kind);
    UT_ASSERT_MSG(recs[0].index == 0, "index %u", (unsigned)recs[0].index);
    UT_ASSERT_MSG(recs[0].added == 0, "a removal wrote added=%u",
                  (unsigned)recs[0].added);
    UT_ASSERT_MSG(recs[0].recLen == 6, "pill record is %u bytes",
                  (unsigned)recs[0].recLen);
    UT_ASSERT_MSG(recs[0].rec[0] == oldPill.x && recs[0].rec[1] == oldPill.y,
                  "removed pill cell %u,%u, list had %u,%u",
                  (unsigned)recs[0].rec[0], (unsigned)recs[0].rec[1],
                  (unsigned)oldPill.x, (unsigned)oldPill.y);
    UT_ASSERT_MSG(recs[0].rec[2] == oldPill.owner, "owner %u",
                  (unsigned)recs[0].rec[2]);
    UT_ASSERT_MSG(recs[0].rec[3] == oldPill.armour, "armour %u",
                  (unsigned)recs[0].rec[3]);
    UT_ASSERT_MSG(recs[0].rec[4] == oldPill.speed, "speed %u",
                  (unsigned)recs[0].rec[4]);
    UT_ASSERT_MSG(recs[0].rec[5] == 0, "inTank %u", (unsigned)recs[0].rec[5]);

    /* Base removal: x, y, owner, armour, shells, mines. */
    UT_ASSERT_MSG(recs[1].kind == ENTITY_KIND_BASE, "kind %u",
                  (unsigned)recs[1].kind);
    UT_ASSERT_MSG(recs[1].index == 0, "index %u", (unsigned)recs[1].index);
    UT_ASSERT_MSG(recs[1].added == 0, "a removal wrote added=%u",
                  (unsigned)recs[1].added);
    UT_ASSERT_MSG(recs[1].recLen == 6, "base record is %u bytes",
                  (unsigned)recs[1].recLen);
    UT_ASSERT_MSG(recs[1].rec[0] == oldBase.x && recs[1].rec[1] == oldBase.y,
                  "removed base cell %u,%u, list had %u,%u",
                  (unsigned)recs[1].rec[0], (unsigned)recs[1].rec[1],
                  (unsigned)oldBase.x, (unsigned)oldBase.y);
    UT_ASSERT_MSG(recs[1].rec[2] == oldBase.owner, "owner %u",
                  (unsigned)recs[1].rec[2]);
    UT_ASSERT_MSG(recs[1].rec[3] == oldBase.armour, "armour %u",
                  (unsigned)recs[1].rec[3]);
    UT_ASSERT_MSG(recs[1].rec[4] == oldBase.shells, "shells %u",
                  (unsigned)recs[1].rec[4]);
    UT_ASSERT_MSG(recs[1].rec[5] == oldBase.mines, "mines %u",
                  (unsigned)recs[1].rec[5]);

    /* Start removal: x, y, dir — three bytes, not six padded. */
    UT_ASSERT_MSG(recs[2].kind == ENTITY_KIND_START, "kind %u",
                  (unsigned)recs[2].kind);
    UT_ASSERT_MSG(recs[2].index == 0, "index %u", (unsigned)recs[2].index);
    UT_ASSERT_MSG(recs[2].added == 0, "a removal wrote added=%u",
                  (unsigned)recs[2].added);
    UT_ASSERT_MSG(recs[2].recLen == 3, "start record is %u bytes",
                  (unsigned)recs[2].recLen);
    UT_ASSERT_MSG(recs[2].rec[0] == oldStart.x && recs[2].rec[1] == oldStart.y,
                  "removed start cell %u,%u, list had %u,%u",
                  (unsigned)recs[2].rec[0], (unsigned)recs[2].rec[1],
                  (unsigned)oldStart.x, (unsigned)oldStart.y);
    UT_ASSERT_MSG(recs[2].rec[2] == oldStart.dir, "dir %u",
                  (unsigned)recs[2].rec[2]);

    /* The pillbox add, at the index the list reported. */
    UT_ASSERT_MSG(recs[3].kind == ENTITY_KIND_PILL, "kind %u",
                  (unsigned)recs[3].kind);
    UT_ASSERT_MSG(recs[3].index == addedPill,
                  "the record named index %u and the op reported %u",
                  (unsigned)recs[3].index, (unsigned)addedPill);
    UT_ASSERT_MSG(recs[3].added == 1, "an add wrote added=%u",
                  (unsigned)recs[3].added);
    UT_ASSERT_MSG(recs[3].recLen == 6, "pill record is %u bytes",
                  (unsigned)recs[3].recLen);
    UT_ASSERT_MSG(recs[3].rec[0] == lx && recs[3].rec[1] == ly,
                  "added pill cell %u,%u, wanted %u,%u",
                  (unsigned)recs[3].rec[0], (unsigned)recs[3].rec[1],
                  (unsigned)lx, (unsigned)ly);
    UT_ASSERT_MSG(recs[3].rec[2] == NEUTRAL, "owner %u",
                  (unsigned)recs[3].rec[2]);
    UT_ASSERT_MSG(recs[3].rec[3] == 9, "armour %u", (unsigned)recs[3].rec[3]);
    UT_ASSERT_MSG(recs[3].rec[4] == 60, "speed %u", (unsigned)recs[3].rec[4]);
    UT_ASSERT_MSG(recs[3].rec[5] == 0, "inTank %u", (unsigned)recs[3].rec[5]);

    /* The base add. */
    UT_ASSERT_MSG(recs[4].kind == ENTITY_KIND_BASE, "kind %u",
                  (unsigned)recs[4].kind);
    UT_ASSERT_MSG(recs[4].index == addedBase,
                  "the record named index %u and the op reported %u",
                  (unsigned)recs[4].index, (unsigned)addedBase);
    UT_ASSERT_MSG(recs[4].added == 1, "an add wrote added=%u",
                  (unsigned)recs[4].added);
    UT_ASSERT_MSG(recs[4].recLen == 6, "base record is %u bytes",
                  (unsigned)recs[4].recLen);
    UT_ASSERT_MSG(recs[4].rec[0] == bx && recs[4].rec[1] == by,
                  "added base cell %u,%u, wanted %u,%u",
                  (unsigned)recs[4].rec[0], (unsigned)recs[4].rec[1],
                  (unsigned)bx, (unsigned)by);
    UT_ASSERT_MSG(recs[4].rec[2] == NEUTRAL, "owner %u",
                  (unsigned)recs[4].rec[2]);
    UT_ASSERT_MSG(recs[4].rec[3] == 40, "armour %u", (unsigned)recs[4].rec[3]);
    UT_ASSERT_MSG(recs[4].rec[4] == 30, "shells %u", (unsigned)recs[4].rec[4]);
    UT_ASSERT_MSG(recs[4].rec[5] == 20, "mines %u", (unsigned)recs[4].rec[5]);

    /* And the start add. */
    UT_ASSERT_MSG(recs[5].kind == ENTITY_KIND_START, "kind %u",
                  (unsigned)recs[5].kind);
    UT_ASSERT_MSG(recs[5].index == addedStart,
                  "the record named index %u and the op reported %u",
                  (unsigned)recs[5].index, (unsigned)addedStart);
    UT_ASSERT_MSG(recs[5].added == 1, "an add wrote added=%u",
                  (unsigned)recs[5].added);
    UT_ASSERT_MSG(recs[5].recLen == 3, "start record is %u bytes",
                  (unsigned)recs[5].recLen);
    UT_ASSERT_MSG(recs[5].rec[0] == sx && recs[5].rec[1] == sy,
                  "added start cell %u,%u, wanted %u,%u",
                  (unsigned)recs[5].rec[0], (unsigned)recs[5].rec[1],
                  (unsigned)sx, (unsigned)sy);
    UT_ASSERT_MSG(recs[5].rec[2] == 7, "dir %u", (unsigned)recs[5].rec[2]);

    replayHarnessStop(&h);
    return 0;
}

/* ── The viewer, across a snapshot ───────────────────────────────── */

/* A snapshot restates every record and the count, and carries no live flags
 * — they sit past the wire region on both sides. So the viewer keeps the
 * flags the stream has given it: a pill removed before the snapshot is still
 * off the map after it, and a base added before it raises the count the
 * snapshot then confirms. */
int run_entity_record_viewer_across_a_snapshot(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;
    BYTE bx = 0, by = 0;
    BYTE pillsBefore, basesBefore;
    BYTE addedBase;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "entityRecordSnap", "Tester"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);
    pillsBefore = pillsGetNumPills(&sim->sim.pb);
    basesBefore = basesGetNumBases(&sim->sim.bs);
    UT_ASSERT(pillsBefore >= 2);
    UT_ASSERT_MSG(basesBefore < MAX_BASES,
                  "this case needs room for one more base, list holds %u",
                  (unsigned)basesBefore);

    /* A removal, which leaves the count where it is. */
    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* And an add that appends, which raises one. */
    UT_ASSERT_MSG(erFindLand(sim, &bx, &by), "map has no free land for a base");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_BASE;
    op.u.entityAddBase.x = bx;
    op.u.entityAddBase.y = by;
    op.u.entityAddBase.owner = NEUTRAL;
    op.u.entityAddBase.armour = 50;
    op.u.entityAddBase.shells = 25;
    op.u.entityAddBase.mines = 15;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    addedBase = out.index;
    UT_ASSERT_MSG(addedBase == basesBefore,
                  "the add landed at index %u, wanted the append at %u",
                  (unsigned)addedBase, (unsigned)basesBefore);

    /* Flush the two changes, then put a snapshot after them and keep going,
       so the viewer reads both records and then a snapshot that disagrees
       with one of them. */
    replayHarnessTick(&h, 4);
    replayHarnessSnapshot(&h);
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* The whole case turns on a snapshot standing between the two records and
       the end of the round, so say so rather than assume it. The count of
       snapshots is not the thing to check — the writer puts one out every
       FULL_SYNC_INTERVAL game ticks as well as at the head of the file, so a
       round carries as many as it happened to write. What matters is that one
       of them comes after the last record. */
    {
        ErRecord recs[ER_MAX_RECORDS];
        int n = 0;
        int snapshots = 0;
        UT_ASSERT_MSG(erCollect(h.path, recs, ER_MAX_RECORDS, &n, &snapshots),
                      "the recording did not end on a clean quit: %s", h.path);
        UT_ASSERT_MSG(n == 2, "the round wrote %d entity records, wanted 2", n);
        UT_ASSERT_MSG(snapshots > recs[1].snapshotsBefore,
                      "no snapshot stands between the last record and the end "
                      "of the file: %d snapshots, %d of them before that "
                      "record", snapshots, recs[1].snapshotsBefore);
    }

    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode %s", h.path);

    UT_ASSERT_MSG(h.replayed->numPills == pillsBefore,
                  "a removal moved the replayed pill count to %u from %u",
                  (unsigned)h.replayed->numPills, (unsigned)pillsBefore);
    UT_ASSERT_MSG(h.replayed->pills[1].active == false,
                  "the snapshot put the removed pillbox back on the map");
    UT_ASSERT_MSG(h.replayed->pills[0].active == true,
                  "the pillbox next to it came off the map");
    UT_ASSERT_MSG(h.replayed->numBases == (BYTE)(basesBefore + 1),
                  "the replayed base count is %u, wanted %u",
                  (unsigned)h.replayed->numBases, (unsigned)(basesBefore + 1));
    UT_ASSERT_MSG(h.replayed->bases[addedBase].active == true,
                  "the added base is not on the replayed map");
    UT_ASSERT_MSG(h.replayed->bases[addedBase].x == bx &&
                      h.replayed->bases[addedBase].y == by,
                  "the added base replayed at %u,%u, wanted %u,%u",
                  (unsigned)h.replayed->bases[addedBase].x,
                  (unsigned)h.replayed->bases[addedBase].y,
                  (unsigned)bx, (unsigned)by);

    UT_ASSERT_MSG(replayHarnessCompare(&h), "%s", replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}

/* ── The done-when ───────────────────────────────────────────────── */

/* A round that takes a pillbox off the map and later puts it back, with a
 * snapshot in between, replays to the world the sim ended on. */
int run_entity_record_removed_and_restored_pill_replays(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;
    pillbox gone;
    BYTE lx = 0, ly = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "entityRecordRestore",
                                              "Tester"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) >= 1);
    gone = sim->sim.pb->item[0];
    UT_ASSERT_MSG(gone.inTank == FALSE, "pill 1 starts in a tank on this map");
    /* The add arm range-checks what it is handed, so the record this case
       means to put back has to be one it would take. */
    UT_ASSERT_MSG(gone.armour <= PILLS_MAX_ARMOUR &&
                      gone.speed >= PILLBOX_MAX_FIRERATE &&
                      gone.speed <= PILLBOX_ATTACK_NORMAL,
                  "pill 1 carries armour %u speed %u, outside what an add takes",
                  (unsigned)gone.armour, (unsigned)gone.speed);

    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    replayHarnessTick(&h, 4);
    replayHarnessSnapshot(&h);
    replayHarnessTick(&h, 4);

    /* Back onto the map carrying the record it left with, into the slot it
       left — the lowest removed one. The square comes from the same scan the
       other adds here use rather than from the pillbox's own cell: a builder
       places a pillbox without writing any terrain under it (lgm.c), so what
       a map-authored pillbox stands on is whatever the .map says, and the add
       arm refuses a building, a river, a boat or deep sea. The case is about
       the slot and the record surviving a snapshot, not about the cell. */
    UT_ASSERT_MSG(erFindLand(sim, &lx, &ly), "map has no free land for a pill");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_PILL;
    op.u.entityAddPill.x = lx;
    op.u.entityAddPill.y = ly;
    op.u.entityAddPill.owner = gone.owner;
    op.u.entityAddPill.armour = gone.armour;
    op.u.entityAddPill.speed = gone.speed;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK,
                  "putting the pillbox back was refused");
    UT_ASSERT_MSG(out.index == 0,
                  "the pillbox came back at index %u, not the slot it left",
                  (unsigned)out.index);

    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* Two entity records with a snapshot between them is the shape this case
       is about; a recording without the snapshot would prove less. The
       snapshot count is not what says so — the writer puts one out every
       FULL_SYNC_INTERVAL game ticks as well as at the head of the file — so
       the removal and the add are asked to sit on opposite sides of one. */
    {
        ErRecord recs[ER_MAX_RECORDS];
        int n = 0;
        UT_ASSERT_MSG(erCollect(h.path, recs, ER_MAX_RECORDS, &n, NULL),
                      "the recording did not end on a clean quit: %s", h.path);
        UT_ASSERT_MSG(n == 2, "the round wrote %d entity records, wanted 2", n);
        UT_ASSERT_MSG(recs[1].snapshotsBefore > recs[0].snapshotsBefore,
                      "no snapshot stands between the removal and the add: "
                      "%d snapshots before the first record, %d before the "
                      "second", recs[0].snapshotsBefore,
                      recs[1].snapshotsBefore);
    }

    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode %s", h.path);
    UT_ASSERT_MSG(replayHarnessCompare(&h), "%s", replayHarnessDiff(&h));
    UT_ASSERT_MSG(h.replayed->pills[0].active == true,
                  "the restored pillbox is off the map in the replay");
    UT_ASSERT_MSG(h.replayed->pills[0].x == lx &&
                      h.replayed->pills[0].y == ly,
                  "the restored pillbox replayed at %u,%u, wanted %u,%u",
                  (unsigned)h.replayed->pills[0].x,
                  (unsigned)h.replayed->pills[0].y,
                  (unsigned)lx, (unsigned)ly);
    UT_ASSERT_MSG(h.replayed->pills[0].owner == gone.owner &&
                      h.replayed->pills[0].armour == gone.armour,
                  "the restored pillbox replayed as owner %u armour %u, "
                  "wanted %u and %u",
                  (unsigned)h.replayed->pills[0].owner,
                  (unsigned)h.replayed->pills[0].armour,
                  (unsigned)gone.owner, (unsigned)gone.armour);

    replayHarnessStop(&h);
    return 0;
}

/* ── A brain's view ──────────────────────────────────────────────── */

/* brain_data.c builds a brain's pill and base lists from the live items every
 * tick, so a removed one leaves a brain's view on the tick it goes. Nothing
 * changed for this; the case is here to keep it that way. */
int run_entity_record_brain_rect_skips_a_removed_pill(void) {
    ServerSim *sim = ut_make_running_sim("Watcher");
    GameSim *gs;
    ClientSim *cs;
    ScenarioOp op;
    ObjectInfo *objects;
    unsigned short *numObjects;
    unsigned short before, after;
    unsigned short i;
    bool sawPillOne;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(pillsGetNumPills(&gs->pb) >= 1);
    UT_ASSERT_MSG(gs->pb->item[0].inTank == FALSE,
                  "pill 1 starts in a tank on this map");

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimCreate(cs) == true);
    objects = clientSimGetBrainObjects(cs);
    numObjects = clientSimGetBrainsNumObjects(cs);
    UT_ASSERT(objects != NULL && numObjects != NULL);

    /* The whole map, so nothing drops out for being out of view. */
    *numObjects = 0;
    pillsGetBrainPillsInRect(cs, gs, &gs->pb, 0, 255, 0, 255);
    before = *numObjects;
    UT_ASSERT_MSG(before >= 1, "a brain saw %u pillboxes to begin with",
                  (unsigned)before);
    sawPillOne = false;
    for (i = 0; i < before; i++) {
        if (objects[i].object == PILLS_BRAIN_OBJECT_TYPE &&
            objects[i].idnum == 0) {
            sawPillOne = true;
        }
    }
    UT_ASSERT_MSG(sawPillOne, "pill 1 was not in the brain's list to begin with");

    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    *numObjects = 0;
    pillsGetBrainPillsInRect(cs, gs, &gs->pb, 0, 255, 0, 255);
    after = *numObjects;
    UT_ASSERT_MSG(after == (unsigned short)(before - 1),
                  "a brain saw %u pillboxes after the removal, wanted %u",
                  (unsigned)after, (unsigned)(before - 1));
    for (i = 0; i < after; i++) {
        UT_ASSERT_MSG(!(objects[i].object == PILLS_BRAIN_OBJECT_TYPE &&
                        objects[i].idnum == 0),
                      "the removed pillbox is still in the brain's list");
    }

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

/* ── The lobby's counts ──────────────────────────────────────────── */

typedef struct {
    int     count;
    uint8_t pills;
    uint8_t bases;
    uint8_t starts;
} ErSettingsWatch;

static void erSettingsDeliver(void *ctx, const struct ControlEvent *evt) {
    ErSettingsWatch *w = (ErSettingsWatch *)ctx;
    if (evt->type != CTRL_LOBBY_SETTINGS) {
        return;
    }
    w->count++;
    w->pills  = evt->u.lobbySettings.lobbyPillCount;
    w->bases  = evt->u.lobbySettings.lobbyBaseCount;
    w->starts = evt->u.lobbySettings.lobbyStartCount;
}

/* The lobby reads the map's pill, base and start counts off the settings
 * event, so an add at lobby time publishes it again carrying the new count. */
int run_entity_record_lobby_add_republishes_counts(void) {
    ServerSim *sim = ut_make_running_sim("Host");
    GameSim *gs;
    ScenarioOp op;
    ScnOpOut out;
    ErSettingsWatch w;
    SubscriberHandle h;
    BYTE bx = 0, by = 0;
    BYTE basesBefore;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    basesBefore = basesGetNumBases(&gs->bs);
    UT_ASSERT_MSG(basesBefore < MAX_BASES,
                  "this case needs room for one more base, list holds %u",
                  (unsigned)basesBefore);
    UT_ASSERT_MSG(erFindLand(sim, &bx, &by), "map has no free land for a base");

    /* The arms make no state check — the item lists belong to the map, not to
       a round — so a lobby-time op is the same op with the sim in the lobby. */
    sim->state = serverStateLobby;

    memset(&w, 0, sizeof(w));
    h = serverSimRegisterSubscriber(sim, erSettingsDeliver, &w);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    /* The sync replay a registration runs carries the settings once; this
       case is about what comes after it. */
    w.count = 0;

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_ADD_BASE;
    op.u.entityAddBase.x = bx;
    op.u.entityAddBase.y = by;
    op.u.entityAddBase.owner = NEUTRAL;
    op.u.entityAddBase.armour = 20;
    op.u.entityAddBase.shells = 10;
    op.u.entityAddBase.mines = 5;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);

    UT_ASSERT_MSG(w.count == 1,
                  "a lobby-time add published the settings %d times", w.count);
    UT_ASSERT_MSG(w.bases == (uint8_t)(basesBefore + 1),
                  "the settings carried %u bases, wanted %u",
                  (unsigned)w.bases, (unsigned)(basesBefore + 1));
    UT_ASSERT_MSG(w.pills == pillsGetNumPills(&gs->pb),
                  "the settings carried %u pillboxes, the list holds %u",
                  (unsigned)w.pills, (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(w.starts == startsGetNumStarts(&gs->ss),
                  "the settings carried %u starts, the list holds %u",
                  (unsigned)w.starts, (unsigned)startsGetNumStarts(&gs->ss));

    /* A running round has no lobby to tell, so the same op says nothing. */
    sim->state = serverStateRunning;
    w.count = 0;
    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, out.index);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 0,
                  "an op in a running round published the settings %d times",
                  w.count);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ── What only the masks can say ─────────────────────────────────── */

/* A pillbox taken off the map before the round's own recording opens. The
 * log_EntityChange for it went nowhere — there was no log yet — and the
 * opening snapshot restates the same count and the same sixteen records it
 * would have restated anyway, so on the records alone the replay shows a
 * pillbox the round does not have, for the whole round. The masks record that
 * follows the snapshot is what carries it. */
int run_entity_record_lobby_removal_replays(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    BYTE px, py;
    BYTE countBefore;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessPrepare(&h, "entityRecordLobbyRemove", "Tester"),
                  "could not stand the round up");
    sim = h.sim;
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) >= 2);
    countBefore = pillsGetNumPills(&sim->sim.pb);
    px = sim->sim.pb->item[1].x;
    py = sim->sim.pb->item[1].y;
    UT_ASSERT_MSG(sim->sim.pb->item[1].inTank == FALSE,
                  "pill 2 starts in a tank on this map");

    /* The entity arms make no state check, so a lobby-time op is the same op
       with the sim in the lobby. Nothing is recording yet, which is the point:
       whatever this writes has nowhere to go. */
    sim->state = serverStateLobby;
    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pillsIsActive(&sim->sim.pb, 2) == FALSE,
                  "the removal did not take pill 2 off the sim's map");
    sim->state = serverStateRunning;

    /* Now the round opens, and with it the recording's first snapshot. */
    UT_ASSERT_MSG(replayHarnessBeginRecording(&h), "could not start recording");
    replayHarnessTick(&h, 6);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* No entity record is in the file at all — the removal predates it. */
    {
        ErRecord recs[ER_MAX_RECORDS];
        int n = -1;
        UT_ASSERT_MSG(erCollect(h.path, recs, ER_MAX_RECORDS, &n, NULL),
                      "the recording did not end on a clean quit: %s", h.path);
        UT_ASSERT_MSG(n == 0,
                      "the round wrote %d entity records, wanted none", n);
    }

    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode %s", h.path);
    UT_ASSERT_MSG(h.replayed->numPills == countBefore,
                  "the replayed pill count is %u, wanted %u",
                  (unsigned)h.replayed->numPills, (unsigned)countBefore);
    UT_ASSERT_MSG(h.replayed->pills[1].active == false,
                  "the replay has the pillbox at %u,%u on the map, and the "
                  "round never had it", (unsigned)px, (unsigned)py);
    UT_ASSERT_MSG(h.replayed->pills[0].active == true,
                  "the pillbox next to it came off the map");
    UT_ASSERT_MSG(replayHarnessCompare(&h), "%s", replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}

/* Decoding from a mid-round snapshot with every record before it unread —
 * what a scrub back to that point leaves the viewer holding. The removal is
 * before the snapshot and nothing after it puts the pillbox back, so the
 * snapshot and its masks are the whole of what the world is read from. */
int run_entity_record_seek_lands_on_the_right_liveness(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    ReplayWorld *seeked = NULL;
    BYTE countBefore;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "entityRecordSeek", "Tester"),
                  "could not start recording");
    sim = h.sim;
    replayHarnessTick(&h, 4);
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) >= 2);
    countBefore = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(sim->sim.pb->item[1].inTank == FALSE,
                  "pill 2 starts in a tank on this map");

    erRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* Flush the record, then a snapshot after it, then idle to the end so
       nothing later re-states the pillbox. */
    replayHarnessTick(&h, 4);
    replayHarnessSnapshot(&h);
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* replayHarnessDecodeFromLastSnapshot jumps to the last snapshot in the
       file, so that snapshot has to be one written after the record — and it
       is the count of snapshots past the record, not the count in the file,
       that says so. The writer emits one every FULL_SYNC_INTERVAL game ticks
       as well as at the head of the file, and those all land before the
       removal; only the mid-round snapshot this case asks for comes after. */
    {
        ErRecord recs[ER_MAX_RECORDS];
        int n = 0;
        int snapshots = 0;
        UT_ASSERT_MSG(erCollect(h.path, recs, ER_MAX_RECORDS, &n, &snapshots),
                      "the recording did not end on a clean quit: %s", h.path);
        UT_ASSERT_MSG(n == 1, "the round wrote %d entity records, wanted 1", n);
        UT_ASSERT_MSG(snapshots == recs[0].snapshotsBefore + 1,
                      "the jump must land on a snapshot written after the "
                      "record, and it must be the last one: %d snapshots, %d "
                      "of them before the record", snapshots,
                      recs[0].snapshotsBefore);
    }

    /* Played from the start, the record alone is enough. */
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode %s", h.path);
    UT_ASSERT_MSG(h.replayed->pills[1].active == false,
                  "played from the start, the removed pillbox is on the map");

    /* Played from the mid-round snapshot, the record is never read. */
    seeked = (ReplayWorld *) malloc(sizeof(ReplayWorld));
    UT_ASSERT(seeked != NULL);
    if (!replayHarnessDecodeFromLastSnapshot(h.path, seeked)) {
        free(seeked);
        UT_FAIL("could not decode %s from its last snapshot", h.path);
    }
    if (seeked->numPills != countBefore || seeked->pills[1].active != false ||
        seeked->pills[0].active != true) {
        unsigned gotCount = (unsigned)seeked->numPills;
        bool gotOne = seeked->pills[0].active;
        bool gotTwo = seeked->pills[1].active;
        free(seeked);
        UT_FAIL("from the snapshot alone: %u pillboxes, pill 1 %s the map, "
                "pill 2 %s the map — wanted %u, on, off",
                gotCount, gotOne ? "on" : "off", gotTwo ? "on" : "off",
                (unsigned)countBefore);
    }
    free(seeked);

    replayHarnessStop(&h);
    return 0;
}
