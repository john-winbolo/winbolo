/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A scenario rules set too big for one control segment, end to end.
 *
 * One control event is one channel segment, so a set of every rule there is
 * outgrew what CTRL_SCENARIO_RULES could carry as the rule list grew: at nine
 * bytes a row the whole table passed the 1021 bytes a segment leaves for a
 * body, and the encoder's refusal was a silent drop at delivery — the lobby's
 * popup came up empty with nothing said. The set now rides as fragments.
 *
 *   scenario_rules_fragments — a set naming every rule is handed to the sim,
 *       the fragments it publishes are each encoded into a buffer the size of
 *       a real control segment's body and decoded back, and the decoded
 *       fragments are applied to a ClientSim in order. What the client ends
 *       up holding has to be the set that went in, row for row. Going through
 *       the codec at a segment's capacity is the point: a case that applied
 *       the published events directly would pass while the bytes were still
 *       undeliverable.
 *
 *   scenario_rules_fragments_partial — what a reader is holding between two
 *       fragments, and what an interrupted stream leaves behind. A set half
 *       taken must not be visible, and a stream that skips a fragment must
 *       leave the last whole set standing rather than a splice of two.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "channel_mux.h"           /* CHANNEL_CONTROL_SEG — the real ceiling */
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "everard_map.h"
#include "scenario_defs.h"         /* ScnOpSetRule */
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_sim_scenario.h"
#include "sim_rules_names.h"
#include "threads.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* Every fragment of one set, as the sim published them. Sized to the most a
 * set can need, so a run that published more than that overruns nothing and
 * fails on the count instead. */
typedef struct {
    int          count;
    ControlEvent frag[CTRL_SCENARIO_RULES_FRAGS_MAX + 2];
} SrfCapture;

/* Where the subscriber puts what it sees. One capture, bound once at
 * registration: the ctx a subscriber is registered with is fixed, so a case
 * that wants two sets side by side takes a copy of this between publishes
 * rather than publishing into a second one. */
static SrfCapture srfCap;

static void srfCaptureCb(void *ctx, const ControlEvent *evt) {
    SrfCapture *c = (SrfCapture *)ctx;
    if (evt->type != CTRL_SCENARIO_RULES) return;
    if (c->count < (int)(sizeof(c->frag) / sizeof(c->frag[0]))) {
        c->frag[c->count] = *evt;
    }
    c->count++;
}

static ServerSim *srfSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);
    return sim;
}

/* The value this case gives rule i. Distinct per row and not a whole number,
 * so a row that arrives under the wrong index, or a value that lost its
 * fraction on the way, is a mismatch rather than a coincidence. */
static double srfValueFor(int rule) {
    return (double)rule + 0.125;
}

/* Publish a set naming `n` rules. The fragments land in srfCap. */
static void srfPublish(ServerSim *sim, int n) {
    static ScnOpSetRule rules[CTRL_SCENARIO_RULES_MAX];
    int i;

    for (i = 0; i < n; i++) {
        rules[i].rule  = (uint16_t)i;
        rules[i].value = srfValueFor(i);
    }
    srfCap.count = 0;
    threadsWaitForMutex();
    serverSimSetScenarioRules(sim, rules, n);
    threadsReleaseMutex();
}

/* One fragment through the codec at the capacity the delivery path really
 * hands the encoder, and back out again. Returns false if either half
 * refused it. */
static bool srfRoundTrip(const ControlEvent *in, ControlEvent *out) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SCENARIO_RULES);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SCENARIO_RULES);
    /* What udpClientDeliverControl passes: a control segment less the channel
       frame's type(1) and bodyLen(2). */
    uint8_t body[CHANNEL_CONTROL_SEG - 3];
    size_t  len = 0;

    if (enc == NULL || dec == NULL) return false;
    if (enc(in, NULL, body, sizeof(body), &len) != ENCODE_OK) return false;
    return dec(body, len, out);
}

