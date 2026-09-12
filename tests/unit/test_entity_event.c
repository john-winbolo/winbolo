/*
 * The entity-change control event (test_entity_event.c).
 *
 * CTRL_ENTITY_CHANGE is how a client learns that a pillbox, base or start
 * has joined the map or left it on the tick it happens. It carries the kind,
 * the item's index, whether the item is now on the map, and the item's map
 * record — and nothing else: a pillbox's reload, coolDown and justSeen and a
 * base's refuelTime, baseTime and justStopped are the server's per-tick
 * working state and never ride the wire.
 *
 * What the cases pin:
 *   - codec_roundtrip: every field of every kind survives the body encoder
 *     and decoder in both directions, the body is one fixed length, and a
 *     kind with no list behind it is refused rather than guessed at.
 *   - client_adds_at_fresh_index: an add past the end of the list extends it.
 *   - client_add_reuses_removed_slot: an add lands back on a removed slot,
 *     which is the slot the server's own add would have taken.
 *   - client_remove_keeps_the_slot: a remove clears the live flag and leaves
 *     the count and the indices above it alone.
 *   - client_add_lands_on_the_server_index: the number on the event decides
 *     the slot, even where the client's own list disagrees with the server's,
 *     and a number past the count raises it with the gap left removed.
 *   - removed_index_sends_no_delta: the per-tick pill and base diff does not
 *     name a removed index, so the periodic full sync stays the authority on
 *     how long the list is.
 *   - wire_corpus_fixture: the committed golden bytes in
 *     tests/fixtures/wire/entity_change.hex decode and re-encode unchanged,
 *     so the byte order is pinned against something outside this file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "game_sim.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.pb / .bs — the unittests profile */
#include "input_packet.h"          /* EVENT_PILL_UPDATE, EVENT_BASE_UPDATE */
#include "test_harness.h"

#ifndef WB_WIRE_FIXTURE_DIR
#define WB_WIRE_FIXTURE_DIR "tests/fixtures/wire"
#endif

/* [kind 1][index 1][added 1][record 6] — see transport_control_codec.c. */
#define EC_BODY_LEN 9
#define EC_FIXTURE_MAX 32

/* ── Codec ───────────────────────────────────────────────────────── */

/* Encode evt, decode the bytes back into out, and say whether both halves
 * agreed the body was the expected length. */
static bool ecRoundTrip(const ControlEvent *evt, ControlEvent *out) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_ENTITY_CHANGE);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ENTITY_CHANGE);
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;

    if (enc == NULL || dec == NULL) return false;
    if (enc(evt, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) return false;
    if (outLen != EC_BODY_LEN) return false;
    memset(out, 0, sizeof(*out));
    if (!dec(buf, outLen, out)) return false;
    return (out->type == CTRL_ENTITY_CHANGE);
}

/* The three header fields, filled the same way for every kind. */
static void ecHeader(ControlEvent *evt, uint8_t kind, uint8_t index,
                     uint8_t added) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_ENTITY_CHANGE;
    evt->u.entityChange.kind  = kind;
    evt->u.entityChange.index = index;
    evt->u.entityChange.added = added;
}

