/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer - Server Check
 *Filename:      lv_server_check.h
 *Purpose:
 *  The "Server Check" picture of the hit boxes: on the
 *  first step of a shell's blast with a builder out on the
 *  blast's square or one of its eight neighbours, what the
 *  server tested and what it found. It shows the kill area
 *  of the rule, the place the builder was tested at, his
 *  present place, a line from the blast to the tested
 *  place, and a label such as "LIVES 147 > 128 (rewind 2)".
 *
 *  A recording with log_BuilderCheck records
 *  (builder_check_record.h) gives the exact test. An older
 *  recording has none, and the viewer makes an estimate
 *  from what it has: the shooter of the shell, the rewind
 *  of the shooter's PlayerLag less SHELL_LGM_REWIND_LESS,
 *  the builder's history and the map. The label then ends
 *  in "estimate".
 *
 *  This file holds the parts with no drawing: the rule by
 *  version, the test, the label text and the fade. The
 *  viewer draws them (draw.c), and the unit tests check
 *  them.
 *
 *  Header only, with no includes but the C library's and
 *  the header-only rule headers, so the viewer (C and C++)
 *  and the unit tests use the same code.
 *********************************************************/

#ifndef LV_SERVER_CHECK_H
#define LV_SERVER_CHECK_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "builder_check_record.h"
#include "hit_rules.h"

/* How long one check stays on the screen, in playback time. It fades out
   over this time, and a pause keeps it as it is. */
#define LV_SC_SHOW_MS 1000

/* The most checks on the screen at one time. */
#define LV_SC_MAX 32

/* The builder rule of the game that made the recording.
   LV_SC_RULE_221: the rule of version 2.2.1 and before. The blast's square
   is solid when it is a building, a half building, a pillbox (dead or
   alive) or a base, and the thing a shell hits does not count.
   LV_SC_RULE_CURRENT: the rule after it (#644). The blast's square is solid
   when the shell hit the thing on it (a pillbox, wall, forest, boat, shore
   or bridge), or it is a building, a half building, a live pillbox or a
   base. */
#define LV_SC_RULE_221     0
#define LV_SC_RULE_CURRENT 1

/* The kinds of label, for the label text (lvServerCheckLabel). */
#define LV_SC_LABEL_CIRCLE     0 /* "{word} {number} > {number2}" or "<=" */
#define LV_SC_LABEL_BOX        1 /* "{word} {number} >= {number2}" or "<" */
#define LV_SC_LABEL_SQUARE     2 /* "{word} off square" or "on square" */
#define LV_SC_LABEL_NOT_TESTED 3 /* "NOT TESTED" */

/* One check, as the picture draws it. */
typedef struct LvServerCheck {
  bool     exact;      /* from a log_BuilderCheck record; false for an estimate */
  uint8_t  builder;    /* the builder's player slot */
  uint8_t  shooter;    /* the shooter's slot, BUILDER_CHECK_NONE for none */
  uint8_t  result;     /* BUILDER_CHECK_* */
  bool     solid;      /* the square rule: the area is the blast's square */
  bool     onSquare;   /* the tested place is on the blast's square */
  int      shape;      /* the lgm_blast_shape rule, for open ground */
  int      pillRange;  /* the pill_range rule, which caps open ground */
  int      rewind;     /* the builder's rewind, in 20 ms entries */
  int32_t  blastX, blastY;     /* world units */
  int32_t  testedX, testedY;
  int32_t  presentX, presentY;
  uint32_t startMs;    /* the playback time of the blast's first step */
} LvServerCheck;

/* The rule of a recording, from its version. A recording with
   log_BuilderCheck records does not need it: each record holds what the
   server found. Version 2.2.1 and before (and a file with no version) is
   LV_SC_RULE_221, a later one LV_SC_RULE_CURRENT. Note: the version number
   of the code was still 2.2.1 when the current rule came in, so a
   recording made with that code and no records gets the 2.2.1 rule here. */
