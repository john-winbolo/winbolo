/*
 * Entity lifecycle (test_entity_lifecycle.c).
 *
 * Each of the pillbox, base and start containers carries a live flag per
 * slot, past its wire region. Removal is a tombstone: the slot stays, the
 * count stays, and every index above the removed one keeps its number,
 * because those numbers are what the wire and the recordings use to name an
 * item. Addition takes the lowest removed slot before it extends the list.
 *
 * What the cases pin:
 *   - removed_pill_is_gone_from_gameplay: a removed pill does not fire,
 *     cannot be shot, damaged, repaired or picked up, and is not counted by
 *     a rect query.
 *   - removed_base_is_gone_from_gameplay: a removed base does not restock
 *     or refuel, cannot be captured, shot or driven into, masks no armour,
 *     is not in a brain's list, and neither blocks nor completes a base win.
 *   - removed_start_is_never_chosen: neither the batch placement nor the
 *     per-player pickers put a tank on a removed start.
 *   - tournament_removed_start_is_never_chosen: the tournament picker and
 *     its no-candidate fallback likewise.
 *   - tournament_neutral_share_counts_live_bases: the share of neutral bases
 *     that decides the tournament picker's tiers is taken over the bases on
 *     the map, not the slots.
 *   - add_reuses_lowest_removed_slot: the lowest removed slot first, then
 *     an append that raises the count.
 *   - add_refused_when_full: no free slot at MAX_PILLS live items.
 *   - remove_refuses_already_removed: and refuses an index out of range.
 *   - remove_keeps_indices_above: the whole point of the tombstone.
 *   - blob_load_marks_every_item_live: the flags sit past the wire region, so
 *     installing a map blob has to mark them by hand.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "game_sim.h"
#include "gametype.h"     /* gameTournament */
#include "bolo_map.h"
#include "bolo_rand.h"    /* bolo_srand */
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"         /* tankSetShells / tankGetShells */
#include "client_sim.h"   /* the brain object list, and ObjectInfo */
#include "server_sim.h"
#include "server/sim/server_sim_shared.h" /* serverSimWinningOwner */
#include "test_harness.h"

/* pillsIsCapturable is not in pillbox.h — tank.c, its only caller, declares
   it locally. Declared the same way here rather than moved to the header. */
bool pillsIsCapturable(pillboxes *value, BYTE xValue, BYTE yValue);

/* Put `count` pills on the list, each on its own square, all live. */
static void ut_stage_pills(pillboxes *pb, BYTE count) {
    BYTE i;
    pillsSetNumPills(pb, count);
    for (i = 1; i <= count; i++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        item.x = (BYTE)(40 + i);
        item.y = 40;
        item.owner = NEUTRAL;
        item.armour = PILLS_MAX_ARMOUR;
        item.speed = PILLBOX_ATTACK_NORMAL;
        item.inTank = FALSE;
        pillsSetPill(pb, &item, i);
    }
}

/* A removed pill is not on the map: nothing finds it at its square, the
 * firing pass walks past it, and a rect query does not count it. */