int run_entity_event_codec_roundtrip(void) {
    ControlEvent in, out;

    /* A pillbox added at index 0, every record field a value no other field
       carries, so a swapped pair shows up. */
    {
        ecHeader(&in, ENTITY_KIND_PILL, 0, 1);
        in.u.entityChange.rec.pill.x      = 30;
        in.u.entityChange.rec.pill.y      = 40;
        in.u.entityChange.rec.pill.owner  = NEUTRAL;
        in.u.entityChange.rec.pill.armour = 15;
        in.u.entityChange.rec.pill.speed  = 50;
        in.u.entityChange.rec.pill.inTank = 0;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT_MSG(out.u.entityChange.kind == ENTITY_KIND_PILL,
                      "kind became %u", (unsigned)out.u.entityChange.kind);
        UT_ASSERT_MSG(out.u.entityChange.index == 0,
                      "index became %u", (unsigned)out.u.entityChange.index);
        UT_ASSERT_MSG(out.u.entityChange.added == 1,
                      "added became %u", (unsigned)out.u.entityChange.added);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.x == 30,
                      "pill x became %u", (unsigned)out.u.entityChange.rec.pill.x);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.y == 40,
                      "pill y became %u", (unsigned)out.u.entityChange.rec.pill.y);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.owner == NEUTRAL,
                      "pill owner became %u",
                      (unsigned)out.u.entityChange.rec.pill.owner);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.armour == 15,
                      "pill armour became %u",
                      (unsigned)out.u.entityChange.rec.pill.armour);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.speed == 50,
                      "pill speed became %u",
                      (unsigned)out.u.entityChange.rec.pill.speed);
        UT_ASSERT_MSG(out.u.entityChange.rec.pill.inTank == 0,
                      "pill inTank became %u",
                      (unsigned)out.u.entityChange.rec.pill.inTank);
    }

    /* A pillbox removed at the top index, carried in a tank at the time, so
       the removal's record is the item as it stood and the top index is not
       lost to a narrow field. */
    {
        ecHeader(&in, ENTITY_KIND_PILL, MAX_PILLS - 1, 0);
        in.u.entityChange.rec.pill.x      = 65;
        in.u.entityChange.rec.pill.y      = 7;
        in.u.entityChange.rec.pill.owner  = 3;
        in.u.entityChange.rec.pill.armour = 0;
        in.u.entityChange.rec.pill.speed  = 6;
        in.u.entityChange.rec.pill.inTank = 1;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT_MSG(out.u.entityChange.index == MAX_PILLS - 1,
                      "index became %u", (unsigned)out.u.entityChange.index);
        UT_ASSERT_MSG(out.u.entityChange.added == 0,
                      "a removal decoded as added=%u",
                      (unsigned)out.u.entityChange.added);
        UT_ASSERT(out.u.entityChange.rec.pill.x == 65);
        UT_ASSERT(out.u.entityChange.rec.pill.y == 7);
        UT_ASSERT(out.u.entityChange.rec.pill.owner == 3);
        UT_ASSERT(out.u.entityChange.rec.pill.armour == 0);
        UT_ASSERT(out.u.entityChange.rec.pill.speed == 6);
        UT_ASSERT(out.u.entityChange.rec.pill.inTank == 1);
    }

    /* A base added, full of everything, so the three stocks cannot be mixed
       up with each other. */
    {
        ecHeader(&in, ENTITY_KIND_BASE, 5, 1);
        in.u.entityChange.rec.base.x      = 80;
        in.u.entityChange.rec.base.y      = 96;
        in.u.entityChange.rec.base.owner  = 2;
        in.u.entityChange.rec.base.armour = 90;
        in.u.entityChange.rec.base.shells = 89;
        in.u.entityChange.rec.base.mines  = 88;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT_MSG(out.u.entityChange.kind == ENTITY_KIND_BASE,
                      "kind became %u", (unsigned)out.u.entityChange.kind);
        UT_ASSERT(out.u.entityChange.index == 5);
        UT_ASSERT(out.u.entityChange.added == 1);
        UT_ASSERT(out.u.entityChange.rec.base.x == 80);
        UT_ASSERT(out.u.entityChange.rec.base.y == 96);
        UT_ASSERT(out.u.entityChange.rec.base.owner == 2);
        UT_ASSERT_MSG(out.u.entityChange.rec.base.armour == 90,
                      "base armour became %u",
                      (unsigned)out.u.entityChange.rec.base.armour);
        UT_ASSERT_MSG(out.u.entityChange.rec.base.shells == 89,
                      "base shells became %u",
                      (unsigned)out.u.entityChange.rec.base.shells);
        UT_ASSERT_MSG(out.u.entityChange.rec.base.mines == 88,
                      "base mines became %u",
                      (unsigned)out.u.entityChange.rec.base.mines);
    }

    /* A base removed at index 0. */
    {
        ecHeader(&in, ENTITY_KIND_BASE, 0, 0);
        in.u.entityChange.rec.base.x      = 10;
        in.u.entityChange.rec.base.y      = 11;
        in.u.entityChange.rec.base.owner  = NEUTRAL;
        in.u.entityChange.rec.base.armour = 12;
        in.u.entityChange.rec.base.shells = 13;
        in.u.entityChange.rec.base.mines  = 14;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT(out.u.entityChange.index == 0);
        UT_ASSERT(out.u.entityChange.added == 0);
        UT_ASSERT(out.u.entityChange.rec.base.x == 10);
        UT_ASSERT(out.u.entityChange.rec.base.y == 11);
        UT_ASSERT(out.u.entityChange.rec.base.owner == NEUTRAL);
        UT_ASSERT(out.u.entityChange.rec.base.armour == 12);
        UT_ASSERT(out.u.entityChange.rec.base.shells == 13);
        UT_ASSERT(out.u.entityChange.rec.base.mines == 14);
    }

    /* A start added at the top index, with the highest direction there is. */
    {
        ecHeader(&in, ENTITY_KIND_START, MAX_STARTS - 1, 1);
        in.u.entityChange.rec.start.x   = 20;
        in.u.entityChange.rec.start.y   = 21;
        in.u.entityChange.rec.start.dir = 15;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT_MSG(out.u.entityChange.kind == ENTITY_KIND_START,
                      "kind became %u", (unsigned)out.u.entityChange.kind);
        UT_ASSERT(out.u.entityChange.index == MAX_STARTS - 1);
        UT_ASSERT(out.u.entityChange.added == 1);
        UT_ASSERT_MSG(out.u.entityChange.rec.start.x == 20,
                      "start x became %u",
                      (unsigned)out.u.entityChange.rec.start.x);
        UT_ASSERT_MSG(out.u.entityChange.rec.start.y == 21,
                      "start y became %u",
                      (unsigned)out.u.entityChange.rec.start.y);
        UT_ASSERT_MSG(out.u.entityChange.rec.start.dir == 15,
                      "start dir became %u",
                      (unsigned)out.u.entityChange.rec.start.dir);
    }

    /* A start removed. */
    {
        ecHeader(&in, ENTITY_KIND_START, 3, 0);
        in.u.entityChange.rec.start.x   = 100;
        in.u.entityChange.rec.start.y   = 101;
        in.u.entityChange.rec.start.dir = 0;
        UT_ASSERT(ecRoundTrip(&in, &out));
        UT_ASSERT(out.u.entityChange.index == 3);
        UT_ASSERT(out.u.entityChange.added == 0);
        UT_ASSERT(out.u.entityChange.rec.start.x == 100);
        UT_ASSERT(out.u.entityChange.rec.start.y == 101);
        UT_ASSERT(out.u.entityChange.rec.start.dir == 0);
    }

    /* A body of the wrong length, and a kind with no list behind it: both
       refused, because a body this codec cannot place would name an item in
       the wrong list. */
    {
        ControlDecodeBodyFn dec =
            transportControlCodecBodyDecoder(CTRL_ENTITY_CHANGE);
        uint8_t shortBody[EC_BODY_LEN - 1];
        uint8_t longBody[EC_BODY_LEN + 1];
        uint8_t badKind[EC_BODY_LEN];

        UT_ASSERT(dec != NULL);
        memset(shortBody, 0, sizeof(shortBody));
        memset(longBody, 0, sizeof(longBody));
        memset(badKind, 0, sizeof(badKind));
        badKind[0] = ENTITY_KIND_START + 1;

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(shortBody, sizeof(shortBody), &out),
                      "an 8-byte body decoded");
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(longBody, sizeof(longBody), &out),
                      "a 10-byte body decoded");
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(badKind, sizeof(badKind), &out),
                      "kind %u decoded", (unsigned)badKind[0]);
    }

    /* An encoder handed the same unknown kind has no record to place, so it
       skips rather than sending a header the decoder would refuse. */
    {
        ControlEncodeBodyFn enc =
            transportControlCodecBodyEncoder(CTRL_ENTITY_CHANGE);
        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc != NULL);
        ecHeader(&in, (uint8_t)(ENTITY_KIND_START + 1), 0, 1);
        UT_ASSERT_MSG(enc(&in, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK,
                      "an unknown kind encoded %u bytes", (unsigned)outLen);
    }

    return 0;
}

