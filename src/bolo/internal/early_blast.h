/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Early own shell blast
 *Filename:      early_blast.h
 *Purpose:
 *  For a human player (not a bot), when the local player's own
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
 *    place EARLY_BLAST_NEAR_WU (one map square) or more from the
 *    client's place closes the early blast: the server's explosion
 *    is then drawn too, so the two places can be seen.
 *  - EVENT_EXPLOSION has no fireTick, only its place. It is the
 *    early blast's explosion when it is within
 *    EARLY_BLAST_SERVER_SLACK_WU of the server place, or, when the
 *    server place has not come yet, less than EARLY_BLAST_NEAR_WU
 *    from the client's place. The nearest open early blast takes it,
 *    and each early blast takes one explosion at most.
 *  - A hidden explosion whose early blast played a sound gives a
 *    sound credit. The credit holds the sound's type (building,
 *    tree or tank hit) and the hidden explosion's map square. For
 *    the next EARLY_BLAST_SOUND_TICKS, one server sound of the same
 *    type whose pan and distance band fit that square goes
 *    unplayed. The server sends a pan and a distance band measured
 *    from its own place for this player's tank, not the square, so
 *    the client works out the pan and band the square gives from
 *    its own tank, and lets EARLY_BLAST_LISTENER_SLACK squares of
 *    difference through, as the two tank places can differ a
 *    little. With positional sound off only near or far is sent,
 *    and only that is matched.
 *  - An early blast that has no server explosion within
 *    EARLY_BLAST_WAIT_TICKS closes: no later explosion is hidden
 *    for it.
 *
 *  What is still approximate:
 *  - The server sends this player only the nearest sound of each
 *    type in a tick. When two shells of one type hit in the same
 *    tick, one sound comes, and a credit can be left over. For up
 *    to EARLY_BLAST_SOUND_TICKS it can then take a later sound of
 *    that type from about the same place.
 *  - The pan and distance band are coarse (two squares of pan,
 *    five of distance), so a sound of the same type from a near
 *    square in the same band can take the credit.
 *  - A teammate's shell that hits the same square as yours (a
 *    pillbox, a wall or a base, whose explosions are all at the
 *    middle of the square) cannot be told apart by place. Then
 *    the teammate's explosion may be hidden and yours drawn by
 *    the server too. A fix needs a shell id in the explosion event
 *    (a protocol change), and is left for later.
 *
 *  The counts in EarlyBlastStats go to the client's log at the
 *  end of each round when WINBOLO_LOG is set, so wrong early
 *  blasts can be counted.
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
#include <string.h>

/* The most early blasts held at once. The oldest is used again. */
#define EARLY_BLAST_MAX 16

/* A server place this far or farther from the client's place (one map
   square) is shown too. */
#define EARLY_BLAST_NEAR_WU 256

/* How far an explosion can be from the server's shell end place: a place
   cut to the pixel (16 world units) on each axis, with room. */
#define EARLY_BLAST_SERVER_SLACK_WU 48

/* How long an early blast waits for the server's explosion: 3 s. */
#define EARLY_BLAST_WAIT_TICKS 150

/* How long a hidden explosion's sound may come after it: 0.5 s. */
#define EARLY_BLAST_SOUND_TICKS 25

/* How many squares the server's place for this player's tank may differ
   from the client's when a sound's pan and band are matched to a credit. */
#define EARLY_BLAST_LISTENER_SLACK 2

/* The wire's pan and distance band (input_packet.h: SOUND_PAN_MAX,
   SOUND_PAN_STEP, SOUND_DIST_BAND, SOUND_DIST_MAX) and the near range
   (sounddist.h: SDIST_SOFT). client_sim.c checks they are the same. */
#define EARLY_BLAST_PAN_MAX   8
#define EARLY_BLAST_PAN_STEP  2
#define EARLY_BLAST_DIST_BAND 5
#define EARLY_BLAST_DIST_MAX  39
#define EARLY_BLAST_DIST_SOFT 15