int run_entity_removed_pill_is_gone_from_gameplay(void) {
    ServerSim *sim = ut_make_running_sim("Remover");
    GameSim *gs;
    pillbox item;
    BYTE px, py;
    BYTE beforeCount;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard Island should carry at least one pillbox");

    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    item.armour = PILLS_MAX_ARMOUR;
    item.inTank = FALSE;
    pillsSetPill(&gs->pb, &item, 1);
    px = item.x;
    py = item.y;

    /* Live first, so the assertions below are about the flag and not about
       the pill having been somewhere else all along. */
    UT_ASSERT(pillsExistPos(&gs->pb, px, py) == TRUE);
    UT_ASSERT(pillsIsPillHit(&gs->pb, px, py) == TRUE);
    beforeCount = pillsNumInRect(gs, &gs->pb, 0, 255, 0, 255);
    UT_ASSERT_MSG(beforeCount >= 1, "pill 1 should be counted while it is live");

    UT_ASSERT(pillsRemoveItem(&gs->pb, 1) == TRUE);

    UT_ASSERT_MSG(pillsExistPos(&gs->pb, px, py) == FALSE,
                  "a removed pill must not be at its square");
    UT_ASSERT_MSG(pillsViewExistPos(&gs->pb, px, py) == FALSE,
                  "a removed pill must not be drawn at its square");
    UT_ASSERT_MSG(pillsIsPillHit(&gs->pb, px, py) == FALSE,
                  "a removed pill must not be shootable");
    UT_ASSERT_MSG(pillsDeadPos(&gs->pb, px, py) == FALSE,
                  "a removed pill must not read as a dead pill on the ground");
    UT_ASSERT_MSG(pillsIsCapturable(&gs->pb, px, py) == FALSE,
                  "a removed pill must not be capturable");
    UT_ASSERT_MSG(pillsGetPillNum(&gs->pb, px, py, FALSE, FALSE) != 1,
                  "a removed pill must not be found by square");
    UT_ASSERT_MSG(pillsNumInRect(gs, &gs->pb, 0, 255, 0, 255) == beforeCount - 1,
                  "a removed pill must drop out of a rect query");

    /* The firing pass: a live pill's reload counter climbs towards its
       speed every tick. A removed one is skipped, so its counter sits. */
    {
        bool connected[MAX_TANKS];
        BYTE reloadBefore;
        memset(connected, 0, sizeof(connected));
        gs->pb->item[0].reload = 0;
        reloadBefore = gs->pb->item[0].reload;
        pillsUpdate(gs, gs->tanks, connected, MAX_TANKS);
        UT_ASSERT_MSG(gs->pb->item[0].reload == reloadBefore,
                      "a removed pill must not be ticked by the firing pass");
    }

    /* Damage, splash and repair each find a pill by its square, and none of
       them finds a removed one: a shell takes nothing off it and kills
       nothing, an explosion landing on the square leaves its armour alone,
       and the builder carries every tree home. */
    {
        BYTE armourBefore;
        gs->pb->item[0].armour = PILLS_MAX_ARMOUR;
        armourBefore = gs->pb->item[0].armour;
        UT_ASSERT_MSG(pillsDamagePos(gs, px, py, TRUE, TRUE, 0) == FALSE,
                      "a shell reported a kill on a removed pill");
        UT_ASSERT_MSG(gs->pb->item[0].armour == armourBefore,
                      "a shell took armour off a removed pill");
        pillsGetDamagePos(gs, &gs->pb, px, py, PILLS_MAX_ARMOUR);
        UT_ASSERT_MSG(gs->pb->item[0].armour == armourBefore,
                      "an explosion took armour off a removed pill");
        gs->pb->item[0].armour = 1;
        UT_ASSERT_MSG(pillsRepairPos(gs, &gs->pb, px, py, 4) == 4,
                      "the builder spent trees on a removed pill");
        UT_ASSERT_MSG(gs->pb->item[0].armour == 1,
                      "the builder repaired a removed pill");
    }

    /* And the server accessor reports the flag rather than a constant. */
    {
        ServerSimPillInfo info;
        UT_ASSERT(serverSimGetPillInfo(sim, 1, &info) == true);
        UT_ASSERT_MSG(info.active == false,
                      "serverSimGetPillInfo must report a removed pill");
    }

    serverSimDestroy(sim);
    return 0;
}

/* A removed base does not restock, cannot change hands, and is left out of
 * the all-bases-owned win test rather than blocking it forever. */