static inline int lvServerCheckRuleFor(int major, int minor, int revision) {
  if (major != 2) {
    return major < 2 ? LV_SC_RULE_221 : LV_SC_RULE_CURRENT;
  }
  if (minor != 2) {
    return minor < 2 ? LV_SC_RULE_221 : LV_SC_RULE_CURRENT;
  }
  return revision <= 1 ? LV_SC_RULE_221 : LV_SC_RULE_CURRENT;
}

/* Whether the blast's square is solid by the rule. squareHit: the shell hit
   the thing on the square. building: a building or a half building. pill:
   a pillbox is on the square; pillAlive: it has armour. base: a base is on
   the square. */
static inline bool lvServerCheckSolid(int rule, bool squareHit, bool building, bool pill,
                                      bool pillAlive, bool base) {
  if (rule == LV_SC_RULE_221) {
    return building || pill || base;
  }
  return squareHit || building || (pill && pillAlive) || base;
}

/* The check a log_BuilderCheck record gives, first shown at startMs. */
static inline void lvServerCheckFromRecord(const BuilderCheckRecord *r, uint32_t startMs,
                                           LvServerCheck *c) {
  memset(c, 0, sizeof(*c));
  c->exact = true;
  c->builder = r->builder;
  c->shooter = r->shooter;
  c->result = r->result;
  c->solid = (r->flags & BUILDER_CHECK_FLAG_SOLID) != 0;
  c->onSquare = (r->flags & BUILDER_CHECK_FLAG_ON_SQUARE) != 0;
  c->shape = r->shape;
  c->pillRange = r->pillRange;
  c->rewind = r->rewind;
  c->blastX = r->blastX;
  c->blastY = r->blastY;
  c->testedX = r->testedX;
  c->testedY = r->testedY;
  c->presentX = r->presentX;
  c->presentY = r->presentY;
  c->startMs = startMs;
}

/* An estimate of the check, for a recording with no records. The caller
   gives the builder, the shooter (BUILDER_CHECK_NONE for a pillbox's shell
   or none known), the rewind (shellLgmRewind of the shooter's rewind; 0 for
   a pillbox's shell), the blast, the place the builder's history has for
   that rewind, his present place, whether the blast's square is solid
   (lvServerCheckSolid) and the two rules. The test is the engine's,
   hitRulesBuilderCaughtShape. The estimate cannot know a refused death. */
static inline void lvServerCheckEstimate(LvServerCheck *c, uint8_t builder, uint8_t shooter,
                                         int rewind, int32_t blastX, int32_t blastY,
                                         int32_t testedX, int32_t testedY,
                                         int32_t presentX, int32_t presentY, bool solid,
                                         int shape, int pillRange, uint32_t startMs) {
  memset(c, 0, sizeof(*c));
  c->exact = false;
  c->builder = builder;
  c->shooter = shooter;
  c->rewind = rewind;
  c->blastX = blastX;
  c->blastY = blastY;
  c->testedX = testedX;
  c->testedY = testedY;
  c->presentX = presentX;
  c->presentY = presentY;
  c->solid = solid;
  c->shape = shape;
  c->pillRange = pillRange;
  c->startMs = startMs;
  c->onSquare = (testedX >> 8) == (blastX >> 8) && (testedY >> 8) == (blastY >> 8);
  c->result = hitRulesBuilderCaughtShape(shape, solid, c->onSquare,
                                         (double)(blastX - testedX),
                                         (double)(blastY - testedY), pillRange)
                  ? BUILDER_CHECK_DIES
                  : BUILDER_CHECK_LIVES;
}

/* The parts of the label: its kind (LV_SC_LABEL_*), whether he was caught,
   the number (his distance from the blast) and the limit (number2). For
   the circle, the distance is rounded down, but a builder who lives at a
   distance that rounds down to the limit shows the limit and 1, so the
   label never says "LIVES 128 > 128". The circle's limit is pill_range
   when pill_range is less than the shape's own radius. For the box, the
   number is the larger of the two axis distances; a builder inside the box
   but past pill_range (a cut corner) shows the circle of pill_range, the
   test that let him live. */
