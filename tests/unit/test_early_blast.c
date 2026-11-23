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
#include "client_sim.h"
#include "client_sim_internal.h"  /* predictedShells[], interpCtx, earlyBlast */
#include "interpolation.h"
#include "bolo_map.h"             /* mapSetPos */
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
   sound of that type unplayed, for a short time; one that played none
   leaves none. The listener here is square (20, 20), and the explosions
   are on square (19, 19): one square west, pan 0, distance band 5. */
#define EB_LX 20
#define EB_LY 20
int run_early_blast_sound(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;
  int t;

  ebSet(&s);
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 5, EB_LX, EB_LY, true));
  earlyBlastAdd(&s, 1, 5000, 5000, EARLY_BLAST_SOUND_NONE);
  ebServerPlace(5000, 5000, &mx, &my, &px, &py);
  UT_ASSERT(mx == 19 && my == 19);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  /* it ran out: no sound, so no credit */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_BUILDING, 0, 5, EB_LX, EB_LY, true));

  earlyBlastAdd(&s, 2, 5000, 5000, EARLY_BLAST_SOUND_TREE);
  /* not before its explosion */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 5, EB_LX, EB_LY, true));
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 5, EB_LX, EB_LY, true));
  /* one only */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 5, EB_LX, EB_LY, true));
  UT_ASSERT(s.stats.sounds == 1);

  earlyBlastAdd(&s, 3, 5000, 5000, EARLY_BLAST_SOUND_TANK);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  for (t = 0; t <= EARLY_BLAST_SOUND_TICKS; t++) {
    earlyBlastTick(&s);
  }
  /* too late */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TANK, 0, 5, EB_LX, EB_LY, true));
  return 0;
}

/* A sound credit is used only by a sound of its own type whose pan and
   distance band fit the hidden explosion's square. */
int run_early_blast_sound_type_place(void) {
  EarlyBlastSet s;
  uint8_t mx, my, px, py;

  ebSet(&s);
  earlyBlastAdd(&s, 1, 5000, 5000, EARLY_BLAST_SOUND_BUILDING);
  ebServerPlace(5000, 5000, &mx, &my, &px, &py);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));

  /* Another type: a tree or a tank hit from the same place keeps playing. */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 5, EB_LX, EB_LY, true));
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TANK, 0, 5, EB_LX, EB_LY, true));
  /* Another place: east of the listener, not west. */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_BUILDING, 6, 10, EB_LX, EB_LY, true));
  /* Another place: west, but in a far distance band. */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_BUILDING, -8, 25, EB_LX, EB_LY, true));
  /* With positional sound off only near or far comes: far is not it. */
  UT_ASSERT(!earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_BUILDING, 0, 39, EB_LX, EB_LY, false));
  UT_ASSERT(s.stats.sounds == 0);

  /* The listener on the server may be a square or two from this client's
     tank: the same sound measured from (21, 20) is pan -2, band 5. */
  UT_ASSERT(earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_BUILDING, -2, 5, EB_LX, EB_LY, true));
  UT_ASSERT(s.stats.sounds == 1);

  /* Positional sound off: near, centred, is the credit's. */
  earlyBlastAdd(&s, 2, 5000, 5000, EARLY_BLAST_SOUND_TREE);
  UT_ASSERT(earlyBlastClaimExplosion(&s, mx, my, px, py));
  UT_ASSERT(earlyBlastClaimSound(&s, EARLY_BLAST_SOUND_TREE, 0, 15, EB_LX, EB_LY, false));

  /* The wire's pan and band ranges. */
  {
    int lo, hi;
    earlyBlastPanRange(0, &lo, &hi);
    UT_ASSERT(lo == -1 && hi == 1);
    earlyBlastPanRange(2, &lo, &hi);
    UT_ASSERT(lo == 2 && hi == 3);
    earlyBlastPanRange(-6, &lo, &hi);
    UT_ASSERT(lo == -7 && hi == -6);
    earlyBlastPanRange(8, &lo, &hi);
    UT_ASSERT(lo == 8);
    earlyBlastDistRange(5, &lo, &hi);
    UT_ASSERT(lo == 0 && hi == 5);
    earlyBlastDistRange(10, &lo, &hi);
    UT_ASSERT(lo == 6 && hi == 10);
    earlyBlastDistRange(39, &lo, &hi);
    UT_ASSERT(lo == 36 && hi == 39);
  }
  return 0;
}

/* A human player's ClientSim as player 0, with the map left as it is made. */
static ClientSim *ebClient(void) {
  ClientSim *cs = clientSimAlloc();
  if (cs == NULL) {
    return NULL;
  }
  clientSimCreate(cs);
  clientSimSetPlayerNum(cs, 0);
  cs->interpCtx.localPlayer = 0;
  cs->isBot = false;
  return cs;
}