/* ── Client apply ────────────────────────────────────────────────── */

/* A ClientSim with `count` pillboxes and `count` bases on its lists, each on
 * its own square, all live — the state a map install leaves. */
static ClientSim *ecClientWithLists(BYTE count) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    BYTE i;

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);

    pillsSetNumPills(&gs->pb, count);
    basesSetNumBases(&gs->bs, count);
    startsSetNumStarts(&gs->ss, count);
    for (i = 1; i <= count; i++) {
        pillbox pill;
        base bse;
        start st;

        memset(&pill, 0, sizeof(pill));
        pill.x = (BYTE)(40 + i);
        pill.y = 40;
        pill.owner = NEUTRAL;
        pill.armour = PILLS_MAX_ARMOUR;
        pill.speed = PILLBOX_ATTACK_NORMAL;
        pillsSetPill(&gs->pb, &pill, i);

        memset(&bse, 0, sizeof(bse));
        bse.x = (BYTE)(60 + i);
        bse.y = 60;
        bse.owner = NEUTRAL;
        bse.armour = BASE_FULL_ARMOUR;
        bse.shells = BASE_FULL_SHELLS;
        bse.mines = BASE_FULL_MINES;
        basesSetBase(&gs->bs, &bse, i);

        memset(&st, 0, sizeof(st));
        st.x = (BYTE)(80 + i);
        st.y = 80;
        st.dir = 0;
        startsSetStart(&gs->ss, &st, i);
    }
    return cs;
}

/* The add event for one pillbox at `index`. */
static void ecPillAdd(ControlEvent *evt, uint8_t index, BYTE x, BYTE y,
                      BYTE owner, BYTE armour, BYTE speed) {
    ecHeader(evt, ENTITY_KIND_PILL, index, 1);
    evt->u.entityChange.rec.pill.x = x;
    evt->u.entityChange.rec.pill.y = y;
    evt->u.entityChange.rec.pill.owner = owner;
    evt->u.entityChange.rec.pill.armour = armour;
    evt->u.entityChange.rec.pill.speed = speed;
    evt->u.entityChange.rec.pill.inTank = 0;
}

/* An add past the end of the client's list extends it: the count goes up by
 * one and the new number is the one the event named. */
