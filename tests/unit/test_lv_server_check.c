/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Log Viewer's Server Check (src/logviewer/lv_server_check.h), drawn
 * with Hit Boxes: what the server tested when a shell's blast came near a
 * builder. The exact check comes from a log_BuilderCheck record; a
 * recording with no records gets an estimate by the rule of its version.
 * These tests check the rule by version, the estimate of the Oil Rig case
 * at tick 6830, the exact check from a record, and the label text.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "shell_lgm_rewind.h"
#include "lv_server_check.h"
#include "test_harness.h"

/* The English words of src/gui/sdl3/lang.c (STR_LV_SC_*). */
static LvServerCheckTexts lscTexts(void) {
  LvServerCheckTexts t;
  t.lives = "LIVES";
  t.dies = "DIES";
  t.refused = "HOST REFUSED";
  t.notTested = "NOT TESTED";
  t.circleOut = "{string1} {number} > {number2}";
  t.circleIn = "{string1} {number} <= {number2}";
  t.boxOut = "{string1} {number} >= {number2}";
  t.boxIn = "{string1} {number} < {number2}";
  t.offSquare = "{string1} off square";
  t.onSquare = "{string1} on square";
  t.rewind = "(rewind {number})";
  t.estimate = "estimate";
  return t;
}

/* The label of c compared with want. */
static int lscLabelIs(const LvServerCheck *c, const char *want) {
  LvServerCheckTexts t = lscTexts();
  char got[160];
  lvServerCheckLabel(c, &t, got, sizeof(got));
  UT_ASSERT_MSG(strcmp(got, want) == 0, "label \"%s\", want \"%s\"", got, want);
  return 0;
}

/* An estimate with no rewind, with the blast at (b, b) and the builder at
   (b - dx, b - dy), by the given rules. */
#define LSC_B 32896   /* the middle of square 128 */
static LvServerCheck lscOpen(int dx, int dy, int shape, int pillRange) {
  LvServerCheck c;
  lvServerCheckEstimate(&c, 0, 1, 0, LSC_B, LSC_B, LSC_B - dx, LSC_B - dy, LSC_B - dx,
                        LSC_B - dy, false, shape, pillRange, 0);
  c.exact = true;   /* no " estimate" part, so the label tests read short */
  return c;
}

/* The rule by version: 2.2.1 and before (and no version) is the 2.2.1
   rule; a later one is the current rule. The two rules differ in which
   squares are solid. */
int run_lv_server_check_rule_version(void) {
  UT_ASSERT(lvServerCheckRuleFor(0, 0, 0) == LV_SC_RULE_221);
  UT_ASSERT(lvServerCheckRuleFor(1, 9, 9) == LV_SC_RULE_221);
  UT_ASSERT(lvServerCheckRuleFor(2, 1, 9) == LV_SC_RULE_221);
  UT_ASSERT(lvServerCheckRuleFor(2, 2, 0) == LV_SC_RULE_221);
  UT_ASSERT(lvServerCheckRuleFor(2, 2, 1) == LV_SC_RULE_221);
  UT_ASSERT(lvServerCheckRuleFor(2, 2, 2) == LV_SC_RULE_CURRENT);
  UT_ASSERT(lvServerCheckRuleFor(2, 3, 0) == LV_SC_RULE_CURRENT);
  UT_ASSERT(lvServerCheckRuleFor(3, 0, 0) == LV_SC_RULE_CURRENT);

  /* A shell that hits the thing on a square: solid only by the current
     rule. */
  UT_ASSERT(!lvServerCheckSolid(LV_SC_RULE_221, true, false, false, false, false));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_CURRENT, true, false, false, false, false));
  /* A dead pillbox: solid only by the 2.2.1 rule. A live one by both. */
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_221, false, false, true, false, false));
  UT_ASSERT(!lvServerCheckSolid(LV_SC_RULE_CURRENT, false, false, true, false, false));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_221, false, false, true, true, false));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_CURRENT, false, false, true, true, false));
  /* A building and a base: solid by both. Open ground: by neither. */
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_221, false, true, false, false, false));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_CURRENT, false, true, false, false, false));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_221, false, false, false, false, true));
  UT_ASSERT(lvServerCheckSolid(LV_SC_RULE_CURRENT, false, false, false, false, true));
  UT_ASSERT(!lvServerCheckSolid(LV_SC_RULE_221, false, false, false, false, false));
  UT_ASSERT(!lvServerCheckSolid(LV_SC_RULE_CURRENT, false, false, false, false, false));
  return 0;
}