/* The most events of one batch the client checks for early blasts; any
   past it are drawn and played as before. */
#define EARLY_BLAST_EVENTS_MAX 256

/* An explosion's map and pixel place is its middle less this. */
#define EARLY_BLAST_EXPL_OFFSET 128

/* The sound an early blast played: the one shellsCalcCollision plays. */
enum {
  EARLY_BLAST_SOUND_NONE = 0, /* it ran out, or a boat's shell hit grass,
                                 swamp, rubble, a crater or a road that
                                 stays: the server plays none either */
  EARLY_BLAST_SOUND_BUILDING, /* a pillbox, a base, a building, a half
                                 building, a boat, or a road shot away */
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

/* A hidden explosion's sound still to come from the server. */
typedef struct {
  bool     active;
  uint8_t  sound;    /* EARLY_BLAST_SOUND_*, never NONE */
  uint8_t  mx, my;   /* the hidden explosion's map square */
  uint32_t tick;     /* EarlyBlastSet.tick when it was given */
} EarlyBlastSoundCredit;

/* Counts for one round, written to the client's log at its end. */
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
  EarlyBlastSoundCredit credits[EARLY_BLAST_MAX]; /* the oldest is used again */
  int             nextCredit;
  EarlyBlastStats stats;
} EarlyBlastSet;

/* Clear the set and its counts, for a new round. */
static inline void earlyBlastReset(EarlyBlastSet *s) {
  int i;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    s->items[i].active = false;
  }
  s->next = 0;
  s->tick = 0;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    s->credits[i].active = false;
  }
  s->nextCredit = 0;
  memset(&s->stats, 0, sizeof(s->stats));
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
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    EarlyBlastSoundCredit *c = &s->credits[i];
    if (c->active && s->tick - c->tick > EARLY_BLAST_SOUND_TICKS) {
      c->active = false;
    }
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
   blast it belongs to, or NULL. One whose server place is one map square
   or more away closes, so the server's explosion is drawn too. */
