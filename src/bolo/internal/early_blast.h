/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Early own shell blast (TEST OPTION)
 *Filename:      early_blast.h
 *Purpose:
 *  The client option "Early Own Shell Blast", off by default,
 *  for a play test. With it on, when the local player's own
 *  predicted shell ends on this client (it is blocked on this
 *  client's view, or it runs out), the client draws the shell
 *  explosion there at once and plays its sound there at once.
 *  The server's explosion of that same shell, which comes about
 *  a round trip later, is then not drawn, and its sound is not
 *  played.
 *
 *  Nothing else changes: the server still does every hit test,
 *  the damage, the mines, the map changes and the builder rules,
 *  and other players see what they saw before.
 *
 *  Matching the server's explosion to the early blast:
 *  - CTRL_SHELL_DEATH carries the shell's fireTick and the place
 *    the server's shell ended. When it comes first, it gives the
 *    early blast with that fireTick the server place. A server
 *    place more than EARLY_BLAST_NEAR_WU from the client's place
 *    closes the early blast: the server's explosion is then drawn
 *    too, so the two places can be seen.
 *  - EVENT_EXPLOSION has no fireTick, only its place. It is the
 *    early blast's explosion when it is within
 *    EARLY_BLAST_SERVER_SLACK_WU of the server place, or, when the
 *    server place has not come yet, within EARLY_BLAST_NEAR_WU of
 *    the client's place. The nearest open early blast takes it,
 *    and each early blast takes one explosion at most.
 *  - An early blast that played a sound lets the next shell hit
 *    sound (shot building, shot tree, or a tank hit on another
 *    player) of the next EARLY_BLAST_SOUND_TICKS go unplayed.
 *  - An early blast that has no server explosion within
 *    EARLY_BLAST_WAIT_TICKS closes: no later explosion is hidden
 *    for it.
 *
 *  Places are in world units. An explosion's map and pixel place
 *  is its middle less 128 (TANK_SUBTRACT) on each axis, as
 *  shells.c gives explosionsAddItem.
 *
 *  Header only, with no includes but the C library's, so the
 *  client and the unit tests use the same code.
 *********************************************************/

#ifndef EARLY_BLAST_H
#define EARLY_BLAST_H

#include <stdbool.h>
#include <stdint.h>

/* The most early blasts held at once. The oldest is used again. */
#define EARLY_BLAST_MAX 16

/* A server place farther than this from the client's place is shown too. */
#define EARLY_BLAST_NEAR_WU 256

/* How far an explosion can be from the server's shell end place: a place
   cut to the pixel (16 world units) on each axis, with room. */
#define EARLY_BLAST_SERVER_SLACK_WU 48

/* How long an early blast waits for the server's explosion: 3 s. */
#define EARLY_BLAST_WAIT_TICKS 150

/* How long a hidden explosion's sound may come after it: 0.5 s. */
#define EARLY_BLAST_SOUND_TICKS 25

/* The most events of one batch the client checks for early blasts; any
   past it are drawn and played as before. */
#define EARLY_BLAST_EVENTS_MAX 256

/* An explosion's map and pixel place is its middle less this. */
#define EARLY_BLAST_EXPL_OFFSET 128

/* The sound an early blast played. */
enum {
  EARLY_BLAST_SOUND_NONE = 0, /* it ran out: the server plays none either */
  EARLY_BLAST_SOUND_BUILDING, /* a pillbox, a base or a wall */
  EARLY_BLAST_SOUND_TREE,     /* a forest */
  EARLY_BLAST_SOUND_TANK      /* another player's tank */
};

typedef struct {
  bool     active;     /* still waits for the server's explosion */
  uint32_t fireTick;   /* the input tick that fired the shell */
  uint32_t madeTick;   /* EarlyBlastSet.tick when it was made */
  int32_t  x, y;       /* the early blast's middle */
  uint8_t  sound;      /* EARLY_BLAST_SOUND_* */
  bool     serverKnown;
  int32_t  serverX, serverY; /* where the server's shell ended */
} EarlyBlast;

typedef struct {
  uint32_t made;      /* early blasts drawn */
  uint32_t hidden;    /* server explosions not drawn */
  uint32_t farEnds;   /* server explosions drawn too, as the server place was far */
  uint32_t timedOut;  /* early blasts with no server explosion found */
  uint32_t sounds;    /* server sounds not played */
} EarlyBlastStats;

typedef struct {
  EarlyBlast      items[EARLY_BLAST_MAX];
  int             next;
  uint32_t        tick;            /* game ticks this client has run */
  uint8_t         soundCredit;     /* shell hit sounds still to leave unplayed */
  uint32_t        soundCreditTick; /* tick the newest credit was given */
  EarlyBlastStats stats;
} EarlyBlastSet;

static inline void earlyBlastReset(EarlyBlastSet *s) {
  EarlyBlastStats keep = s->stats;
  int i;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    s->items[i].active = false;
  }
  s->next = 0;
  s->tick = 0;
  s->soundCredit = 0;
  s->soundCreditTick = 0;
  s->stats = keep;
}

/* The squared distance between two places. */
static inline int64_t earlyBlastDist2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  int64_t dx = (int64_t)ax - bx, dy = (int64_t)ay - by;
  return dx * dx + dy * dy;
}