int run_entity_removed_base_is_gone_from_gameplay(void) {
    ServerSim *sim = ut_make_running_sim("Remover");
    GameSim *gs;
    base item;
    BYTE bx, by;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "Everard Island should carry at least one base");

    memset(&item, 0, sizeof(item));
    basesGetBase(&gs->bs, &item, 1);
    bx = item.x;
    by = item.y;

    UT_ASSERT(basesExistPos(&gs->bs, bx, by) == TRUE);

    /* Park it short of a full load so a restock would show. */
    gs->bs->item[0].armour = 0;
    gs->bs->item[0].shells = 0;
    gs->bs->item[0].mines = 0;
    gs->bs->item[0].owner = NEUTRAL;

    UT_ASSERT(basesRemoveItem(&gs->bs, 1) == TRUE);

    UT_ASSERT_MSG(basesExistPos(&gs->bs, bx, by) == FALSE,
                  "a removed base must not be at its square");
    UT_ASSERT_MSG(basesGetBaseNum(&gs->bs, bx, by) == BASE_NOT_FOUND,
                  "a removed base must not be found by square");
    UT_ASSERT_MSG(baseIsCapturable(&gs->bs, bx, by) == FALSE,
                  "a removed base must not be capturable");

    /* Refuel: the stock tick must leave it alone. */
    basesUpdateStock(gs, 1);
    UT_ASSERT_MSG(gs->bs->item[0].armour == 0 && gs->bs->item[0].shells == 0,
                  "a removed base must not restock");

    /* Capture: the owner setter must refuse it. */
    basesSetBaseOwner(gs, 1, 0, FALSE, FALSE);
    UT_ASSERT_MSG(gs->bs->item[0].owner == NEUTRAL,
                  "a removed base must not change hands");

    /* A shell, the shot and drive tests, the armour mask and a refuel each
       find a base by its square or its index, and none of them finds a
       removed one. The base is stocked and handed to a hostile slot first,
       so a live one would answer yes to each question below. */
    {
        tank *tnk = &gs->tanks[0];
        UT_ASSERT_MSG(*tnk != NULL, "slot 0 has no tank to refuel");
        gs->bs->item[0].owner = 1;
        gs->bs->item[0].armour = BASE_FULL_ARMOUR;
        gs->bs->item[0].shells = BASE_FULL_SHELLS;
        gs->bs->item[0].mines = BASE_FULL_MINES;
        gs->bs->item[0].refuelTime = 0;
        UT_ASSERT_MSG(basesCanHit(gs, bx, by, 0) == FALSE,
                      "a removed base can be shot");
        UT_ASSERT_MSG(basesCantDrive(gs, bx, by, 0) == FALSE,
                      "a removed base blocks a tank");
        basesDamagePos(gs, bx, by, 0);
        UT_ASSERT_MSG(gs->bs->item[0].armour == BASE_FULL_ARMOUR,
                      "a shell took armour off a removed base");
        UT_ASSERT_MSG(basesArmourVisibleToPlayer(gs, 0, 0) == FALSE,
                      "a removed base's armour is sent as the real value");

        /* The refuel: the base is the tank's own and the tank is short of
           shells, which is all a live base needs to hand some over. */
        gs->bs->item[0].owner = 0;
        tankSetShells(tnk, 0);
        basesRefueling(gs, tnk, 1);
        UT_ASSERT_MSG(tankGetShells(tnk) == 0,
                      "a removed base refuelled a tank");
        UT_ASSERT_MSG(gs->bs->item[0].shells == BASE_FULL_SHELLS &&
                          gs->bs->item[0].armour == BASE_FULL_ARMOUR,
                      "a removed base gave up stock");

        /* Back to the neutral, empty slot the base-win check below is
           about. */
        gs->bs->item[0].owner = NEUTRAL;
        gs->bs->item[0].armour = 0;
        gs->bs->item[0].shells = 0;
        gs->bs->item[0].mines = 0;
    }

    /* A brain's base list is built from the bases on the map, so a removed
       one is not in it, whatever its slot still holds. */
    {
        ClientSim *cs = clientSimAlloc();
        ObjectInfo *objects;
        unsigned short *numObjects;
        unsigned short i;
        UT_ASSERT(cs != NULL);
        UT_ASSERT(clientSimCreate(cs) == true);
        objects = clientSimGetBrainObjects(cs);
        numObjects = clientSimGetBrainsNumObjects(cs);
        UT_ASSERT(objects != NULL && numObjects != NULL);
        *numObjects = 0;
        basesGetBrainBaseInRect(cs, gs, 0, 255, 0, 255);
        UT_ASSERT_MSG(*numObjects == (unsigned short)basesGetNumActive(&gs->bs),
                      "a brain saw %u bases, wanted the %u on the map",
                      (unsigned)*numObjects,
                      (unsigned)basesGetNumActive(&gs->bs));
        for (i = 0; i < *numObjects; i++) {
            UT_ASSERT_MSG(!(objects[i].object == BASES_BRAIN_OBJECT_TYPE &&
                            objects[i].idnum == 0),
                          "the removed base is still in the brain's list");
        }
        clientSimDestroy(cs);
    }

    /* Base win: hand every live base to slot 0 and the round is won even
       though base 1 is a neutral, empty, removed slot. */
    {
        BYTE n = basesGetNumBases(&gs->bs);
        BYTE b;
        for (b = 2; b <= n; b++) {
            gs->bs->item[b - 1].owner = 0;
            gs->bs->item[b - 1].armour = BASE_FULL_ARMOUR;
        }
        if (n >= 2) {
            UT_ASSERT_MSG(serverSimWinningOwner(sim) == 0,
                          "a removed base must not block the base win");
        }
    }

    {
        ServerSimBaseInfo info;
        UT_ASSERT(serverSimGetBaseInfo(sim, 1, &info) == true);
        UT_ASSERT_MSG(info.active == false,
                      "serverSimGetBaseInfo must report a removed base");
    }

    serverSimDestroy(sim);
    return 0;
}

