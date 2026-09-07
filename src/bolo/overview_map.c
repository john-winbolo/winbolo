/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Overview Map
 *Filename:      overview_map.c
 *Purpose:
 *  Keeps an OverviewMap in step with the sim. Every update
 *  works out which squares the player can see right now -
 *  the block their tank could scroll over plus a block round
 *  each item they can view through - and rewrites those
 *  squares from the current map. Squares that have dropped
 *  out of that set keep the tile they last carried, so what
 *  the player has walked past stays as they left it.
 *
 *  Which items those are is the server's business: each of
 *  pillboxes, bases and allied tanks is on a visibility
 *  policy, and the client is told what they are. A category
 *  set to always gives every item of its kind the player
 *  could watch a block; key gives one block, on whatever the
 *  player is watching this moment, and takes the block round
 *  their own tank away while they watch it - one view at a
 *  time, the way the classic screen leaves the tank behind
 *  for as long as the player is in an item view; decay gives
 *  a block to the items the player has driven near recently,
 *  fading out as each clock runs down; and off gives none.
 *  The server culls the data itself on the same rules - these
 *  blocks only say what the client is allowed to draw with
 *  what it has been sent.
 *
 *  Two kinds of square are held current wherever they are,
 *  because the status panels report both live and a frozen
 *  square would leave the map contradicting them: the one a
 *  pillbox has just been lifted from, and every base's. A
 *  base square is its alliance and nothing else. The lifted
 *  pill's square is the one place a square outside a live
 *  region reads current ground, which is one square, and the
 *  alternative is a pillbox drawn where there is none.
 *
 *  Dying closes the block round the tank rather than
 *  holding it: the player watches their own explosion, and
 *  then the fog comes back over the wreck at the moment the
 *  classic main view cuts to static. That is what stops a
 *  dead player watching the square that killed them, which
 *  the classic view has always denied them. The static that
 *  goes with it comes at the end of the wait instead - the
 *  map goes dark, and the snow is the last thing before the
 *  respawn.
 *
 *  The memory opens seeded: when a map lands, every square
 *  is stamped once from it in the remembered (not-live)
 *  style, so the whole map reads dimmed from the first
 *  frame - the terrain is in the map file every client
 *  holds, so the picture gives nothing away - and only the
 *  live regions brighten over it. From then on a seeded
 *  square behaves exactly like a walked-past one: it holds
 *  what it was stamped with until it goes live, so terrain
 *  changes on ground the player cannot see stay invisible.
 *********************************************************/

#include "global.h"
#include "overview_map.h"
#include "bases.h"
#include "facing_table.h" /* kForwardX / kForwardY — where the tank points */
#include "game_sim.h"
#include "pillbox.h"
#include "players.h"
#include "sight.h"
#include "viewport.h"

/* ------------------------------------------------------------------
 * Fog experiment selector: which rule builds the block round the player's
 * own tank, and what stops the player seeing inside it. Process-global
 * and runtime-switchable, like the scroll mechanism selector, and not
 * saved - every launch starts on Envelope with sight off. */
static FogExperiment g_fogExperiment = fogExperimentEnvelope;
static FogSightMode  g_fogSightMode  = fogSightOff;
/* Whether the live regions are drawn as outlines over the map, so the block an
 * experiment builds can be seen through the fog ramp that softens its edge. */
static bool          g_fogShowRegions = FALSE;

FogExperiment overviewFogExperimentGet(void) { return g_fogExperiment; }
void overviewFogExperimentSet(FogExperiment e) { g_fogExperiment = e; }
FogSightMode overviewFogSightGet(void) { return g_fogSightMode; }
void overviewFogSightSet(FogSightMode m) { g_fogSightMode = m; }
bool overviewFogShowRegionsGet(void) { return g_fogShowRegions; }
void overviewFogShowRegionsSet(bool on) { g_fogShowRegions = on; }

/* What each experiment is called and what it does, in enum order. Plain
 * English rather than lang.h ids for the same reason the keys that switch
 * them are hardcoded scancodes: this is a playtest readout, not shipped UI.
 * If one of them is kept the strings move to lang.h and the generator runs. */
static const char *kFogExperimentNames[FOG_EXPERIMENT_COUNT] = {
  "Envelope",
  "Lens",
  "Headlights, envelope",
  "Headlights, lens",
  "Halo",
  "Afterimage"
};

static const char *kFogExperimentBlurbs[FOG_EXPERIMENT_COUNT] = {
  "Everything the classic view could scroll to",
  "The classic window, moved by autoscroll and the scroll keys",
  "Two squares round you and a wedge to the envelope edge",
  "Two squares round you and a wedge to the lens edge",
  "Ground all round, enemies only where you look",
  "What you scrolled over lingers, then fades"
};

const char *overviewFogExperimentName(FogExperiment e) {
  if ((int)e < 0 || (int)e >= FOG_EXPERIMENT_COUNT) {
    return "Unknown";
  }
  return kFogExperimentNames[(int)e];
}

const char *overviewFogExperimentBlurb(FogExperiment e) {
  if ((int)e < 0 || (int)e >= FOG_EXPERIMENT_COUNT) {
    return "";
  }
  return kFogExperimentBlurbs[(int)e];
}

/* And what each sight mode is called, in enum order, for the same readout. */
static const char *kFogSightNames[FOG_SIGHT_COUNT] = {
  "Off",
  "Buildings",
  "Buildings and trees"
};

static const char *kFogSightBlurbs[FOG_SIGHT_COUNT] = {
  "Buildings do not block sight",
  "You cannot see through a building",
  "Two trees deep is as far as you see"
};

const char *overviewFogSightName(FogSightMode m) {
  if ((int)m < 0 || (int)m >= FOG_SIGHT_COUNT) {
    return "Unknown";
  }
  return kFogSightNames[(int)m];
}

const char *overviewFogSightBlurb(FogSightMode m) {
  if ((int)m < 0 || (int)m >= FOG_SIGHT_COUNT) {
    return "";
  }
  return kFogSightBlurbs[(int)m];
}

/* What the walk is handed for a mode that is having a mask built. fogSightOff
 * never reaches the walk through the live stamp - the caller builds no sight
 * mask at all - and the farewell stamp reads last update's hiddenActive rather
 * than the mode, so a mode dropped on the same tick a block goes still masks
 * that last stamp by buildings, exactly as it did before trees were counted. */