typedef struct {
  int  kind;
  bool caught;
  int  number;
  int  limit;
} LvServerCheckLabelParts;

static inline LvServerCheckLabelParts lvServerCheckLabelPartsOf(const LvServerCheck *c) {
  LvServerCheckLabelParts p;
  double dx = (double)(c->blastX - c->testedX);
  double dy = (double)(c->blastY - c->testedY);
  double d = sqrt(dx * dx + dy * dy);
  int ax = (int)(dx < 0 ? -dx : dx), ay = (int)(dy < 0 ? -dy : dy);

  memset(&p, 0, sizeof(p));
  p.caught = (c->result == BUILDER_CHECK_DIES || c->result == BUILDER_CHECK_REFUSED);
  if (c->result == BUILDER_CHECK_NOT_TESTED) {
    p.kind = LV_SC_LABEL_NOT_TESTED;
    return p;
  }
  if (c->solid) {
    p.kind = LV_SC_LABEL_SQUARE;
    return p;
  }
  p.number = (int)floor(d);
  if (c->shape == LGM_BLAST_SHAPE_MAC_BOX) {
    if (d > (double)c->pillRange && ax < LGM_BLAST_MAC_BOX_HALF &&
        ay < LGM_BLAST_MAC_BOX_HALF) {
      /* In the box, but past pill_range: the cut corner. */
      p.kind = LV_SC_LABEL_CIRCLE;
      p.limit = c->pillRange;
    } else {
      p.kind = LV_SC_LABEL_BOX;
      p.number = ax > ay ? ax : ay;
      p.limit = LGM_BLAST_MAC_BOX_HALF;
    }
  } else {
    int full = (c->shape == LGM_BLAST_SHAPE_MAC_AREA) ? LGM_BLAST_MAC_AREA_RADIUS
                                                     : HIT_RULES_HALF_SQUARE;
    p.kind = LV_SC_LABEL_CIRCLE;
    p.limit = (c->pillRange >= 0 && c->pillRange < full) ? c->pillRange : full;
  }
  if (p.kind == LV_SC_LABEL_CIRCLE && !p.caught && p.number <= p.limit) {
    p.number = p.limit + 1;
  }
  return p;
}

/* The words of the label, in the viewer's language. The formats take
   {string1} (the word), {number} and {number2}; rewind takes {number}.
   The rewind and the estimate parts go after the rest, each after a
   space. */
typedef struct {
  const char *lives;        /* "LIVES" */
  const char *dies;         /* "DIES" */
  const char *refused;      /* "HOST REFUSED" */
  const char *notTested;    /* "NOT TESTED" */
  const char *circleOut;    /* "{string1} {number} > {number2}" */
  const char *circleIn;     /* "{string1} {number} <= {number2}" */
  const char *boxOut;       /* "{string1} {number} >= {number2}" */
  const char *boxIn;        /* "{string1} {number} < {number2}" */
  const char *offSquare;    /* "{string1} off square" */
  const char *onSquare;     /* "{string1} on square" */
  const char *rewind;       /* "(rewind {number})" */
  const char *estimate;     /* "estimate" */
} LvServerCheckTexts;

/* Puts fmt into out with {string1}, {number} and {number2} filled in, after
   the len bytes out holds. Returns the new length. */