/* Neither the batch placement nor the per-player pickers put a tank on a
 * removed start. */
int run_entity_removed_start_is_never_chosen(void) {
    ServerSim *sim = ut_make_running_sim("Remover");
    GameSim *gs;
    BYTE n;
    BYTE keep;
    BYTE i;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    n = startsGetNumStarts(&gs->ss);
    UT_ASSERT_MSG(n >= 2, "Everard Island should carry at least two starts");

    /* Leave exactly the last start live. */
    keep = n;
    for (i = 1; i < keep; i++) {
        UT_ASSERT(startsRemoveItem(&gs->ss, i) == TRUE);
    }

    /* startsGetRandStart draws from the live starts only and does not
       scatter, so the square it returns names the start it picked. */
    {
        BYTE rx = 0, ry = 0;
        TURNTYPE rdir = 0;
        int attempt;
        for (attempt = 0; attempt < 16; attempt++) {
            startsGetRandStart(gs, &gs->ss, &rx, &ry, &rdir);
            UT_ASSERT_MSG(rx == gs->ss->item[keep - 1].x &&
                              ry == gs->ss->item[keep - 1].y,
                          "startsGetRandStart must only draw a live start");
        }
    }

    /* A pending reservation naming a removed start is not honoured, so it
       is left unconsumed and the pickers below choose instead. */
    {
        BYTE rx = 0, ry = 0;
        TURNTYPE rdir = 0;
        gs->pendingStartIdx[0] = 0;   /* 0-based: start 1, which is removed */
        startsGetStart(gs, &gs->ss, &rx, &ry, &rdir, 0);
        UT_ASSERT_MSG(gs->pendingStartIdx[0] == 0,
                      "a reservation on a removed start must not be taken");
    }

    /* The batch placement puts the one connected slot on the live start. */
    {
        bool connected[MAX_TANKS];
        BYTE teamNumber[MAX_TANKS];
        BYTE outStartIdx[MAX_TANKS];
        memset(connected, 0, sizeof(connected));
        memset(teamNumber, 0, sizeof(teamNumber));
        memset(outStartIdx, 0, sizeof(outStartIdx));
        connected[0] = true;
        startsAssignBatch(gs, &gs->ss, connected, teamNumber, outStartIdx,
                          NULL, NULL);
        /* Either it found the one live start or it found nothing; what it
           must never do is name a removed slot. */
        UT_ASSERT_MSG(outStartIdx[0] == (BYTE)(keep - 1) ||
                          outStartIdx[0] == MAX_STARTS,
                      "the batch placement named start %u, which is removed",
                      outStartIdx[0]);
    }

    {
        ServerSimStartInfo info;
        UT_ASSERT(serverSimGetStartInfo(sim, 1, &info) == true);
        UT_ASSERT_MSG(info.active == false,
                      "serverSimGetStartInfo must report a removed start");
        UT_ASSERT(serverSimGetStartInfo(sim, keep, &info) == true);
        UT_ASSERT(info.active == true);
    }

    serverSimDestroy(sim);
    return 0;
}