static SightMode overviewSightMode(uint8_t mode) {
  return (mode == (uint8_t)fogSightBuildingsAndTrees) ? sightModeBuildingsAndTrees
                                                      : sightModeBuildings;
}

/* Which of the experiments are the two Headlights, asked in one place so the
 * block, the mask and the farewell stamp cannot come apart over it. */
static bool overviewIsHeadlights(uint8_t experiment) {
  return experiment == (uint8_t)fogExperimentHeadlightsEnvelope ||
         experiment == (uint8_t)fogExperimentHeadlightsLens;
}

bool overviewFogBlockFollowsView(FogExperiment e) {
  return e == fogExperimentLens || e == fogExperimentHalo ||
         e == fogExperimentAfterimage;
}

bool overviewHeadlightSees(int dx, int dy, BYTE facing) {
  int     idx; /* The facing, brought inside the table */
  int     fx;  /* Where the tank points, in 256ths of a square */
  int     fy;
  int     dot; /* How much of the way to the square runs along the facing */
  int64_t lhs; /* That, squared and scaled to compare against the cosine */
  int64_t rhs; /* The cosine's share of the two lengths it came from */

  /* The near squares, read as a square block rather than a circle, so there is
   * no rounding to argue about at the diagonals. The tank's own square is
   * inside it, so the reticle is never dropped. */
  if (dx >= -OVERVIEW_HEADLIGHT_NEAR && dx <= OVERVIEW_HEADLIGHT_NEAR &&
      dy >= -OVERVIEW_HEADLIGHT_NEAR && dy <= OVERVIEW_HEADLIGHT_NEAR) {
    return TRUE;
  }

  idx = (int)(facing & 15);
  fx = kForwardX[idx];
  fy = kForwardY[idx];
  dot = dx * fx + dy * fy;
  if (dot <= 0) {
    return FALSE; /* the rear half, whatever the angle works out to */
  }

  /* The angle between the square and the facing, as a dot product against the
   * lengths it came from and squared so there is no square root:
   *
   *   dot^2 * ONE >= COS2 * |d|^2 * |f|^2
   *
   * The facing table is in 256ths of a square, so |f|^2 is about 65536 and the
   * right-hand side reaches eleven figures over the widest block - past what a
   * 32-bit int holds - which is why both sides are widened before they are
   * multiplied. Whole numbers rather than floats, so every machine draws the
   * same beam. */
  lhs = (int64_t)dot * (int64_t)dot * (int64_t)OVERVIEW_HEADLIGHT_COS2_ONE;
  rhs = (int64_t)OVERVIEW_HEADLIGHT_COS2 * (int64_t)(dx * dx + dy * dy) *
        (int64_t)(fx * fx + fy * fy);
  return (lhs >= rhs) ? TRUE : FALSE;
}

/* Zeroes every square of the rect the Headlights blocks do not hold live,
 * leaving the ones they do exactly as they were - so a mask the sight walk has
 * already written keeps its own zeroes and the two rules stack: a square has to
 * be in the beam and have a clear line to it to come through both.
 *
 * The origin is the square the block was placed from, which is its centre; the
 * mask is indexed over the rect the way sight.h describes. */
static void overviewHeadlightMask(BYTE facing, BYTE tankMX, BYTE tankMY,
                                  const OverviewRect *r, BYTE *vis) {
  int stride; /* Squares across the rect, which is the mask's row length */
  int x;      /* Looping variable */
  int y;      /* Looping variable */

  stride = r->right - r->left + 1;
  for (x = r->left; x <= r->right; x++) {
    for (y = r->top; y <= r->bottom; y++) {
      if (overviewHeadlightSees(x - (int)tankMX, y - (int)tankMY, facing) ==
          FALSE) {
        vis[(y - r->top) * stride + (x - r->left)] = 0;
      }
    }
  }
}

/* Brings an inclusive rect back inside the map. Shared so a block placed by
 * its corner is trimmed exactly the way one built round a centre is. */
static void overviewRectTrimToMap(OverviewRect *r) {
  if (r->left < 0) {
    r->left = 0;
  }
  if (r->top < 0) {
    r->top = 0;
  }
  if (r->right > MAP_ARRAY_SIZE - 1) {
    r->right = MAP_ARRAY_SIZE - 1;
  }
  if (r->bottom > MAP_ARRAY_SIZE - 1) {
    r->bottom = MAP_ARRAY_SIZE - 1;
  }
}

/* An inclusive square block centred on (cx,cy), trimmed to the map, live
 * outright. The centre and half-width are ints so a block over the top or left
 * edge clamps instead of wrapping through zero. Zeroed first so the whole
 * object is written: the frontend compares stored rects byte for byte to
 * decide it can reuse a fog mask, and padding it never sees would otherwise
 * report a change on a tick where nothing moved. */
static OverviewRect overviewRectAround(int cx, int cy, int half) {
  OverviewRect r; /* Rect to return */

  memset(&r, 0, sizeof(r));
  r.alpha = 255;
  r.left = cx - half;
  r.top = cy - half;
  r.right = cx + half;
  r.bottom = cy + half;
  overviewRectTrimToMap(&r);
  return r;
}

/* The block of squares round the player's own tank, which is the one thing the
 * fog experiment picks between. Envelope is the whole scroll envelope centred
 * on the tank, the block the map has always drawn, and the two Headlights are
 * centred on the tank as well - the wider one over the same scroll envelope,
 * the narrower one over the classic window's own 15x15. What the classic view
 * is doing does not come into any of the three, because the beam the mask cuts
 * out of the block is pointed by the tank rather than by the window.
 *
 * The rest are that 15x15 placed where the classic view is sitting: it keeps
 * scrolling whether or not it is on screen, so its first visible square says
 * where the player is looking, and autoscroll and the scroll keys move the
 * block by moving it. With no reading to place it from - a dead tank, or an
 * item view just left - the same 15x15 goes round the tank instead. The rect
 * comes out live outright either way, so it is the same kind of rect the tank
 * has always had. */