int run_scenario_rules_fragments(void) {
    ServerSim       *sim;
    ClientSim       *cs;
    SubscriberHandle handle;
    int              wanted = (int)CTRL_SCENARIO_RULES_MAX;
    int              rows = 0;
    int              i;

    sim = srfSim();
    UT_ASSERT(sim != NULL);

    memset(&srfCap, 0, sizeof(srfCap));
    handle = serverSimRegisterSubscriber(sim, srfCaptureCb, &srfCap);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the capture could not register");

    /* ── a set naming every rule there is ── */
    srfPublish(sim, wanted);

    UT_ASSERT_MSG(srfCap.count == (int)CTRL_SCENARIO_RULES_FRAGS_MAX,
                  "a set of %d rules published %d fragment(s), wanted %d",
                  wanted, srfCap.count, (int)CTRL_SCENARIO_RULES_FRAGS_MAX);

    /* Each fragment says which of how many it is, carries no more rows than
       one may, and the rows together are the set. */
    for (i = 0; i < srfCap.count; i++) {
        const ControlEvent *f = &srfCap.frag[i];
        UT_ASSERT_MSG(f->u.scenarioRules.seq == (uint8_t)i,
                      "fragment %d says it is seq %u", i,
                      (unsigned)f->u.scenarioRules.seq);
        UT_ASSERT_MSG(f->u.scenarioRules.fragCount == (uint8_t)srfCap.count,
                      "fragment %d says there are %u of them, not %d", i,
                      (unsigned)f->u.scenarioRules.fragCount, srfCap.count);
        UT_ASSERT_MSG(f->u.scenarioRules.count <= (uint8_t)SCN_RULES_FRAG_ROWS,
                      "fragment %d carries %u rows, past the cap of %d", i,
                      (unsigned)f->u.scenarioRules.count,
                      (int)SCN_RULES_FRAG_ROWS);
        rows += (int)f->u.scenarioRules.count;
    }
    UT_ASSERT_MSG(rows == wanted,
                  "the fragments carry %d rows between them, wanted %d",
                  rows, wanted);

    /* ── through the codec at a segment's capacity, then into a client ── */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    for (i = 0; i < srfCap.count; i++) {
        ControlEvent back;
        UT_ASSERT_MSG(srfRoundTrip(&srfCap.frag[i], &back),
                      "fragment %d did not survive a body the size of one "
                      "control segment — this is the drop the fragmenting "
                      "exists to end", i);
        clientSimApplyControl(cs, &back);
    }

    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == wanted,
                  "the client ended up holding %d rules, wanted %d",
                  clientSimGetScenarioRulesCount(cs), wanted);
    for (i = 0; i < wanted; i++) {
        UT_ASSERT_MSG(clientSimGetScenarioRuleIndex(cs, i) == i,
                      "row %d names rule %d", i,
                      clientSimGetScenarioRuleIndex(cs, i));
        UT_ASSERT_MSG(clientSimGetScenarioRuleValue(cs, i) == srfValueFor(i),
                      "row %d reads %f, wanted %f", i,
                      clientSimGetScenarioRuleValue(cs, i), srfValueFor(i));
    }

    /* ── a set that shrinks replaces the whole of the one before it ── */
    srfPublish(sim, 3);
    UT_ASSERT_MSG(srfCap.count == 1,
                  "a three-rule set published %d fragment(s), wanted 1",
                  srfCap.count);
    for (i = 0; i < srfCap.count; i++) {
        ControlEvent back;
        UT_ASSERT_MSG(srfRoundTrip(&srfCap.frag[i], &back),
                      "the short set's fragment %d was refused", i);
        clientSimApplyControl(cs, &back);
    }
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 3,
                  "after a three-rule set the client holds %d rules",
                  clientSimGetScenarioRulesCount(cs));

    /* ── and an empty one empties it ── */
    srfPublish(sim, 0);
    UT_ASSERT_MSG(srfCap.count == 1,
                  "an empty set published %d fragment(s), wanted 1",
                  srfCap.count);
    {
        ControlEvent back;
        UT_ASSERT_MSG(srfRoundTrip(&srfCap.frag[0], &back),
                      "the empty set's fragment was refused");
        clientSimApplyControl(cs, &back);
    }
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 0,
                  "after an empty set the client still holds %d rules",
                  clientSimGetScenarioRulesCount(cs));

    clientSimDestroy(cs);
    serverSimUnregisterSubscriber(sim, handle);
    serverSimDestroy(sim);
    return 0;
}

int run_scenario_rules_fragments_partial(void) {
    ServerSim       *sim;
    ClientSim       *cs;
    SubscriberHandle handle;
    static SrfCapture big;
    int              i;

    sim = srfSim();
    UT_ASSERT(sim != NULL);
    memset(&srfCap, 0, sizeof(srfCap));
    handle = serverSimRegisterSubscriber(sim, srfCaptureCb, &srfCap);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the capture could not register");

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    /* A small set the client is holding, to have something to lose. */
    srfPublish(sim, 2);
    UT_ASSERT_MSG(srfCap.count == 1, "a two-rule set took %d fragments",
                  srfCap.count);
    clientSimApplyControl(cs, &srfCap.frag[0]);
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 2,
                  "the client did not take the two-rule set");

    /* A set that needs more than one fragment, to interrupt. Copied out of
       the capture because the next publish overwrites it. */
    srfPublish(sim, (int)CTRL_SCENARIO_RULES_MAX);
    big = srfCap;
    UT_ASSERT_MSG(big.count >= 2,
                  "a full set took %d fragment(s); this case needs at least 2",
                  big.count);

    /* ── half taken is not shown ── */
    clientSimApplyControl(cs, &big.frag[0]);
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 2,
                  "after one fragment of a new set the client is showing %d "
                  "rules — a set half taken must not be visible",
                  clientSimGetScenarioRulesCount(cs));
    UT_ASSERT_MSG(clientSimGetScenarioRuleValue(cs, 0) == srfValueFor(0),
                  "the set in hand changed under a fragment of the next one");

    /* ── and a stream that skips a fragment leaves it standing ── */
    clientSimApplyControl(cs, &big.frag[big.count - 1]);   /* the gap */
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 2,
                  "a stream that skipped a fragment left the client holding "
                  "%d rules — a partial set must be thrown away, not spliced",
                  clientSimGetScenarioRulesCount(cs));

    /* The aborted stream must not have poisoned the next whole one. */
    for (i = 0; i < big.count; i++) {
        clientSimApplyControl(cs, &big.frag[i]);
    }
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) ==
                      (int)CTRL_SCENARIO_RULES_MAX,
                  "after an aborted stream a whole one landed as %d rules, "
                  "wanted %d", clientSimGetScenarioRulesCount(cs),
                  (int)CTRL_SCENARIO_RULES_MAX);

    clientSimDestroy(cs);
    serverSimUnregisterSubscriber(sim, handle);
    serverSimDestroy(sim);
    return 0;
}
