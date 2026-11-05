/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The early own shell blast's matching (early_blast.h): how the client
 * finds the server's explosion and sound of an own shell it has
 * already drawn an explosion for, and when it shows the server's too.
 */
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "tank.h"        /* TANK_SUBTRACT */
#include "explosions.h"  /* EXPLOSION_START */
#include "early_blast.h"
#include "test_harness.h"

static void ebSet(EarlyBlastSet *s) {
  memset(s, 0, sizeof(*s));
  earlyBlastReset(s);
}

/* The explosion's place as shells.c works it out from a world place. */
static void ebServerPlace(int32_t x, int32_t y, uint8_t *mx, uint8_t *my, uint8_t *px,
                          uint8_t *py) {
  WORLD conv = (WORLD)(x - TANK_SUBTRACT);
  *mx = (uint8_t)(conv >> TANK_SHIFT_MAPSIZE);
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  *px = (uint8_t)conv;
  conv = (WORLD)(y - TANK_SUBTRACT);
  *my = (uint8_t)(conv >> TANK_SHIFT_MAPSIZE);
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  *py = (uint8_t)conv;
}

/* The header's place for an explosion is the server's, and its middle is
   within a pixel of the world place. */
int run_early_blast_place(void) {
  int32_t x, y;
  UT_ASSERT(EARLY_BLAST_EXPL_OFFSET == TANK_SUBTRACT);
  for (x = 300; x < 60000; x += 997) {
    for (y = 300; y < 60000; y += 1499) {
      uint8_t a[4], b[4];
      int32_t cx, cy;
      ebServerPlace(x, y, &a[0], &a[1], &a[2], &a[3]);
      earlyBlastExplosionPlace(x, y, &b[0], &b[1], &b[2], &b[3]);
      UT_ASSERT(memcmp(a, b, 4) == 0);
      earlyBlastExplosionMiddle(b[0], b[1], b[2], b[3], &cx, &cy);
      UT_ASSERT(x - cx >= 0 && x - cx < 16 && y - cy >= 0 && y - cy < 16);
    }
  }
  return 0;
}

/* With no server place yet, an explosion near the client's place is the
   early blast's; one far off, or a second one, is not. */
int run_early_blast_claim_explosion(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;
  ebSet(&s);
  earlyBlastAdd(&s, 100, 20000, 20000, EARLY_BLAST_SOUND_NONE);

  ebServerPlace(20000 + 400, 20000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py)); /* too far */
  ebServerPlace(20000 + 100, 20000 - 100, &mx, &my, &px, &py);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(s.stats.hidden == 1 && !s.items[0].active);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py)); /* one each */

  /* Two open early blasts: the nearer one takes it. */
  ebSet(&s);
  earlyBlastAdd(&s, 1, 10000, 10000, EARLY_BLAST_SOUND_NONE);
  earlyBlastAdd(&s, 2, 10200, 10000, EARLY_BLAST_SOUND_NONE);
  ebServerPlace(10180, 10000, &mx, &my, &px, &py);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(s.items[0].active && !s.items[1].active);
  return 0;
}

/* CTRL_SHELL_DEATH by fireTick: a near server place makes the match exact;
   a far one shows the server's explosion too. */
