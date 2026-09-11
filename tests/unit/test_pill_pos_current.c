/*
 * The per-pill square state (test_pill_pos_current.c).
 *
 * A client is told a pill's square only while the pill is inside one of its
 * viewport rects. For a pill it cannot see it keeps the last square it was
 * given, and the snapshot's position-current bit says whether the pill is
 * still on it. A square it holds can only go wrong through a pickup, so a pill
 * seen going into a tank and coming out again with no square in between is
 * somewhere this client was never told about. These cases cover the split that
 * rests on:
 *
 *   - pillsExistPos and pillsDeadPos, which movement, turn rate and shell
 *     collision ask, pass over a pill whose square is not current, so nothing
 *     solid stands on ground the pill has left;
 *   - pillsViewExistPos and pillsGetViewPillNum answer for a remembered pill,
 *     so the view builder keeps drawing it and the camera keeps reaching it;
 *   - the three states and the transitions between them, and the one case that
 *     stops drawing: a pill carried off draws nowhere until its real square
 *     arrives;
 *   - two pills on one square — which only a remembered square makes possible —
 *     resolve to the one that is really there, whichever order they sit in
 *     item[];
 *   - the client's own apply path folds the bit and the in-tank flag into the
 *     state, on the full-sync pill block and on EVENT_PILL_UPDATE alike,
 *     writing the square as sent either way;
 *   - a server's pill list is untouched, which is what the zero-is-confirmed
 *     polarity buys.
 *
 * All but the last drive a bare pillsObj and the real ClientSim apply entry
 * point; only the last needs a running server.
 */

#include <string.h>

#include "global.h"
#include "input_packet.h"
#include "pillbox.h"
#include "game_sim.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_snapshot.h"
#include "test_harness.h"

/* A pill list with count entries, every pill alive, none carried, none stale —
 * the state a map install leaves. Pills sit ten squares apart along one row so
 * a case can move one onto another's square deliberately. */
static void pc_make_pills(struct pillsObj *pills, BYTE count) {
    pillboxes handle = pills;
    BYTE i;

    memset(pills, 0, sizeof(*pills));
    pillsSetNumPills(&handle, count);
    for (i = 0; i < count; i++) {
        pills->item[i].x      = (BYTE)(20 + i * 10);
        pills->item[i].y      = 40;
        pills->item[i].owner  = NEUTRAL;
        pills->item[i].armour = PILLS_MAX_ARMOUR;
        pills->item[i].inTank = FALSE;
    }
}

/* One PillSnapshot as the server builds it: the square this recipient holds,
 * with the position-current bit saying whether the pill is on it now. */
static PillSnapshot pc_pill_snap(BYTE x, BYTE y, BYTE owner, BYTE armour,
                                 bool inTank, bool posCurrent) {
    PillSnapshot snap;

    snap.x = x;
    snap.y = y;
    snap.owner = owner;
    snap.armourInTank =
        pillSetPosCurrent(pillPackArmourInTank(armour, inTank), posCurrent);
    return snap;
}

/* An EVENT_PILL_UPDATE carrying the same five fields. */
static GameEvent pc_pill_event(BYTE idx, BYTE x, BYTE y, BYTE owner,
                               BYTE armour, bool inTank, bool posCurrent) {
    GameEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_PILL_UPDATE;
    ev.data[0] = idx;
    ev.data[1] = x;
    ev.data[2] = y;
    ev.data[3] = owner;
    ev.data[4] =
        pillSetPosCurrent(pillPackArmourInTank(armour, inTank), posCurrent);
    return ev;
}

/* A header with nothing in it but the tick: no checksum, so the apply skips the
 * map compare a bare ClientSim has no terrain for. */
static SnapshotHeader pc_header(uint32_t tick, BYTE pillCount) {
    SnapshotHeader hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.serverTick = tick;
    hdr.pillCount = pillCount;
    hdr.mapChecksum = 0;
    return hdr;
}