int run_entity_event_client_adds_at_fresh_index(void) {
    ClientSim *cs = ecClientWithLists(3);
    GameSim *gs;
    ControlEvent evt;
    pillbox got;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(pillsGetNumPills(&gs->pb) == 3);

    /* Index 3 is one past the last of the three, so the add appends. */
    ecPillAdd(&evt, 3, 77, 78, NEUTRAL, 10, PILLBOX_ATTACK_NORMAL);
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 4,
                  "the list is %u long after an append",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 4) == TRUE,
                  "the appended pillbox is not on the map");
    memset(&got, 0, sizeof(got));
    pillsGetPill(&gs->pb, &got, 4);
    UT_ASSERT_MSG(got.x == 77 && got.y == 78,
                  "the appended pillbox landed at %u,%u",
                  (unsigned)got.x, (unsigned)got.y);
    UT_ASSERT(got.armour == 10);
    UT_ASSERT(got.speed == PILLBOX_ATTACK_NORMAL);
    UT_ASSERT(got.owner == NEUTRAL);

    /* The three that were already there are untouched. */
    UT_ASSERT(pillsIsActive(&gs->pb, 1) == TRUE);
    UT_ASSERT(pillsIsActive(&gs->pb, 2) == TRUE);
    UT_ASSERT(pillsIsActive(&gs->pb, 3) == TRUE);

    /* A base and a start take the same route. */
    {
        base gotBase;
        start gotStart;
        ecHeader(&evt, ENTITY_KIND_BASE, 3, 1);
        evt.u.entityChange.rec.base.x = 21;
        evt.u.entityChange.rec.base.y = 22;
        evt.u.entityChange.rec.base.owner = 1;
        evt.u.entityChange.rec.base.armour = 23;
        evt.u.entityChange.rec.base.shells = 24;
        evt.u.entityChange.rec.base.mines = 25;
        clientSimApplyControl(cs, &evt);
        UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == 4,
                      "the base list is %u long after an append",
                      (unsigned)basesGetNumBases(&gs->bs));
        UT_ASSERT(basesIsActive(&gs->bs, 4) == TRUE);
        memset(&gotBase, 0, sizeof(gotBase));
        basesGetBase(&gs->bs, &gotBase, 4);
        UT_ASSERT_MSG(gotBase.x == 21 && gotBase.y == 22,
                      "the appended base landed at %u,%u",
                      (unsigned)gotBase.x, (unsigned)gotBase.y);
        UT_ASSERT(gotBase.owner == 1);
        UT_ASSERT(gotBase.armour == 23);
        UT_ASSERT(gotBase.shells == 24);
        UT_ASSERT(gotBase.mines == 25);

        ecHeader(&evt, ENTITY_KIND_START, 3, 1);
        evt.u.entityChange.rec.start.x = 31;
        evt.u.entityChange.rec.start.y = 32;
        evt.u.entityChange.rec.start.dir = 9;
        clientSimApplyControl(cs, &evt);
        UT_ASSERT_MSG(startsGetNumStarts(&gs->ss) == 4,
                      "the start list is %u long after an append",
                      (unsigned)startsGetNumStarts(&gs->ss));
        UT_ASSERT(startsIsActive(&gs->ss, 4) == TRUE);
        memset(&gotStart, 0, sizeof(gotStart));
        startsGetStartStruct(&gs->ss, &gotStart, 4);
        UT_ASSERT_MSG(gotStart.x == 31 && gotStart.y == 32,
                      "the appended start landed at %u,%u",
                      (unsigned)gotStart.x, (unsigned)gotStart.y);
        UT_ASSERT(gotStart.dir == 9);
    }

    clientSimDestroy(cs);
    return 0;
}

/* An add whose index names a removed slot lands back on that slot rather
 * than extending the list — the same choice the server's own add made. */
int run_entity_event_client_add_reuses_removed_slot(void) {
    ClientSim *cs = ecClientWithLists(3);
    GameSim *gs;
    ControlEvent evt;
    pillbox got;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);

    ecHeader(&evt, ENTITY_KIND_PILL, 1, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 2) == FALSE,
                  "pillbox 2 survived its removal");

    ecPillAdd(&evt, 1, 90, 91, 0, 5, 20);
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 3,
                  "reusing a slot changed the count to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 2) == TRUE,
                  "the reused slot is not on the map");
    memset(&got, 0, sizeof(got));
    pillsGetPill(&gs->pb, &got, 2);
    UT_ASSERT_MSG(got.x == 90 && got.y == 91,
                  "the reused slot holds %u,%u",
                  (unsigned)got.x, (unsigned)got.y);
    UT_ASSERT(got.owner == 0);
    UT_ASSERT(got.armour == 5);
    UT_ASSERT(got.speed == 20);

    clientSimDestroy(cs);
    return 0;
}

/* A remove clears the live flag and leaves everything else where it was:
 * the count, the record, and every index above the removed one. */