int run_early_blast_server_end(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;
  EarlyBlast *b;

  ebSet(&s);
  earlyBlastAdd(&s, 500, 30000, 30000, EARLY_BLAST_SOUND_BUILDING);
  UT_ASSERT(earlyBlastServerEnd(&s, 499, 30000, 30000) == NULL); /* not ours */
  b = earlyBlastServerEnd(&s, 500, 30000 + 200, 30000);
  UT_ASSERT(b != NULL && b->active && b->serverKnown);
  /* Near the client's place but not the server's: another explosion. */
  ebServerPlace(30000, 30000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  ebServerPlace(30000 + 200, 30000, &mx, &my, &px, &py);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));

  /* Far: closed, so the server's explosion is drawn. */
  ebSet(&s);
  earlyBlastAdd(&s, 600, 30000, 30000, EARLY_BLAST_SOUND_BUILDING);
  b = earlyBlastServerEnd(&s, 600, 30000 + 300, 30000);
  UT_ASSERT(b != NULL && !b->active && s.stats.farEnds == 1);
  ebServerPlace(30000 + 300, 30000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  ebServerPlace(30000 + 100, 30000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  /* Exactly one map square away is another place: the server's
     explosion there is drawn. */
  ebSet(&s);
  earlyBlastAdd(&s, 700, 30000, 30000, EARLY_BLAST_SOUND_NONE);
  b = earlyBlastServerEnd(&s, 700, 30000, 30000 + EARLY_BLAST_NEAR_WU);
  UT_ASSERT(b != NULL && !b->active && s.stats.farEnds == 1);
  ebServerPlace(30000, 30000 + EARLY_BLAST_NEAR_WU, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  /* With no server end yet, an explosion one map square from the client's
     place is not the early blast's either. */
  ebSet(&s);
  earlyBlastAdd(&s, 800, 30000, 30000, EARLY_BLAST_SOUND_NONE);
  ebServerPlace(30000 + EARLY_BLAST_NEAR_WU, 30000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(s.items[0].active);
  return 0;
}

/* A hidden explosion whose early blast played a sound leaves one shell hit
   sound unplayed, for a short time; one that played none leaves none. */
int run_early_blast_sound(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;
  int t;

  ebSet(&s);
  UT_ASSERT(!earlyBlastClaimSound(&s));
  earlyBlastAdd(&s, 1, 5000, 5000, EARLY_BLAST_SOUND_NONE);
  ebServerPlace(5000, 5000, &mx, &my, &px, &py);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(!earlyBlastClaimSound(&s)); /* it ran out: no sound */

  earlyBlastAdd(&s, 2, 5000, 5000, EARLY_BLAST_SOUND_TREE);
  UT_ASSERT(!earlyBlastClaimSound(&s)); /* not before its explosion */
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(earlyBlastClaimSound(&s));
  UT_ASSERT(!earlyBlastClaimSound(&s)); /* one only */
  UT_ASSERT(s.stats.sounds == 1);

  earlyBlastAdd(&s, 3, 5000, 5000, EARLY_BLAST_SOUND_TANK);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  for (t = 0; t <= EARLY_BLAST_SOUND_TICKS; t++) {
    earlyBlastTick(&s);
  }
  UT_ASSERT(!earlyBlastClaimSound(&s)); /* too late */
  return 0;
}

/* An early blast with no server explosion closes after the wait, and a
   later explosion at its place is drawn. */
int run_early_blast_timeout(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;
  int t;

  ebSet(&s);
  earlyBlastAdd(&s, 9, 8000, 8000, EARLY_BLAST_SOUND_NONE);
  for (t = 0; t < EARLY_BLAST_WAIT_TICKS; t++) {
    earlyBlastTick(&s);
  }
  UT_ASSERT(s.items[0].active);
  earlyBlastTick(&s);
  UT_ASSERT(!s.items[0].active && s.stats.timedOut == 1);
  ebServerPlace(8000, 8000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));

  /* The ring of EARLY_BLAST_MAX uses the oldest again. */
  ebSet(&s);
  for (t = 0; t < EARLY_BLAST_MAX + 1; t++) {
    earlyBlastAdd(&s, (uint32_t)t, 1000 + t * 1000, 1000, EARLY_BLAST_SOUND_NONE);
  }
  UT_ASSERT(s.items[0].fireTick == (uint32_t)EARLY_BLAST_MAX && s.next == 1);
  UT_ASSERT(EXPLOSION_START == 8);

  /* A new round clears the set and its counts, so an early blast of the
     last round hides no explosion of the new one. */
  earlyBlastTick(&s);
  earlyBlastReset(&s);
  UT_ASSERT(s.next == 0 && s.tick == 0 && s.stats.made == 0);
  for (t = 0; t < EARLY_BLAST_MAX; t++) {
    UT_ASSERT(!s.items[t].active);
  }
  ebServerPlace(2000, 1000, &mx, &my, &px, &py);
  UT_ASSERT(!earlyBlastClaimExplosion(&s, mx, my, px, py));
  return 0;
}
