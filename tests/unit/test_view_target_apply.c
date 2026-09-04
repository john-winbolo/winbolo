/*
 * Coverage for the CTRL_VIEW_TARGET arm in client_sim_control.c — the
 * client half of server-side ally picking. The client no longer chooses
 * which ally to watch (its own idea of who is watchable is only the
 * players it has happened to be sent); it asks with CMD_VIEW_CYCLE and
 * parks the camera on whatever the server names here.
 *
 * Tests build a fresh ClientSim, dispatch a CTRL_VIEW_TARGET through
 * clientSimApplyControl, and read the result back through
 * clientSimGetViewKind / clientSimGetViewTarget, with the camera offsets
 * checking that the answer's map square reached the scroll state.
 *
 * The two rejection cases are the ones that matter in a real game.
 * In-process subscribers (SP-host, bots) receive every published answer,
 * so an answer for another slot must not move this client's camera; and
 * the ally key auto-repeats, so several requests are in flight at any
 * real ping and an answer whose fromEcho does not match what this client
 * is stepping from belongs to an earlier press.
 *
 * A last case covers clientSimAllyViewAwaitingFirstData, which is how the
 * per-tick upkeep tells "the answer has landed but the ally has not streamed
 * yet" from "this ally has stopped qualifying".
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "client_command.h"   /* VIEW_KIND_*, VIEW_CYCLE_FROM_NONE */
#include "client_sim_internal.h" /* clientSimAllyViewAwaitingFirstData, allySeenTick */
#include "control_event.h"
#include "test_harness.h"

/* scrollCenterObject parks the camera SCROLL_CENTER squares up and left of
 * the target, so a square chosen well clear of the origin gives offsets that
 * can be compared without worrying about the BYTE wrap at the map edge. */
#define VT_SCROLL_CENTER 8

static ClientSim *vt_fresh_sim_as_slot(BYTE me) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, me);
    /* Creates this slot's own tank. clientSimCreate does not — only
     * clientSimSetupSelf does — and the not-found answer below returns to the
     * tank view, which needs a tank to centre on. */
    clientSimSetupSelf(cs, me, "Tester", 0, 0);
    return cs;
}

static void vt_make_answer(ControlEvent *evt, uint8_t origSlot, uint8_t target,
                           uint8_t mapX, uint8_t mapY, uint8_t found,
                           uint8_t fromEcho) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_VIEW_TARGET;
    evt->u.viewTarget.origSlot = origSlot;
    evt->u.viewTarget.kind     = (uint8_t)VIEW_KIND_ALLY;
    evt->u.viewTarget.target   = target;
    evt->u.viewTarget.mapX     = mapX;
    evt->u.viewTarget.mapY     = mapY;
    evt->u.viewTarget.found    = found;
    evt->u.viewTarget.fromEcho = fromEcho;
}

/* Puts cs into an ally view on `target` the way the game does: the answer to
 * a press made from the tank view, which echoes VIEW_CYCLE_FROM_NONE. */
static void vt_enter_ally_view(ClientSim *cs, BYTE me, BYTE target,
                               BYTE mapX, BYTE mapY) {
    ControlEvent evt;
    vt_make_answer(&evt, me, target, mapX, mapY, /*found=*/1,
                   (uint8_t)VIEW_CYCLE_FROM_NONE);
    clientSimApplyControl(cs, &evt);
}