int run_entity_event_client_remove_keeps_the_slot(void) {
    ClientSim *cs = ecClientWithLists(3);
    GameSim *gs;
    ControlEvent evt;
    pillbox before, after;
    base baseAfter;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);
    memset(&before, 0, sizeof(before));
    pillsGetPill(&gs->pb, &before, 1);

    ecHeader(&evt, ENTITY_KIND_PILL, 0, 0);
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 3,
                  "a removal changed the count to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 1) == FALSE,
                  "the removed pillbox is still on the map");
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 2) == TRUE &&
                      pillsIsActive(&gs->pb, 3) == TRUE,
                  "a removal took the pillboxes above it with it");
    memset(&after, 0, sizeof(after));
    pillsGetPill(&gs->pb, &after, 1);
    UT_ASSERT_MSG(after.x == before.x && after.y == before.y,
                  "the removed slot's square moved to %u,%u",
                  (unsigned)after.x, (unsigned)after.y);

    /* A second removal of the same index is refused and changes nothing. */
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(pillsGetNumPills(&gs->pb) == 3);
    UT_ASSERT(pillsIsActive(&gs->pb, 1) == FALSE);

    /* A base removal reads the same way. */
    ecHeader(&evt, ENTITY_KIND_BASE, 2, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == 3,
                  "a base removal changed the count to %u",
                  (unsigned)basesGetNumBases(&gs->bs));
    UT_ASSERT(basesIsActive(&gs->bs, 3) == FALSE);
    UT_ASSERT(basesIsActive(&gs->bs, 1) == TRUE);
    UT_ASSERT(basesIsActive(&gs->bs, 2) == TRUE);
    memset(&baseAfter, 0, sizeof(baseAfter));
    basesGetBase(&gs->bs, &baseAfter, 3);
    UT_ASSERT(baseAfter.x == 63 && baseAfter.y == 60);

    /* And a start. */
    ecHeader(&evt, ENTITY_KIND_START, 1, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(startsGetNumStarts(&gs->ss) == 3);
    UT_ASSERT(startsIsActive(&gs->ss, 2) == FALSE);
    UT_ASSERT(startsIsActive(&gs->ss, 1) == TRUE);
    UT_ASSERT(startsIsActive(&gs->ss, 3) == TRUE);

    clientSimDestroy(cs);
    return 0;
}

/* The number on the event is the server's, and the client writes the slot it
 * names rather than picking one of its own. Both halves stage a client list
 * that disagrees with the server's, which is the ordinary state of a client
 * that joined after a removal: the map blob marks every item live, so the
 * client has no removed slot where the server has one, and any rule that
 * picked a slot from the client's own list would pick a different one. */