static void overviewTankBlock(const OverviewViewInputs *in, BYTE tankMX,
                              BYTE tankMY, OverviewRect *out) {
  int half; /* How wide the block round the tank is */

  if (overviewFogBlockFollowsView((FogExperiment)in->experiment) == FALSE) {
    half = (in->experiment == (uint8_t)fogExperimentHeadlightsLens)
               ? OVERVIEW_LENS_HALF
               : OVERVIEW_TANK_HALF;
    *out = overviewRectAround((int)tankMX, (int)tankMY, half);
    return;
  }
  if (in->viewValid == FALSE) {
    *out = overviewRectAround((int)tankMX, (int)tankMY, OVERVIEW_LENS_HALF);
    return;
  }

  memset(out, 0, sizeof(*out));
  out->alpha = 255;
  out->left = (int)in->viewLeft;
  out->top = (int)in->viewTop;
  out->right = out->left + 2 * OVERVIEW_LENS_HALF;
  out->bottom = out->top + 2 * OVERVIEW_LENS_HALF;
  overviewRectTrimToMap(out);
}

/* Rewrites every square of an inclusive rect from the current sim state.
 * This is the only writer of OverviewMap::tile in the codebase - nothing
 * else may touch it, or the memory stops being a record of what was seen.
 * Returns TRUE if any byte came out different.
 *
 * A live rect grants sight with the ground unless it is terrain-only, which is
 * the halo: its squares are close enough to read the ground on and not close
 * enough to make out what is moving there. The flags are assigned rather than
 * merged, so the last rect stamped over a square decides both bits, and a
 * square the halo and the lens both cover keeps sight because the lens is
 * stamped second.
 *
 * vis is the rect's sight mask - one byte per square, indexed over this rect
 * the way sight.h describes - and NULL for a rect nothing blocks sight in. A
 * square the mask calls hidden is not rewritten at all: it holds the tile and
 * the mine it last showed, which is the whole of what the player remembers of
 * ground they cannot see into, and carries OVERVIEW_F_HIDDEN in place of the
 * live and sight bits so nothing moving on it is drawn and the fog stays over
 * it. */
static bool overviewStampRect(OverviewMap *om, struct GameSim *sim, BYTE me,
                              const OverviewRect *r, bool setLive,
                              const BYTE *vis) {
  bool changed;  /* Did any byte move */
  bool isMine;   /* Mine visible on this square */
  BYTE tileNum;  /* Tile the square shows now */
  BYTE liveBits; /* What being inside this rect is worth, the same everywhere */
  BYTE flagBits; /* Flags the square carries now */
  int stride;    /* Squares across the rect, which is the mask's row length */
  int x;         /* Looping variable */
  int y;         /* Looping variable */

  liveBits = 0;
  if (setLive == TRUE) {
    liveBits = (BYTE)(OVERVIEW_F_LIVE |
                      (r->terrainOnly == 0 ? OVERVIEW_F_SIGHT : 0));
  }
  stride = r->right - r->left + 1;

  changed = FALSE;
  for (x = r->left; x <= r->right; x++) {
    for (y = r->top; y <= r->bottom; y++) {
      if (vis != NULL && vis[(y - r->top) * stride + (x - r->left)] == 0) {
        flagBits = (BYTE)(OVERVIEW_F_HIDDEN |
                          (om->flags[x][y] & OVERVIEW_F_MINE));
        if (om->flags[x][y] != flagBits) {
          om->flags[x][y] = flagBits;
          changed = TRUE;
        }
        continue;
      }

      isMine = FALSE;
      tileNum = viewportCalcSquarePure(sim, me, (BYTE)x, (BYTE)y, &isMine);
      flagBits = (BYTE)(liveBits | (isMine == TRUE ? OVERVIEW_F_MINE : 0));

      if (om->tile[x][y] == OVERVIEW_UNSEEN && tileNum != OVERVIEW_UNSEEN) {
        om->seenCount++;
      }
      if (om->tile[x][y] != tileNum) {
        om->tile[x][y] = tileNum;
        changed = TRUE;
      }
      if (om->flags[x][y] != flagBits) {
        om->flags[x][y] = flagBits;
        changed = TRUE;
      }
    }
  }
  return changed;
}

/* The mask for the last stamp one of the tank's own blocks gets as it stops
 * being live, or NULL when there is none to build. That stamp writes the ground
 * as it is now, so without a mask a block that is going away would show the
 * player everything it had been keeping from them on the way out - which is
 * repeatable on purpose by watching a pillbox for a tick.
 *
 * The origin is the square the block was last actually placed from rather than
 * anything this update was handed: a tank that has really gone reports the map
 * origin, and the honest answer is where it was standing when it last had a
 * block. With no such square recorded there is nothing to work from.
 *
 * The rules are the ones the live stamp below is about to use, so a block on
 * its way out is masked the same way it was drawn rather than another - the
 * sight walk and, under either Headlights, the beam over the top of it, exactly
 * as the live stamp stacks them.
 *
 * Every square is marked seen first, so a block too wide for the buffer reads
 * as nothing being hidden rather than as whatever was in it. */
static const BYTE *overviewFarewellMask(const OverviewMap *om,
                                        struct GameSim *sim,
                                        const OverviewViewInputs *in,
                                        SightMode mode, const OverviewRect *r,
                                        BYTE *vis) {
  if (om->hiddenActive == FALSE || om->haveLastTank == FALSE) {
    return NULL;
  }
  memset(vis, 1, SIGHT_MASK_BYTES);
  sightBuildMask(&sim->mp, om->lastTankMX, om->lastTankMY, mode, r, vis);
  if (overviewIsHeadlights(in->experiment) == TRUE) {
    overviewHeadlightMask(in->facing, om->lastTankMX, om->lastTankMY, r, vis);
  }
  return vis;
}

/* TRUE when the square falls inside any of the count rects. There are never
 * more than OVERVIEW_MAX_REGIONS of them, so the walk is cheap. */
