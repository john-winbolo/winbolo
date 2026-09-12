/*
 * Pillbox repair load (test_pill_repair_load.c).
 *
 * The man used to be handed a load sized to the damage the pill had at the
 * moment the order was given (1 tree for a scratch, 4 for a dead pill). The
 * pill keeps taking fire while he walks, so a load picked from the armour
 * back then arrives short: order a repair on a pill sitting at 11, watch it
 * get shot to 0, and the man turns up with a single tree and leaves it on 4.
 *
 * He now carries a full load and spends only what the pill needs when he
 * gets there, so:
 *   - repair_tops_up_from_arrival_armour: a full load reaches
 *     PILLS_MAX_ARMOUR from any starting armour, and hands back the trees
 *     it didn't need.
 *   - repair_short_load_spends_what_it_has: a load smaller than a full one
 *     (all the tank had) still repairs as far as it goes.
 *   - repair_full_load_covers_a_dead_pill: the constants stay consistent —
 *     a full load has to be worth at least PILLS_MAX_ARMOUR.
 */

#include <stdint.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "game_sim.h"
#include "lgm.h"                  /* LGM_LOAD_PILLREPAIR */
#include "pillbox.h"              /* pillsRepairPos and friends */
#include "server_sim.h"           /* serverSimGetGameSim, serverSimDestroy */
#include "test_harness.h"

/* Park pill 1 on a known armour, repair it with `load` trees, and report
 * where it ended up along with the trees handed back. */
static bool ut_repair_pill(GameSim *gs, BYTE armourBefore, BYTE load,
                           BYTE *outArmour, BYTE *outReturned) {
    pillbox item;

    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    item.armour = armourBefore;
    item.inTank = FALSE;
    pillsSetPill(gs, &gs->pb, &item, 1);

    if (pillsGetArmourPos(&gs->pb, item.x, item.y) != armourBefore) {
        return false;
    }

    *outReturned = pillsRepairPos(gs, &gs->pb, item.x, item.y, load);
    *outArmour = pillsGetArmourPos(&gs->pb, item.x, item.y);
    return true;
}

/* A full load tops the pill up wherever it happens to be on arrival, and
 * the leftovers come home. The armour column is the whole point: every row
 * ends on PILLS_MAX_ARMOUR, including the rows where the pill was shot to
 * pieces (or patched by someone else) after the order went out. */
int run_pill_repair_tops_up_from_arrival_armour(void) {
    static const struct {
        BYTE before;
        BYTE expectArmour;
        BYTE expectReturned;
    } cases[] = {
        {  0, PILLS_MAX_ARMOUR, 0 },   /* shot dead while he walked */
        {  2, PILLS_MAX_ARMOUR, 0 },
        {  3, PILLS_MAX_ARMOUR, 1 },
        {  6, PILLS_MAX_ARMOUR, 1 },
        {  7, PILLS_MAX_ARMOUR, 2 },
        { 10, PILLS_MAX_ARMOUR, 2 },
        { 11, PILLS_MAX_ARMOUR, 3 },
        { 14, PILLS_MAX_ARMOUR, 3 },
        { PILLS_MAX_ARMOUR, PILLS_MAX_ARMOUR, LGM_LOAD_PILLREPAIR },
                                       /* an ally beat him to it */
    };
    int i;

    ServerSim *sim = ut_make_running_sim("Repairer");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) > 0,
                  "Everard Island should carry at least one pillbox");

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        BYTE armour = 0;
        BYTE returned = 0;

        UT_ASSERT_MSG(ut_repair_pill(gs, cases[i].before,
                                     LGM_LOAD_PILLREPAIR,
                                     &armour, &returned) == true,
                      "could not stage pill 1 on armour %u",
                      (unsigned)cases[i].before);

        UT_ASSERT_MSG(armour == cases[i].expectArmour,
                      "armour %u + full load: expected %u, got %u",
                      (unsigned)cases[i].before,
                      (unsigned)cases[i].expectArmour, (unsigned)armour);
        UT_ASSERT_MSG(returned == cases[i].expectReturned,
                      "armour %u + full load: expected %u trees back, got %u",
                      (unsigned)cases[i].before,
                      (unsigned)cases[i].expectReturned, (unsigned)returned);
    }

    serverSimDestroy(sim);
    return 0;
}

/* When the tank couldn't fill the load, the man repairs as far as what he
 * has takes him and keeps nothing back — except where even that short load
 * overshoots, which still returns the difference. */
int run_pill_repair_short_load_spends_what_it_has(void) {
    static const struct {
        BYTE before;
        BYTE load;
        BYTE expectArmour;
        BYTE expectReturned;
    } cases[] = {
        { 0, 1,  PILL_REPAIR_AMOUNT,       0 },
        { 0, 2,  PILL_REPAIR_AMOUNT * 2,   0 },
        { 0, 3,  PILL_REPAIR_AMOUNT * 3,   0 },
        { 12, 2, PILLS_MAX_ARMOUR,         1 },  /* only needed one of them */
    };
    int i;

    ServerSim *sim = ut_make_running_sim("Repairer");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        BYTE armour = 0;
        BYTE returned = 0;

        UT_ASSERT_MSG(ut_repair_pill(gs, cases[i].before, cases[i].load,
                                     &armour, &returned) == true,
                      "could not stage pill 1 on armour %u",
                      (unsigned)cases[i].before);

        UT_ASSERT_MSG(armour == cases[i].expectArmour,
                      "armour %u + %u trees: expected %u, got %u",
                      (unsigned)cases[i].before, (unsigned)cases[i].load,
                      (unsigned)cases[i].expectArmour, (unsigned)armour);
        UT_ASSERT_MSG(returned == cases[i].expectReturned,
                      "armour %u + %u trees: expected %u trees back, got %u",
                      (unsigned)cases[i].before, (unsigned)cases[i].load,
                      (unsigned)cases[i].expectReturned, (unsigned)returned);
    }

    serverSimDestroy(sim);
    return 0;
}

/* The "a full load always finishes the job" guarantee only holds while the
 * load is worth at least a whole pill's armour. Retuning PILL_REPAIR_AMOUNT
 * or PILLS_MAX_ARMOUR without revisiting LGM_LOAD_PILLREPAIR would quietly
 * bring back the arrive-short behaviour. */
int run_pill_repair_full_load_covers_a_dead_pill(void) {
    UT_ASSERT_MSG(LGM_LOAD_PILLREPAIR * PILL_REPAIR_AMOUNT >= PILLS_MAX_ARMOUR,
                  "a full load is worth %d armour, a dead pill needs %d",
                  LGM_LOAD_PILLREPAIR * PILL_REPAIR_AMOUNT, PILLS_MAX_ARMOUR);
    return 0;
}