/* Add takes the lowest removed slot before it extends the list. */
int run_entity_add_reuses_lowest_removed_slot(void) {
    pillboxes pb = NULL;
    pillbox item;
    BYTE got = 0;

    pillsCreate(&pb);
    ut_stage_pills(&pb, 5);

    UT_ASSERT(pillsRemoveItem(&pb, 2) == TRUE);
    UT_ASSERT(pillsRemoveItem(&pb, 4) == TRUE);
    UT_ASSERT(pillsGetNumPills(&pb) == 5);

    memset(&item, 0, sizeof(item));
    item.x = 100; item.y = 100;
    item.owner = NEUTRAL;
    item.armour = PILLS_MAX_ARMOUR;
    item.speed = PILLBOX_ATTACK_NORMAL;

    UT_ASSERT(pillsAddItem(&pb, &item, &got) == TRUE);
    UT_ASSERT_MSG(got == 2, "the lowest removed slot goes first, got %u", got);
    UT_ASSERT(pillsGetNumPills(&pb) == 5);

    UT_ASSERT(pillsAddItem(&pb, &item, &got) == TRUE);
    UT_ASSERT_MSG(got == 4, "then the next removed slot, got %u", got);
    UT_ASSERT(pillsGetNumPills(&pb) == 5);

    UT_ASSERT(pillsAddItem(&pb, &item, &got) == TRUE);
    UT_ASSERT_MSG(got == 6, "then an append, got %u", got);
    UT_ASSERT_MSG(pillsGetNumPills(&pb) == 6,
                  "an append raises the count");

    pillsDestroy(&pb);
    return 0;
}

/* With every slot live there is nowhere to put another one. */
int run_entity_add_refused_when_full(void) {
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;
    pillbox p;
    base b;
    start s;
    BYTE got = 0xEE;

    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    ut_stage_pills(&pb, MAX_PILLS);
    basesSetNumBases(&bs, MAX_BASES);
    startsSetNumStarts(&ss, MAX_STARTS);

    memset(&p, 0, sizeof(p));
    memset(&b, 0, sizeof(b));
    memset(&s, 0, sizeof(s));

    UT_ASSERT_MSG(pillsAddItem(&pb, &p, &got) == FALSE,
                  "no pill slot at MAX_PILLS live");
    UT_ASSERT_MSG(got == 0xEE, "a refused add must not write the out param");
    UT_ASSERT_MSG(basesAddItem(&bs, &b, &got) == FALSE,
                  "no base slot at MAX_BASES live");
    UT_ASSERT_MSG(startsAddItem(&ss, &s, &got) == FALSE,
                  "no start slot at MAX_STARTS live");
    UT_ASSERT(got == 0xEE);

    /* Free one and the add lands in it. */
    UT_ASSERT(pillsRemoveItem(&pb, 7) == TRUE);
    UT_ASSERT(pillsAddItem(&pb, &p, &got) == TRUE);
    UT_ASSERT(got == 7);

    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}

/* Remove refuses an index it has already cleared, and one out of range. */
int run_entity_remove_refuses_already_removed(void) {
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;

    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    ut_stage_pills(&pb, 4);
    basesSetNumBases(&bs, 4);
    startsSetNumStarts(&ss, 4);

    UT_ASSERT(pillsRemoveItem(&pb, 3) == TRUE);
    UT_ASSERT_MSG(pillsRemoveItem(&pb, 3) == FALSE,
                  "a second remove of the same pill is refused");
    UT_ASSERT_MSG(pillsRemoveItem(&pb, 0) == FALSE, "pill 0 is out of range");
    UT_ASSERT_MSG(pillsRemoveItem(&pb, 5) == FALSE,
                  "past the count is out of range");

    UT_ASSERT(basesRemoveItem(&bs, 2) == TRUE);
    UT_ASSERT(basesRemoveItem(&bs, 2) == FALSE);
    UT_ASSERT(basesRemoveItem(&bs, 0) == FALSE);
    UT_ASSERT(basesRemoveItem(&bs, 5) == FALSE);

    UT_ASSERT(startsRemoveItem(&ss, 1) == TRUE);
    UT_ASSERT(startsRemoveItem(&ss, 1) == FALSE);
    UT_ASSERT(startsRemoveItem(&ss, 0) == FALSE);
    UT_ASSERT(startsRemoveItem(&ss, 5) == FALSE);

    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}