int run_entity_event_client_add_lands_on_the_server_index(void) {
    /* Drift: the server removed its pillbox 2 and put a new one back in that
       slot. This client never heard the removal, so all four of its pillboxes
       are live and its own lowest free slot is off the end of the list. The
       add still has to land on pillbox 2. */
    {
        ClientSim *cs = ecClientWithLists(4);
        GameSim *gs;
        ControlEvent evt;
        pillbox got, kept1, kept3, kept4;

        UT_ASSERT(cs != NULL);
        gs = clientSimGetGameSim(cs);
        UT_ASSERT(pillsGetNumPills(&gs->pb) == 4);
        UT_ASSERT_MSG(pillsIsActive(&gs->pb, 1) == TRUE &&
                          pillsIsActive(&gs->pb, 2) == TRUE &&
                          pillsIsActive(&gs->pb, 3) == TRUE &&
                          pillsIsActive(&gs->pb, 4) == TRUE,
                      "the drift setup needs a list with no removed slot");
        memset(&kept1, 0, sizeof(kept1));
        memset(&kept3, 0, sizeof(kept3));
        memset(&kept4, 0, sizeof(kept4));
        pillsGetPill(&gs->pb, &kept1, 1);
        pillsGetPill(&gs->pb, &kept3, 3);
        pillsGetPill(&gs->pb, &kept4, 4);

        ecPillAdd(&evt, 1, 111, 112, 2, 9, 40);
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 4,
                      "an add inside the count made the list %u long",
                      (unsigned)pillsGetNumPills(&gs->pb));
        UT_ASSERT_MSG(pillsIsActive(&gs->pb, 2) == TRUE,
                      "pillbox 2 is not on the map after its add");
        memset(&got, 0, sizeof(got));
        pillsGetPill(&gs->pb, &got, 2);
        UT_ASSERT_MSG(got.x == 111 && got.y == 112,
                      "the add landed at %u,%u instead of pillbox 2",
                      (unsigned)got.x, (unsigned)got.y);
        UT_ASSERT(got.owner == 2);
        UT_ASSERT(got.armour == 9);
        UT_ASSERT(got.speed == 40);

        /* Nothing else moved. */
        {
            pillbox now1, now3, now4;
            memset(&now1, 0, sizeof(now1));
            memset(&now3, 0, sizeof(now3));
            memset(&now4, 0, sizeof(now4));
            pillsGetPill(&gs->pb, &now1, 1);
            pillsGetPill(&gs->pb, &now3, 3);
            pillsGetPill(&gs->pb, &now4, 4);
            UT_ASSERT_MSG(now1.x == kept1.x && now1.y == kept1.y,
                          "pillbox 1 moved to %u,%u",
                          (unsigned)now1.x, (unsigned)now1.y);
            UT_ASSERT_MSG(now3.x == kept3.x && now3.y == kept3.y,
                          "pillbox 3 moved to %u,%u",
                          (unsigned)now3.x, (unsigned)now3.y);
            UT_ASSERT_MSG(now4.x == kept4.x && now4.y == kept4.y,
                          "pillbox 4 moved to %u,%u",
                          (unsigned)now4.x, (unsigned)now4.y);
            UT_ASSERT(pillsIsActive(&gs->pb, 1) == TRUE);
            UT_ASSERT(pillsIsActive(&gs->pb, 3) == TRUE);
            UT_ASSERT(pillsIsActive(&gs->pb, 4) == TRUE);
        }

        clientSimDestroy(cs);
    }

    /* The mirror: the number is past this client's count, so the client has
       missed more than one event. The count rises to cover the number and the
       slots the gap opens up are removed — they are bases this client has not
       been told about, not bases it has. */
    {
        ClientSim *cs = ecClientWithLists(2);
        GameSim *gs;
        ControlEvent evt;
        base got, kept1, kept2, now1, now2;

        UT_ASSERT(cs != NULL);
        gs = clientSimGetGameSim(cs);
        UT_ASSERT(basesGetNumBases(&gs->bs) == 2);
        memset(&kept1, 0, sizeof(kept1));
        memset(&kept2, 0, sizeof(kept2));
        basesGetBase(&gs->bs, &kept1, 1);
        basesGetBase(&gs->bs, &kept2, 2);

        /* Index 4 is base 5 — two past the end of a two-base list. */
        ecHeader(&evt, ENTITY_KIND_BASE, 4, 1);
        evt.u.entityChange.rec.base.x = 41;
        evt.u.entityChange.rec.base.y = 42;
        evt.u.entityChange.rec.base.owner = 3;
        evt.u.entityChange.rec.base.armour = 43;
        evt.u.entityChange.rec.base.shells = 44;
        evt.u.entityChange.rec.base.mines = 45;
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == 5,
                      "the count is %u, so it does not cover base 5",
                      (unsigned)basesGetNumBases(&gs->bs));
        UT_ASSERT_MSG(basesIsActive(&gs->bs, 5) == TRUE,
                      "base 5 is not on the map after its add");
        memset(&got, 0, sizeof(got));
        basesGetBase(&gs->bs, &got, 5);
        UT_ASSERT_MSG(got.x == 41 && got.y == 42,
                      "base 5 holds %u,%u", (unsigned)got.x, (unsigned)got.y);
        UT_ASSERT(got.owner == 3);
        UT_ASSERT(got.armour == 43);
        UT_ASSERT(got.shells == 44);
        UT_ASSERT(got.mines == 45);

        /* The gap: bases 3 and 4 are inside the count and off the map. */
        UT_ASSERT_MSG(basesIsActive(&gs->bs, 3) == FALSE,
                      "the gap slot 3 came out live");
        UT_ASSERT_MSG(basesIsActive(&gs->bs, 4) == FALSE,
                      "the gap slot 4 came out live");

        /* And the two the client already had are untouched. */
        memset(&now1, 0, sizeof(now1));
        memset(&now2, 0, sizeof(now2));
        basesGetBase(&gs->bs, &now1, 1);
        basesGetBase(&gs->bs, &now2, 2);
        UT_ASSERT(now1.x == kept1.x && now1.y == kept1.y);
        UT_ASSERT(now2.x == kept2.x && now2.y == kept2.y);
        UT_ASSERT(basesIsActive(&gs->bs, 1) == TRUE);
        UT_ASSERT(basesIsActive(&gs->bs, 2) == TRUE);

        clientSimDestroy(cs);
    }

    return 0;
}

/* ── The per-tick delta ──────────────────────────────────────────── */

/* How many events of `type` this tick raised naming item index `idx0`. */
static int ecCountFor(ServerSim *sim, BYTE type, BYTE idx0) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    int found = 0;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type && evs[i].data[0] == idx0) found++;
    }
    return found;
}

/* A live item whose record changes is named by the tick's diff; a removed
 * one is not, however much its slot is written to. The periodic full sync's
 * count stays the authority on how long the list is. */