static inline EarlyBlast *earlyBlastServerEnd(EarlyBlastSet *s, uint32_t fireTick,
                                              int32_t x, int32_t y) {
  int i;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    EarlyBlast *b = &s->items[i];
    if (b->active && b->fireTick == fireTick) {
      b->serverKnown = true;
      b->serverX = x;
      b->serverY = y;
      if (earlyBlastDist2(b->x, b->y, x, y) >=
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
    bool match;
    if (b->serverKnown) {
      d2 = earlyBlastDist2(b->serverX, b->serverY, ex, ey);
      tol = (int64_t)EARLY_BLAST_SERVER_SLACK_WU * EARLY_BLAST_SERVER_SLACK_WU;
      match = d2 <= tol;
    } else {
      /* One map square or more from the client's place is another place. */
      d2 = earlyBlastDist2(b->x, b->y, ex, ey);
      tol = (int64_t)EARLY_BLAST_NEAR_WU * EARLY_BLAST_NEAR_WU;
      match = d2 < tol;
    }
    if (match && (best < 0 || d2 < bestD2)) {
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
    EarlyBlastSoundCredit *c = &s->credits[s->nextCredit];
    s->nextCredit = (s->nextCredit + 1) % EARLY_BLAST_MAX;
    c->active = true;
    c->sound = s->items[best].sound;
    c->mx = (uint8_t)(((uint32_t)ex >> 8) & 0xFFu);
    c->my = (uint8_t)(((uint32_t)ey >> 8) & 0xFFu);
    c->tick = s->tick;
  }
  return true;
}

/* The east-west offsets, in squares, that a wire pan stands for. The
   server clamps the offset to +-EARLY_BLAST_PAN_MAX and cuts it towards
   zero to a multiple of EARLY_BLAST_PAN_STEP. */
static inline void earlyBlastPanRange(int pan, int *lo, int *hi) {
  if (pan >= EARLY_BLAST_PAN_MAX) {
    *lo = EARLY_BLAST_PAN_MAX;
    *hi = 255;
  } else if (pan <= -EARLY_BLAST_PAN_MAX) {
    *lo = -255;
    *hi = -EARLY_BLAST_PAN_MAX;
  } else if (pan > 0) {
    *lo = pan;
    *hi = pan + EARLY_BLAST_PAN_STEP - 1;
  } else if (pan < 0) {
    *lo = pan - EARLY_BLAST_PAN_STEP + 1;
    *hi = pan;
  } else {
    *lo = -(EARLY_BLAST_PAN_STEP - 1);
    *hi = EARLY_BLAST_PAN_STEP - 1;
  }
}

/* The larger-axis distances, in squares, that a wire distance band top
   stands for: 5 is 0 to 5, 10 is 6 to 10, ... 39 is 36 to 39. */
static inline void earlyBlastDistRange(int dist, int *lo, int *hi) {
  *lo = ((dist - 1) / EARLY_BLAST_DIST_BAND) * EARLY_BLAST_DIST_BAND + 1;
  if (*lo <= 1) {
    *lo = 0;
  }
  *hi = dist;
}

/* True when v is in lo .. hi, give or take EARLY_BLAST_LISTENER_SLACK. */
static inline bool earlyBlastInRange(int v, int lo, int hi) {
  return v >= lo - EARLY_BLAST_LISTENER_SLACK && v <= hi + EARLY_BLAST_LISTENER_SLACK;
}

/* True when a sound sent with pan and dist can come from square (mx, my)
   for a listener at (listenerMX, listenerMY). */
static inline bool earlyBlastSoundFits(uint8_t mx, uint8_t my, int8_t pan, uint8_t dist,
                                       uint8_t listenerMX, uint8_t listenerMY,
                                       bool positional) {
  int dx = (int)mx - (int)listenerMX;
  int dy = (int)my - (int)listenerMY;
  int ax = dx < 0 ? -dx : dx;
  int ay = dy < 0 ? -dy : dy;
  int d = ax > ay ? ax : ay;
  int lo, hi;
  if (!positional) {
    /* Only near (EARLY_BLAST_DIST_SOFT) or far is sent, centred. */
    if (dist <= EARLY_BLAST_DIST_SOFT) {
      return earlyBlastInRange(d, 0, EARLY_BLAST_DIST_SOFT);
    }
    return earlyBlastInRange(d, EARLY_BLAST_DIST_SOFT + 1, EARLY_BLAST_DIST_MAX);
  }
  earlyBlastPanRange((int)pan, &lo, &hi);
  if (!earlyBlastInRange(dx, lo, hi)) {
    return false;
  }
  earlyBlastDistRange((int)dist, &lo, &hi);
  return earlyBlastInRange(d, lo, hi);
}

/* A server shell hit sound of type sound (EARLY_BLAST_SOUND_*) came, sent
   with pan and dist, while this player's tank is at (listenerMX,
   listenerMY) on this client. Returns true when a hidden explosion's early
   blast already played it, so the client does not play it again: a credit
   of the same type whose square fits the pan and band is used. */
static inline bool earlyBlastClaimSound(EarlyBlastSet *s, uint8_t sound, int8_t pan,
                                        uint8_t dist, uint8_t listenerMX,
                                        uint8_t listenerMY, bool positional) {
  int i;
  for (i = 0; i < EARLY_BLAST_MAX; i++) {
    EarlyBlastSoundCredit *c = &s->credits[i];
    if (!c->active || c->sound != sound ||
        s->tick - c->tick > EARLY_BLAST_SOUND_TICKS) {
      continue;
    }
    if (!earlyBlastSoundFits(c->mx, c->my, pan, dist, listenerMX, listenerMY,
                             positional)) {
      continue;
    }
    c->active = false;
    s->stats.sounds++;
    return true;
  }
  return false;
}

#endif /* EARLY_BLAST_H */