/* The Oil Rig case at tick 6830 ("2026-11-19 19.28 DH-Oil Rig"), made up
   from its numbers: the explosion record is at (33792, 32256), so the
   blast is at (33920, 32384). The shooter's PlayerLag rewind is 3, so the
   builder test rewinds 2 entries. Builder 0 is at (33872, 32496) now and
   was at (33888, 32528) 2 entries back. The shell hit the thing on its
   square, on no building, pillbox or base. */
int run_lv_server_check_estimate_6830(void) {
  const int32_t bx = 33792 + 128, by = 32256 + 128;
  const int32_t tx = 33888, ty = 32528, px = 33872, py = 32496;
  const int rewind = (int)shellLgmRewind(3);
  bool solid;
  LvServerCheck c;

  UT_ASSERT_MSG(rewind == 2, "rewind %d (want 2)", rewind);
  UT_ASSERT(builderCheckNearSquare(tx >> 8, ty >> 8, bx >> 8, by >> 8));

  /* The 2.2.1 rule: open ground, a circle of 128. He is 147.5 away. */
  solid = lvServerCheckSolid(LV_SC_RULE_221, true, false, false, false, false);
  lvServerCheckEstimate(&c, 0, 2, rewind, bx, by, tx, ty, px, py, solid, 0, 2560, 6830u * 20u);
  UT_ASSERT(!c.exact && !c.solid && !c.onSquare);
  UT_ASSERT_MSG(c.result == BUILDER_CHECK_LIVES, "result %u", (unsigned)c.result);
  if (lscLabelIs(&c, "LIVES 147 > 128 (rewind 2) estimate") != 0) return 1;

  /* The current rule: the hit square is solid, and he is off it. */
  solid = lvServerCheckSolid(LV_SC_RULE_CURRENT, true, false, false, false, false);
  lvServerCheckEstimate(&c, 0, 2, rewind, bx, by, tx, ty, px, py, solid, 0, 2560, 6830u * 20u);
  UT_ASSERT(c.solid && !c.onSquare);
  UT_ASSERT_MSG(c.result == BUILDER_CHECK_LIVES, "result %u", (unsigned)c.result);
  if (lscLabelIs(&c, "LIVES off square (rewind 2) estimate") != 0) return 1;

  /* It shows for one second of playback time and fades. */
  UT_ASSERT(lvServerCheckAlpha(c.startMs, c.startMs) == 1.0);
  UT_ASSERT(lvServerCheckAlpha(c.startMs, c.startMs + 500) == 0.5);
  UT_ASSERT(lvServerCheckAlpha(c.startMs, c.startMs + LV_SC_SHOW_MS) == 0.0);
  UT_ASSERT(lvServerCheckAlpha(c.startMs, c.startMs - 1) == 0.0);
  return 0;
}

/* The exact check from a log_BuilderCheck record: the record's own result
   and places, through its bytes, with no " estimate" part. */
int run_lv_server_check_exact_record(void) {
  BuilderCheckRecord rec, back;
  uint8_t bytes[BUILDER_CHECK_RECORD_BYTES];
  LvServerCheck c;

  memset(&rec, 0, sizeof(rec));
  rec.builder = 0;
  rec.shooter = 2;
  rec.pill = BUILDER_CHECK_NONE;
  rec.result = BUILDER_CHECK_LIVES;
  rec.flags = BUILDER_CHECK_FLAG_SOLID | BUILDER_CHECK_FLAG_REWOUND |
              BUILDER_CHECK_FLAG_SQUARE_HIT;
  rec.shape = 0;
  rec.rewind = 2;
  rec.shellRewind = 3;
  rec.blastX = 33920;
  rec.blastY = 32384;
  rec.testedX = 33888;
  rec.testedY = 32528;
  rec.presentX = 33872;
  rec.presentY = 32496;
  rec.pillRange = 2560;
  UT_ASSERT(builderCheckRecordEncode(&rec, bytes, (int)sizeof(bytes)) ==
            BUILDER_CHECK_RECORD_BYTES);
  UT_ASSERT(builderCheckRecordDecode(bytes, (int)sizeof(bytes), &back));
  lvServerCheckFromRecord(&back, 136600u, &c);
  UT_ASSERT(c.exact && c.solid && !c.onSquare);
  UT_ASSERT(c.builder == 0 && c.shooter == 2 && c.rewind == 2 && c.startMs == 136600u);
  UT_ASSERT(c.blastX == 33920 && c.blastY == 32384);
  UT_ASSERT(c.testedX == 33888 && c.testedY == 32528);
  UT_ASSERT(c.presentX == 33872 && c.presentY == 32496);
  UT_ASSERT(c.shape == 0 && c.pillRange == 2560);
  if (lscLabelIs(&c, "LIVES off square (rewind 2)") != 0) return 1;

  /* The record's result wins over the viewer's own test: a death on open
     ground 90 away. */
  rec.result = BUILDER_CHECK_DIES;
  rec.flags = BUILDER_CHECK_FLAG_REWOUND;
  rec.testedX = 33920 - 90;
  rec.testedY = 32384;
  lvServerCheckFromRecord(&rec, 0, &c);
  if (lscLabelIs(&c, "DIES 90 <= 128 (rewind 2)") != 0) return 1;
  /* The host refused that death. */
  rec.result = BUILDER_CHECK_REFUSED;
  lvServerCheckFromRecord(&rec, 0, &c);
  if (lscLabelIs(&c, "HOST REFUSED 90 <= 128 (rewind 2)") != 0) return 1;
  /* The shooter's screen showed him in his tank: no rewind part. */
  rec.result = BUILDER_CHECK_NOT_TESTED;
  lvServerCheckFromRecord(&rec, 0, &c);
  if (lscLabelIs(&c, "NOT TESTED") != 0) return 1;
  return 0;
}