/* 1. The gameplay lookups pass over a pill whose square is not current, and
 *    take it again the moment the flag clears — with nothing else about the
 *    pill touched in between. The view siblings answer for it throughout,
 *    which is what keeps it on screen and in the cycle. */
int run_pill_pos_current_lookups(void) {
    struct pillsObj pills;
    pillboxes pb = &pills;

    pc_make_pills(&pills, 2);
    BYTE px = pills.item[0].x;
    BYTE py = pills.item[0].y;

    /* A current pill is there for everybody. */
    UT_ASSERT_MSG(pillsExistPos(&pb, px, py) == TRUE,
                  "a current pill should exist at (%u,%u)", px, py);
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == TRUE,
                  "a current pill should exist for the view at (%u,%u)",
                  px, py);
    UT_ASSERT_MSG(pillsIsPosStale(&pb, 0) == FALSE,
                  "a fresh pill list should hold no stale flags");

    /* Losing sight of it takes it out of the gameplay answer only. */
    pillsSetPosState(&pb, 0, PILL_SQUARE_REMEMBERED);
    UT_ASSERT_MSG(pillsIsPosStale(&pb, 0) == TRUE,
                  "the flag should read back as set");
    UT_ASSERT_MSG(pillsExistPos(&pb, px, py) == FALSE,
                  "a pill whose square is not current must not be solid");
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == TRUE,
                  "the view must still see the pill at its remembered square");

    /* Nothing else about the pill moved. */
    UT_ASSERT_MSG(pills.item[0].x == px && pills.item[0].y == py,
                  "the remembered square should be untouched, got (%u,%u)",
                  pills.item[0].x, pills.item[0].y);
    UT_ASSERT_MSG(pills.item[0].armour == PILLS_MAX_ARMOUR &&
                      pills.item[0].inTank == FALSE &&
                      pills.item[0].owner == NEUTRAL,
                  "armour, owner and the in-tank flag should be untouched");

    /* The dead-pill lookup takes the same view. It has no view sibling — every
     * caller is gameplay. */
    pills.item[0].armour = 0;
    UT_ASSERT_MSG(pillsDeadPos(&pb, px, py) == FALSE,
                  "a dead pill whose square is not current must not be found");
    pillsSetPosState(&pb, 0, PILL_SQUARE_CONFIRMED);
    UT_ASSERT_MSG(pillsDeadPos(&pb, px, py) == TRUE,
                  "the dead pill should be found once its square is current");
    UT_ASSERT_MSG(pillsExistPos(&pb, px, py) == TRUE,
                  "the pill should be solid again once its square is current");

    /* The other pill was never touched. */
    UT_ASSERT_MSG(pillsIsPosStale(&pb, 1) == FALSE,
                  "setting pill 0's flag should not have reached pill 1");
    UT_ASSERT_MSG(pillsExistPos(&pb, pills.item[1].x, pills.item[1].y) == TRUE,
                  "pill 1 should still be solid");

    /* Out-of-range indexes are refused rather than written past the array. */
    pillsSetPosState(&pb, MAX_PILLS, PILL_SQUARE_REMEMBERED);
    UT_ASSERT_MSG(pillsIsPosStale(&pb, MAX_PILLS) == FALSE,
                  "an index past MAX_PILLS should answer FALSE");

    return 0;
}

/* 2. The pill-number split this phase rests on: the gameplay lookup skips a
 *    pill whose square is not current, the camera's finds it, and both agree
 *    once the flag clears. */