int run_view_target_apply(void) {
    /* A found answer addressed to our slot, echoing the FROM_NONE we send
     * from the tank view, parks the camera on the named ally. */
    {
        ClientSim *cs = vt_fresh_sim_as_slot(0);
        UT_ASSERT(cs != NULL);
        UT_ASSERT_MSG(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_TANK,
                      "a fresh ClientSim must start in the tank view");

        ControlEvent evt;
        vt_make_answer(&evt, /*origSlot=*/0, /*target=*/4, /*mapX=*/40,
                       /*mapY=*/30, /*found=*/1,
                       (uint8_t)VIEW_CYCLE_FROM_NONE);
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_ALLY,
                      "a found answer must enter the ally view (kind %u)",
                      (unsigned)clientSimGetViewKind(cs));
        UT_ASSERT_MSG(clientSimGetViewTarget(cs) == 4,
                      "expected to be watching ally 4, got %u",
                      (unsigned)clientSimGetViewTarget(cs));
        UT_ASSERT_MSG(clientSimGetXOffset(cs) == (BYTE)(40 - VT_SCROLL_CENTER),
                      "camera x should centre on the answer's square (got %u)",
                      (unsigned)clientSimGetXOffset(cs));
        UT_ASSERT_MSG(clientSimGetYOffset(cs) == (BYTE)(30 - VT_SCROLL_CENTER),
                      "camera y should centre on the answer's square (got %u)",
                      (unsigned)clientSimGetYOffset(cs));

        clientSimDestroy(cs);
    }

    /* The same answer addressed to another slot changes nothing. In-process
     * subscribers see every publish, so without this every bot and the
     * single-player host would apply someone else's selection. */
    {
        ClientSim *cs = vt_fresh_sim_as_slot(2);
        UT_ASSERT(cs != NULL);

        ControlEvent evt;
        vt_make_answer(&evt, /*origSlot=*/5, /*target=*/4, /*mapX=*/40,
                       /*mapY=*/30, /*found=*/1,
                       (uint8_t)VIEW_CYCLE_FROM_NONE);
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_TANK,
                      "an answer for another slot must not move our view"
                      " (kind %u)",
                      (unsigned)clientSimGetViewKind(cs));

        clientSimDestroy(cs);
    }

    /* With the client already watching ally 3, an answer echoing anything
     * else is the answer to an earlier press and is dropped; the one echoing
     * 3 is the answer to the press we are waiting on and is applied. */
    {
        ClientSim *cs = vt_fresh_sim_as_slot(1);
        UT_ASSERT(cs != NULL);

        vt_enter_ally_view(cs, /*me=*/1, /*target=*/3, /*mapX=*/50,
                           /*mapY=*/50);
        UT_ASSERT(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_ALLY);
        UT_ASSERT(clientSimGetViewTarget(cs) == 3);

        ControlEvent stale;
        vt_make_answer(&stale, /*origSlot=*/1, /*target=*/7, /*mapX=*/20,
                       /*mapY=*/20, /*found=*/1, /*fromEcho=*/9);
        clientSimApplyControl(cs, &stale);
        UT_ASSERT_MSG(clientSimGetViewTarget(cs) == 3,
                      "an answer echoing an ally we are not stepping from must"
                      " be dropped (target became %u)",
                      (unsigned)clientSimGetViewTarget(cs));
        UT_ASSERT_MSG(clientSimGetXOffset(cs) == (BYTE)(50 - VT_SCROLL_CENTER),
                      "a dropped answer must not move the camera (got %u)",
                      (unsigned)clientSimGetXOffset(cs));

        ControlEvent current;
        vt_make_answer(&current, /*origSlot=*/1, /*target=*/7, /*mapX=*/20,
                       /*mapY=*/20, /*found=*/1, /*fromEcho=*/3);
        clientSimApplyControl(cs, &current);
        UT_ASSERT_MSG(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_ALLY,
                      "the matching answer must keep us in the ally view"
                      " (kind %u)",
                      (unsigned)clientSimGetViewKind(cs));
        UT_ASSERT_MSG(clientSimGetViewTarget(cs) == 7,
                      "the answer echoing the ally we are stepping from must"
                      " be applied (target %u)",
                      (unsigned)clientSimGetViewTarget(cs));

        clientSimDestroy(cs);
    }

    /* found = 0 means the server had nothing to offer — every ally dead,
     * un-allied or gone. The client stays on its own tank. */
    {
        ClientSim *cs = vt_fresh_sim_as_slot(0);
        UT_ASSERT(cs != NULL);

        ControlEvent evt;
        vt_make_answer(&evt, /*origSlot=*/0, /*target=*/0, /*mapX=*/0,
                       /*mapY=*/0, /*found=*/0,
                       (uint8_t)VIEW_CYCLE_FROM_NONE);
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_TANK,
                      "a not-found answer must leave the tank view in place"
                      " (kind %u)",
                      (unsigned)clientSimGetViewKind(cs));

        clientSimDestroy(cs);
    }

    /* clientSimAllyViewAwaitingFirstData is what stops the per-tick upkeep
     * closing an ally view before the ally the server picked has streamed. It
     * reads that ally's allySeenTick, which the snapshot path stamps on the
     * first real record for the slot and the round reset clears. This pins the
     * predicate on its own — whether the upkeep in client_ui_events.c uses it
     * correctly is something only playing the game shows. */
    {
        ClientSim *cs = vt_fresh_sim_as_slot(0);
        UT_ASSERT(cs != NULL);

        vt_enter_ally_view(cs, /*me=*/0, /*target=*/6, /*mapX=*/60,
                           /*mapY=*/45);
        UT_ASSERT(clientSimGetViewKind(cs) == (uint8_t)VIEW_KIND_ALLY);
        UT_ASSERT_MSG(clientSimAllyViewAwaitingFirstData(cs) == TRUE,
                      "no record has arrived for ally 6, so the view must be"
                      " held open");

        /* Stands in for that ally's first real snapshot. */
        cs->allySeenTick[6] = 1;
        UT_ASSERT_MSG(clientSimAllyViewAwaitingFirstData(cs) == FALSE,
                      "once a record has arrived the ordinary drop applies"
                      " again");

        /* Clear the stamp again so the view kind is the only thing left that
         * can answer FALSE here. */
        cs->allySeenTick[6] = 0;
        clientSimTankView(cs);
        UT_ASSERT_MSG(clientSimAllyViewAwaitingFirstData(cs) == FALSE,
                      "the tank view is never waiting on an ally");

        clientSimDestroy(cs);
    }

    return 0;
}