static inline size_t lvServerCheckFill(char *out, size_t size, size_t len, const char *fmt,
                                       const char *word, int number, int number2) {
  const char *s = fmt != NULL ? fmt : "";
  while (*s != '\0' && len + 1 < size) {
    char num[16];
    const char *put = NULL;
    size_t skip = 0;
    if (strncmp(s, "{string1}", 9) == 0) {
      put = word != NULL ? word : "";
      skip = 9;
    } else if (strncmp(s, "{number2}", 9) == 0) {
      snprintf(num, sizeof(num), "%d", number2);
      put = num;
      skip = 9;
    } else if (strncmp(s, "{number}", 8) == 0) {
      snprintf(num, sizeof(num), "%d", number);
      put = num;
      skip = 8;
    }
    if (put != NULL) {
      while (*put != '\0' && len + 1 < size) {
        out[len++] = *put++;
      }
      s += skip;
    } else {
      out[len++] = *s++;
    }
  }
  if (size > 0) {
    out[len < size ? len : size - 1] = '\0';
  }
  return len;
}

/* The label of check c, in out. */
static inline void lvServerCheckLabel(const LvServerCheck *c, const LvServerCheckTexts *t,
                                      char *out, size_t size) {
  LvServerCheckLabelParts p = lvServerCheckLabelPartsOf(c);
  const char *word = (c->result == BUILDER_CHECK_REFUSED) ? t->refused
                     : p.caught                          ? t->dies
                                                         : t->lives;
  size_t len = 0;

  if (size == 0) return;
  out[0] = '\0';
  switch (p.kind) {
  case LV_SC_LABEL_NOT_TESTED:
    len = lvServerCheckFill(out, size, len, t->notTested, NULL, 0, 0);
    break;
  case LV_SC_LABEL_SQUARE:
    len = lvServerCheckFill(out, size, len, p.caught ? t->onSquare : t->offSquare, word, 0, 0);
    break;
  case LV_SC_LABEL_BOX:
    len = lvServerCheckFill(out, size, len, p.caught ? t->boxIn : t->boxOut, word, p.number,
                            p.limit);
    break;
  default:
    len = lvServerCheckFill(out, size, len, p.caught ? t->circleIn : t->circleOut, word,
                            p.number, p.limit);
    break;
  }
  if (c->rewind > 0 && p.kind != LV_SC_LABEL_NOT_TESTED) {
    len = lvServerCheckFill(out, size, len, " ", NULL, 0, 0);
    len = lvServerCheckFill(out, size, len, t->rewind, NULL, c->rewind, 0);
  }
  if (!c->exact) {
    len = lvServerCheckFill(out, size, len, " ", NULL, 0, 0);
    len = lvServerCheckFill(out, size, len, t->estimate, NULL, 0, 0);
  }
}

/* How solid the check draws at playback time nowMs: 1 on its first step,
   down to 0 at LV_SC_SHOW_MS. 0 before it starts and after it ends. */
static inline double lvServerCheckAlpha(uint32_t startMs, uint32_t nowMs) {
  uint32_t age;
  if (nowMs < startMs) return 0.0;
  age = nowMs - startMs;
  if (age >= LV_SC_SHOW_MS) return 0.0;
  return 1.0 - (double)age / (double)LV_SC_SHOW_MS;
}

/* The kill area to draw on open ground: a circle of radius *r (the box's
   corners cut to pill_range included), or the box of half side *half when
   *half is above 0. Both can be set for a box that pill_range cuts. The
   square rule draws the blast's square instead. */
static inline void lvServerCheckOpenArea(int shape, int pillRange, int *r, int *half) {
  *half = 0;
  *r = 0;
  if (pillRange < 0) return;
  if (shape == LGM_BLAST_SHAPE_MAC_BOX) {
    *half = LGM_BLAST_MAC_BOX_HALF;
    if ((double)pillRange * pillRange <
        2.0 * LGM_BLAST_MAC_BOX_HALF * LGM_BLAST_MAC_BOX_HALF) {
      *r = pillRange;
    }
    return;
  }
  *r = (shape == LGM_BLAST_SHAPE_MAC_AREA) ? LGM_BLAST_MAC_AREA_RADIUS : HIT_RULES_HALF_SQUARE;
  if (pillRange < *r) *r = pillRange;
}

#endif /* LV_SERVER_CHECK_H */
