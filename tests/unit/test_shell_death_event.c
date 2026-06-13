/*
 * Coverage for CTRL_SHELL_DEATH — the server's owner-only shell-end
 * notification (control_event.h) and its client handler in
 * client_sim_control.c.
 *
 * The server emits this on collision/expiry so the firing client can
 * match the death to its predicted shell by fireTick, cull the ghost,
 * and draw the impact at the authoritative position. SHELL_OUTCOME_REJECTED
 * is never emitted server-side but is part of the wire contract and the
 * client must handle it forward-compatibly (cull only, no impact), so the
 * rejected path is exercised here as a client-side unit test.
 *
 * Tests:
 *  (a) codec encode/decode round-trips every field;
 *  (b) a matching-fireTick event culls the right predicted shell and only
 *      it, and an event for another owner is a no-op;
 *  (c) a SHELL_OUTCOME_REJECTED event also culls its matching ghost.
 *
 * The handler is cull-only: it draws no impact (the owner already receives
 * the authoritative EVENT_EXPLOSION), so there is nothing to assert about
 * explosions here.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"  /* predictedShells[] / predictedShellCount / sim.expl */
#include "client_sim_control.h"
#include "control_event.h"
#include "shells.h"               /* SHELL_OUTCOME_* */
#include "transport_control_codec.h"
#include "netpacks.h"             /* PACKET_HEADER_SIZE */
#include "test_harness.h"

static ClientSim *fresh_client_sim_as_slot(BYTE me) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, me);
    return cs;
}

/* Seed a predicted shell with a known fireTick directly (the live add
 * path is static and needs a fully-initialised predicted tank). The cull
 * handler keys off fireTick + owner only, so the other fields are inert. */
static void seed_predicted_shell(ClientSim *cs, uint32_t fireTick, BYTE owner) {
    PredictedShell *ps = &cs->predictedShells[cs->predictedShellCount];
    memset(ps, 0, sizeof(*ps));
    ps->fireTick = fireTick;
    ps->owner = owner;
    ps->active = true;
    cs->predictedShellCount++;
}

/* (a) Round-trip a CTRL_SHELL_DEATH through the full encoder (which stamps
 * PACKET_SHELL_DEATH) and the decoder keyed by that wire type. */
int run_shell_death_codec_roundtrip(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_SHELL_DEATH;
    in.u.shellDeath.fireTick = 0x01ABCDEFu;
    in.u.shellDeath.impactWX = 0x1234;
    in.u.shellDeath.impactWY = 0xFEDC;
    in.u.shellDeath.owner    = 7;
    in.u.shellDeath.outcome  = SHELL_OUTCOME_TANK_KILL;

    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_SHELL_DEATH);
    UT_ASSERT_MSG(enc != NULL, "no encoder registered for CTRL_SHELL_DEATH");
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT(outLen >= PACKET_HEADER_SIZE);

    /* packHeader writes the packet type at byte 2. */
    ControlDecodeFn dec = transportControlCodecDecoder(buf[2]);
    UT_ASSERT_MSG(dec != NULL, "no decoder for the encoded packet type");
    memset(&out, 0, sizeof(out));
    UT_ASSERT(dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, &out));

    UT_ASSERT(out.type == CTRL_SHELL_DEATH);
    UT_ASSERT_MSG(out.u.shellDeath.fireTick == 0x01ABCDEFu,
                  "fireTick did not round-trip (got %u)",
                  (unsigned)out.u.shellDeath.fireTick);
    UT_ASSERT(out.u.shellDeath.impactWX == 0x1234);
    UT_ASSERT(out.u.shellDeath.impactWY == 0xFEDC);
    UT_ASSERT(out.u.shellDeath.owner == 7);
    UT_ASSERT(out.u.shellDeath.outcome == SHELL_OUTCOME_TANK_KILL);
    return 0;
}

/* (b) A matching-fireTick event culls exactly the matching predicted shell
 * (swap-with-last), and an event addressed to another owner leaves the
 * predicted shells alone. */
int run_shell_death_culls_matching_predicted_shell(void) {
    ClientSim *cs = fresh_client_sim_as_slot(0);
    UT_ASSERT(cs != NULL);

    seed_predicted_shell(cs, 100, /*owner=*/0);
    seed_predicted_shell(cs, 200, /*owner=*/0);
    seed_predicted_shell(cs, 300, /*owner=*/0);
    UT_ASSERT(clientSimGetPredictedShellCount(cs) == 3);

    /* Event for another slot: must be a no-op. */
    ControlEvent other;
    memset(&other, 0, sizeof(other));
    other.type = CTRL_SHELL_DEATH;
    other.u.shellDeath.owner    = 5;
    other.u.shellDeath.fireTick = 200;
    other.u.shellDeath.outcome  = SHELL_OUTCOME_IMPACT;
    clientSimApplyControl(cs, &other);
    UT_ASSERT_MSG(clientSimGetPredictedShellCount(cs) == 3,
                  "event for another slot must not cull (count=%d)",
                  clientSimGetPredictedShellCount(cs));

    /* Our own shell ends with an impact. */
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SHELL_DEATH;
    evt.u.shellDeath.owner    = 0;
    evt.u.shellDeath.fireTick = 200;
    evt.u.shellDeath.impactWX = 5000;
    evt.u.shellDeath.impactWY = 6000;
    evt.u.shellDeath.outcome  = SHELL_OUTCOME_IMPACT;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetPredictedShellCount(cs) == 2,
                  "exactly one predicted shell should be culled (count=%d)",
                  clientSimGetPredictedShellCount(cs));

    /* fireTick 200 gone; 100 and 300 still present (order-agnostic). */
    const PredictedShell *ps = clientSimGetPredictedShells(cs);
    bool has100 = false, has200 = false, has300 = false;
    for (int i = 0; i < clientSimGetPredictedShellCount(cs); i++) {
        if (ps[i].fireTick == 100) has100 = true;
        if (ps[i].fireTick == 200) has200 = true;
        if (ps[i].fireTick == 300) has300 = true;
    }
    UT_ASSERT_MSG(has100 && has300 && !has200,
                  "wrong shell culled (has100=%d has200=%d has300=%d)",
                  has100, has200, has300);

    clientSimDestroy(cs);
    return 0;
}

/* (c) SHELL_OUTCOME_REJECTED also culls its matching ghost — the handler is
 * outcome-agnostic about the cull. */
int run_shell_death_rejected_culls_without_impact(void) {
    ClientSim *cs = fresh_client_sim_as_slot(0);
    UT_ASSERT(cs != NULL);

    seed_predicted_shell(cs, 555, /*owner=*/0);
    UT_ASSERT(clientSimGetPredictedShellCount(cs) == 1);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SHELL_DEATH;
    evt.u.shellDeath.owner    = 0;
    evt.u.shellDeath.fireTick = 555;
    evt.u.shellDeath.impactWX = 5000;
    evt.u.shellDeath.impactWY = 6000;
    evt.u.shellDeath.outcome  = SHELL_OUTCOME_REJECTED;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetPredictedShellCount(cs) == 0,
                  "rejected fire should cull the matching ghost (count=%d)",
                  clientSimGetPredictedShellCount(cs));

    clientSimDestroy(cs);
    return 0;
}