int run_pill_pos_current_num_split(void) {
    struct pillsObj pills;
    pillboxes pb = &pills;

    pc_make_pills(&pills, 3);
    BYTE px = pills.item[1].x;
    BYTE py = pills.item[1].y;

    UT_ASSERT_MSG(pillsGetPillNum(&pb, px, py, FALSE, FALSE) == 2,
                  "pill 1 should answer as number 2, got %u",
                  pillsGetPillNum(&pb, px, py, FALSE, FALSE));

    pillsSetPosState(&pb, 1, PILL_SQUARE_REMEMBERED);
    UT_ASSERT_MSG(pillsGetPillNum(&pb, px, py, FALSE, FALSE) != 2,
                  "the gameplay lookup should skip a pill whose square is not "
                  "current");
    UT_ASSERT_MSG(pillsGetViewPillNum(&pb, px, py, FALSE, FALSE) == 2,
                  "the camera should still reach pill 1, got %u",
                  pillsGetViewPillNum(&pb, px, py, FALSE, FALSE));

    /* The careInTank arm behaves the same on both sides of the split. */
    UT_ASSERT_MSG(pillsGetViewPillNum(&pb, px, py, TRUE, FALSE) == 2,
                  "the camera should reach the pill with careInTank set");
    UT_ASSERT_MSG(pillsGetViewPillNum(&pb, px, py, TRUE, TRUE) != 2,
                  "an in-tank test the pill fails should still not match");

    pillsSetPosState(&pb, 1, PILL_SQUARE_CONFIRMED);
    UT_ASSERT_MSG(pillsGetPillNum(&pb, px, py, FALSE, FALSE) == 2 &&
                      pillsGetViewPillNum(&pb, px, py, FALSE, FALSE) == 2,
                  "both lookups should agree once the square is current");

    return 0;
}

/* 3. Two pills on one square, which only a remembered position makes possible:
 *    the stale one must not mask the live one. The scans return the first
 *    match, so both orderings in item[] are covered. */
int run_pill_pos_current_duplicate_square(void) {
    struct pillsObj pills;
    pillboxes pb = &pills;
    BYTE sx = 60;
    BYTE sy = 60;

    /* Stale pill first in item[], live pill second. */
    pc_make_pills(&pills, 2);
    pills.item[0].x = sx; pills.item[0].y = sy;
    pills.item[1].x = sx; pills.item[1].y = sy;
    pillsSetPosState(&pb, 0, PILL_SQUARE_REMEMBERED);

    UT_ASSERT_MSG(pillsExistPos(&pb, sx, sy) == TRUE,
                  "the live pill should be found past the stale one");
    UT_ASSERT_MSG(pillsGetPillNum(&pb, sx, sy, FALSE, FALSE) == 2,
                  "the stale entry must not mask the live pill, got %u",
                  pillsGetPillNum(&pb, sx, sy, FALSE, FALSE));
    UT_ASSERT_MSG(pillsGetViewPillNum(&pb, sx, sy, FALSE, FALSE) == 1,
                  "the camera takes the first entry on the square, got %u",
                  pillsGetViewPillNum(&pb, sx, sy, FALSE, FALSE));
    UT_ASSERT_MSG(pillsViewExistPos(&pb, sx, sy) == TRUE,
                  "the view should see a pill on a square holding both");

    /* The dead-pill lookup answers for the live one only. */
    pills.item[0].armour = 0;
    UT_ASSERT_MSG(pillsDeadPos(&pb, sx, sy) == FALSE,
                  "a stale dead pill must not report the square dead while a "
                  "live pill stands on it");

    /* Live pill first in item[], stale pill second. */
    pc_make_pills(&pills, 2);
    pills.item[0].x = sx; pills.item[0].y = sy;
    pills.item[1].x = sx; pills.item[1].y = sy;
    pillsSetPosState(&pb, 1, PILL_SQUARE_REMEMBERED);

    UT_ASSERT_MSG(pillsExistPos(&pb, sx, sy) == TRUE,
                  "the live pill should be found in the other ordering too");
    UT_ASSERT_MSG(pillsGetPillNum(&pb, sx, sy, FALSE, FALSE) == 1,
                  "the live pill should answer first, got %u",
                  pillsGetPillNum(&pb, sx, sy, FALSE, FALSE));
    UT_ASSERT_MSG(pillsViewExistPos(&pb, sx, sy) == TRUE,
                  "the view should see a pill in the other ordering too");

    /* Both stale: nothing is solid there, and the view still draws one. */
    pillsSetPosState(&pb, 0, PILL_SQUARE_REMEMBERED);
    UT_ASSERT_MSG(pillsExistPos(&pb, sx, sy) == FALSE,
                  "two stale pills should leave the square clear for movement");
    UT_ASSERT_MSG(pillsViewExistPos(&pb, sx, sy) == TRUE,
                  "two stale pills should still draw one");

    return 0;
}