int run_entity_event_removed_index_sends_no_delta(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    pillbox pill;
    base bse;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) >= 1);
    UT_ASSERT(basesGetNumBases(&sim->sim.bs) >= 1);

    /* One tick to settle the diff's record of the previous frame. */
    serverSimTick(sim);

    /* Live: a changed square is a delta at that index. */
    memset(&pill, 0, sizeof(pill));
    pillsGetPill(&sim->sim.pb, &pill, 1);
    pill.owner = (pill.owner == NEUTRAL) ? 0 : NEUTRAL;
    pillsSetPill(&sim->sim.pb, &pill, 1);
    serverSimTick(sim);
    UT_ASSERT_MSG(ecCountFor(sim, EVENT_PILL_UPDATE, 0) > 0,
                  "a live pillbox's change raised no update");

    /* Removed: the same change raises nothing. */
    UT_ASSERT(pillsRemoveItem(&sim->sim.pb, 1) == TRUE);
    memset(&pill, 0, sizeof(pill));
    pillsGetPill(&sim->sim.pb, &pill, 1);
    pill.owner = (pill.owner == NEUTRAL) ? 0 : NEUTRAL;
    pill.x = (BYTE)(pill.x + 1);
    pillsSetPill(&sim->sim.pb, &pill, 1);
    serverSimTick(sim);
    UT_ASSERT_MSG(ecCountFor(sim, EVENT_PILL_UPDATE, 0) == 0,
                  "a removed pillbox raised %d update(s)",
                  ecCountFor(sim, EVENT_PILL_UPDATE, 0));

    /* Bases, the same way, across both lines the diff can raise. */
    memset(&bse, 0, sizeof(bse));
    basesGetBase(&sim->sim.bs, &bse, 1);
    bse.owner = (bse.owner == NEUTRAL) ? 0 : NEUTRAL;
    basesSetBase(&sim->sim.bs, &bse, 1);
    serverSimTick(sim);
    UT_ASSERT_MSG(ecCountFor(sim, EVENT_BASE_UPDATE, 0) > 0,
                  "a live base's change raised no update");

    UT_ASSERT(basesRemoveItem(&sim->sim.bs, 1) == TRUE);
    memset(&bse, 0, sizeof(bse));
    basesGetBase(&sim->sim.bs, &bse, 1);
    bse.owner = (bse.owner == NEUTRAL) ? 0 : NEUTRAL;
    bse.shells = (BYTE)(bse.shells > 0 ? bse.shells - 1 : 1);
    basesSetBase(&sim->sim.bs, &bse, 1);
    serverSimTick(sim);
    UT_ASSERT_MSG(ecCountFor(sim, EVENT_BASE_UPDATE, 0) == 0,
                  "a removed base raised %d update(s)",
                  ecCountFor(sim, EVENT_BASE_UPDATE, 0));
    UT_ASSERT_MSG(ecCountFor(sim, EVENT_BASE_STOCK, 0) == 0,
                  "a removed base raised %d stock line(s)",
                  ecCountFor(sim, EVENT_BASE_STOCK, 0));

    serverSimDestroy(sim);
    return 0;
}

/* ── Wire corpus ─────────────────────────────────────────────────── */

/* Parse one hex char; -1 if not a hex digit. */
static int ecHexVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one lowercase-hex line into out. Returns the byte count, or -1 on a
 * malformed line. */