/* The label text of each kind, from the engine's own test. */
int run_lv_server_check_label(void) {
  LvServerCheck c;

  /* The circle of 128, edge included. */
  c = lscOpen(90, 0, 0, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_DIES);
  if (lscLabelIs(&c, "DIES 90 <= 128") != 0) return 1;
  c = lscOpen(128, 0, 0, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_DIES);
  if (lscLabelIs(&c, "DIES 128 <= 128") != 0) return 1;
  /* 128.004 away: he lives, and the label never says "128 > 128". */
  c = lscOpen(128, 1, 0, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_LIVES);
  if (lscLabelIs(&c, "LIVES 129 > 128") != 0) return 1;
  /* pill_range under 128 cuts the circle. */
  c = lscOpen(110, 0, 0, 100);
  UT_ASSERT(c.result == BUILDER_CHECK_LIVES);
  if (lscLabelIs(&c, "LIVES 110 > 100") != 0) return 1;
  /* The circle of 144. */
  c = lscOpen(140, 0, 2, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_DIES);
  if (lscLabelIs(&c, "DIES 140 <= 144") != 0) return 1;

  /* The Mac box: under 128 on each axis. */
  c = lscOpen(127, 100, 1, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_DIES);
  if (lscLabelIs(&c, "DIES 127 < 128") != 0) return 1;
  c = lscOpen(-128, 0, 1, 2560);
  UT_ASSERT(c.result == BUILDER_CHECK_LIVES);
  if (lscLabelIs(&c, "LIVES 128 >= 128") != 0) return 1;
  /* In the box, but past pill_range: the cut corner shows the circle. */
  c = lscOpen(120, 120, 1, 150);
  UT_ASSERT(c.result == BUILDER_CHECK_LIVES);
  if (lscLabelIs(&c, "LIVES 169 > 150") != 0) return 1;

  /* The square rule: on the square or off it. */
  lvServerCheckEstimate(&c, 0, 1, 0, LSC_B, LSC_B, LSC_B + 100, LSC_B, LSC_B + 100, LSC_B, true,
                        0, 2560, 0);
  UT_ASSERT(c.onSquare && c.result == BUILDER_CHECK_DIES);
  if (lscLabelIs(&c, "DIES on square estimate") != 0) return 1;
  lvServerCheckEstimate(&c, 0, 1, 3, LSC_B, LSC_B, LSC_B + 200, LSC_B, LSC_B + 200, LSC_B, true,
                        0, 2560, 0);
  UT_ASSERT(!c.onSquare && c.result == BUILDER_CHECK_LIVES);
  if (lscLabelIs(&c, "LIVES off square (rewind 3) estimate") != 0) return 1;

  /* A small buffer cuts the label and ends it. */
  {
    LvServerCheckTexts t = lscTexts();
    char cut[6];
    lvServerCheckLabel(&c, &t, cut, sizeof(cut));
    UT_ASSERT_MSG(strcmp(cut, "LIVES") == 0, "cut label \"%s\"", cut);
  }
  return 0;
}