/* 4. The client's own apply path, driven through clientApplySnapshot: the
 *    full-sync pill block and EVENT_PILL_UPDATE both write the square exactly
 *    as sent and set the flag from the position-current bit. */
int run_pill_pos_current_snapshot_apply(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    UT_ASSERT_MSG(clientSimCreate(cs) == TRUE, "clientSimCreate failed");

    GameSim *gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(gs != NULL && gs->pb != NULL, "the client holds no pill list");

    /* The count normally arrives with the map; this client has no map, so set
     * it by hand or every lookup below stops at the empty list. */
    pillsSetNumPills(&gs->pb, 2);

    /* Two pills: the first at a square we can see, the second at one we can
     * not — the shape a fogged full sync arrives in. */
    PillSnapshot po[2];
    po[0] = pc_pill_snap(30, 30, NEUTRAL, PILLS_MAX_ARMOUR, false, true);
    po[1] = pc_pill_snap(90, 90, NEUTRAL, PILLS_MAX_ARMOUR, false, false);

    SnapshotHeader hdr = pc_header(1, 2);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 2, NULL, 0, 0);

    UT_ASSERT_MSG((*gs->pb).item[0].x == 30 && (*gs->pb).item[0].y == 30,
                  "the seen pill's square should be written as sent, got "
                  "(%u,%u)", (*gs->pb).item[0].x, (*gs->pb).item[0].y);
    UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, 0) == FALSE,
                  "the bit was set, so pill 0's square is current");
    UT_ASSERT_MSG((*gs->pb).item[1].x == 90 && (*gs->pb).item[1].y == 90,
                  "the withheld pill's square should be written as sent — the "
                  "checksum is taken over it — got (%u,%u)",
                  (*gs->pb).item[1].x, (*gs->pb).item[1].y);
    UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, 1) == TRUE,
                  "the bit was clear, so pill 1's square is not current");

    /* Which is the whole point: pill 1 draws but does not block. */
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, 90, 90) == TRUE,
                  "the withheld pill should still draw at 90,90");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, 90, 90) == FALSE,
                  "the withheld pill should not be solid at 90,90");

    /* An EVENT_PILL_UPDATE with the bit clear does the same for pill 0: it
     * carries the square this client holds, and marks it not current. */
    GameEvent ev = pc_pill_event(0, 30, 30, NEUTRAL, PILLS_MAX_ARMOUR, false,
                                 false);
    hdr = pc_header(2, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG((*gs->pb).item[0].x == 30 && (*gs->pb).item[0].y == 30,
                  "the event's square should be written as sent, got (%u,%u)",
                  (*gs->pb).item[0].x, (*gs->pb).item[0].y);
    UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, 0) == TRUE,
                  "an event with the bit clear should mark the square not "
                  "current");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, 30, 30) == FALSE,
                  "pill 0 should have stopped being solid");

    /* Driving back into view: the event arrives with the bit set and the true
     * square, and the pill is solid there from then on. */
    ev = pc_pill_event(0, 35, 30, NEUTRAL, PILLS_MAX_ARMOUR, false, true);
    hdr = pc_header(3, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG((*gs->pb).item[0].x == 35,
                  "the pill should have snapped to its true square, got %u",
                  (*gs->pb).item[0].x);
    UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, 0) == FALSE,
                  "an event with the bit set should mark the square current");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, 35, 30) == TRUE,
                  "the pill should be solid at its true square");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, 30, 30) == FALSE,
                  "the square the pill left should be clear");

    /* And the full-sync block clears a flag the same way. */
    po[1] = pc_pill_snap(90, 90, NEUTRAL, PILLS_MAX_ARMOUR, false, true);
    hdr = pc_header(4, 2);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 2, NULL, 0, 0);
    UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, 1) == FALSE,
                  "a pill block with the bit set should clear the flag");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, 90, 90) == TRUE,
                  "pill 1 should be solid again");

    clientSimDestroy(cs);
    return 0;
}