/* Player 0's own predicted shell at (x, y), moving east by vx each tick. */
static void ebShell(ClientSim *cs, WORLD x, WORLD y, float vx, bool onBoat) {
  PredictedShell *ps = &cs->predictedShells[cs->predictedShellCount++];
  memset(ps, 0, sizeof(*ps));
  ps->x = x;
  ps->y = y;
  ps->fx = (float)x;
  ps->fy = (float)y;
  ps->vx = vx;
  ps->vy = 0.0f;
  ps->angle = 64; /* east */
  ps->length = 20;
  ps->owner = 0;
  ps->onBoat = onBoat;
  ps->fireTick = 7;
  ps->active = true;
}

/* Player 1's tank, alive, at (x, y) on this client's view. */
static void ebOtherTank(ClientSim *cs, WORLD x, WORLD y) {
  InterpSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.worldX = x;
  snap.worldY = y;
  snap.alive = true;
  interpUpdate(&cs->interpCtx, 1, &snap, 1, 0);
}

/* A tank on a forest square is a tank hit, as on the server
   (shellsCalcCollision tests tanks before terrain): the early blast is at
   the shell's own place with the tank hit sound, not at the middle of the
   square with the tree sound. With no tank there, it is a tree hit. */
int run_early_blast_tank_in_forest(void) {
  const WORLD sq = (WORLD)(100 * 256);
  const WORLD tankX = (WORLD)(sq + 80), y = (WORLD)(sq + 128);
  ClientSim *cs = ebClient();
  UT_ASSERT(cs != NULL);
  mapSetPos(&cs->sim, &cs->sim.mp, 100, 100, FOREST, FALSE, TRUE);
  mapSetPos(&cs->sim, &cs->sim.mp, 99, 100, GRASS, FALSE, TRUE);
  ebOtherTank(cs, tankX, y);
  ebShell(cs, (WORLD)(sq + 74), y, 16.0f, false);
  clientSimAdvancePredictedShells(cs);
  UT_ASSERT(cs->predictedShellCount == 0);
  UT_ASSERT(cs->earlyBlast.stats.made == 1);
  UT_ASSERT_MSG(cs->earlyBlast.items[0].sound == EARLY_BLAST_SOUND_TANK,
                "sound %u, want the tank hit", (unsigned)cs->earlyBlast.items[0].sound);
  UT_ASSERT(cs->earlyBlast.items[0].x == (int32_t)(sq + 90));
  UT_ASSERT(cs->earlyBlast.items[0].y == (int32_t)y);
  clientSimDestroy(cs);

  /* The same shell with no tank there hits the tree. */
  cs = ebClient();
  UT_ASSERT(cs != NULL);
  mapSetPos(&cs->sim, &cs->sim.mp, 100, 100, FOREST, FALSE, TRUE);
  ebShell(cs, (WORLD)(sq + 74), y, 16.0f, false);
  clientSimAdvancePredictedShells(cs);
  UT_ASSERT(cs->earlyBlast.stats.made == 1);
  UT_ASSERT(cs->earlyBlast.items[0].sound == EARLY_BLAST_SOUND_TREE);
  UT_ASSERT(cs->earlyBlast.items[0].x == (int32_t)(sq + MAP_SQUARE_MIDDLE));
  clientSimDestroy(cs);
  return 0;
}

/* A shell from a boat that lands on grass: the server plays no sound for
   grass, so the early blast plays none and leaves no sound credit. A
   building on the same shore plays the building sound. */
int run_early_blast_boat_grass_silent(void) {
  const WORLD y = (WORLD)(50 * 256 + 128);
  ClientSim *cs = ebClient();
  UT_ASSERT(cs != NULL);
  mapSetPos(&cs->sim, &cs->sim.mp, 49, 50, RIVER, FALSE, TRUE);
  mapSetPos(&cs->sim, &cs->sim.mp, 50, 50, GRASS, FALSE, TRUE);
  ebShell(cs, (WORLD)(49 * 256 + 250), y, 16.0f, true);
  clientSimAdvancePredictedShells(cs);
  UT_ASSERT(cs->predictedShellCount == 0);
  UT_ASSERT(cs->earlyBlast.stats.made == 1);
  UT_ASSERT_MSG(cs->earlyBlast.items[0].sound == EARLY_BLAST_SOUND_NONE,
                "sound %u, want none", (unsigned)cs->earlyBlast.items[0].sound);
  UT_ASSERT(cs->earlyBlast.items[0].x == (int32_t)(50 * 256 + MAP_SQUARE_MIDDLE));
  clientSimDestroy(cs);

  cs = ebClient();
  UT_ASSERT(cs != NULL);
  mapSetPos(&cs->sim, &cs->sim.mp, 49, 50, RIVER, FALSE, TRUE);
  mapSetPos(&cs->sim, &cs->sim.mp, 50, 50, BUILDING, FALSE, TRUE);
  ebShell(cs, (WORLD)(49 * 256 + 250), y, 16.0f, true);
  clientSimAdvancePredictedShells(cs);
  UT_ASSERT(cs->earlyBlast.stats.made == 1);
  UT_ASSERT(cs->earlyBlast.items[0].sound == EARLY_BLAST_SOUND_BUILDING);
  clientSimDestroy(cs);
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