/* The tombstone renumbers nothing: the count and every index above the
 * removed one are exactly as they were. */
int run_entity_remove_keeps_indices_above(void) {
    pillboxes pb = NULL;
    BYTE i;
    BYTE xs[6];

    pillsCreate(&pb);
    ut_stage_pills(&pb, 5);
    for (i = 1; i <= 5; i++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        pillsGetPill(&pb, &item, i);
        xs[i] = item.x;
    }

    UT_ASSERT(pillsRemoveItem(&pb, 2) == TRUE);
    UT_ASSERT_MSG(pillsGetNumPills(&pb) == 5,
                  "remove leaves the count alone");

    for (i = 1; i <= 5; i++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        pillsGetPill(&pb, &item, i);
        UT_ASSERT_MSG(item.x == xs[i],
                      "pill %u moved: expected x %u, got %u", i, xs[i], item.x);
        if (i == 2) {
            UT_ASSERT_MSG(pillsIsActive(&pb, i) == FALSE,
                          "pill 2 is the removed one");
        } else {
            UT_ASSERT_MSG(pillsIsActive(&pb, i), "pill %u stays live", i);
        }
    }

    pillsDestroy(&pb);
    return 0;
}

/* A blob is a map, and every item a map carries is on it. The live flags sit
 * past the wire region, so the blob neither carries them nor is reached by the
 * memcpy that installs it: loading has to mark the whole count live by hand.
 * Both halves are staged against that — the container the blob is made from
 * has removals, and the container it is loaded into has one of its own, so a
 * stale flag left behind on either side shows up here. */