/* 5. The server never writes a flag, so its own pill list answers exactly as it
 *    did before there was one — which is what the zero-is-current polarity is
 *    for. */
int run_pill_pos_current_server_unchanged(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");

    BYTE numPills = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(numPills > 0, "the map carries no pills");

    BYTE i;
    for (i = 0; i < numPills; i++) {
        UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, i) == FALSE,
                      "pill %u starts stale on the server", i);
    }

    /* Ticking the sim — which is where the per-client records are written —
     * leaves them alone. */
    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }

    for (i = 0; i < numPills; i++) {
        BYTE px = (*gs->pb).item[i].x;
        BYTE py = (*gs->pb).item[i].y;

        UT_ASSERT_MSG(pillsIsPosStale(&gs->pb, i) == FALSE,
                      "the server set a stale flag on pill %u", i);
        if ((*gs->pb).item[i].inTank == FALSE) {
            UT_ASSERT_MSG(pillsExistPos(&gs->pb, px, py) == TRUE,
                          "pill %u stopped existing at %u,%u on the server",
                          i, px, py);
            UT_ASSERT_MSG(pillsExistPos(&gs->pb, px, py) ==
                              pillsViewExistPos(&gs->pb, px, py),
                          "the two existence answers differ on the server for "
                          "pill %u", i);
            UT_ASSERT_MSG(pillsGetPillNum(&gs->pb, px, py, FALSE, FALSE) ==
                              pillsGetViewPillNum(&gs->pb, px, py, FALSE, FALSE),
                          "the two number lookups differ on the server for "
                          "pill %u", i);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

/* 6. The three states and the moves between them, driven through the rule the
 *    apply sites call. The pill's own inTank is written after each update, the
 *    way the apply sites write it, so every call sees the flag the client held
 *    when it arrived. */
int run_pill_pos_current_moved_state(void) {
    struct pillsObj pills;
    pillboxes pb = &pills;

    pc_make_pills(&pills, 1);
    BYTE px = pills.item[0].x;
    BYTE py = pills.item[0].y;

    /* An update carrying the square says where the pill is. */
    pillsUpdatePosState(&pb, 0, true, false);
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_CONFIRMED,
                  "the bit set should confirm the square, got %u",
                  pillsGetPosState(&pb, 0));

    /* Without it, on a pill that is not in a tank, the square is the one we
     * were last given and the pill cannot have left it. */
    pillsUpdatePosState(&pb, 0, false, false);
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_REMEMBERED,
                  "the bit clear on a pill nobody has touched should leave the "
                  "square remembered, got %u", pillsGetPosState(&pb, 0));

    /* Into a tank: still remembered — the square is where it was picked up
     * from, and a pill in a tank draws nowhere anyway. */
    pillsUpdatePosState(&pb, 0, false, true);
    pills.item[0].inTank = TRUE;
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_REMEMBERED,
                  "going into a tank should not move the state on, got %u",
                  pillsGetPosState(&pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == FALSE,
                  "a pill in a tank should not draw at its old square");

    /* And out again with no square: what we hold means nothing now. */
    pillsUpdatePosState(&pb, 0, false, false);
    pills.item[0].inTank = FALSE;
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_MOVED,
                  "coming out of a tank with the bit clear should mark the "
                  "square moved, got %u", pillsGetPosState(&pb, 0));

    /* Further unconfirmed updates leave it there — this is what stops the pill
     * being drawn back at the square it was taken from. */
    pillsUpdatePosState(&pb, 0, false, false);
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_MOVED,
                  "an unconfirmed update must not downgrade a moved square, "
                  "got %u", pillsGetPosState(&pb, 0));
    UT_ASSERT_MSG(pillsIsPosStale(&pb, 0) == TRUE,
                  "a moved square is not one we have been told is current");

    /* The split that matters: a moved pill draws nowhere, a remembered one
     * draws exactly as it did before, and neither is solid. */
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == FALSE,
                  "a moved pill must not draw at the square it left");
    UT_ASSERT_MSG(pillsExistPos(&pb, px, py) == FALSE,
                  "a moved pill must not be solid either");

    pillsSetPosState(&pb, 0, PILL_SQUARE_REMEMBERED);
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == TRUE,
                  "a remembered pill must still draw at its square");
    UT_ASSERT_MSG(pillsExistPos(&pb, px, py) == FALSE,
                  "a remembered pill must not be solid");

    /* The true square arriving clears the moved state outright. */
    pillsSetPosState(&pb, 0, PILL_SQUARE_MOVED);
    pillsUpdatePosState(&pb, 0, true, false);
    pills.item[0].x = (BYTE)(px + 5);
    UT_ASSERT_MSG(pillsGetPosState(&pb, 0) == PILL_SQUARE_CONFIRMED,
                  "the bit set should clear a moved square, got %u",
                  pillsGetPosState(&pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&pb, (BYTE)(px + 5), py) == TRUE &&
                      pillsExistPos(&pb, (BYTE)(px + 5), py) == TRUE,
                  "the pill should draw and be solid at its true square");
    UT_ASSERT_MSG(pillsViewExistPos(&pb, px, py) == FALSE,
                  "the square the pill left should be clear");

    /* Out-of-range indexes are refused by both new calls. */
    pillsUpdatePosState(&pb, MAX_PILLS, false, false);
    UT_ASSERT_MSG(pillsGetPosState(&pb, MAX_PILLS) == PILL_SQUARE_CONFIRMED,
                  "an index past MAX_PILLS should answer confirmed");

    return 0;
}