static bool overviewPointInRects(const OverviewRect *r, int count, int x,
                                 int y) {
  int i; /* Looping variable */

  for (i = 0; i < count; i++) {
    if (x >= r[i].left && x <= r[i].right && y >= r[i].top &&
        y <= r[i].bottom) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Drops OVERVIEW_F_LIVE, OVERVIEW_F_SIGHT and OVERVIEW_F_HIDDEN over a rect,
 * leaving the tiles alone. All three go together: a square that has left every
 * rect is neither live, in sight, nor inside a block it could be hidden in, and
 * dropping one without the others would leave a stale bit on it for good.
 * Squares that still fall inside one of the keep rects are left untouched: they
 * have not left the live set, and stripping the flags off them only to have
 * them put straight back would report a change on a tick where nothing moved.
 * Returns TRUE if any square that has genuinely left was carrying one of
 * them. */
static bool overviewClearLiveRect(OverviewMap *om, const OverviewRect *r,
                                  const OverviewRect *keep, int keepCount) {
  bool changed; /* Did any byte move */
  int x;        /* Looping variable */
  int y;        /* Looping variable */

  changed = FALSE;
  for (x = r->left; x <= r->right; x++) {
    for (y = r->top; y <= r->bottom; y++) {
      if (overviewPointInRects(keep, keepCount, x, y) == TRUE) {
        continue;
      }
      if ((om->flags[x][y] &
           (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT | OVERVIEW_F_HIDDEN)) != 0) {
        om->flags[x][y] = (BYTE)(om->flags[x][y] &
                                 ~(OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT |
                                   OVERVIEW_F_HIDDEN));
        changed = TRUE;
      }
    }
  }
  return changed;
}

/* TRUE when the two region sets are not the same rects in the same order.
 * Alpha counts: a region a tick further into its fade is a region the map has
 * to be redrawn for, so a tick that only moves the fade still reports a
 * change. So does terrainOnly, or a rect that had swapped one kind for the
 * other over the same squares would report a build that had not moved. */
static bool overviewRegionsDiffer(const OverviewRect *a, int aCount,
                                  const OverviewRect *b, int bCount) {
  int i; /* Looping variable */

  if (aCount != bCount) {
    return TRUE;
  }
  for (i = 0; i < aCount; i++) {
    if (a[i].left != b[i].left || a[i].top != b[i].top ||
        a[i].right != b[i].right || a[i].bottom != b[i].bottom ||
        a[i].alpha != b[i].alpha || a[i].terrainOnly != b[i].terrainOnly) {
      return TRUE;
    }
  }
  return FALSE;
}

void overviewMapReset(OverviewMap *om) {
  if (om == NULL) {
    return;
  }

  memset(om->tile, OVERVIEW_UNSEEN, sizeof(om->tile));
  memset(om->flags, 0, sizeof(om->flags));
  memset(om->fade, 0, sizeof(om->fade));
  om->fadeSpan = 0;
  om->hiddenActive = FALSE;
  memset(om->live, 0, sizeof(om->live));
  om->liveCount = 0;
  memset(om->prevLive, 0, sizeof(om->prevLive));
  om->prevLiveCount = 0;
  om->haloWasLive = FALSE;
  om->tankWasLive = FALSE;
  om->lastTankMX = 0;
  om->lastTankMY = 0;
  om->haveLastTank = FALSE;
  om->lastViewLeft = 0;
  om->lastViewTop = 0;
  om->haveLastView = FALSE;
  memset(om->pillWasLive, 0, sizeof(om->pillWasLive));
  memset(om->baseWasLive, 0, sizeof(om->baseWasLive));
  memset(om->allyWasLive, 0, sizeof(om->allyWasLive));
  memset(om->pillWasInTank, 0, sizeof(om->pillWasInTank));
  om->generation = 0;
  om->seenCount = 0;
}

void overviewMapSeedAll(OverviewMap *om, struct GameSim *sim,
                        BYTE myPlayerNum) {
  OverviewRect all; /* The whole map, one stamp */

  if (om == NULL || sim == NULL) {
    return;
  }

  memset(&all, 0, sizeof(all));
  all.left = 0;
  all.top = 0;
  all.right = MAP_ARRAY_SIZE - 1;
  all.bottom = MAP_ARRAY_SIZE - 1;
  if (overviewStampRect(om, sim, myPlayerNum, &all, FALSE, NULL) == TRUE) {
    om->generation++;
  }
}

/* Where the blackout starts for a death of this kind, in ticks left on the
 * death wait — the same tick the classic main view cuts to static. A tank that
 * has drowned sinks slowly and is given longer to watch; anything else takes
 * the shell and mine figure, which is also what an unrecognised cause gets:
 * the blackout is what stops a dead player watching the square that killed
 * them, so a cause this does not know has to black out too. */
static int overviewDeathBlackoutStart(int lastDeath) {
  if (lastDeath == LAST_DEATH_BY_DEEPSEA) {
    return STATIC_ON_TICKS_DEEPSEA;
  }
  return STATIC_ON_TICKS;
}

bool overviewMapDeathBlackout(int deathWait, int lastDeath) {
  return deathWait > 0 && deathWait <= overviewDeathBlackoutStart(lastDeath);
}

void overviewViewInputsDefaults(OverviewViewInputs *in) {
  if (in == NULL) {
    return;
  }

  memset(in, 0, sizeof(*in));
  in->policy[viewCategoryPill] = viewPolicyAlways;
  in->policy[viewCategoryBase] = viewPolicyOff;
  in->policy[viewCategoryAlly] = viewPolicyAlways;
  in->viewKind = VIEW_KIND_TANK;
}

/* One item's proximity clock, or 0 for a caller that keeps no clocks. */
static uint32_t overviewNearTick(const uint32_t *clocks, int idx) {
  return (clocks == NULL) ? 0u : clocks[idx];
}

bool overviewViewDecayLive(const OverviewViewInputs *in, ViewCategory cat,
                           uint32_t nearTick, BYTE *outAlpha) {
  uint32_t window;    /* Ticks a stamp stays good for */
  uint32_t fade;      /* Ticks of that window the fade takes */
  uint32_t age;       /* Ticks since the stamp */
  uint32_t remaining; /* Ticks of the window left */
  BYTE     ignored;   /* Somewhere to put the alpha a caller does not want */

  if (outAlpha == NULL) {
    outAlpha = &ignored;
  }
  *outAlpha = 255;
  if (in == NULL || nearTick == 0 || in->ticksPerSec == 0) {
    return FALSE;
  }
  window = (uint32_t)in->decaySecs[cat] * in->ticksPerSec;
  age = in->nowTick - nearTick;
  if (age > window) {
    return FALSE;
  }

  remaining = window - age;
  fade = (uint32_t)VIEW_DECAY_FADE_SECS * in->ticksPerSec;
  if (remaining < fade) {
    *outAlpha = (BYTE)((255u * remaining + fade - 1u) / fade);
  }
  return TRUE;
}

/* Whether an item of this category earns a region this update, and how bright
 * it is. qualifies is the item-view test for its kind — the policy only ever
 * takes items away from that, never adds one. index is the pill or base index
 * or the ally's player number, which viewPolicyKey compares against what the
 * player is watching. */
static bool overviewItemLive(const OverviewViewInputs *in, ViewCategory cat,
                             bool qualifies, uint8_t kind, BYTE index,
                             const uint32_t *clocks, BYTE *outAlpha) {
  *outAlpha = 255;
  if (in == NULL || qualifies == FALSE) {
    return FALSE;
  }

  switch (in->policy[cat]) {
  case viewPolicyAlways:
    return TRUE;
  case viewPolicyKey:
    return in->viewKind == kind && in->viewTarget == index;
  case viewPolicyDecay:
    return overviewViewDecayLive(in, cat, overviewNearTick(clocks, (int)index),
                                 outAlpha);
  default: /* viewPolicyOff, and anything this client does not know */
    return FALSE;
  }
}

/* The three per-category predicates the build and the farewell replay share.
 * Both have to reach the same answer for the same item, or a stale rect would
 * be paired with the wrong region. */
static bool overviewPillLive(struct GameSim *sim, BYTE myPlayerNum,
                             const OverviewViewInputs *in, BYTE i,
                             BYTE *outAlpha) {
  bool canView = pillsCanView(sim, &sim->pb, i, myPlayerNum);

  return overviewItemLive(in, viewCategoryPill, canView, VIEW_KIND_PILL, i,
                          in == NULL ? NULL : in->pillNearTick, outAlpha);
}

static bool overviewBaseLive(struct GameSim *sim, BYTE myPlayerNum,
                             const OverviewViewInputs *in, BYTE i,
                             BYTE *outAlpha) {
  bool canView = basesCanView(sim, &sim->bs, i, myPlayerNum);

  return overviewItemLive(in, viewCategoryBase, canView, VIEW_KIND_BASE, i,
                          in == NULL ? NULL : in->baseNearTick, outAlpha);
}

/* playersCanAllyView answers for sim->viewPlayer, which is the local player on
 * a client. Our own slot is turned away here as well, so a caller whose
 * viewPlayer has not been set still never watches itself. */
static bool overviewAllyLive(struct GameSim *sim, BYTE myPlayerNum,
                             const OverviewViewInputs *in, BYTE t,
                             BYTE *outAlpha) {
  bool canView;

  *outAlpha = 255;
  if (in == NULL || t == myPlayerNum) {
    return FALSE;
  }
  canView = playersCanAllyView(sim, in->allyViewable, t);
  return overviewItemLive(in, viewCategoryAlly, canView, VIEW_KIND_ALLY, t,
                          in->allyNearTick, outAlpha);
}

/* Whether the player is watching an item through a category on viewPolicyKey
 * and that item still earns its block. Key is one view at a time: while this
 * holds, the block round the player's own tank is not built either, so the map
 * shows the one thing being watched and nothing else — the same single screen
 * the classic view gives them while they are in an item view. This is the map
 * choosing what to draw, not what it has: the server sends the recipient's own
 * tank screen whatever it is watching, because the client predicts its tank
 * against that ground. The three predicates each turn an out-of-range target
 * away, so the watched index needs no checking here. */
static bool overviewKeyViewLive(struct GameSim *sim, BYTE myPlayerNum,
                                const OverviewViewInputs *in) {
  BYTE alpha; /* Where the predicates report brightness; unwanted here */

  if (in == NULL) {
    return FALSE;
  }

  switch (in->viewKind) {
  case VIEW_KIND_PILL:
    return in->policy[viewCategoryPill] == viewPolicyKey &&
           overviewPillLive(sim, myPlayerNum, in, in->viewTarget, &alpha);
  case VIEW_KIND_BASE:
    return in->policy[viewCategoryBase] == viewPolicyKey &&
           overviewBaseLive(sim, myPlayerNum, in, in->viewTarget, &alpha);
  case VIEW_KIND_ALLY:
    return in->policy[viewCategoryAlly] == viewPolicyKey &&
           overviewAllyLive(sim, myPlayerNum, in, in->viewTarget, &alpha);
  default:
    return FALSE; /* the tank view, and a kind this client does not know */
  }
}

int overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                            const OverviewViewInputs *in,
                            const OverviewRect *haloRect,
                            const OverviewRect *tankRect, OverviewRect *out,
                            int maxOut) {
  int count;     /* Rects written so far */
  bool keyView;  /* Is the player watching an item that closes the tank's own
                    blocks */
  BYTE alpha;    /* How bright the item under test is */
  BYTE numPills; /* Pills on the map */
  BYTE numBases; /* Bases on the map */
  BYTE i;        /* Looping variable */

  count = 0;
  if (sim == NULL || in == NULL || out == NULL || maxOut <= 0) {
    return 0;
  }

  /* Watching an item closes both blocks round the tank, not just the one the
   * player sees things moving in. */
  keyView = overviewKeyViewLive(sim, myPlayerNum, in);

  if (haloRect != NULL && count < maxOut && keyView == FALSE) {
    out[count] = *haloRect;
    count++;
  }

  if (tankRect != NULL && count < maxOut && keyView == FALSE) {
    out[count] = *tankRect;
    count++;
  }

  numPills = pillsGetNumPills(&sim->pb);
  for (i = 0; i < numPills && count < maxOut; i++) {
    if (overviewPillLive(sim, myPlayerNum, in, i, &alpha) == TRUE) {
      out[count] = overviewRectAround((int)sim->pb->item[i].x,
                                      (int)sim->pb->item[i].y,
                                      OVERVIEW_PILL_HALF);
      out[count].alpha = alpha;
      count++;
    }
  }

  numBases = basesGetNumBases(&sim->bs);
  for (i = 0; i < numBases && count < maxOut; i++) {
    if (overviewBaseLive(sim, myPlayerNum, in, i, &alpha) == TRUE) {
      out[count] = overviewRectAround((int)sim->bs->item[i].x,
                                      (int)sim->bs->item[i].y,
                                      OVERVIEW_PILL_HALF);
      out[count].alpha = alpha;
      count++;
    }
  }

  /* An allied tank is watched at the square it was last seen on, which is the
   * one the ally view centres on — the client holds no tank object for anybody
   * but itself, so the players struct is the only position there is.
   *
   * The block is the watched-item size, not the tank one. An item view cannot
   * be scrolled — the pan keys step to the next item rather than moving the
   * picture — so watching an ally shows exactly the 15x15 around them, the
   * same as a pillbox or a base. The wider block is the player's own tank's,
   * because that is the one view the classic screen can be scrolled around. */
  for (i = 0; i < MAX_TANKS && count < maxOut; i++) {
    if (overviewAllyLive(sim, myPlayerNum, in, i, &alpha) == TRUE) {
      out[count] = overviewRectAround((int)sim->plyrs->item[i].mapX,
                                      (int)sim->plyrs->item[i].mapY,
                                      OVERVIEW_PILL_HALF);
      out[count].alpha = alpha;
      count++;
    }
  }

  return count;
}

/* Afterimage's per-square countdown, one tick of it. A square inside a live
 * region is held at the full span; one the block has left steps down by one
 * until it is back in full fog, which is what makes the ground behind the
 * player dissolve instead of going dark the moment the block leaves it. The
 * ticks left are what is stored rather than the brightness, so the step is
 * exactly one tick and the fade takes the seconds it says it does whatever the
 * tick rate is; how bright that leaves the square is worked out where it is
 * drawn.
 *
 * Every square is stepped first and the live regions are written second, so
 * neither pass has to ask whether a square is inside a region - which would be
 * the whole map tested against every rect, every tick. The cost is one walk of
 * the map plus the area of the regions.
 *
 * Returns TRUE if any square ends the tick on a different value than it
 * started it on. A square the block still covers is stepped and then put
 * straight back, which is no movement at all, so those are counted and
 * discounted rather than reported: without that the map would report a change
 * every tick for as long as the player was on this experiment. */
static bool overviewFadeStep(OverviewMap *om, BYTE span) {
  bool changed;  /* Did any square end up somewhere else */
  int  stepped;  /* Squares the countdown moved on */
  int  restored; /* Of those, the ones a live region put straight back */
  BYTE was;      /* What a square in a region held before it was refreshed */
  int  i;        /* Looping variable */
  int  x;        /* Looping variable */
  int  y;        /* Looping variable */

  changed = FALSE;
  stepped = 0;
  restored = 0;
  om->fadeSpan = span;

  for (x = 0; x < MAP_ARRAY_SIZE; x++) {
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
      if (om->fade[x][y] > 0) {
        om->fade[x][y]--;
        stepped++;
      }
    }
  }

  for (i = 0; i < om->liveCount; i++) {
    for (x = om->live[i].left; x <= om->live[i].right; x++) {
      for (y = om->live[i].top; y <= om->live[i].bottom; y++) {
        was = om->fade[x][y];
        om->fade[x][y] = span;
        if ((int)was + 1 == (int)span) {
          restored++;
        } else if (was != span) {
          changed = TRUE;
        }
      }
    }
  }

  if (stepped > restored) {
    changed = TRUE;
  }
  return changed;
}

