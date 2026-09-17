/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * CTRL_SCENARIO_RULES on the wire: the rules a scenario's own manifest sets,
 * as [count 1] then count rows of [rule 1][value 8, the double's bit pattern
 * most significant byte first].
 *
 * Delivered body-only (no full-packet wrapper, no PACKET_* type), so the
 * functions are resolved through the body tables the live control path uses,
 * the way CTRL_SIM_RULES and the four CTRL_SCN_* are.
 *
 *   scenario_rules_codec — a set encoded from a filled event and compared
 *       against bytes written out by hand, then decoded from those same
 *       hand-written bytes and checked field by field. The bytes are composed
 *       here rather than taken from the encoder: a fixture that reads its own
 *       output back through the codec under test passes whenever the two
 *       drift together. The same case refuses a body one byte short, a count
 *       past the row cap, and a rule index that names no rule; decodes a
 *       zero-count body to an empty set; checks that the decode zeroes the
 *       rows above the count, so a stale row cannot survive a shorter set;
 *       and holds a full set against MAX_CONTROL_PACKET.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "sim_rules_names.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* The two rules the hand-written body names. Written as literals because the
 * body is, and held against the list below so a reordered SIM_RULE_LIST says
 * which row moved rather than failing as a byte mismatch. */
#define SR_RULE_RELOAD 0x00   /* tank_reload_ticks — a whole number */
#define SR_RULE_ACCEL  0x0C   /* tank_accel_rate  — a rate */

/* CTRL_SCENARIO_RULES body: [count 1] then count rows of [rule 1][value 8].
 * 5.0 is 0x4014000000000000 and 0.25 is 0x3FD0000000000000. */