/* 7. The reported sequence, replayed on one client through the real apply
 *    entry point: a dead pill in sight, carried off by a player we cannot see,
 *    put down out of sight, and finally seen again on its true square. The
 *    square it was taken from must stop drawing the moment the pill leaves the
 *    tank, and the stored coordinates must only ever change when the bit is
 *    set — the checksum is taken over them. */
int run_pill_pos_current_carry_cycle(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    UT_ASSERT_MSG(clientSimCreate(cs) == TRUE, "clientSimCreate failed");

    GameSim *gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(gs != NULL && gs->pb != NULL, "the client holds no pill list");

    /* The count normally arrives with the map; this client has no map. */
    pillsSetNumPills(&gs->pb, 1);

    const BYTE oldX = 30, oldY = 30;
    const BYTE newX = 44, newY = 51;
    uint32_t tick = 1;

    /* Both of us in sight of a dead pill: the full sync carries its square. */
    PillSnapshot po[1];
    po[0] = pc_pill_snap(oldX, oldY, NEUTRAL, 0, false, true);
    SnapshotHeader hdr = pc_header(tick++, 1);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 1, NULL, 0, 0);

    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_CONFIRMED,
                  "the pill should start confirmed, got %u",
                  pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == TRUE,
                  "the pill should draw where we can see it");

    /* The other player picks it up while we watch: in a tank, so nothing draws
     * at the square, and the square is still the one it was taken from. */
    GameEvent ev = pc_pill_event(0, oldX, oldY, NEUTRAL, 0, true, true);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "a pill in a tank should stop drawing at once");

    /* We drive away. The updates keep coming — inTank is public — but without
     * the square, and there is still nothing to draw. */
    ev = pc_pill_event(0, oldX, oldY, NEUTRAL, 0, true, false);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG((*gs->pb).item[0].x == oldX && (*gs->pb).item[0].y == oldY,
                  "an unconfirmed update must not move the stored square, got "
                  "(%u,%u)", (*gs->pb).item[0].x, (*gs->pb).item[0].y);
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "the pill is still in a tank and must not draw");

    /* The carrier drops it somewhere we cannot see: inTank goes false with no
     * square. This is the step the reported ghost came out of. */
    ev = pc_pill_event(0, oldX, oldY, NEUTRAL, 0, false, false);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_MOVED,
                  "the drop we could not see should mark the square moved, got "
                  "%u", pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG((*gs->pb).item[0].x == oldX && (*gs->pb).item[0].y == oldY,
                  "the stored square must still be the one sent, got (%u,%u)",
                  (*gs->pb).item[0].x, (*gs->pb).item[0].y);
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "nothing may draw at the square the pill was taken from");
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "and nothing may be solid there either");

    /* Driving back over that square: still nothing, however many unconfirmed
     * updates arrive on the way. */
    ev = pc_pill_event(0, oldX, oldY, NEUTRAL, 0, false, false);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "the pill must not reappear at the square it left");

    /* The real square finally arrives, and the pill is back to normal there. */
    ev = pc_pill_event(0, newX, newY, NEUTRAL, 0, false, true);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG((*gs->pb).item[0].x == newX && (*gs->pb).item[0].y == newY,
                  "the confirmed square should be written as sent, got (%u,%u)",
                  (*gs->pb).item[0].x, (*gs->pb).item[0].y);
    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_CONFIRMED,
                  "the confirmed square should clear the moved state, got %u",
                  pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, newX, newY) == TRUE &&
                      pillsExistPos(&gs->pb, newX, newY) == TRUE,
                  "the pill should draw and be solid at its true square");
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, oldX, oldY) == FALSE,
                  "the old square should stay clear");

    /* Losing sight of it again only remembers the true square — the pill has
     * not been picked up since, so it is still drawn there. */
    ev = pc_pill_event(0, newX, newY, NEUTRAL, 0, false, false);
    hdr = pc_header(tick++, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);

    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_REMEMBERED,
                  "losing sight of an untouched pill should only remember its "
                  "square, got %u", pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, newX, newY) == TRUE,
                  "an untouched pill outside our view must keep drawing");

    /* A full sync with the bit clear says the same thing, and the block's
     * square is written as sent whichever way the bit reads. */
    po[0] = pc_pill_snap(newX, newY, NEUTRAL, 0, false, false);
    hdr = pc_header(tick++, 1);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 1, NULL, 0, 0);

    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_REMEMBERED,
                  "a full sync with the bit clear should leave the square "
                  "remembered, got %u", pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, newX, newY) == TRUE,
                  "the pill should still draw after a fogged full sync");

    /* And a full sync is enough on its own to see the carry: in a tank, then
     * out of it with no square. */
    po[0] = pc_pill_snap(newX, newY, NEUTRAL, 0, true, false);
    hdr = pc_header(tick++, 1);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 1, NULL, 0, 0);
    po[0] = pc_pill_snap(newX, newY, NEUTRAL, 0, false, false);
    hdr = pc_header(tick++, 1);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        po, 1, NULL, 0, 0);

    UT_ASSERT_MSG(pillsGetPosState(&gs->pb, 0) == PILL_SQUARE_MOVED,
                  "a carry seen only through full syncs should mark the square "
                  "moved, got %u", pillsGetPosState(&gs->pb, 0));
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, newX, newY) == FALSE,
                  "nothing may draw at the square the pill was taken from");

    clientSimDestroy(cs);
    return 0;
}