static int ecDecodeHexLine(const char *line, uint8_t *out, int outCap) {
    int n = 0;
    while (line[0] && line[0] != '\n' && line[0] != '\r') {
        int hi, lo;
        if (n >= outCap) return -1;
        hi = ecHexVal((unsigned char)line[0]);
        lo = ecHexVal((unsigned char)line[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        line += 2;
    }
    return n;
}

/* The committed golden bytes: every vector decodes, re-encodes to exactly
 * the bytes on disk, and the first vector of each kind is read field by
 * field so a record written in the wrong order cannot pass by round-tripping
 * against itself. */
int run_entity_event_wire_corpus_fixture(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_ENTITY_CHANGE);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ENTITY_CHANGE);
    uint8_t vecs[EC_FIXTURE_MAX][EC_BODY_LEN];
    int lens[EC_FIXTURE_MAX];
    char path[512];
    char line[256];
    FILE *f;
    int n = 0;
    int k;
    int seenPill = 0, seenBase = 0, seenStart = 0;

    UT_ASSERT(enc != NULL);
    UT_ASSERT(dec != NULL);

    snprintf(path, sizeof(path), "%s/entity_change.hex", WB_WIRE_FIXTURE_DIR);
    f = fopen(path, "r");
    UT_ASSERT_MSG(f != NULL, "no fixture at %s", path);
    while (n < EC_FIXTURE_MAX && fgets(line, sizeof(line), f)) {
        int len;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
        len = ecDecodeHexLine(line, vecs[n], EC_BODY_LEN);
        if (len <= 0) {
            fclose(f);
            UT_ASSERT_MSG(0, "malformed hex on line %d of %s", n + 1, path);
        }
        lens[n] = len;
        n++;
    }
    fclose(f);
    UT_ASSERT_MSG(n > 0, "%s holds no vectors", path);

    for (k = 0; k < n; k++) {
        ControlEvent evt;
        uint8_t gen[MAX_CONTROL_PACKET];
        size_t outLen = 0;

        UT_ASSERT_MSG(lens[k] == EC_BODY_LEN,
                      "fixture vector %d is %d bytes", k, lens[k]);
        memset(&evt, 0, sizeof(evt));
        UT_ASSERT_MSG(dec(vecs[k], (size_t)lens[k], &evt),
                      "fixture vector %d did not decode", k);
        memset(gen, 0, sizeof(gen));
        UT_ASSERT_MSG(enc(&evt, NULL, gen, sizeof(gen), &outLen) == ENCODE_OK,
                      "fixture vector %d did not re-encode", k);
        UT_ASSERT_MSG(outLen == (size_t)lens[k],
                      "fixture vector %d re-encoded to %u bytes",
                      k, (unsigned)outLen);
        UT_ASSERT_MSG(memcmp(gen, vecs[k], (size_t)lens[k]) == 0,
                      "fixture vector %d re-encoded to different bytes", k);

        /* The first vector of each kind, read out in full. */
        if (evt.u.entityChange.kind == ENTITY_KIND_PILL && !seenPill) {
            seenPill = 1;
            UT_ASSERT(evt.u.entityChange.index == vecs[k][1]);
            UT_ASSERT(evt.u.entityChange.rec.pill.x      == vecs[k][3]);
            UT_ASSERT(evt.u.entityChange.rec.pill.y      == vecs[k][4]);
            UT_ASSERT(evt.u.entityChange.rec.pill.owner  == vecs[k][5]);
            UT_ASSERT(evt.u.entityChange.rec.pill.armour == vecs[k][6]);
            UT_ASSERT(evt.u.entityChange.rec.pill.speed  == vecs[k][7]);
            UT_ASSERT(evt.u.entityChange.rec.pill.inTank ==
                      (vecs[k][8] ? 1 : 0));
        } else if (evt.u.entityChange.kind == ENTITY_KIND_BASE && !seenBase) {
            seenBase = 1;
            UT_ASSERT(evt.u.entityChange.index == vecs[k][1]);
            UT_ASSERT(evt.u.entityChange.rec.base.x      == vecs[k][3]);
            UT_ASSERT(evt.u.entityChange.rec.base.y      == vecs[k][4]);
            UT_ASSERT(evt.u.entityChange.rec.base.owner  == vecs[k][5]);
            UT_ASSERT(evt.u.entityChange.rec.base.armour == vecs[k][6]);
            UT_ASSERT(evt.u.entityChange.rec.base.shells == vecs[k][7]);
            UT_ASSERT(evt.u.entityChange.rec.base.mines  == vecs[k][8]);
        } else if (evt.u.entityChange.kind == ENTITY_KIND_START && !seenStart) {
            seenStart = 1;
            UT_ASSERT(evt.u.entityChange.index == vecs[k][1]);
            UT_ASSERT(evt.u.entityChange.rec.start.x   == vecs[k][3]);
            UT_ASSERT(evt.u.entityChange.rec.start.y   == vecs[k][4]);
            UT_ASSERT(evt.u.entityChange.rec.start.dir == vecs[k][5]);
            /* A start spends three of the six record bytes; the rest are
               zero on the wire so the body stays one length. */
            UT_ASSERT_MSG(vecs[k][6] == 0 && vecs[k][7] == 0 &&
                              vecs[k][8] == 0,
                          "a start vector carries a non-zero pad");
        }
    }

    UT_ASSERT_MSG(seenPill && seenBase && seenStart,
                  "the fixture is missing a kind (pill=%d base=%d start=%d)",
                  seenPill, seenBase, seenStart);
    return 0;
}

/* An index past the array, from a server that should not have sent one, is
 * refused at the client's lists for every kind and both directions, and the
 * lists are left exactly as they were. */
int run_entity_event_client_refuses_out_of_range_index(void) {
    ClientSim *cs = ecClientWithLists(3);
    GameSim *gs;
    ControlEvent evt;
    static const uint8_t bad[3] = { MAX_PILLS, 200, 255 };
    int i;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);

    for (i = 0; i < 3; i++) {
        ecPillAdd(&evt, bad[i], 50, 50, NEUTRAL, PILLS_MAX_ARMOUR, PILLBOX_ATTACK_NORMAL);
        clientSimApplyControl(cs, &evt);
        UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 3,
                      "a pill add at index %u changed the count to %u",
                      (unsigned)bad[i], (unsigned)pillsGetNumPills(&gs->pb));
        ecHeader(&evt, ENTITY_KIND_PILL, bad[i], 0);
        clientSimApplyControl(cs, &evt);
        ecHeader(&evt, ENTITY_KIND_BASE, bad[i], 0);
        clientSimApplyControl(cs, &evt);
        ecHeader(&evt, ENTITY_KIND_START, bad[i], 0);
        clientSimApplyControl(cs, &evt);
        ecHeader(&evt, ENTITY_KIND_BASE, bad[i], 1);
        clientSimApplyControl(cs, &evt);
        ecHeader(&evt, ENTITY_KIND_START, bad[i], 1);
        clientSimApplyControl(cs, &evt);
    }
    UT_ASSERT(pillsGetNumPills(&gs->pb) == 3 && basesGetNumBases(&gs->bs) == 3 &&
              startsGetNumStarts(&gs->ss) == 3);
    for (i = 1; i <= 3; i++) {
        UT_ASSERT_MSG(pillsIsActive(&gs->pb, (BYTE)i) && basesIsActive(&gs->bs, (BYTE)i) &&
                      startsIsActive(&gs->ss, (BYTE)i),
                      "an out-of-range change touched item %d's flag", i);
    }

    clientSimDestroy(cs);
    return 0;
}