static const uint8_t kRulesBody[] = {
    0x02,                                                 /* two rows */
    SR_RULE_RELOAD, 0x40, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    SR_RULE_ACCEL,  0x3F, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* One row on the wire: the rule byte and the eight value bytes. */
#define SR_ROW_LEN 9

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
    uint8_t             buf[1 + CTRL_SCENARIO_RULES_MAX * SR_ROW_LEN];
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

    /* A full set has to fit one control datagram; that is the whole of why
       the row cap is the rule count rather than a number of its own. */
    UT_ASSERT_MSG(1 + CTRL_SCENARIO_RULES_MAX * SR_ROW_LEN <=
                      MAX_CONTROL_PACKET,
                  "a full set is %d bytes, past MAX_CONTROL_PACKET (%d)",
                  1 + CTRL_SCENARIO_RULES_MAX * SR_ROW_LEN,
                  (int)MAX_CONTROL_PACKET);

    /* ── encode ── */
    enc = transportControlCodecBodyEncoder(CTRL_SCENARIO_RULES);
    UT_ASSERT_MSG(enc != NULL, "no body encoder is registered");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCENARIO_RULES;
    evt.u.scenarioRules.count = 2;
    evt.u.scenarioRules.rule[0]  = SR_RULE_RELOAD;
    evt.u.scenarioRules.value[0] = 5.0;
    evt.u.scenarioRules.rule[1]  = SR_RULE_ACCEL;
    evt.u.scenarioRules.value[1] = 0.25;

    memset(buf, 0xCD, sizeof(buf));
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused a two-row set");
    UT_ASSERT_MSG(outLen == sizeof(kRulesBody),
                  "wrote %u bytes, wanted %u",
                  (unsigned)outLen, (unsigned)sizeof(kRulesBody));
    UT_ASSERT_MSG(memcmp(buf, kRulesBody, sizeof(kRulesBody)) == 0,
                  "the encoded body differs from the hand-written one");

    /* A buffer one byte short of the set is refused rather than half-filled. */
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(kRulesBody) - 1, &outLen) ==
                      ENCODE_OVERFLOW,
                  "the encoder wrote a two-row set into a buffer too small "
                  "for it");

    /* ── decode, from the hand-written bytes ── */
    memset(&back, 0xCD, sizeof(back));
    UT_ASSERT_MSG(srDecode(kRulesBody, sizeof(kRulesBody), &back),
                  "the hand-written body was refused");
    UT_ASSERT_MSG(back.type == CTRL_SCENARIO_RULES,
                  "the decode set type %d", (int)back.type);
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

    /* Every entry above the count is zeroed in both arrays, so a set that
       shrinks cannot leave the row it replaced standing behind the new one.
       The event went into the decode filled with 0xCD, which is what makes
       this worth asking. */
    for (i = 2; i < CTRL_SCENARIO_RULES_MAX; i++) {
        UT_ASSERT_MSG(back.u.scenarioRules.rule[i] == 0 &&
                          back.u.scenarioRules.value[i] == 0.0,
                      "row %d survived the decode above the count", i);
    }

    /* ── an empty set ── */
    {
        const uint8_t empty[] = { 0x00 };
        memset(&back, 0xCD, sizeof(back));
        UT_ASSERT_MSG(srDecode(empty, sizeof(empty), &back),
                      "a zero-count body was refused");
        UT_ASSERT_MSG(back.u.scenarioRules.count == 0,
                      "a zero-count body decoded to %u rows",
                      (unsigned)back.u.scenarioRules.count);
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
    /* An empty body: there is not even a count in it. */
    UT_ASSERT_MSG(!srDecode(kRulesBody, 0, &back),
                  "a body with no count byte was taken");

    /* A count one past the cap, with a body exactly as long as that count
       says — so the length check cannot be what refuses it. */
    {
        uint8_t big[1 + (CTRL_SCENARIO_RULES_MAX + 1) * SR_ROW_LEN];
        memset(big, 0, sizeof(big));
        big[0] = (uint8_t)(CTRL_SCENARIO_RULES_MAX + 1);
        UT_ASSERT_MSG(!srDecode(big, sizeof(big), &back),
                      "a count of %d, past the cap of %d, was taken",
                      CTRL_SCENARIO_RULES_MAX + 1, CTRL_SCENARIO_RULES_MAX);
    }

    /* A rule index that names no rule, in a body whose length agrees with its
       count — so the index check is the only thing that can refuse it. */
    {
        uint8_t bad[1 + SR_ROW_LEN];
        memset(bad, 0, sizeof(bad));
        bad[0] = 0x01;
        bad[1] = (uint8_t)CTRL_SCENARIO_RULES_MAX;   /* one past the last */
        UT_ASSERT_MSG(!srDecode(bad, sizeof(bad), &back),
                      "rule index %d, which names no rule, was taken",
                      CTRL_SCENARIO_RULES_MAX);
    }

    /* ── a full set, encoded and read back ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCENARIO_RULES;
    evt.u.scenarioRules.count = (uint8_t)CTRL_SCENARIO_RULES_MAX;
    for (i = 0; i < CTRL_SCENARIO_RULES_MAX; i++) {
        evt.u.scenarioRules.rule[i]  = (uint8_t)i;
        evt.u.scenarioRules.value[i] = (double)i + 0.5;
    }
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused a full set");
    UT_ASSERT_MSG(outLen == (size_t)(1 + CTRL_SCENARIO_RULES_MAX * SR_ROW_LEN),
                  "a full set wrote %u bytes", (unsigned)outLen);
    memset(&back, 0xCD, sizeof(back));
    UT_ASSERT_MSG(srDecode(buf, outLen, &back), "a full set was refused");
    UT_ASSERT_MSG(back.u.scenarioRules.count == CTRL_SCENARIO_RULES_MAX,
                  "a full set decoded to %u rows",
                  (unsigned)back.u.scenarioRules.count);
    for (i = 0; i < CTRL_SCENARIO_RULES_MAX; i++) {
        UT_ASSERT_MSG(back.u.scenarioRules.rule[i] == (uint8_t)i &&
                          back.u.scenarioRules.value[i] ==
                              (double)i + 0.5,
                      "row %d did not survive the round trip", i);
    }

    return 0;
}
