/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CTRL_SCENARIO_RULES on the wire: one fragment of the rules a scenario's own
 * manifest sets, as [seq 1][fragCount 1][count 1] then count rows of
 * [rule 1][value 8, the double's bit pattern most significant byte first].
 *
 * Delivered body-only (no full-packet wrapper, no PACKET_* type), so the
 * functions are resolved through the body tables the live control path uses,
 * the way CTRL_SIM_RULES and the four CTRL_SCN_* are.
 *
 *   scenario_rules_codec — a fragment encoded from a filled event and compared
 *       against bytes written out by hand, then decoded from those same
 *       hand-written bytes and checked field by field. The bytes are composed
 *       here rather than taken from the encoder: a fixture that reads its own
 *       output back through the codec under test passes whenever the two
 *       drift together. The same case refuses a body one byte short, a count
 *       past the row cap, a fragCount of zero, a seq outside its own
 *       fragCount, and a rule index that names no rule; decodes a zero-count
 *       body to an empty set; checks that the decode zeroes the rows above
 *       the count, so a stale row cannot survive a shorter fragment; and
 *       holds a full fragment against the control segment it has to ride in.
 *
 * The segment, not MAX_CONTROL_PACKET, is the ceiling that matters: one
 * control event is one channel segment, and the encoder is handed
 * CHANNEL_CONTROL_SEG less the channel frame's type(1) and bodyLen(2). A
 * fragment measured against the 1400-byte datagram instead would pass here
 * and be dropped at delivery.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "channel_mux.h"   /* CHANNEL_CONTROL_SEG — the real ceiling */
#include "control_event.h"
#include "sim_rules_names.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* The two rules the hand-written body names. Written as literals because the
 * body is, and held against the list below so a reordered SIM_RULE_LIST says
 * which row moved rather than failing as a byte mismatch. */
#define SR_RULE_RELOAD 0x00   /* tank_reload_ticks — a whole number */
#define SR_RULE_ACCEL  0x11   /* tank_accel_rate  — a rate */

/* CTRL_SCENARIO_RULES body: [seq][fragCount][count] then count rows of
 * [rule 1][value 8]. 5.0 is 0x4014000000000000 and 0.25 is
 * 0x3FD0000000000000. A set small enough to fit one fragment is seq 0 of 1. */
static const uint8_t kRulesBody[] = {
    0x00, 0x01, 0x02,                                     /* frag 0 of 1, two rows */
    SR_RULE_RELOAD, 0x40, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    SR_RULE_ACCEL,  0x3F, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* One row on the wire: the rule byte and the eight value bytes. */
#define SR_ROW_LEN 9
/* And the three bytes ahead of the rows. */
#define SR_HDR_LEN 3

/* What the encoder is really handed at delivery: a control segment less the
 * channel frame's type(1) and bodyLen(2). */
#define SR_BODY_CAP (CHANNEL_CONTROL_SEG - 3)

static bool srDecode(const uint8_t *body, size_t len, ControlEvent *out) {
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_SCENARIO_RULES);
    if (dec == NULL) return false;
    return dec(body, len, out);
}

int run_scenario_rules_codec(void) {
    ControlEncodeBodyFn enc;
    ControlEvent        evt;
    ControlEvent        back;
    uint8_t             buf[SR_HDR_LEN + SCN_RULES_FRAG_ROWS * SR_ROW_LEN];
    size_t              outLen = 0;
    int                 i;

    UT_ASSERT_MSG((int)SIM_RULE_tank_reload_ticks == SR_RULE_RELOAD,
                  "tank_reload_ticks is rule %d, not %d — the hand-written "
                  "body names the wrong row",
                  (int)SIM_RULE_tank_reload_ticks, SR_RULE_RELOAD);
    UT_ASSERT_MSG((int)SIM_RULE_tank_accel_rate == SR_RULE_ACCEL,
                  "tank_accel_rate is rule %d, not %d — the hand-written "
                  "body names the wrong row",
                  (int)SIM_RULE_tank_accel_rate, SR_RULE_ACCEL);

    /* One fragment has to fit one control segment; that is the whole of why
       the row cap is a number of its own rather than the rule count. */
    UT_ASSERT_MSG(SR_HDR_LEN + SCN_RULES_FRAG_ROWS * SR_ROW_LEN <= SR_BODY_CAP,
                  "a full fragment is %d bytes, past the %d a control segment "
                  "leaves for a body",
                  SR_HDR_LEN + SCN_RULES_FRAG_ROWS * SR_ROW_LEN,
                  (int)SR_BODY_CAP);

    /* And the fragment count has to cover the rule list, or a full set could
       not be said at all. */
    UT_ASSERT_MSG(CTRL_SCENARIO_RULES_FRAGS_MAX * SCN_RULES_FRAG_ROWS >=
                      CTRL_SCENARIO_RULES_MAX,
                  "%d fragments of %d rows cannot carry %d rules",
                  (int)CTRL_SCENARIO_RULES_FRAGS_MAX,
                  (int)SCN_RULES_FRAG_ROWS, (int)CTRL_SCENARIO_RULES_MAX);

    /* ── encode ── */
    enc = transportControlCodecBodyEncoder(CTRL_SCENARIO_RULES);
    UT_ASSERT_MSG(enc != NULL, "no body encoder is registered");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCENARIO_RULES;
    evt.u.scenarioRules.seq       = 0;
    evt.u.scenarioRules.fragCount = 1;
    evt.u.scenarioRules.count     = 2;
    evt.u.scenarioRules.rule[0]  = SR_RULE_RELOAD;
    evt.u.scenarioRules.value[0] = 5.0;
    evt.u.scenarioRules.rule[1]  = SR_RULE_ACCEL;
    evt.u.scenarioRules.value[1] = 0.25;

    memset(buf, 0xCD, sizeof(buf));
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused a two-row fragment");
    UT_ASSERT_MSG(outLen == sizeof(kRulesBody),
                  "wrote %u bytes, wanted %u",
                  (unsigned)outLen, (unsigned)sizeof(kRulesBody));
    UT_ASSERT_MSG(memcmp(buf, kRulesBody, sizeof(kRulesBody)) == 0,
                  "the encoded body differs from the hand-written one");

    /* A buffer one byte short of the fragment is refused rather than
       half-filled. */
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(kRulesBody) - 1, &outLen) ==
                      ENCODE_OVERFLOW,
                  "the encoder wrote a two-row fragment into a buffer too "
                  "small for it");

    /* The encoder refuses what the decoder would: a fragCount of zero, and a
       seq that names no fragment of its own count. Refused rather than
       trimmed, because a fragment the encoder quietly changed would arrive as
       a different set than the one the sim published. */
    {
        ControlEvent bad = evt;
        bad.u.scenarioRules.fragCount = 0;
        UT_ASSERT_MSG(enc(&bad, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK,
                      "the encoder took a fragCount of zero");
        bad = evt;
        bad.u.scenarioRules.seq       = 2;
        bad.u.scenarioRules.fragCount = 2;
        UT_ASSERT_MSG(enc(&bad, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK,
                      "the encoder took seq 2 of 2, which names no fragment");
        bad = evt;
        bad.u.scenarioRules.count = (uint8_t)(SCN_RULES_FRAG_ROWS + 1);
        UT_ASSERT_MSG(enc(&bad, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK,
                      "the encoder took a count past the row cap");
    }

    /* ── decode, from the hand-written bytes ── */
    memset(&back, 0xCD, sizeof(back));
    UT_ASSERT_MSG(srDecode(kRulesBody, sizeof(kRulesBody), &back),
                  "the hand-written body was refused");
    UT_ASSERT_MSG(back.type == CTRL_SCENARIO_RULES,
                  "the decode set type %d", (int)back.type);
    UT_ASSERT_MSG(back.u.scenarioRules.seq == 0 &&
                      back.u.scenarioRules.fragCount == 1,
                  "the decode read fragment %u of %u, wanted 0 of 1",
                  (unsigned)back.u.scenarioRules.seq,
                  (unsigned)back.u.scenarioRules.fragCount);
    UT_ASSERT_MSG(back.u.scenarioRules.count == 2,
                  "the decode read %u rows, wanted 2",
                  (unsigned)back.u.scenarioRules.count);
    UT_ASSERT_MSG(back.u.scenarioRules.rule[0] == SR_RULE_RELOAD,
                  "row 0 named rule %u",
                  (unsigned)back.u.scenarioRules.rule[0]);
    UT_ASSERT_MSG(back.u.scenarioRules.value[0] == 5.0,
                  "row 0 read %f, wanted 5", back.u.scenarioRules.value[0]);
    UT_ASSERT_MSG(back.u.scenarioRules.rule[1] == SR_RULE_ACCEL,
                  "row 1 named rule %u",
                  (unsigned)back.u.scenarioRules.rule[1]);
    UT_ASSERT_MSG(back.u.scenarioRules.value[1] == 0.25,
                  "row 1 read %f, wanted 0.25",
                  back.u.scenarioRules.value[1]);

    /* Every entry above the count is zeroed in both arrays, so a fragment
       that shrinks cannot leave the row it replaced standing behind the new
       one. The event went into the decode filled with 0xCD, which is what
       makes this worth asking. */
    for (i = 2; i < SCN_RULES_FRAG_ROWS; i++) {
        UT_ASSERT_MSG(back.u.scenarioRules.rule[i] == 0 &&
                          back.u.scenarioRules.value[i] == 0.0,
                      "row %d survived the decode above the count", i);
    }

    /* ── an empty set ── */
    {
        const uint8_t empty[] = { 0x00, 0x01, 0x00 };
        memset(&back, 0xCD, sizeof(back));
        UT_ASSERT_MSG(srDecode(empty, sizeof(empty), &back),
                      "a zero-count body was refused");
        UT_ASSERT_MSG(back.u.scenarioRules.count == 0,
                      "a zero-count body decoded to %u rows",
                      (unsigned)back.u.scenarioRules.count);
        UT_ASSERT_MSG(back.u.scenarioRules.fragCount == 1,
                      "an empty set decoded to %u fragments, wanted 1",
                      (unsigned)back.u.scenarioRules.fragCount);
        UT_ASSERT_MSG(back.u.scenarioRules.rule[0] == 0 &&
                          back.u.scenarioRules.value[0] == 0.0,
                      "a zero-count body left row 0 filled");
    }

    /* ── refusals ── */

    /* One byte short of what the count declares. */
    UT_ASSERT_MSG(!srDecode(kRulesBody, sizeof(kRulesBody) - 1, &back),
                  "a body one byte short of its count was taken");
    /* And a byte over it, which is the same disagreement the other way. */
    {
        uint8_t over[sizeof(kRulesBody) + 1];
        memcpy(over, kRulesBody, sizeof(kRulesBody));
        over[sizeof(kRulesBody)] = 0x00;
        UT_ASSERT_MSG(!srDecode(over, sizeof(over), &back),
                      "a body one byte longer than its count was taken");
    }
    /* A body shorter than the three bytes ahead of the rows. */
    UT_ASSERT_MSG(!srDecode(kRulesBody, 0, &back),
                  "a body with no header at all was taken");
    UT_ASSERT_MSG(!srDecode(kRulesBody, SR_HDR_LEN - 1, &back),
                  "a body one byte short of its header was taken");

    /* A fragCount of zero: no set has no fragments. The body is a valid
       zero-row one otherwise, so only that check can refuse it. */
    {
        const uint8_t noFrags[] = { 0x00, 0x00, 0x00 };
        UT_ASSERT_MSG(!srDecode(noFrags, sizeof(noFrags), &back),
                      "a fragCount of zero was taken");
    }

    /* A seq outside its own fragCount — fragment 2 of 2, which does not
       exist, the pair running 0 and 1. */
    {
        const uint8_t pastEnd[] = { 0x02, 0x02, 0x00 };
        UT_ASSERT_MSG(!srDecode(pastEnd, sizeof(pastEnd), &back),
                      "seq 2 of 2, which names no fragment, was taken");
    }

    /* A count one past the row cap, with a body exactly as long as that count
       says — so the length check cannot be what refuses it. */
    {
        uint8_t big[SR_HDR_LEN + (SCN_RULES_FRAG_ROWS + 1) * SR_ROW_LEN];
        memset(big, 0, sizeof(big));
        big[0] = 0x00;
        big[1] = 0x01;
        big[2] = (uint8_t)(SCN_RULES_FRAG_ROWS + 1);
        UT_ASSERT_MSG(!srDecode(big, sizeof(big), &back),
                      "a count of %d, past the row cap of %d, was taken",
                      SCN_RULES_FRAG_ROWS + 1, SCN_RULES_FRAG_ROWS);
    }

    /* A rule index that names no rule, in a body whose length agrees with its
       count — so the index check is the only thing that can refuse it. */
    {
        uint8_t bad[SR_HDR_LEN + SR_ROW_LEN];
        memset(bad, 0, sizeof(bad));
        bad[0] = 0x00;
        bad[1] = 0x01;
        bad[2] = 0x01;
        bad[SR_HDR_LEN] = (uint8_t)CTRL_SCENARIO_RULES_MAX;  /* past the last */
        UT_ASSERT_MSG(!srDecode(bad, sizeof(bad), &back),
                      "rule index %d, which names no rule, was taken",
                      CTRL_SCENARIO_RULES_MAX);
    }

    /* ── a full fragment, encoded and read back ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCENARIO_RULES;
    evt.u.scenarioRules.seq       = 1;
    evt.u.scenarioRules.fragCount = 3;
    evt.u.scenarioRules.count     = (uint8_t)SCN_RULES_FRAG_ROWS;
    for (i = 0; i < SCN_RULES_FRAG_ROWS; i++) {
        evt.u.scenarioRules.rule[i]  = (uint8_t)i;
        evt.u.scenarioRules.value[i] = (double)i + 0.5;
    }
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused a full fragment");
    UT_ASSERT_MSG(outLen ==
                      (size_t)(SR_HDR_LEN + SCN_RULES_FRAG_ROWS * SR_ROW_LEN),
                  "a full fragment wrote %u bytes", (unsigned)outLen);
    UT_ASSERT_MSG(outLen <= (size_t)SR_BODY_CAP,
                  "a full fragment wrote %u bytes, past the %d a control "
                  "segment leaves for a body",
                  (unsigned)outLen, (int)SR_BODY_CAP);
    memset(&back, 0xCD, sizeof(back));
    UT_ASSERT_MSG(srDecode(buf, outLen, &back), "a full fragment was refused");
    UT_ASSERT_MSG(back.u.scenarioRules.seq == 1 &&
                      back.u.scenarioRules.fragCount == 3,
                  "a full fragment decoded as %u of %u, wanted 1 of 3",
                  (unsigned)back.u.scenarioRules.seq,
                  (unsigned)back.u.scenarioRules.fragCount);
    UT_ASSERT_MSG(back.u.scenarioRules.count == SCN_RULES_FRAG_ROWS,
                  "a full fragment decoded to %u rows",
                  (unsigned)back.u.scenarioRules.count);
    for (i = 0; i < SCN_RULES_FRAG_ROWS; i++) {
        UT_ASSERT_MSG(back.u.scenarioRules.rule[i] == (uint8_t)i &&
                          back.u.scenarioRules.value[i] ==
                              (double)i + 0.5,
                      "row %d did not survive the round trip", i);
    }

    return 0;
}