/* One game tick: early blasts that waited too long close, and old sound
   credits go. */
static inline void earlyBlastTick(EarlyBlastSet *s) {
  int i;
  s->tick++;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    EarlyBlast *b = &s->items[i];
    if (b->active && s->tick - b->madeTick > EARLY_BLAST_WAIT_TICKS) {
      b->active = false;
      s->stats.timedOut++;
    }
  }
  if (s->soundCredit > 0 && s->tick - s->soundCreditTick > EARLY_BLAST_SOUND_TICKS) {
    s->soundCredit = 0;
  }
}

/* Note an early blast at (x, y) for the shell fired at fireTick. */
static inline void earlyBlastAdd(EarlyBlastSet *s, uint32_t fireTick, int32_t x, int32_t y,
                                 uint8_t sound) {
  EarlyBlast *b = &s->items[s->next];
  s->next = (s->next + 1) % EARLY_BLAST_MAX;
  b->active = true;
  b->fireTick = fireTick;
  b->madeTick = s->tick;
  b->x = x;
  b->y = y;
  b->sound = sound;
  b->serverKnown = false;
  b->serverX = b->serverY = 0;
  s->stats.made++;
}

/* The server's shell fired at fireTick ended at (x, y). Returns the early
   blast it belongs to, or NULL. One whose server place is far closes, so
   the server's explosion is drawn too. */
static inline EarlyBlast *earlyBlastServerEnd(EarlyBlastSet *s, uint32_t fireTick,
                                              int32_t x, int32_t y) {
  int i;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    EarlyBlast *b = &s->items[i];
    if (b->active && b->fireTick == fireTick) {
      b->serverKnown = true;
      b->serverX = x;
      b->serverY = y;
      if (earlyBlastDist2(b->x, b->y, x, y) >
          (int64_t)EARLY_BLAST_NEAR_WU * EARLY_BLAST_NEAR_WU) {
        b->active = false;
        s->stats.farEnds++;
      }
      return b;
    }
  }
  return NULL;
}

/* An explosion's middle from its map and pixel place. */
static inline void earlyBlastExplosionMiddle(uint8_t mx, uint8_t my, uint8_t px, uint8_t py,
                                             int32_t *x, int32_t *y) {
  *x = (int32_t)mx * 256 + (int32_t)(px & 15) * 16 + EARLY_BLAST_EXPL_OFFSET;
  *y = (int32_t)my * 256 + (int32_t)(py & 15) * 16 + EARLY_BLAST_EXPL_OFFSET;
}

/* An explosion's map and pixel place from its middle, as shells.c works
   it out for explosionsAddItem. */
static inline void earlyBlastExplosionPlace(int32_t x, int32_t y, uint8_t *mx, uint8_t *my,
                                            uint8_t *px, uint8_t *py) {
  uint32_t ex = (uint32_t)(x - EARLY_BLAST_EXPL_OFFSET) & 0xFFFFu;
  uint32_t ey = (uint32_t)(y - EARLY_BLAST_EXPL_OFFSET) & 0xFFFFu;
  *mx = (uint8_t)(ex >> 8);
  *my = (uint8_t)(ey >> 8);
  *px = (uint8_t)((ex & 255u) >> 4);
  *py = (uint8_t)((ey & 255u) >> 4);
}

/* A server explosion at map and pixel place (mx, my, px, py) came. Returns
   true when it is an open early blast's, which then closes: the client
   does not draw it. */
static inline bool earlyBlastClaimExplosion(EarlyBlastSet *s, uint8_t mx, uint8_t my,
                                            uint8_t px, uint8_t py) {
  int32_t ex, ey;
  int i, best = -1;
  int64_t bestD2 = 0;
  earlyBlastExplosionMiddle(mx, my, px, py, &ex, &ey);
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    const EarlyBlast *b = &s->items[i];
    int64_t d2, tol;
    if (!b->active) {
      continue;
    }
    if (b->serverKnown) {
      d2 = earlyBlastDist2(b->serverX, b->serverY, ex, ey);
      tol = (int64_t)EARLY_BLAST_SERVER_SLACK_WU * EARLY_BLAST_SERVER_SLACK_WU;
    } else {
      d2 = earlyBlastDist2(b->x, b->y, ex, ey);
      tol = (int64_t)EARLY_BLAST_NEAR_WU * EARLY_BLAST_NEAR_WU;
    }
    if (d2 <= tol && (best < 0 || d2 < bestD2)) {
      best = i;
      bestD2 = d2;
    }
  }
  if (best < 0) {
    return false;
  }
  s->items[best].active = false;
  s->stats.hidden++;
  if (s->items[best].sound != EARLY_BLAST_SOUND_NONE) {
    if (s->soundCredit < 255) {
      s->soundCredit++;
    }
    s->soundCreditTick = s->tick;
  }
  return true;
}

/* A server shell hit sound came. Returns true when a hidden explosion's
   early blast already played it, so the client does not play it again. */
static inline bool earlyBlastClaimSound(EarlyBlastSet *s) {
  if (s->soundCredit == 0 || s->tick - s->soundCreditTick > EARLY_BLAST_SOUND_TICKS) {
    return false;
  }
  s->soundCredit--;
  s->stats.sounds++;
  return true;
}

#endif /* EARLY_BLAST_H */