void overviewMapUpdate(OverviewMap *om, struct GameSim *sim, BYTE myPlayerNum,
                       const OverviewViewInputs *in, bool haveTank,
                       int tankDeathWait, BYTE tankMX, BYTE tankMY) {
  bool tankLive; /* Is there a tank region this update */
  bool haloLive; /* Is there a halo region round it as well */
  BYTE useMX;    /* Square that region is placed from */
  BYTE useMY;    /* Square that region is placed from */
  OverviewRect tankRect;   /* The region itself, once placed */
  OverviewRect haloRect;   /* The ground round it, under Halo */
  OverviewViewInputs held; /* in, with the view a dead tank last had */
  const OverviewViewInputs *blockIn; /* Which of the two places the block */
  bool changed;  /* Did anything move this update */
  BYTE alpha;    /* Where the predicates report brightness; only whether
                    there is a region at all is wanted here */
  BYTE numPills; /* Pills on the map */
  BYTE numBases; /* Bases on the map */
  unsigned span;       /* Ticks Afterimage's fade runs for */
  bool nowInTank;      /* Is this pill being carried this update */
  OverviewRect square; /* A single square being held current on its own */
  bool hideOn;         /* Is anything hiding squares inside the tank's blocks */
  SightMode sightRule; /* Which blockers the sight walk is built from */
  int  ownBlocks;      /* Rects at the head of the list that are those blocks */
  const BYTE *visPtr;  /* The mask the rect being stamped is masked with */
  BYTE vis[SIGHT_MASK_BYTES]; /* One block's mask, rebuilt for each of them */
  int idx;       /* Which prevLive rect the replay is up to */
  int i;         /* Looping variable */

  if (om == NULL || sim == NULL || in == NULL) {
    return;
  }

  /* Settled once, so the farewell stamp below and the live stamp under it
   * cannot mask their blocks by different rules. */
  sightRule = overviewSightMode(in->sightMode);

  /* Which square the tank block sits on, if there is one at all. A tank with a
   * position of its own records the square here on the way past; one that is
   * dead and still in its slot has none to give - a dead tank reads as the map
   * origin - so it holds the block on the square it last had one, and the
   * player watches their own wreck instead of the ground round it greying out
   * the moment they die. The block is held at full size for the whole wait;
   * what stops a dead player watching the square that killed them is the
   * blackout the view draws over the lot, not the block shrinking underneath
   * it. A tank that has really gone has no block at all, which is what
   * releases it and lets the farewell stamp below run.
   *
   * Where the classic view was sitting is recorded alongside the square, for
   * the same reason: a dead tank leaves that view wherever it stopped and
   * reports nothing, so an experiment that places the block from the view has
   * to be handed the last readings taken while the tank was alive. The block
   * then holds where the player was looking rather than snapping to the wreck.
   */
  tankLive = haveTank;
  useMX = tankMX;
  useMY = tankMY;
  blockIn = in;
  if (haveTank == TRUE) {
    om->lastTankMX = tankMX;
    om->lastTankMY = tankMY;
    om->haveLastTank = TRUE;
    if (in->viewValid == TRUE) {
      om->lastViewLeft = in->viewLeft;
      om->lastViewTop = in->viewTop;
      om->haveLastView = TRUE;
    }
  } else if (tankDeathWait > 0 && om->haveLastTank == TRUE) {
    tankLive = TRUE;
    useMX = om->lastTankMX;
    useMY = om->lastTankMY;
    held = *in;
    held.viewLeft = om->lastViewLeft;
    held.viewTop = om->lastViewTop;
    held.viewValid = om->haveLastView;
    blockIn = &held;
  }
  overviewTankBlock(blockIn, useMX, useMY, &tankRect);

  /* Watching an item under viewPolicyKey closes the blocks round the tank, so
   * the map shows the one thing being watched. Asked here as well so
   * tankWasLive and haloWasLive record what was actually built and the farewell
   * replay below stays paired with the regions that produced its rects. */
  if (overviewKeyViewLive(sim, myPlayerNum, in) == TRUE) {
    tankLive = FALSE;
  }

  /* Halo puts a second, wider block under the first: live ground that grants no
   * sight, at half fog, so the player reads the terrain all round while what is
   * moving on it still only shows where they are looking. It goes round the
   * tank rather than round the block the player is looking through - the ground
   * they can make out is the ground they are standing in the middle of, and the
   * lens travels over it. */
  haloLive = (tankLive == TRUE &&
              in->experiment == (uint8_t)fogExperimentHalo);
  if (haloLive == TRUE) {
    haloRect = overviewRectAround((int)useMX, (int)useMY, OVERVIEW_HALO_HALF);
    haloRect.alpha = OVERVIEW_HALO_ALPHA;
    haloRect.terrainOnly = 1;
  }

  memcpy(om->prevLive, om->live, sizeof(om->prevLive));
  om->prevLiveCount = om->liveCount;

  om->liveCount = overviewMapBuildRegions(
      sim, myPlayerNum, in, (haloLive == TRUE) ? &haloRect : NULL,
      (tankLive == TRUE) ? &tankRect : NULL, om->live, OVERVIEW_MAX_REGIONS);
  changed = overviewRegionsDiffer(om->live, om->liveCount, om->prevLive,
                                  om->prevLiveCount);

  /* Farewell stamp. A region that has just stopped being live gets one last
   * write from the state as it is now, so an allied pill that has died freezes
   * dead, one that has been captured freezes in the captor's colour, a base
   * that has changed hands freezes in the new one, and an ally that has died
   * or left freezes on the ground they were last standing on rather than a
   * tick stale. A decay clock running out ends a region the same way.
   * overviewMapBuildRegions emits the halo rect first when there is one, then
   * the tank rect, then pills, bases and allied tanks each in ascending index,
   * so walking the same order over last update's owners pairs each stale rect
   * with the region that produced it. The order is contractual: position on
   * the list is the only thing tying a stale rect to its region.
   *
   * The two blocks round the tank take the mask they were stamped under, so
   * ground the player could not see into stays as they last saw it on the way
   * out as well as on the way in. Watched items never carry one, here as in the
   * live stamp. om->hiddenActive is still last update's value at this point -
   * it is rewritten below, after this replay - and last update is what these
   * rects came from, so this is the flag to read. Moving that assignment above
   * here would quietly take the mask away. */
  idx = 0;
  if (om->haloWasLive == TRUE) {
    if (haloLive == FALSE && idx < om->prevLiveCount) {
      visPtr = overviewFarewellMask(om, sim, in, sightRule, &om->prevLive[idx],
                                    vis);
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE,
                            visPtr) == TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }
  if (om->tankWasLive == TRUE) {
    if (tankLive == FALSE && idx < om->prevLiveCount) {
      visPtr = overviewFarewellMask(om, sim, in, sightRule, &om->prevLive[idx],
                                    vis);
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE,
                            visPtr) == TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }
  for (i = 0; i < MAX_PILLS; i++) {
    if (om->pillWasLive[i] == FALSE) {
      continue;
    }
    if (idx < om->prevLiveCount &&
        overviewPillLive(sim, myPlayerNum, in, (BYTE)i, &alpha) == FALSE) {
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE,
                            NULL) == TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }
  for (i = 0; i < MAX_BASES; i++) {
    if (om->baseWasLive[i] == FALSE) {
      continue;
    }
    if (idx < om->prevLiveCount &&
        overviewBaseLive(sim, myPlayerNum, in, (BYTE)i, &alpha) == FALSE) {
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE,
                            NULL) == TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }
  for (i = 0; i < MAX_TANKS; i++) {
    if (om->allyWasLive[i] == FALSE) {
      continue;
    }
    if (idx < om->prevLiveCount &&
        overviewAllyLive(sim, myPlayerNum, in, (BYTE)i, &alpha) == FALSE) {
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE,
                            NULL) == TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }

  /* Pickup stamp. A pillbox that has just been lifted into a tank leaves the
   * square it stood on, and the memory has to be told: the square is frozen,
   * so nothing else will ever rewrite it and the map would keep drawing a
   * pillbox on ground that has not had one since. Only the one square is
   * restamped, and a pill leaves its map position behind when it is carried,
   * so that position is the square the memory is showing it at.
   *
   * This says nothing the player was not already being told - the status
   * panel draws every carried pill as in-tank, whoever is carrying it. Where
   * it is put down is a different matter, and stays hidden: the new square is
   * written only if it is one the player can see.
   *
   * The single-square rect is zeroed whole here, once for both passes that
   * use it: only its edges are written below, and the rest of it has to be
   * something defined rather than whatever was on the stack, so a stamp that
   * reads another field one day reads a rect and not rubbish. */
  memset(&square, 0, sizeof(square));
  numPills = pillsGetNumPills(&sim->pb);
  for (i = 0; i < MAX_PILLS; i++) {
    nowInTank = (i < (int)numPills) ? sim->pb->item[i].inTank : FALSE;
    if (nowInTank == TRUE && om->pillWasInTank[i] == FALSE) {
      square.left = square.right = (int)sim->pb->item[i].x;
      square.top = square.bottom = (int)sim->pb->item[i].y;
      if (overviewStampRect(om, sim, myPlayerNum, &square, FALSE, NULL) ==
          TRUE) {
        changed = TRUE;
      }
    }
    om->pillWasInTank[i] = nowInTank;
  }

  /* Ownership stamp. A base changing hands shows on the status panel the
   * moment it happens, whoever took it, so a map still drawing the old colour
   * on a square the player has walked away from is the map contradicting the
   * panel. Every base square outside the live regions is kept current
   * instead. It gives nothing else away: a base square's tile is its
   * alliance and nothing more - the per-square calculator answers from the
   * base before it looks at terrain or mines - so this says only what the
   * panel is already saying. Dead does not enter into it, because a dead base
   * draws in its owner's colour and the tile does not move.
   *
   * A base inside a live region is skipped and left to the pass below, which
   * is about to write the square anyway. */
  numBases = basesGetNumBases(&sim->bs);
  for (i = 0; i < (int)numBases; i++) {
    square.left = square.right = (int)sim->bs->item[i].x;
    square.top = square.bottom = (int)sim->bs->item[i].y;
    if (overviewPointInRects(om->live, om->liveCount, square.left,
                             square.top) == TRUE) {
      continue;
    }
    if (overviewStampRect(om, sim, myPlayerNum, &square, FALSE, NULL) == TRUE) {
      changed = TRUE;
    }
  }

  /* The mask covers the blocks round the player's own tank and nothing else.
   * The player is looking out of their tank, so a building - and under the mode
   * that counts them a deep enough stand of trees - stops them seeing past it,
   * and under either Headlights everything outside the near squares and the
   * beam is dark as well; an item view looks out of the pillbox, base or allied
   * tank it is watching, and neither what is in the way of the tank nor which
   * way it points is anything to it. The build writes the halo first when there
   * is one and the tank block after it, so those are the rects at the head of
   * the list.
   *
   * Each block is masked over its own extent rather than one mask being shared:
   * the halo is centred on the tank while the lens sits wherever the classic
   * view has scrolled to, so either can reach squares the other does not. The
   * origin is the square the blocks were placed from, which for a tank that has
   * died and is holding its block is where it died. */
  hideOn = (tankLive == TRUE &&
            (in->sightMode != (uint8_t)fogSightOff ||
             overviewIsHeadlights(in->experiment) == TRUE));
  ownBlocks = 0;
  if (hideOn == TRUE) {
    ownBlocks = ((haloLive == TRUE) ? 1 : 0) + 1;
    if (ownBlocks > om->liveCount) {
      ownBlocks = om->liveCount;
    }
  }
  om->hiddenActive = (ownBlocks > 0);

  for (i = 0; i < om->liveCount; i++) {
    visPtr = NULL;
    if (i < ownBlocks) {
      /* Every square seen until something says otherwise, so a block too big
       * for the buffer - which no experiment builds today - reads as nothing
       * being hidden rather than as whatever the last one left behind. The beam
       * goes over the sight walk's answer and only ever takes squares away, so
       * with both running a square has to be in the beam and have a clear line
       * to it to come through. */
      memset(vis, 1, sizeof(vis));
      if (in->sightMode != (uint8_t)fogSightOff) {
        sightBuildMask(&sim->mp, useMX, useMY, sightRule, &om->live[i], vis);
      }
      if (overviewIsHeadlights(in->experiment) == TRUE) {
        overviewHeadlightMask(in->facing, useMX, useMY, &om->live[i], vis);
      }
      visPtr = vis;
    }
    if (overviewStampRect(om, sim, myPlayerNum, &om->live[i], TRUE, visPtr) ==
        TRUE) {
      changed = TRUE;
    }
  }

  /* Stripping the stale flag comes after the stamp, and skips whatever is
   * still live, so a square that was live and stays live is never written
   * twice for no reason. Only a square that has genuinely dropped out of the
   * live set reports a change here, which is what keeps generation still on a
   * tick where nothing moved. */
  for (i = 0; i < om->prevLiveCount; i++) {
    if (overviewClearLiveRect(om, &om->prevLive[i], om->live, om->liveCount) ==
        TRUE) {
      changed = TRUE;
    }
  }

  /* What produced a region this update, for the replay above to walk next
   * time. Recorded from the same predicates the build just used, so the two
   * cannot come apart. */
  om->haloWasLive = haloLive;
  om->tankWasLive = tankLive;
  for (i = 0; i < MAX_PILLS; i++) {
    om->pillWasLive[i] = overviewPillLive(sim, myPlayerNum, in, (BYTE)i,
                                          &alpha);
  }
  for (i = 0; i < MAX_BASES; i++) {
    om->baseWasLive[i] = overviewBaseLive(sim, myPlayerNum, in, (BYTE)i,
                                          &alpha);
  }
  for (i = 0; i < MAX_TANKS; i++) {
    om->allyWasLive[i] = overviewAllyLive(sim, myPlayerNum, in, (BYTE)i,
                                          &alpha);
  }

  /* Afterimage's fade, which is the only thing that writes om->fade. The span
   * is the seconds the fade takes in the caller's own ticks, kept inside a
   * byte, and never 0 - a span of 0 is what says nothing is fading, so a fade
   * that is running cannot have one. With no tick rate to measure the seconds
   * against there is nothing to fade with, so that reads as the experiment
   * being off.
   *
   * Leaving the experiment drops the whole trail on the tick it is left, so
   * coming back to it later starts from fog rather than from what was on the
   * map when the player last used it. Under every other experiment the span is
   * already 0 and none of this runs at all: nothing but Afterimage pays for a
   * walk of the map. */
  if (in->experiment == (uint8_t)fogExperimentAfterimage &&
      in->ticksPerSec > 0) {
    span = (unsigned)OVERVIEW_AFTERIMAGE_SECS * in->ticksPerSec;
    if (span > 255) {
      span = 255;
    }
    if (span < 1) {
      span = 1;
    }
    if (overviewFadeStep(om, (BYTE)span) == TRUE) {
      changed = TRUE;
    }
  } else if (om->fadeSpan != 0) {
    memset(om->fade, 0, sizeof(om->fade));
    om->fadeSpan = 0;
    changed = TRUE;
  }

  if (changed == TRUE) {
    om->generation++;
  }
}