int run_entity_blob_load_marks_every_item_live(void) {
    map mp = NULL, mp2 = NULL;
    pillboxes pb = NULL, pb2 = NULL;
    bases bs = NULL, bs2 = NULL;
    starts ss = NULL, ss2 = NULL;
    BYTE *blob = NULL;
    int len;
    int rc = 0;
    BYTE i;

    mapCreate(&mp);   pillsCreate(&pb);   basesCreate(&bs);   startsCreate(&ss);
    mapCreate(&mp2);  pillsCreate(&pb2);  basesCreate(&bs2);  startsCreate(&ss2);
    blob = (BYTE *)malloc(MAP_COMPRESSED_MAX_SIZE);

    if (blob == NULL) {
        rc = 1;
    }

    if (rc == 0) {
        ut_stage_pills(&pb, 4);
        basesSetNumBases(&bs, 4);
        startsSetNumStarts(&ss, 4);
        for (i = 1; i <= 4; i++) {
            base b;
            start s;
            memset(&b, 0, sizeof(b));
            memset(&s, 0, sizeof(s));
            b.x = (BYTE)(60 + i); b.y = 60; b.owner = NEUTRAL;
            s.x = (BYTE)(80 + i); s.y = 80; s.dir = 4;
            basesSetBase(&bs, &b, i);
            startsSetStart(&ss, &s, i);
        }
        if (pillsRemoveItem(&pb, 2) != TRUE ||
            basesRemoveItem(&bs, 3) != TRUE ||
            startsRemoveItem(&ss, 4) != TRUE) {
            fprintf(stderr, "FAIL %s:%d: could not stage removals\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    /* The destination holds a longer list with a removal of its own, at a slot
       the blob does not remove. Loading must clear that removal and must also
       stop the slots past the blob's shorter count reading live. */
    if (rc == 0) {
        ut_stage_pills(&pb2, 8);
        basesSetNumBases(&bs2, 8);
        startsSetNumStarts(&ss2, 8);
        if (pillsRemoveItem(&pb2, 1) != TRUE ||
            basesRemoveItem(&bs2, 1) != TRUE ||
            startsRemoveItem(&ss2, 1) != TRUE) {
            fprintf(stderr, "FAIL %s:%d: could not stage the destination\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    if (rc == 0) {
        len = mapSaveCompressedMap(&mp, &pb, &bs, &ss, blob,
                                   MAP_COMPRESSED_MAX_SIZE);
        if (len <= 0) {
            fprintf(stderr, "FAIL %s:%d: mapSaveCompressedMap returned %d\n",
                    __FILE__, __LINE__, len);
            rc = 1;
        } else if (mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, blob, len)
                   != TRUE) {
            fprintf(stderr, "FAIL %s:%d: mapLoadCompressedMap refused\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    if (rc == 0) {
        if (pillsGetNumPills(&pb2) != 4 || basesGetNumBases(&bs2) != 4 ||
            startsGetNumStarts(&ss2) != 4) {
            fprintf(stderr, "FAIL %s:%d: counts did not survive the blob\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    if (rc == 0) {
        for (i = 1; i <= 4; i++) {
            if (!pillsIsActive(&pb2, i) || !basesIsActive(&bs2, i) ||
                !startsIsActive(&ss2, i)) {
                fprintf(stderr,
                        "FAIL %s:%d: item %u came out of the blob removed "
                        "(pill %d base %d start %d)\n",
                        __FILE__, __LINE__, i, (int)pillsIsActive(&pb2, i),
                        (int)basesIsActive(&bs2, i),
                        (int)startsIsActive(&ss2, i));
                rc = 1;
            }
        }
    }

    /* The slots the longer destination list used to hold are cleared too. The
       predicate answers FALSE for them on the count alone, so read the arrays
       directly to see the flag itself. */
    if (rc == 0) {
        for (i = 4; i < 8; i++) {
            if (pb2->active[i] != FALSE || bs2->active[i] != FALSE ||
                ss2->active[i] != FALSE) {
                fprintf(stderr,
                        "FAIL %s:%d: slot %u past the count still reads live "
                        "(pill %u base %u start %u)\n",
                        __FILE__, __LINE__, i, pb2->active[i], bs2->active[i],
                        ss2->active[i]);
                rc = 1;
            }
        }
    }

    free(blob);
    mapDestroy(&mp);   pillsDestroy(&pb);   basesDestroy(&bs);   startsDestroy(&ss);
    mapDestroy(&mp2);  pillsDestroy(&pb2);  basesDestroy(&bs2);  startsDestroy(&ss2);
    return rc;
}

/* ── The tournament picker ───────────────────────────────────────── */

#define TP_A 0
#define TP_B 1
#define TP_C 2

/* Three starts far apart on deep sea, the map's own pills and bases cleared
 * so only what a case places counts, and the game type that sends the
 * dispatcher to the tournament picker. */
static void ut_tournament_scene(GameSim *gs) {
    static const BYTE sx[3] = {40, 200, 40};
    static const BYTE sy[3] = {40, 40, 200};
    int i;
    for (i = 0; i < 3; i++) {
        mapSetPos(gs, &gs->mp, sx[i], sy[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = sx[i];
        gs->ss->item[i].y = sy[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, 3);
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    gs->game = gameTournament;
}

/* Which of the three start squares the returned square is nearest to. The
 * starts are far apart, so the scatter round one never reaches another. */
static int ut_nearest_of_three(GameSim *gs, BYTE x, BYTE y) {
    int best = -1;
    int bestD = 1 << 30;
    int i;
    for (i = 0; i < 3; i++) {
        int dx = (int)x - gs->ss->item[i].x;
        int dy = (int)y - gs->ss->item[i].y;
        int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

/* One pick for slot 0 through the dispatcher with no reservation and no
 * named start, so the tournament picker is what answers. */
static int ut_tournament_pick(GameSim *gs, uint64_t seed) {
    BYTE x = 0;
    BYTE y = 0;
    TURNTYPE dir = 0;
    gs->pendingStartIdx[0] = MAX_STARTS;
    gs->scenarioStartIdx[0] = MAX_STARTS;
    bolo_srand(seed);
    startsGetStart(gs, &gs->ss, &x, &y, &dir, 0);
    return ut_nearest_of_three(gs, x, y);
}

/* The tournament picker never chooses a removed start, and when nothing
 * qualifies its fallback is the first start on the map rather than slot 0. */
int run_entity_tournament_removed_start_is_never_chosen(void) {
    ServerSim *sim = ut_make_running_sim("Remover");
    GameSim *gs;
    int chosenA = 0;
    int s;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    ut_tournament_scene(gs);
    UT_ASSERT(startsRemoveItem(&gs->ss, 1) == TRUE);

    for (s = 0; s < 64; s++) {
        if (ut_tournament_pick(gs, (uint64_t)(s + 1)) == TP_A) {
            chosenA++;
        }
    }
    UT_ASSERT_MSG(chosenA == 0,
                  "the tournament picker chose a removed start %d times",
                  chosenA);

    /* With the other two starts on land nothing qualifies, and the fallback
       is still not the removed one. */
    mapSetPos(gs, &gs->mp, gs->ss->item[TP_B].x, gs->ss->item[TP_B].y, GRASS, FALSE, TRUE);
    mapSetPos(gs, &gs->mp, gs->ss->item[TP_C].x, gs->ss->item[TP_C].y, GRASS, FALSE, TRUE);
    UT_ASSERT_MSG(ut_tournament_pick(gs, 7) != TP_A,
                  "the tournament fallback placed the tank on a removed start");

    serverSimDestroy(sim);
    return 0;
}

/* The share of neutral bases that decides whether the tournament picker
 * pools own and neutral starts is measured over the bases on the map. With
 * one neutral base in six the player's own base decides and the neutral
 * start is never chosen; take four bases off the map and the same neutral
 * base is one in two, so the two starts are pooled. */
int run_entity_tournament_neutral_share_counts_live_bases(void) {
    static const BYTE bx[6] = {45, 205, 120, 130, 140, 150};
    static const BYTE by[6] = {40, 40, 200, 200, 200, 200};
    ServerSim *sim = ut_make_running_sim("Remover");
    GameSim *gs;
    int chosenA;
    int chosenB;
    int s;
    BYTE i;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    ut_tournament_scene(gs);

    /* Two starts: A with the player's own base beside it, B with a neutral
       base beside it. The other four bases are the player's, far from both. */
    startsSetNumStarts(&gs->ss, 2);
    basesSetNumBases(&gs->bs, 6);
    for (i = 0; i < 6; i++) {
        base b;
        memset(&b, 0, sizeof(b));
        b.x = bx[i];
        b.y = by[i];
        b.owner = (i == 1) ? NEUTRAL : 0;
        b.armour = BASE_FULL_ARMOUR;
        b.shells = BASE_FULL_SHELLS;
        b.mines = BASE_FULL_MINES;
        basesSetBase(&gs->bs, &b, (BYTE)(i + 1));
    }

    chosenA = 0;
    chosenB = 0;
    for (s = 0; s < 32; s++) {
        int p = ut_tournament_pick(gs, (uint64_t)(s + 1));
        if (p == TP_A) chosenA++;
        if (p == TP_B) chosenB++;
    }
    UT_ASSERT_MSG(chosenB == 0,
                  "with one neutral base in six the neutral start was chosen "
                  "%d times", chosenB);
    UT_ASSERT_MSG(chosenA == 32,
                  "the own start was chosen %d times of 32", chosenA);

    for (i = 3; i <= 6; i++) {
        UT_ASSERT(basesRemoveItem(&gs->bs, i) == TRUE);
    }
    chosenA = 0;
    chosenB = 0;
    for (s = 0; s < 32; s++) {
        int p = ut_tournament_pick(gs, (uint64_t)(s + 1));
        if (p == TP_A) chosenA++;
        if (p == TP_B) chosenB++;
    }
    UT_ASSERT_MSG(chosenB > 0,
                  "the neutral start was never chosen: a removed base still "
                  "counts against the neutral share");
    UT_ASSERT_MSG(chosenA > 0, "the own start dropped out of the pool");

    serverSimDestroy(sim);
    return 0;
}
